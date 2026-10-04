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
 * some of them hundreds of megabytes long. Marks are kept at three levels: each 64 MB leaf has a
 * header with a mask of the 1 MB groups it has anything marked in and of those marked whole, and
 * only groups marked in part keep a mask of their 16 KB words with anything marked and a bitmap
 * of the 256 byte granules in those. A range then costs a header for each 64 MB it spans, plus
 * the words of the groups at its ends, whatever its length, and a check of memory nothing was
 * marked in mostly stops at the header. Headers sit together, so that check mostly hits the
 * cache.
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
    static constexpr u64 GranulesPerGroup = u64{1} << GroupGranuleBits;
    static constexpr u64 GroupsPerLeaf = u64{1} << LeafGroupBits;
    static constexpr u64 WordsPerGroup = u64{1} << (GroupGranuleBits - WordGranuleBits);
    static_assert(GroupsPerLeaf == 64 && WordsPerGroup == 64);

    /// Marks [start, end) accessed in the epoch.
    void Add(u64 start, u64 end, u64 epoch) {
        if (start >= end) [[unlikely]] {
            return;
        }
        // Draws mostly bind what the draw before did, so the range just marked often comes again.
        if (last_epoch == epoch && last.start <= start && end <= last.end) {
            return;
        }
        last = {start, end};
        last_epoch = epoch;

        const Span span{start, end};
        if (span.last_leaf >= leaves.size()) {
            leaves.resize(span.last_leaf + 1);
            groups.resize(span.last_leaf + 1);
        }
        if (span.first_group == span.last_group) [[likely]] {
            // Most ranges lie in one group.
            Leaf& leaf = CurrentLeaf(span.first_leaf, epoch);
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
            MarkWords(span.first_leaf, edge, epoch);
            return;
        }
        for (u64 leaf_index = span.first_leaf; leaf_index <= span.last_leaf; ++leaf_index) {
            Leaf& leaf = CurrentLeaf(leaf_index, epoch);
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
    }

    /// Returns true if any of [start, end) was marked accessed in the epoch.
    [[nodiscard]] bool Overlaps(u64 start, u64 end, u64 epoch) const {
        if (start >= end || leaves.empty()) [[unlikely]] {
            return false;
        }
        const Span span{start, end};
        if (span.first_group == span.last_group) [[likely]] {
            if (span.first_leaf >= leaves.size()) {
                return false;
            }
            const Leaf& leaf = leaves[span.first_leaf];
            const u64 group = span.first_group & (GroupsPerLeaf - 1);
            if (leaf.epoch != epoch || !(leaf.any_groups & Bit(group))) {
                return false;
            }
            const Edge edge{group, span.first_granule & (GranulesPerGroup - 1),
                            span.last_granule & (GranulesPerGroup - 1)};
            return (leaf.full_groups & Bit(group)) ||
                   (edge.first == 0 && edge.last == GranulesPerGroup - 1) ||
                   WordsOverlap(span.first_leaf, edge, epoch);
        }
        const u64 last_leaf = std::min<u64>(span.last_leaf, leaves.size() - 1);
        for (u64 leaf_index = span.first_leaf; leaf_index <= last_leaf; ++leaf_index) {
            const Leaf& leaf = leaves[leaf_index];
            if (leaf.epoch != epoch) {
                continue;
            }
            const LeafSpan part = span.InLeaf(leaf_index);
            const u64 hits = leaf.any_groups & part.groups;
            if (hits == 0) {
                continue;
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
        }
        return false;
    }

private:
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

    Leaf& CurrentLeaf(u64 leaf_index, u64 epoch) {
        Leaf& leaf = leaves[leaf_index];
        if (leaf.epoch != epoch) {
            leaf.epoch = epoch;
            leaf.any_groups = 0;
            leaf.full_groups = 0;
        }
        return leaf;
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

    struct Range {
        u64 start{};
        u64 end{};
    };
    /// The range marked last, in the epoch it was.
    Range last{};
    u64 last_epoch{};
    /// Headers of each 64 MB, kept together.
    std::vector<Leaf> leaves;
    /// Bitmaps of groups marked in part, made for a leaf the first time one of it is.
    std::vector<std::unique_ptr<std::array<Group, GroupsPerLeaf>>> groups;
};

/// What the GPU read and wrote of a buffer since the last barrier, and what it touched in any way
/// in the command buffer being recorded.
struct BufferAccesses {
    AccessTracker reads;
    AccessTracker writes;
    AccessTracker session;
};

} // namespace VideoCore
