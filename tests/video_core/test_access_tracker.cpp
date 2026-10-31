// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/buffer_cache/access_tracker.h"

namespace VideoCore {
namespace {

constexpr u64 MB = u64{1} << 20;

TEST(AccessTracker, FindsOverlapsInTheCurrentEpochOnly) {
    AccessTracker tracker;
    tracker.Add(0x1000, 0x1100, 1);
    EXPECT_TRUE(tracker.Overlaps(0x10F0, 0x2000, 1));
    EXPECT_FALSE(tracker.Overlaps(0x1100, 0x2000, 1));
    EXPECT_FALSE(tracker.Overlaps(0x0, 0x1000, 1));
    // The next epoch starts with nothing marked.
    EXPECT_FALSE(tracker.Overlaps(0x0, 0x2000, 2));
    tracker.Add(0x5000, 0x5004, 2);
    EXPECT_FALSE(tracker.Overlaps(0x1000, 0x1100, 2));
    EXPECT_TRUE(tracker.Overlaps(0x5000, 0x5001, 2));
}

TEST(AccessTracker, RoundsOutToGranules) {
    AccessTracker tracker;
    tracker.Add(0x1010, 0x1020, 1);
    // Same 256 byte granule, so it counts as overlapping.
    EXPECT_TRUE(tracker.Overlaps(0x1080, 0x1090, 1));
    EXPECT_FALSE(tracker.Overlaps(0x1100, 0x1200, 1));
}

TEST(AccessTracker, HandlesRangesCoveringWholeGroups) {
    AccessTracker tracker;
    tracker.Add(3 * MB - 0x100, 70 * MB + 0x100, 1);
    EXPECT_TRUE(tracker.Overlaps(64 * MB, 64 * MB + 1, 1));
    EXPECT_TRUE(tracker.Overlaps(70 * MB, 70 * MB + 1, 1));
    EXPECT_FALSE(tracker.Overlaps(70 * MB + 0x100, 71 * MB, 1));
    EXPECT_FALSE(tracker.Overlaps(0, 3 * MB - 0x100, 1));
    // A query covering a group with anything marked in it overlaps.
    tracker.Add(100 * MB + 0x4000, 100 * MB + 0x4010, 1);
    EXPECT_TRUE(tracker.Overlaps(99 * MB, 102 * MB, 1));
    EXPECT_FALSE(tracker.Overlaps(101 * MB, 102 * MB, 1));
}

TEST(AccessTracker, MatchesAGranuleModel) {
    std::mt19937_64 rng{1234};
    constexpr u64 Space = 300 * MB;
    const auto random_range = [&] {
        const u64 start = rng() % Space;
        u64 size{};
        switch (rng() % 4) {
        case 0:
            size = rng() % 64 + 1;
            break;
        case 1:
            size = rng() % 20000 + 1;
            break;
        case 2:
            size = rng() % (3 * MB) + 1;
            break;
        default:
            size = rng() % (80 * MB) + 1;
            break;
        }
        return std::pair{start, std::min(start + size, Space)};
    };
    for (int round = 0; round < 50; ++round) {
        AccessTracker tracker;
        std::vector<std::pair<u64, u64>> marked;
        u64 epoch = 1;
        for (int op = 0; op < 2000; ++op) {
            const u64 kind = rng() % 100;
            const auto [start, end] = random_range();
            if (kind < 40) {
                tracker.Add(start, end, epoch);
                marked.emplace_back(start, end);
            } else if (kind < 99) {
                bool overlaps = false;
                for (const auto& [marked_start, marked_end] : marked) {
                    overlaps |= (start >> AccessTracker::GranuleBits) <=
                                    ((marked_end - 1) >> AccessTracker::GranuleBits) &&
                                (marked_start >> AccessTracker::GranuleBits) <=
                                    ((end - 1) >> AccessTracker::GranuleBits);
                }
                ASSERT_EQ(tracker.Overlaps(start, end, epoch), overlaps);
            } else {
                ++epoch;
                marked.clear();
            }
        }
    }
}

TEST(AccessTracker, MatchesAGranuleModelWithRangesSpanningLeaves) {
    // Ranges up to gigabytes, starting and ending anywhere around 64 MB leaf and 1 MB group
    // boundaries.
    std::mt19937_64 rng{5678};
    constexpr u64 Space = 3 * 1024 * MB;
    const auto random_point = [&] {
        const u64 boundary = (rng() % (Space / (64 * MB))) * 64 * MB;
        switch (rng() % 4) {
        case 0:
            return boundary;
        case 1:
            return boundary + (rng() % 64) * MB;
        case 2:
            return boundary + (rng() % 64) * MB + rng() % 4096;
        default:
            return rng() % Space;
        }
    };
    for (int round = 0; round < 40; ++round) {
        AccessTracker tracker;
        std::vector<std::pair<u64, u64>> marked;
        u64 epoch = round * 3 + 1;
        for (int op = 0; op < 1500; ++op) {
            u64 start = random_point();
            u64 end = random_point();
            if (start > end) {
                std::swap(start, end);
            }
            if (start == end) {
                end = start + 1;
            }
            const u64 kind = rng() % 100;
            if (kind < 40) {
                tracker.Add(start, end, epoch);
                marked.emplace_back(start, end);
            } else if (kind < 98) {
                bool overlaps = false;
                for (const auto& [marked_start, marked_end] : marked) {
                    overlaps |= (start >> AccessTracker::GranuleBits) <=
                                    ((marked_end - 1) >> AccessTracker::GranuleBits) &&
                                (marked_start >> AccessTracker::GranuleBits) <=
                                    ((end - 1) >> AccessTracker::GranuleBits);
                }
                ASSERT_EQ(tracker.Overlaps(start, end, epoch), overlaps);
            } else {
                ++epoch;
                marked.clear();
            }
        }
    }
}

TEST(AccessTracker, HandlesRangesCoveringWholeLeaves) {
    AccessTracker tracker;
    constexpr u64 GB = 1024 * MB;
    tracker.Add(64 * MB - 0x100, 5 * GB + 0x100, 1);
    EXPECT_TRUE(tracker.Overlaps(4 * GB - 1, 4 * GB, 1));
    EXPECT_TRUE(tracker.Overlaps(2 * GB, 2 * GB + 1, 1));
    EXPECT_TRUE(tracker.Overlaps(5 * GB, 5 * GB + 1, 1));
    EXPECT_FALSE(tracker.Overlaps(5 * GB + 0x100, 6 * GB, 1));
    EXPECT_FALSE(tracker.Overlaps(0, 64 * MB - 0x100, 1));
    // Marking within a leaf marked whole changes nothing, and a later epoch has nothing marked.
    tracker.Add(GB + 0x40, GB + 0x80, 1);
    EXPECT_TRUE(tracker.Overlaps(GB + 0x1000, GB + 0x1100, 1));
    EXPECT_FALSE(tracker.Overlaps(GB, GB + 0x1000, 2));
    tracker.Add(GB + 0x40, GB + 0x80, 2);
    EXPECT_TRUE(tracker.Overlaps(GB, GB + 0x1000, 2));
    EXPECT_FALSE(tracker.Overlaps(GB + 0x1000, 2 * GB, 2));
}

TEST(AccessTracker, MatchesAGranuleModelWithRangesSpanningTops) {
    // Ranges up to many gigabytes, starting and ending anywhere around 4 GB top, 64 MB leaf and
    // 1 MB group boundaries.
    std::mt19937_64 rng{91011};
    constexpr u64 GB = 1024 * MB;
    constexpr u64 Space = 10 * GB;
    const auto random_point = [&] {
        const u64 top = (rng() % (Space / (4 * GB) + 1)) * 4 * GB;
        const u64 leaf = (rng() % (Space / (64 * MB))) * 64 * MB;
        switch (rng() % 6) {
        case 0:
            return top;
        case 1:
            return top - std::min<u64>(top, (rng() % 3) * 64 * MB + rng() % 3);
        case 2:
            return leaf;
        case 3:
            return leaf + (rng() % 64) * MB;
        case 4:
            return leaf + (rng() % 64) * MB + rng() % 4096;
        default:
            return rng() % Space;
        }
    };
    for (int round = 0; round < 30; ++round) {
        AccessTracker tracker;
        std::vector<std::pair<u64, u64>> marked;
        u64 epoch = round * 3 + 1;
        for (int op = 0; op < 1500; ++op) {
            u64 start = std::min(random_point(), Space - 1);
            u64 end = std::min(random_point(), Space);
            if (start > end) {
                std::swap(start, end);
            }
            if (start == end) {
                end = start + 1;
            }
            const u64 kind = rng() % 100;
            if (kind < 40) {
                // Half the marks are short, so long checks over them find both outcomes.
                if (rng() % 2 == 0) {
                    end = std::min(start + rng() % (2 * MB) + 1, Space);
                }
                tracker.Add(start, end, epoch);
                marked.emplace_back(start, end);
            } else if (kind < 98) {
                bool overlaps = false;
                for (const auto& [marked_start, marked_end] : marked) {
                    overlaps |= (start >> AccessTracker::GranuleBits) <=
                                    ((marked_end - 1) >> AccessTracker::GranuleBits) &&
                                (marked_start >> AccessTracker::GranuleBits) <=
                                    ((end - 1) >> AccessTracker::GranuleBits);
                }
                ASSERT_EQ(tracker.Overlaps(start, end, epoch), overlaps)
                    << std::hex << start << " " << end;
            } else {
                ++epoch;
                marked.clear();
            }
        }
    }
}

TEST(AccessTracker, RemembersRangesPerEpoch) {
    // A range marked again in a later epoch has to be marked anew.
    AccessTracker tracker;
    for (u64 epoch = 1; epoch < 50; ++epoch) {
        for (u64 i = 0; i < 40; ++i) {
            const u64 start = (i * 7919 % 97) * 0x10000;
            tracker.Add(start, start + 0x100 + i, epoch);
        }
        for (u64 i = 0; i < 40; ++i) {
            const u64 start = (i * 7919 % 97) * 0x10000;
            ASSERT_TRUE(tracker.Overlaps(start, start + 1, epoch));
            ASSERT_FALSE(tracker.Overlaps(start, start + 1, epoch + 1));
        }
    }
}

} // Anonymous namespace
} // namespace VideoCore
