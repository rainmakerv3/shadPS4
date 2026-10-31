// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <memory>
#include <random>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "tests/stubs/page_manager_stub.h"
#include "video_core/buffer_cache/region_manager.h"

namespace VideoCore {
namespace {

constexpr VAddr Base = 0x10000000;

using Range = std::pair<VAddr, u64>;
using Protection = std::pair<VAddr, PageOp>;

/// Page by page model of what RegionManager tracks, with GPU readbacks off.
class Model {
public:
    Model() {
        cpu.fill(true);
        gpu.fill(false);
    }

    template <StateOp cpu_op, StateOp gpu_op>
    void Change(u64 offset, u64 size, std::vector<Protection>& protections) {
        for (u64 page = offset / BYTES_PER_PAGE; page <= (offset + size - 1) / BYTES_PER_PAGE;
             ++page) {
            Apply<cpu_op, gpu_op>(page, protections);
        }
    }

    template <Type type, StateOp cpu_op, StateOp gpu_op>
    std::vector<Range> ForEach(u64 offset, u64 size, std::vector<Protection>& protections) {
        std::vector<Range> ranges;
        const auto& state = type == Type::CPU ? cpu : gpu;
        for (u64 page = offset / BYTES_PER_PAGE; page <= (offset + size - 1) / BYTES_PER_PAGE;
             ++page) {
            if (state[page]) {
                const VAddr addr = Base + page * BYTES_PER_PAGE;
                if (!ranges.empty() && ranges.back().first + ranges.back().second == addr) {
                    ranges.back().second += BYTES_PER_PAGE;
                } else {
                    ranges.emplace_back(addr, BYTES_PER_PAGE);
                }
            }
            Apply<cpu_op, gpu_op>(page, protections);
        }
        return ranges;
    }

    template <Type type>
    bool IsModified(u64 offset, u64 size) const {
        const auto& state = type == Type::CPU ? cpu : gpu;
        for (u64 page = offset / BYTES_PER_PAGE; page <= (offset + size - 1) / BYTES_PER_PAGE;
             ++page) {
            if (state[page]) {
                return true;
            }
        }
        return false;
    }

private:
    template <StateOp cpu_op, StateOp gpu_op>
    void Apply(u64 page, std::vector<Protection>& protections) {
        if constexpr (cpu_op != StateOp::None) {
            const bool dirty = cpu_op == StateOp::Set;
            if (cpu[page] != dirty) {
                // A dirty page is unprotected, so the CPU can write it freely.
                protections.emplace_back(Base + page * BYTES_PER_PAGE,
                                         dirty ? PageOp::Untrack : PageOp::Track);
            }
            cpu[page] = dirty;
        }
        if constexpr (gpu_op != StateOp::None) {
            gpu[page] = gpu_op == StateOp::Set;
        }
    }

    std::array<bool, NUM_REGION_PAGES> cpu;
    std::array<bool, NUM_REGION_PAGES> gpu;
};

class RegionManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Disabled);
        PageManagerStub::calls.clear();
        manager = std::make_unique<RegionManager>(&tracker, Base);
    }

    void TearDown() override {
        EmulatorSettings.SetReadbacksMode(GpuReadbacksMode::Precise);
    }

    std::vector<Protection> TakeProtections() {
        std::vector<Protection> protections;
        for (const auto& call : PageManagerStub::calls) {
            protections.emplace_back(call.page, call.op);
        }
        PageManagerStub::calls.clear();
        return protections;
    }

    template <Type type, StateOp cpu_op, StateOp gpu_op>
    void CheckForEach(u64 offset, u64 size) {
        std::vector<Range> ranges;
        manager->ForEachModifiedRange<type, cpu_op, gpu_op>(
            offset, size,
            [&](VAddr addr, u64 range_size) { ranges.emplace_back(addr, range_size); });
        std::vector<Protection> expected_protections;
        const auto expected =
            model.ForEach<type, cpu_op, gpu_op>(offset, size, expected_protections);
        ASSERT_EQ(ranges, expected) << "offset " << offset << " size " << size;
        ASSERT_EQ(TakeProtections(), expected_protections)
            << "offset " << offset << " size " << size;
    }

    template <StateOp cpu_op, StateOp gpu_op>
    void CheckChange(u64 offset, u64 size) {
        manager->ChangeRegionState<cpu_op, gpu_op>(offset, size);
        std::vector<Protection> expected_protections;
        model.Change<cpu_op, gpu_op>(offset, size, expected_protections);
        ASSERT_EQ(TakeProtections(), expected_protections)
            << "offset " << offset << " size " << size;
    }

    PageManager tracker{nullptr};
    std::unique_ptr<RegionManager> manager;
    Model model;
};

TEST_F(RegionManagerTest, CleanRangeHasNothingToUpload) {
    CheckForEach<Type::CPU, StateOp::Clear, StateOp::None>(0, HIGHER_PAGE_SIZE);
    CheckForEach<Type::CPU, StateOp::Clear, StateOp::None>(0, HIGHER_PAGE_SIZE);
    EXPECT_FALSE(manager->IsRegionModified<Type::CPU>(0, HIGHER_PAGE_SIZE));
    CheckChange<StateOp::Set, StateOp::None>(5 * BYTES_PER_PAGE + 12, 8);
    EXPECT_TRUE(manager->IsRegionModified<Type::CPU>(0, HIGHER_PAGE_SIZE));
    EXPECT_FALSE(manager->IsRegionModified<Type::CPU>(6 * BYTES_PER_PAGE, BYTES_PER_PAGE));
    CheckForEach<Type::CPU, StateOp::Clear, StateOp::None>(0, HIGHER_PAGE_SIZE);
}

TEST_F(RegionManagerTest, MatchesPageByPageModel) {
    std::mt19937_64 rng{1234};
    const auto random_range = [&]() -> std::pair<u64, u64> {
        // Mostly small ranges, as the game writes them, with some spanning many words.
        const u64 offset = rng() % HIGHER_PAGE_SIZE;
        const u64 max_size = HIGHER_PAGE_SIZE - offset;
        const u64 size = rng() % 4 == 0 ? 1 + rng() % max_size
                                        : 1 + rng() % std::min<u64>(max_size, 64 * BYTES_PER_PAGE);
        return {offset, size};
    };
    for (int i = 0; i < 50000; ++i) {
        const auto [offset, size] = random_range();
        switch (rng() % 7) {
        case 0:
            CheckChange<StateOp::Set, StateOp::None>(offset, size);
            break;
        case 1:
            CheckChange<StateOp::None, StateOp::Clear>(offset, size);
            break;
        case 2:
            CheckForEach<Type::CPU, StateOp::Clear, StateOp::None>(offset, size);
            break;
        case 3:
            CheckForEach<Type::CPU, StateOp::Clear, StateOp::Set>(offset, size);
            break;
        case 4:
            CheckForEach<Type::GPU, StateOp::None, StateOp::Clear>(offset, size);
            break;
        case 5:
            CheckForEach<Type::GPU, StateOp::None, StateOp::None>(offset, size);
            break;
        default:
            ASSERT_EQ(manager->IsRegionModified<Type::CPU>(offset, size),
                      model.IsModified<Type::CPU>(offset, size));
            ASSERT_EQ(manager->IsRegionModified<Type::GPU>(offset, size),
                      model.IsModified<Type::GPU>(offset, size));
            break;
        }
        if (HasFatalFailure()) {
            return;
        }
    }
}

} // namespace
} // namespace VideoCore
