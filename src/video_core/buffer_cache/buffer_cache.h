// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>
#include <boost/container/small_vector.hpp>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/fault_manager.h"
#include "video_core/buffer_cache/range_set.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class GraphicsPipeline;
struct SubmitInfo;
class Runtime;
class StagingBufferPool;
} // namespace Vulkan

namespace VideoCore {

class TextureCache;
class MemoryTracker;
class PageManager;

class BufferCache {
    static constexpr u64 ADDRESS_SPACE_BITS = 40;
    static constexpr u64 ARENA_PAGE_BITS = 32;
    static constexpr u64 ARENA_PAGE_SIZE = u64{1} << ARENA_PAGE_BITS;
    static constexpr u64 NUM_ARENA_PAGES = u64{1} << (ADDRESS_SPACE_BITS - ARENA_PAGE_BITS);
    static constexpr u64 MIN_BLOCK_SIZE = 16_KB;
    static constexpr u64 STREAM_THRESHOLD = 16_KB;

public:
    explicit BufferCache(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                         Vulkan::Runtime& runtime, AmdGpu::Liverpool* liverpool,
                         TextureCache& texture_cache, PageManager& tracker);
    ~BufferCache();

    /// Returns a pointer to GDS device local buffer.
    [[nodiscard]] const Buffer* GetGdsBuffer() const noexcept {
        return &gds_buffer;
    }

    /// Retrieves the device local DBA page table buffer.
    [[nodiscard]] Buffer* GetBdaPageTableBuffer() noexcept {
        return bda_pagetable_buffer.get();
    }

    /// Retrieves the fault buffer.
    [[nodiscard]] Buffer* GetFaultBuffer() noexcept {
        return fault_manager->GetFaultBuffer();
    }

    /// Retrieves the stream buffer.
    [[nodiscard]] StreamBuffer& GetStreamBuffer() noexcept {
        return stream_buffer;
    }

    /// Returns minimum granularity of a sparse memory bind.
    u32 GetSparsePageShift() const noexcept {
        return block_shift;
    }

    void TickFrame();

    /// Copies back GPU modified memory that game threads read back recently, before they read
    /// it again. Called when the game is signalled that GPU work is done.
    void PrefetchReadbacks();

    /// Invalidates any buffer in the logical page range.
    void InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks = false);

    /// Flushes any GPU modified buffer in the logical page range back to CPU memory.
    void ReadMemory(VAddr device_addr, u64 size, bool is_write = false, bool assume_locks = false);

    /// Notes memory written straight to its backing, past the page protection, so the GPU copy
    /// of it is uploaded again before it is used.
    void OnBackingWritten(VAddr device_addr, u64 size);

    /// Finds a buffer for the specified region. is_read_tracked tells that the caller reports
    /// its accesses to the runtime, which lets small reads use the cached copy in place.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBuffer(VAddr device_addr, u32 size,
                                                             bool is_written,
                                                             bool is_texel_buffer = false,
                                                             bool is_read_tracked = false);

    /// Attempts to obtain a buffer without modifying the cache contents.
    [[nodiscard]] std::pair<const Buffer*, u64> ObtainBufferForImage(VAddr device_addr, u32 size);

    /// Return true when a region is modified from the CPU
    [[nodiscard]] bool IsRegionCpuModified(VAddr addr, size_t size);

    /// Return true when a region is modified from the GPU
    [[nodiscard]] bool IsRegionGpuModified(VAddr addr, size_t size);

    /// Synchronizes all buffers needed for DMA.
    void SynchronizeDmaBuffers();

    /// Commits pending sparse buffer memory binds. Must be called before every scheduler submit.
    void SubmitPendingArenaBinds(Vulkan::SubmitInfo& info);

