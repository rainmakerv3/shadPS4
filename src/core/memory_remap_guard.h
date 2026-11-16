// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <thread>
#include "common/types.h"

namespace Core {

/**
 * Tracks the windows in which a mapped region is temporarily unmapped in order to be split.
 *
 * A mapped view cannot be split in place on Windows, so AddressSpace::Impl::SplitRegion unmaps
 * the whole region and maps the pieces back afterwards. While that happens every page of the
 * region is inaccessible, so guest threads that touch it take an access violation for memory
 * that is about to come back. Those faults are transient and must not be reported as unhandled
 * guest exceptions, which is what the signal handler uses this guard for.
 *
 * Rebuilds are serialised by the address space lock, so at most one range is rebuilt at a time.
 */
class MemoryRemapGuard {
public:
    /// Marks the given range as being rebuilt for as long as this object is alive.
    class Scope {
    public:
        Scope(u64 base, u64 size) noexcept : was_owner_{t_owner} {
            t_owner = true;
            Base.store(base, std::memory_order_relaxed);
            End.store(base + size, std::memory_order_relaxed);
            Depth.fetch_add(1, std::memory_order_release);
        }

        ~Scope() {
            Depth.fetch_sub(1, std::memory_order_release);
            t_owner = was_owner_;
        }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        bool was_owner_;
    };

    /// Whether some thread is currently rebuilding a region.
    [[nodiscard]] static bool IsActive() noexcept {
        return Depth.load(std::memory_order_acquire) != 0;
    }

    /// Whether the given address belongs to the range currently being rebuilt.
    [[nodiscard]] static bool Contains(u64 addr) noexcept {
        return addr >= Base.load(std::memory_order_relaxed) &&
               addr < End.load(std::memory_order_relaxed);
    }

    /// Whether the current thread is the one performing the rebuild. Such a thread cannot wait
    /// for itself, so a fault it takes during the rebuild is not retried.
    [[nodiscard]] static bool IsOwnedByCurrentThread() noexcept {
        return t_owner;
    }

    /// Waits for the rebuild to finish. The pieces of a region are remapped immediately after it
    /// is unmapped, so this normally returns before the first sleep. Returns false if the rebuild
    /// did not finish within the timeout, so a lost wakeup cannot wedge the faulting thread.
    static bool WaitForRebuild() noexcept {
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + milliseconds(250);
        for (int attempt = 0; IsActive(); ++attempt) {
            if (steady_clock::now() >= deadline) {
                return false;
            }
            if (attempt < 64) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(microseconds(100));
            }
        }
        return true;
    }

private:
    inline static std::atomic<u64> Base{0};
    inline static std::atomic<u64> End{0};
    inline static std::atomic<int> Depth{0};
    inline static thread_local bool t_owner{false};
};

} // namespace Core
