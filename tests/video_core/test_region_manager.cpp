// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "tests/stubs/page_manager_stub.h"
#include "video_core/buffer_cache/region_manager.h"

namespace VideoCore {
namespace {

constexpr VAddr Base = 0x10000000;

class RegionManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        PageManagerStub::calls.clear();
        manager = std::make_unique<RegionManager>(&tracker, Base);
    }

    /// The game writes into a page, as a fault on a protected page reports it.
    void Write(u64 page) {
        manager->ChangeRegionState<StateOp::Set, StateOp::None>(page * BYTES_PER_PAGE, 8);
    }

    /// The GPU reads the given pages, returning the pages that were uploaded.
    std::vector<u64> Read(u64 page, u64 count = 1) {
        std::vector<u64> uploaded;
        manager->ForEachModifiedRange<Type::CPU, StateOp::Clear, StateOp::None>(
            page * BYTES_PER_PAGE, count * BYTES_PER_PAGE, [&](VAddr addr, u64 size) {
                for (u64 offset = 0; offset < size; offset += BYTES_PER_PAGE) {
                    uploaded.push_back((addr + offset - Base) / BYTES_PER_PAGE);
                }
            });
        return uploaded;
    }

    /// The GPU writes the given page.
    void GpuWrite(u64 page) {
        manager->ForEachModifiedRange<Type::CPU, StateOp::Clear, StateOp::Set>(
            page * BYTES_PER_PAGE, BYTES_PER_PAGE, [](VAddr, u64) {});
    }

    /// Returns how the protection of a page changed since the last call.
    std::vector<PageOp> TakeOps(u64 page) {
        std::vector<PageOp> ops;
        for (const auto& call : PageManagerStub::calls) {
            if (call.page == Base + page * BYTES_PER_PAGE) {
                ops.push_back(call.op);
            }
        }
        PageManagerStub::calls.clear();
        return ops;
    }

    static void NextFrame() {
        AdvanceFrameEpoch();
    }

    PageManager tracker{nullptr};
    std::unique_ptr<RegionManager> manager;
};

TEST_F(RegionManagerTest, UploadProtectsPage) {
    EXPECT_EQ(Read(5), std::vector<u64>{5});
    EXPECT_EQ(TakeOps(5), std::vector<PageOp>{PageOp::Track});
    // Clean pages are not uploaded again.
    EXPECT_TRUE(Read(5).empty());
}

TEST_F(RegionManagerTest, PageWrittenEveryFrameStopsBeingProtected) {
    Read(7);
    TakeOps(7);
    for (int frame = 0; frame < 2; ++frame) {
        NextFrame();
        Write(7);
        EXPECT_EQ(TakeOps(7), std::vector<PageOp>{PageOp::Untrack});
        EXPECT_EQ(Read(7), std::vector<u64>{7});
        EXPECT_EQ(TakeOps(7), std::vector<PageOp>{PageOp::Track});
    }

    // The third write in a row makes the page volatile: it is uploaded but stays unprotected.
    NextFrame();
    Write(7);
    EXPECT_EQ(TakeOps(7), std::vector<PageOp>{PageOp::Untrack});
    EXPECT_EQ(Read(7), std::vector<u64>{7});
    EXPECT_TRUE(TakeOps(7).empty());
    EXPECT_TRUE(manager->IsRegionModified<Type::CPU>(7 * BYTES_PER_PAGE, BYTES_PER_PAGE));

    // Within one sync epoch it is uploaded once, after the next one it is uploaded again even
    // though no write was seen, since writes to it are no longer caught.
    EXPECT_TRUE(Read(7).empty());
    AdvanceSyncEpoch();
    EXPECT_EQ(Read(7), std::vector<u64>{7});
    NextFrame();
    EXPECT_EQ(Read(7), std::vector<u64>{7});
    EXPECT_TRUE(TakeOps(7).empty());
}

TEST_F(RegionManagerTest, NeighboursOfVolatilePageStayTracked) {
    Read(10, 3);
    TakeOps(11);
    for (int frame = 0; frame < 3; ++frame) {
        NextFrame();
        Write(11);
        Read(10, 3);
    }
    TakeOps(11);
    AdvanceSyncEpoch();
    // Only the page the game keeps writing comes back.
    EXPECT_EQ(Read(10, 3), std::vector<u64>{11});
}

TEST_F(RegionManagerTest, OccasionalWritesKeepProtection) {
    Read(3);
    TakeOps(3);
    for (int write = 0; write < 5; ++write) {
        for (int frame = 0; frame < 4; ++frame) {
            NextFrame();
        }
        Write(3);
        EXPECT_EQ(Read(3), std::vector<u64>{3});
        EXPECT_EQ(TakeOps(3), (std::vector<PageOp>{PageOp::Untrack, PageOp::Track}));
    }
}

TEST_F(RegionManagerTest, GpuWriteTracksVolatilePageAgain) {
    Read(9);
    for (int frame = 0; frame < 3; ++frame) {
        NextFrame();
        Write(9);
        Read(9);
    }
    TakeOps(9);
    GpuWrite(9);
    EXPECT_EQ(TakeOps(9), std::vector<PageOp>{PageOp::Track});
    EXPECT_FALSE(manager->IsRegionModified<Type::CPU>(9 * BYTES_PER_PAGE, BYTES_PER_PAGE));
    EXPECT_TRUE(manager->IsRegionModified<Type::GPU>(9 * BYTES_PER_PAGE, BYTES_PER_PAGE));
}

TEST_F(RegionManagerTest, VolatilePagesAreRecheckedLater) {
    Read(2);
    for (int frame = 0; frame < 3; ++frame) {
        NextFrame();
        Write(2);
        Read(2);
    }
    TakeOps(2);
    for (int frame = 0; frame < 600; ++frame) {
        NextFrame();
    }
    // Protected again, in case the game stopped writing the page.
    EXPECT_EQ(Read(2), std::vector<u64>{2});
    EXPECT_EQ(TakeOps(2), std::vector<PageOp>{PageOp::Track});
    // Still written every frame, it turns volatile again right away.
    NextFrame();
    Write(2);
    Read(2);
    EXPECT_EQ(TakeOps(2), std::vector<PageOp>{PageOp::Untrack});
}

} // namespace
} // namespace VideoCore
