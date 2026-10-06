// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <vector>

#include "common/types.h"

namespace VideoCore {

/**
 * Parts of a buffer the GPU accessed since some point, rounded out to 256 bytes.
 *
 * Draws add a few ranges each and check a few, tens of thousands a frame in inFAMOUS Second Son,
 * some of them hundreds of megabytes long. Marks are kept at four levels: each 4 GB has a header
 * with a mask of the 64 MB leaves it has anything marked in and of those marked whole, each leaf
 * has one with a mask of its 1 MB groups, and only groups marked in part keep a mask of their 16
 * KB words with anything marked and a bitmap of the 256 byte granules in those. A range then costs
 * a header for each 4 GB it spans, plus the leaves and groups at its ends, whatever its length,
 * and a check of memory nothing was marked in mostly stops at a header.
 *
 * Every mark carries the epoch it was made in, and only those of the current one count, so moving
 * to the next epoch clears them all. Rounding out can only find more overlaps than there are,
 * which costs a barrier that wasn't needed: memory is uploaded whole 4 KB pages at a time anyway.
 */
class AccessTracker {
public:
    static constexpr u64 GranuleBits = 8;
    static constexpr u64 WordGranuleBits = 6;
    static constexpr u64 GroupGranuleBits = 12;
    static constexpr u64 LeafGroupBits = 6;
    static constexpr u64 TopLeafBits = 6;
    static constexpr u64 GranulesPerGroup = u64{1} << GroupGranuleBits;
    static constexpr u64 GroupsPerLeaf = u64{1} << LeafGroupBits;
    static constexpr u64 LeavesPerTop = u64{1} << TopLeafBits;
    static constexpr u64 LeafGranuleBits = GroupGranuleBits + LeafGroupBits;
    static constexpr u64 GranulesPerLeaf = u64{1} << LeafGranuleBits;
    static constexpr u64 WordsPerGroup = u64{1} << (GroupGranuleBits - WordGranuleBits);
    static_assert(GroupsPerLeaf == 64 && WordsPerGroup == 64 && LeavesPerTop == 64);

    /// Marks [start, end) accessed in the epoch.
    void Add(u64 start, u64 end, u64 epoch) {
        if (start >= end) [[unlikely]] {
            return;
        }
        // Draws mostly bind what the draws before did, so ranges marked lately often come again.
        Recent& recent = recent_ranges[RecentIndex(start, end)];
        if (recent.epoch == epoch && recent.start == start && recent.end == end) {
            return;
        }
        recent = {start, end, epoch};

        const Span span{start, end};
        if (span.last_leaf >= leaves.size()) {
            leaves.resize(span.last_leaf + 1);
            groups.resize(span.last_leaf + 1);
            tops.resize((span.last_leaf >> TopLeafBits) + 1);
        }
        if (span.first_leaf == span.last_leaf) [[likely]] {
            Top& top = CurrentTop(span.first_leaf >> TopLeafBits, epoch);
            const u64 leaf_bit = Bit(span.first_leaf & (LeavesPerTop - 1));
            top.any_leaves |= leaf_bit;
            if (!(top.full_leaves & leaf_bit)) {
                AddInLeaf(span, span.first_leaf, epoch);
            }
            return;
        }
        const u64 last_top = span.last_leaf >> TopLeafBits;
        for (u64 top_index = span.first_leaf >> TopLeafBits; top_index <= last_top; ++top_index) {
            Top& top = CurrentTop(top_index, epoch);
            const TopSpan part = span.InTop(top_index);
            top.any_leaves |= part.leaves;
            top.full_leaves |= part.full_leaves;
            // Leaves at the ends of the range it covers in part, unless already marked whole.
            for (const u64 leaf_index : part.edges) {
                if (leaf_index != NoLeaf &&
                    !(top.full_leaves & Bit(leaf_index & (LeavesPerTop - 1)))) {
                    AddInLeaf(span, leaf_index, epoch);
                }
            }
        }
    }

