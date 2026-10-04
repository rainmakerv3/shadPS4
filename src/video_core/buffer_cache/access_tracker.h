// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

#include "common/types.h"

namespace VideoCore {

/**
 * Parts of a buffer the GPU accessed since the last barrier, rounded out to 256 bytes.
 *
 * Draws add a few ranges each, thousands of them between barriers in busy scenes, and keeping
 * them as sorted lists took a tenth of the GPU thread in inFAMOUS Second Son. Here a range sets
 * bits in a bitmap instead, each 64 bit word of it covering 16 KB, and a range covering all of
 * the 1 MB group of words it falls in only marks the group. Every mark carries the epoch it was
 * made in and only those of the current one count, so a barrier clears them all by moving to the
 * next epoch. Rounding out can only find more overlaps than there are, which costs a barrier that
 * wasn't needed: memory is uploaded whole 4 KB pages at a time anyway.
 */
class AccessTracker {
public:
    static constexpr u64 GranuleBits = 8;
    static constexpr u64 GranulesPerWord = 64;
    static constexpr u64 WordsPerGroup = 64;
    static constexpr u64 GroupsPerLeaf = 64;
    static constexpr u64 GranulesPerGroup = GranulesPerWord * WordsPerGroup;
    static constexpr u64 GroupGranuleBits = 12;
    static_assert(GranulesPerGroup == u64{1} << GroupGranuleBits);

    /// Marks [start, end) accessed in the epoch.
    void Add(u64 start, u64 end, u64 epoch) {
        if (start >= end) [[unlikely]] {
            return;
        }
        const u64 first = start >> GranuleBits;
        const u64 last = (end - 1) >> GranuleBits;
        for (u64 group_index = first >> GroupGranuleBits; group_index <= last >> GroupGranuleBits;
             ++group_index) {
            Group& group = GetGroup(group_index);
            const u64 group_first = group_index << GroupGranuleBits;
            const u64 group_last = group_first + GranulesPerGroup - 1;
            const u64 lo = std::max(first, group_first);
            const u64 hi = std::min(last, group_last);
            group.any_epoch = epoch;
            if (group.full_epoch == epoch) {
                continue;
            }
            if (lo == group_first && hi == group_last) {
                group.full_epoch = epoch;
                continue;
            }
            ForEachWord(lo - group_first, hi - group_first, [&](u64 word, u64 mask) {
                if (group.word_epochs[word] != epoch) {
                    group.word_epochs[word] = epoch;
                    group.words[word] = 0;
                }
                group.words[word] |= mask;
            });
        }
    }

    /// Returns true if any of [start, end) was marked accessed in the epoch.
    [[nodiscard]] bool Overlaps(u64 start, u64 end, u64 epoch) const {
        if (start >= end) [[unlikely]] {
            return false;
        }
        const u64 first = start >> GranuleBits;
        const u64 last = (end - 1) >> GranuleBits;
        for (u64 group_index = first >> GroupGranuleBits; group_index <= last >> GroupGranuleBits;
             ++group_index) {
            const Group* group = FindGroup(group_index);
            if (!group || group->any_epoch != epoch) {
                continue;
            }
            const u64 group_first = group_index << GroupGranuleBits;
            const u64 group_last = group_first + GranulesPerGroup - 1;
            const u64 lo = std::max(first, group_first);
            const u64 hi = std::min(last, group_last);
            // Something in the group was marked, and the range covers all of it.
            if (group->full_epoch == epoch || (lo == group_first && hi == group_last)) {
                return true;
            }
            bool found = false;
            ForEachWord(lo - group_first, hi - group_first, [&](u64 word, u64 mask) {
                found |= group->word_epochs[word] == epoch && (group->words[word] & mask) != 0;
            });
            if (found) {
                return true;
            }
        }
        return false;
    }

private:
    struct Group {
        u64 any_epoch{};
        u64 full_epoch{};
        std::array<u64, WordsPerGroup> word_epochs{};
        std::array<u64, WordsPerGroup> words{};
    };
    struct Leaf {
        std::array<Group, GroupsPerLeaf> groups{};
    };

    /// Calls func with each word overlapping the granules [lo, hi] of a group and the mask of
    /// the granules in it.
    static void ForEachWord(u64 lo, u64 hi, auto&& func) {
        const u64 first_word = lo / GranulesPerWord;
        const u64 last_word = hi / GranulesPerWord;
        const u64 first_mask = ~u64{0} << (lo % GranulesPerWord);
        const u64 last_mask = ~u64{0} >> (GranulesPerWord - 1 - hi % GranulesPerWord);
        if (first_word == last_word) {
            func(first_word, first_mask & last_mask);
            return;
        }
        func(first_word, first_mask);
        for (u64 word = first_word + 1; word < last_word; ++word) {
            func(word, ~u64{0});
        }
        func(last_word, last_mask);
    }

    Group& GetGroup(u64 group_index) {
        const u64 leaf_index = group_index / GroupsPerLeaf;
        if (leaf_index >= leaves.size()) {
            leaves.resize(leaf_index + 1);
        }
        auto& leaf = leaves[leaf_index];
        if (!leaf) {
            leaf = std::make_unique<Leaf>();
        }
        return leaf->groups[group_index % GroupsPerLeaf];
    }

    const Group* FindGroup(u64 group_index) const {
        const u64 leaf_index = group_index / GroupsPerLeaf;
        if (leaf_index >= leaves.size() || !leaves[leaf_index]) {
            return nullptr;
        }
        return &leaves[leaf_index]->groups[group_index % GroupsPerLeaf];
    }

    std::vector<std::unique_ptr<Leaf>> leaves;
};

/// What the GPU read and wrote of a buffer since the last barrier.
struct BufferAccesses {
    AccessTracker reads;
    AccessTracker writes;
};

} // namespace VideoCore