private:
    using DownloadCopies = boost::container::small_vector<vk::BufferCopy, 8>;

    /// A copy of GPU modified memory back to the game, recorded and submitted by the GPU thread
    /// and waited for by the game thread that touched the memory.
    struct Readback {
        Vulkan::StagingBufferRef staging;
        /// Source offsets are into the arena, destination offsets into the staging buffer.
        DownloadCopies copies;
        VAddr arena_base{};
        VAddr start{};
        VAddr end{};
        u64 tick{};
        /// Set by the GPU thread when it writes the memory again, so the copy is outdated.
        std::atomic<bool> stale{};
        std::atomic<bool> applied{};
        /// The copy won't be applied, and its ranges are GPU modified again.
        std::atomic<bool> recovered{};
        /// Made ahead of a game thread touching the memory.
        bool prefetched{};
        std::mutex mutex;

        bool Done() const noexcept {
            return applied.load(std::memory_order_acquire) ||
                   recovered.load(std::memory_order_acquire);
        }
    };

    struct ArenaBinds {
        const Buffer* arena;
        boost::container::small_vector<vk::SparseMemoryBind, 32> binds;
    };

    ArenaBinds* BindsForArena(const Buffer* arena) {
        auto it = std::ranges::find(pending_binds, arena, &ArenaBinds::arena);
        if (it != pending_binds.end()) {
            return std::addressof(*it);
        }
        return &pending_binds.emplace_back(arena);
    }

    const Buffer* GetArena(u64 first_block, u64 last_block);

    void EnsureResident(const Buffer* arena, u64 first_block, u64 last_block);

    /// Returns device memory and an offset into it to back size bytes of arena blocks.
    std::pair<vk::DeviceMemory, u64> AllocateResidency(u64 size);

    /// Returns the arena and the window around a range that is read back with it.
    std::tuple<const Buffer*, VAddr, VAddr> GetReadbackWindow(VAddr device_addr, u64 size);

    /// Records and submits a copy back of the GPU modified memory around a range. GPU thread.
    std::shared_ptr<Readback> StartReadback(VAddr device_addr, u64 size);

    /// Waits for a copy back and writes it to the game's memory. Returns false if it can't be
    /// used as the GPU wrote the memory again. Any thread.
    bool FinishReadback(Readback& readback);

    /// Makes the memory of a copy back that won't be used GPU modified again. GPU thread.
    void RecoverReadback(Readback& readback);

    /// Finishes or recovers the copies back overlapping a range. GPU thread.
    void SettleReadbacks(VAddr start, VAddr end);

    /// Frees the staging memory of copies back that are done. GPU thread.
    void PruneReadbacks();

    /// Writes back the copies the GPU has finished, without waiting. GPU thread.
    void ApplyFinishedReadbacks();

    /// Records a copy back of the GPU modified memory in a window, or returns null if there is
    /// none. GPU thread.
    std::shared_ptr<Readback> RecordReadback(const Buffer* arena, VAddr start, VAddr end);

    /// Takes the GPU modified ranges in a range out of the tracked ones, adding copies of them.
    u64 CollectDownloads(const Buffer* arena, VAddr device_addr, u64 size, DownloadCopies& copies);

    void DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size);

    bool SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size, bool is_written,
                           bool is_texel_buffer);

    bool SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    Vulkan::StagingBufferPool& staging_pool;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    TextureCache& texture_cache;
    std::unique_ptr<MemoryTracker> memory_tracker;

    StreamBuffer stream_buffer;
    Buffer gds_buffer;
    RangeSet gpu_modified_ranges;
    std::vector<std::shared_ptr<Readback>> readbacks;

    /// Windows game threads read back recently, which are copied back ahead.
    struct HotWindow {
        VAddr start;
        VAddr end;
        std::chrono::steady_clock::time_point last_fault;
    };
    std::vector<HotWindow> hot_windows;
    struct ReadbackStats {
        u64 on_fault{};
        u64 joined{};
        u64 prefetched{};
        u64 written_ahead{};
    } readback_stats;
    std::chrono::steady_clock::time_point last_readback_report{};

    std::unique_ptr<FaultManager> fault_manager;
    std::unique_ptr<Buffer> bda_pagetable_buffer;
    bool fault_process_pending{};

    std::array<const Buffer*, NUM_ARENA_PAGES> address_space{};
    std::deque<Buffer> arenas;
    std::vector<ArenaBinds> pending_binds;
    Vulkan::Semaphore memory_semaphore;

    struct Backing : public Interval {
        vk::DeviceMemory memory;
        u64 offset;
        constexpr bool CanMergeWith(const Backing& other) const noexcept {
            return memory == other.memory && offset + (end - start) == other.offset;
        }
        constexpr Backing SubRange(u64 a, u64 b) const noexcept {
            return {{a, b}, memory, offset + (a - start)};
        }
    };
    IntervalList<Backing> resident_ranges;
    vk::DeviceMemory residency_chunk{};
    u64 residency_chunk_used{};

    u32 arena_memory_type_index{};
    u32 block_size{};
    u32 block_shift{};
    u32 blocks_per_arena_page{};
    u32 blocks_per_arena_page_shift{};
};

} // namespace VideoCore
