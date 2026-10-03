// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <utility>

#include "common/types.h"

namespace VideoCore {

constexpr u64 PAGES_PER_WORD = 64;
constexpr u64 BYTES_PER_PAGE = 4_KB;
constexpr u64 BYTES_PER_WORD = PAGES_PER_WORD * BYTES_PER_PAGE;

constexpr u64 HIGHER_PAGE_BITS = 24;
constexpr u64 HIGHER_PAGE_SIZE = 1ULL << HIGHER_PAGE_BITS;
constexpr u64 HIGHER_PAGE_MASK = HIGHER_PAGE_SIZE - 1ULL;

constexpr u64 NUM_REGION_PAGES = HIGHER_PAGE_SIZE / BYTES_PER_PAGE;
constexpr u64 NUM_REGION_WORDS = HIGHER_PAGE_SIZE / BYTES_PER_WORD;

enum class Type : u8 {
    CPU = 1 << 0,
    GPU = 1 << 1,
};

enum class StateOp : u8 {
    None = 0,
    Set = 1,
    Clear = 2,
};

constexpr bool operator&(Type a, Type b) noexcept {
    return std::to_underlying(a) & std::to_underlying(b);
}

constexpr Type operator|(Type a, Type b) noexcept {
    return static_cast<Type>(std::to_underlying(a) | std::to_underlying(b));
}

/// Advances once per guest frame. Used to spot pages the game writes every frame.
inline std::atomic<u32> g_frame_epoch{};

/// Advances whenever the guest may have written memory that later GPU commands read: at the end
/// of every submission and after the GPU waited on the game. Pages that are not write protected
/// are uploaded again at most once per epoch.
inline std::atomic<u32> g_sync_epoch{};

inline void AdvanceSyncEpoch() noexcept {
    g_sync_epoch.fetch_add(1, std::memory_order_relaxed);
}

inline void AdvanceFrameEpoch() noexcept {
    g_frame_epoch.fetch_add(1, std::memory_order_relaxed);
    AdvanceSyncEpoch();
}

struct Bounds {
    u64 start_word;
    u64 start_page;
    u64 end_word;
    u64 end_page;
};

struct RegionBits {
    constexpr void Fill(u64 value) {
        data.fill(value);
    }

    constexpr bool GetPage(u64 page) const {
        return data[page / PAGES_PER_WORD] & (1ULL << (page % PAGES_PER_WORD));
    }

    constexpr u64& operator[](u64 index) {
        return data[index];
    }

private:
    alignas(64) std::array<u64, NUM_REGION_WORDS> data;
};

} // namespace VideoCore