    /// Returns true if any of [start, end) was marked accessed in the epoch.
    [[nodiscard]] bool Overlaps(u64 start, u64 end, u64 epoch) const {
        if (start >= end || tops.empty()) [[unlikely]] {
            return false;
        }
        const Span span{start, end};
        if (span.first_leaf == span.last_leaf) [[likely]] {
            const u64 top_index = span.first_leaf >> TopLeafBits;
            if (top_index >= tops.size()) {
                return false;
            }
            const Top& top = tops[top_index];
            const u64 leaf_bit = Bit(span.first_leaf & (LeavesPerTop - 1));
            if (top.epoch != epoch || !(top.any_leaves & leaf_bit)) {
                return false;
            }
            return (top.full_leaves & leaf_bit) || OverlapsInLeaf(span, span.first_leaf, epoch);
        }
        const u64 last_top = std::min<u64>(span.last_leaf >> TopLeafBits, tops.size() - 1);
        for (u64 top_index = span.first_leaf >> TopLeafBits; top_index <= last_top; ++top_index) {
            const Top& top = tops[top_index];
            if (top.epoch != epoch) {
                continue;
            }
            const TopSpan part = span.InTop(top_index);
            const u64 hits = top.any_leaves & part.leaves;
            if (hits == 0) {
                continue;
            }
            // A leaf marked whole that the range touches, or one with anything marked that the
            // range covers whole.
            if ((top.full_leaves & part.leaves) != 0 || (hits & part.full_leaves) != 0) {
                return true;
            }
            for (const u64 leaf_index : part.edges) {
                if (leaf_index != NoLeaf && (hits & Bit(leaf_index & (LeavesPerTop - 1))) &&
                    OverlapsInLeaf(span, leaf_index, epoch)) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    /// Fills the slots of TopSpan::edges that hold no leaf.
    static constexpr u64 NoLeaf = ~u64{0};

    /// Leaves marked whole are only marked here, so a leaf's header is only current when it is
    /// marked in part.
    struct Top {
        u64 epoch{};
        u64 any_leaves{};
        u64 full_leaves{};
    };

    struct Leaf {
        u64 epoch{};
        u64 any_groups{};
        u64 full_groups{};
    };

    struct Group {
        u64 epoch{};
        /// Words with anything marked, only those words of the bitmap are valid.
        u64 any_words{};
        /// Words marked whole, whose bitmap words needn't be set.
        u64 full_words{};
        std::array<u64, WordsPerGroup> words{};
    };

    /// A group a range covers in part, with the first and last granule of it the range covers.
    struct Edge {
        u64 group = GroupsPerLeaf;
        u64 first{};
        u64 last{};
    };

    struct LeafSpan {
        u64 groups{};
        u64 full_groups{};
        std::array<Edge, 2> edges{};
    };

    struct TopSpan {
        u64 leaves{};
        u64 full_leaves{};
        /// Leaves the range covers in part, by index.
        std::array<u64, 2> edges{NoLeaf, NoLeaf};
    };

    struct Span {
        u64 first_granule;
        u64 last_granule;
        u64 first_group;
        u64 last_group;
        u64 first_leaf;
        u64 last_leaf;

        Span(u64 start, u64 end)
            : first_granule{start >> GranuleBits}, last_granule{(end - 1) >> GranuleBits},
              first_group{first_granule >> GroupGranuleBits},
              last_group{last_granule >> GroupGranuleBits},
              first_leaf{first_group >> LeafGroupBits}, last_leaf{last_group >> LeafGroupBits} {}

        /// The leaves of a top the range touches and covers whole, and the ones it covers in part.
        TopSpan InTop(u64 top_index) const {
            const u64 base = top_index << TopLeafBits;
            const u64 lo = std::max(first_leaf, base) - base;
            const u64 hi = std::min(last_leaf, base + LeavesPerTop - 1) - base;
            TopSpan part{};
            part.leaves = BitRange(lo, hi);
            part.full_leaves = part.leaves;
            size_t num_edges = 0;
            const auto add_edge = [&](u64 leaf) {
                if (leaf < base || leaf >= base + LeavesPerTop) {
                    return;
                }
                const u64 leaf_first = leaf << LeafGranuleBits;
                const u64 first = std::max(first_granule, leaf_first) - leaf_first;
                const u64 last =
                    std::min(last_granule, leaf_first + GranulesPerLeaf - 1) - leaf_first;
                if (first == 0 && last == GranulesPerLeaf - 1) {
                    return;
                }
                part.full_leaves &= ~Bit(leaf - base);
                part.edges[num_edges++] = leaf;
            };
            add_edge(first_leaf);
            if (last_leaf != first_leaf) {
                add_edge(last_leaf);
            }
            return part;
        }

        /// The groups of a leaf the range touches and covers whole, and the ones it covers in part.
        LeafSpan InLeaf(u64 leaf_index) const {
            const u64 base = leaf_index << LeafGroupBits;
            const u64 lo = std::max(first_group, base) - base;
            const u64 hi = std::min(last_group, base + GroupsPerLeaf - 1) - base;
            LeafSpan part{};
            part.groups = BitRange(lo, hi);
            part.full_groups = part.groups;
            size_t num_edges = 0;
            const auto add_edge = [&](u64 group) {
                if (group < base || group >= base + GroupsPerLeaf) {
                    return;
                }
                const u64 group_first = group << GroupGranuleBits;
                const u64 first = std::max(first_granule, group_first) - group_first;
                const u64 last =
                    std::min(last_granule, group_first + GranulesPerGroup - 1) - group_first;
                if (first == 0 && last == GranulesPerGroup - 1) {
                    return;
                }
                part.full_groups &= ~Bit(group - base);
                part.edges[num_edges++] = Edge{group - base, first, last};
            };
            add_edge(first_group);
            if (last_group != first_group) {
                add_edge(last_group);
            }
            return part;
        }
    };

    static constexpr u64 Bit(u64 index) {
        return u64{1} << index;
    }

    /// Bits lo to hi inclusive, both below 64.
    static constexpr u64 BitRange(u64 lo, u64 hi) {
        return (~u64{0} << lo) & (~u64{0} >> (63 - hi));
    }

    Top& CurrentTop(u64 top_index, u64 epoch) {
        Top& top = tops[top_index];
        if (top.epoch != epoch) {
            top.epoch = epoch;
            top.any_leaves = 0;
            top.full_leaves = 0;
        }
        return top;
    }

    Leaf& CurrentLeaf(u64 leaf_index, u64 epoch) {
        Leaf& leaf = leaves[leaf_index];
        if (leaf.epoch != epoch) {
            leaf.epoch = epoch;
            leaf.any_groups = 0;
            leaf.full_groups = 0;
        }
        return leaf;
    }

    /// Marks the part of a range in a leaf it doesn't cover whole.
    void AddInLeaf(const Span& span, u64 leaf_index, u64 epoch) {
        Leaf& leaf = CurrentLeaf(leaf_index, epoch);
        if (span.first_group == span.last_group) [[likely]] {
            // Most ranges lie in one group.
            const u64 group = span.first_group & (GroupsPerLeaf - 1);
            leaf.any_groups |= Bit(group);
            if (leaf.full_groups & Bit(group)) {
                return;
            }
            const Edge edge{group, span.first_granule & (GranulesPerGroup - 1),
                            span.last_granule & (GranulesPerGroup - 1)};
            if (edge.first == 0 && edge.last == GranulesPerGroup - 1) {
                leaf.full_groups |= Bit(group);
                return;
            }
            MarkWords(leaf_index, edge, epoch);
            return;
        }
        const LeafSpan part = span.InLeaf(leaf_index);
        leaf.any_groups |= part.groups;
        leaf.full_groups |= part.full_groups;
        // Groups at the ends of the range it covers in part, unless already marked whole.
        for (const auto& edge : part.edges) {
            if (edge.group < GroupsPerLeaf && !(leaf.full_groups & Bit(edge.group))) {
                MarkWords(leaf_index, edge, epoch);
            }
        }
    }

    /// Returns true if the part of a range in a leaf marked in part overlaps what is marked there.
    bool OverlapsInLeaf(const Span& span, u64 leaf_index, u64 epoch) const {
        const Leaf& leaf = leaves[leaf_index];
        if (leaf.epoch != epoch) {
            return false;
        }
        if (span.first_group == span.last_group) [[likely]] {
            const u64 group = span.first_group & (GroupsPerLeaf - 1);
            if (!(leaf.any_groups & Bit(group))) {
                return false;
            }
            const Edge edge{group, span.first_granule & (GranulesPerGroup - 1),
                            span.last_granule & (GranulesPerGroup - 1)};
            return (leaf.full_groups & Bit(group)) ||
                   (edge.first == 0 && edge.last == GranulesPerGroup - 1) ||
                   WordsOverlap(leaf_index, edge, epoch);
        }
        const LeafSpan part = span.InLeaf(leaf_index);
        const u64 hits = leaf.any_groups & part.groups;
        if (hits == 0) {
            return false;
        }
        // A group marked whole that the range touches, or one with anything marked that the
        // range covers whole.
        if ((leaf.full_groups & part.groups) != 0 || (hits & part.full_groups) != 0) {
            return true;
        }
        for (const auto& edge : part.edges) {
            if (edge.group < GroupsPerLeaf && (hits & Bit(edge.group)) &&
                WordsOverlap(leaf_index, edge, epoch)) {
                return true;
            }
        }
        return false;
    }

    Group& GetGroup(u64 leaf_index, u64 group) {
        auto& leaf_groups = groups[leaf_index];
        if (!leaf_groups) {
            leaf_groups = std::make_unique<std::array<Group, GroupsPerLeaf>>();
        }
        return (*leaf_groups)[group];
    }

    /// The words of a group the granules [first, last] of it touch and cover whole, and the masks
    /// of the granules in the first and last of them.
    struct WordSpan {
        u64 first_word;
        u64 last_word;
        u64 words;
        u64 full_words;
        u64 first_mask;
        u64 last_mask;

        explicit WordSpan(const Edge& edge)
            : first_word{edge.first >> WordGranuleBits}, last_word{edge.last >> WordGranuleBits},
              words{BitRange(first_word, last_word)}, full_words{words},
              first_mask{BitRange(edge.first & 63, first_word == last_word ? edge.last & 63 : 63)},
              last_mask{BitRange(first_word == last_word ? edge.first & 63 : 0, edge.last & 63)} {
            if (first_mask != ~u64{0}) {
                full_words &= ~Bit(first_word);
            }
            if (last_mask != ~u64{0}) {
                full_words &= ~Bit(last_word);
            }
        }
    };

    void MarkWords(u64 leaf_index, const Edge& edge, u64 epoch) {
        Group& group = GetGroup(leaf_index, edge.group);
        if (group.epoch != epoch) {
            group.epoch = epoch;
            group.any_words = 0;
            group.full_words = 0;
        }
        const WordSpan span{edge};
        // Only the words at the ends can be covered in part.
        const auto mark = [&](u64 word, u64 mask) {
            if (mask == ~u64{0} || (group.full_words & Bit(word))) {
                return;
            }
            if (group.any_words & Bit(word)) {
                group.words[word] |= mask;
            } else {
                group.words[word] = mask;
            }
        };
        mark(span.first_word, span.first_mask);
        if (span.last_word != span.first_word) {
            mark(span.last_word, span.last_mask);
        }
        group.any_words |= span.words;
        group.full_words |= span.full_words;
    }

    bool WordsOverlap(u64 leaf_index, const Edge& edge, u64 epoch) const {
        const auto& leaf_groups = groups[leaf_index];
        if (!leaf_groups) {
            return false;
        }
        const Group& group = (*leaf_groups)[edge.group];
        if (group.epoch != epoch) {
            return false;
        }
        const WordSpan span{edge};
        const u64 touched = group.any_words & span.words;
        if (touched == 0) {
            return false;
        }
        // A word marked whole that the range touches, or one with anything marked that the range
        // covers whole.
        if ((group.full_words & span.words) != 0 || (touched & span.full_words) != 0) {
            return true;
        }
        const auto overlaps = [&](u64 word, u64 mask) {
            return (touched & Bit(word)) && (group.words[word] & mask) != 0;
        };
        return overlaps(span.first_word, span.first_mask) ||
               (span.last_word != span.first_word && overlaps(span.last_word, span.last_mask));
    }

    struct Recent {
        u64 start{};
        u64 end{};
        u64 epoch{};
    };
    static constexpr size_t NumRecent = 16;

    static size_t RecentIndex(u64 start, u64 end) {
        const u64 hash = (start ^ (end * 0x9E3779B97F4A7C15ULL)) * 0xBF58476D1CE4E5B9ULL;
        return static_cast<size_t>(hash >> (64 - std::countr_zero(NumRecent)));
    }

    /// Ranges marked lately, with the epoch each was marked in.
    std::array<Recent, NumRecent> recent_ranges{};
    /// Headers of each 4 GB.
    std::vector<Top> tops;
    /// Headers of each 64 MB, kept together.
    std::vector<Leaf> leaves;
    /// Bitmaps of groups marked in part, made for a leaf the first time one of it is.
    std::vector<std::unique_ptr<std::array<Group, GroupsPerLeaf>>> groups;
};

/**
 * Ranges of a buffer the GPU accessed in an epoch, kept to the byte as a list.
 *
 * Rounding accesses out to the granules of AccessTracker made ones next to each other look like
 * they overlap. inFAMOUS Second Son runs hundreds of tiny dispatches a frame that each write 24
 * bytes next to where the one before wrote, and every one of them got a barrier, waiting for the
 * one before to finish. Ends of larger ranges rounded out the same way. Accesses are kept here
 * instead, up to a number of them an epoch, and only the ones that don't fit go to the tracker.
 * Adding one is cheap, and checks of memory far from all of them stop at their bounds.
 */
template <size_t Capacity>
class ExactRanges {
public:
    /// Keeps [start, end) as accessed in the epoch. Returns false if no more fit, and it has to
    /// be marked in the tracker.
    bool Add(u64 start, u64 end, u64 epoch) {
        if (start >= end) [[unlikely]] {
            return true;
        }
        if (epoch != current_epoch) {
            current_epoch = epoch;
            count = 0;
            low = ~u64{0};
            high = 0;
        }
        // Draws mostly bind what the few draws before them did.
        for (size_t i = count, seen = 0; i > 0 && seen < 4; --i, ++seen) {
            if (ranges[i - 1].start == start && ranges[i - 1].end == end) {
                return true;
            }
        }
        if (count == Capacity) {
            return false;
        }
        ranges[count++] = {start, end};
        low = std::min(low, start);
        high = std::max(high, end);
        return true;
    }

    /// Returns true if any of [start, end) was kept as accessed in the epoch.
    [[nodiscard]] bool Overlaps(u64 start, u64 end, u64 epoch) const {
        if (epoch != current_epoch || end <= low || start >= high) {
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            if (ranges[i].start < end && start < ranges[i].end) {
                return true;
            }
        }
        return false;
    }

private:
    struct Range {
        u64 start;
        u64 end;
    };

    std::array<Range, Capacity> ranges{};
    size_t count{};
    u64 current_epoch{};
    /// Bounds of the ranges kept.
    u64 low{~u64{0}};
    u64 high{};
};

/**
 * Pages of a buffer the GPU touched in a session, as a flat bitmap with a bit for each 4 KB.
 *
 * Every buffer a draw binds is noted as touched in the session, over 50000 times a frame in
 * inFAMOUS Second Son, and marking each in an AccessTracker went through its headers, a cache miss
 * at each level. It is only asked about memory uploads copy to, which comes a page at a time, so
 * keeping pages answers the same. A word covers 64 pages and is marked with the session it was
 * marked in, and a chunk of 64 words can be marked whole, so ranges of any length cost a word or
 * two at their ends.
 */
class SessionPages {
public:
    static constexpr u64 PageBits = 12;

    /// Marks [start, end) touched in the session.
    void Add(u64 start, u64 end, u64 session) {
        if (start >= end) [[unlikely]] {
            return;
        }
        const u64 first_page = start >> PageBits;
        const u64 last_page = (end - 1) >> PageBits;
        const u64 first_word = first_page >> 6;
        const u64 last_word = last_page >> 6;
        if (last_word >= words.size()) {
            words.resize(last_word + 1);
            chunks.resize((last_word >> 6) + 1);
        }
        if (first_word == last_word) [[likely]] {
            MarkWord(first_word, BitRange(first_page & 63, last_page & 63), session);
            return;
        }
        MarkWord(first_word, BitRange(first_page & 63, 63), session);
        MarkWord(last_word, BitRange(0, last_page & 63), session);
        // The words in between are covered whole, which their chunks note.
        if (last_word - first_word < 2) {
            return;
        }
        const u64 lo = first_word + 1;
        const u64 hi = last_word - 1;
        for (u64 chunk = lo >> 6; chunk <= hi >> 6; ++chunk) {
            const u64 base = chunk << 6;
            MarkChunk(chunk, BitRange(std::max(lo, base) - base, std::min(hi, base + 63) - base),
                      session);
        }
    }

    /// Returns true if any page of [start, end) was touched in the session.
    [[nodiscard]] bool Overlaps(u64 start, u64 end, u64 session) const {
        if (start >= end || words.empty()) {
            return false;
        }
        const u64 first_page = start >> PageBits;
        const u64 last_page = (end - 1) >> PageBits;
        const u64 first_word = first_page >> 6;
        const u64 last_word = std::min<u64>(last_page >> 6, words.size() - 1);
        for (u64 word = first_word; word <= last_word; ++word) {
            const Chunk& chunk = chunks[word >> 6];
            if (chunk.session == session && (chunk.full_words & Bit(word & 63)) != 0) {
                return true;
            }
            const u64 mask = BitRange(word == first_word ? first_page & 63 : 0,
                                      word == (last_page >> 6) ? last_page & 63 : 63);
            if (words[word].session == session && (words[word].pages & mask) != 0) {
                return true;
            }
        }
        return false;
    }

private:
    /// The pages of 256 KB marked in a session.
    struct Word {
        u64 session{};
        u64 pages{};
    };

    /// The words of 16 MB marked whole in a session.
    struct Chunk {
        u64 session{};
        u64 full_words{};
    };

    static constexpr u64 Bit(u64 index) {
        return u64{1} << index;
    }

    /// Bits lo to hi inclusive, both below 64.
    static constexpr u64 BitRange(u64 lo, u64 hi) {
        return (~u64{0} << lo) & (~u64{0} >> (63 - hi));
    }

    void MarkWord(u64 index, u64 mask, u64 session) {
        Word& word = words[index];
        if (word.session != session) {
            word = {session, mask};
        } else {
            word.pages |= mask;
        }
    }

    void MarkChunk(u64 index, u64 mask, u64 session) {
        Chunk& chunk = chunks[index];
        if (chunk.session != session) {
            chunk = {session, mask};
        } else {
            chunk.full_words |= mask;
        }
    }

    std::vector<Word> words;
    std::vector<Chunk> chunks;
};

/// What the GPU read and wrote of a buffer since the last barrier, and what it touched in any way
/// in the command buffer being recorded. Reads are only looked through for writes, which are far
/// fewer, so more of them are kept to the byte.
struct BufferAccesses {
    AccessTracker reads;
    AccessTracker writes;
    ExactRanges<256> exact_reads;
    ExactRanges<64> exact_writes;
    SessionPages session;
};

} // namespace VideoCore
