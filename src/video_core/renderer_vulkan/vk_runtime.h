// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class BlitHelper;
} // namespace VideoCore

namespace Vulkan {

class Instance;
class Scheduler;

class Runtime {
public:
    explicit Runtime(const Instance& instance, Scheduler& scheduler);
    ~Runtime() = default;

    const Instance& GetInstance() const {
        return instance;
    }

    StagingBufferPool& GetStagingPool() {
        return staging_pool;
    }

    void TickFrame();

    void CopyBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                    std::span<const vk::BufferCopy> copies);

    void FillBuffer(const VideoCore::Buffer* dst, u64 offset, u64 size, u32 value);

    void InlineData(VideoCore::Buffer* dst, u64 offset, u32 value);

    bool Transit(VideoCore::Image* image, vk::ImageLayout dst_layout,
                 vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                 std::optional<VideoCore::SubresourceRange> subres_range = {});

    void UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                     std::span<const vk::BufferImageCopy> upload_copies);
    void DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                       std::span<const vk::BufferImageCopy> download_copies);

    void CopyImage(VideoCore::Image* src, VideoCore::Image* dst);
    void CopyImageWithBuffer(VideoCore::Image* src, VideoCore::Image* dst,
                             const VideoCore::Buffer* buffer, u64 offset);
    void CopyMip(VideoCore::Image* src, VideoCore::Image* dst, u32 mip, u32 slice);

    void CopyColorAndDepth(VideoCore::Image* src, VideoCore::Image* dst);

    void CopyDepthStencil(VideoCore::Image* src, VideoCore::Image* dst,
                          const VideoCore::SubresourceRange& sub_range);

    void ResolveImage(VideoCore::Image* src, VideoCore::Image* dst,
                      const VideoCore::SubresourceRange& src_range,
                      const VideoCore::SubresourceRange& dst_range);
    void ClearImage(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                    const vk::ClearValue& clear_value);

    void SetBackingSamples(VideoCore::Image* image, u32 num_samples, bool copy_backing = true);

    void AccessBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size,
                      vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access);

    bool IsBufferAccessed(const VideoCore::Buffer* handle, u64 offset, u64 size,
                          bool check_read_access = false);

    void FlushBarriers();

private:
    void MakeCurrent(const VideoCore::Buffer* handle);

private:
    /// Ranges of a buffer accessed since the last barrier. Draws add a few each, thousands of
    /// them between barriers in busy scenes, and keeping them in one sorted list moved the rest
    /// of it on every add: over a tenth of the GPU thread in inFAMOUS Second Son's city. Merging
    /// batches of 32 into it instead still took 7%, as the list grows to thousands. New ranges go
    /// to a small sorted list, which is merged into the large one once it holds about the square
    /// root of its size, and ranges the large one already covers, like the same vertex buffers
    /// read again by the next draw, are skipped.
    class AccessRanges {
    public:
        void Add(u64 start, u64 end) {
            if (start >= end) [[unlikely]] {
                return;
            }
            if (Covers(sorted, start, end)) {
                return;
            }
            Insert(pending, start, end);
            if (pending.size() >= pending_limit) {
                Merge();
            }
        }

        bool Overlaps(u64 start, u64 end) const {
            if (start >= end) [[unlikely]] {
                return false;
            }
            return Overlaps(pending, start, end) || Overlaps(sorted, start, end);
        }

        void Clear() {
            sorted.clear();
            pending.clear();
            pending_limit = MinPending;
        }

    private:
        static constexpr size_t MinPending = 32;

        /// The first range of a disjoint, ordered list that ends at or after start.
        template <typename List>
        static auto FirstEndingFrom(List& list, u64 start) {
            return std::ranges::lower_bound(list, start, {}, &Interval::end);
        }

        static bool Covers(const std::vector<Interval>& list, u64 start, u64 end) {
            const auto it = FirstEndingFrom(list, end);
            return it != list.end() && it->start <= start;
        }

        static bool Overlaps(const std::vector<Interval>& list, u64 start, u64 end) {
            const auto it = std::ranges::upper_bound(list, start, {}, &Interval::end);
            return it != list.end() && it->start < end;
        }

        /// Adds to a disjoint, ordered list, joining the ranges it overlaps or touches.
        static void Insert(std::vector<Interval>& list, u64 start, u64 end) {
            auto first = FirstEndingFrom(list, start);
            auto last = first;
            while (last != list.end() && last->start <= end) {
                start = std::min(start, last->start);
                end = std::max(end, last->end);
                ++last;
            }
            if (first == last) {
                list.insert(first, Interval{start, end});
                return;
            }
            *first = Interval{start, end};
            list.erase(first + 1, last);
        }

        void Merge() {
            merged.clear();
            merged.reserve(sorted.size() + pending.size());
            const auto push = [this](const Interval& range) {
                if (!merged.empty() && range.start <= merged.back().end) {
                    merged.back().end = std::max(merged.back().end, range.end);
                } else {
                    merged.push_back(range);
                }
            };
            auto a = sorted.begin();
            auto b = pending.begin();
            while (a != sorted.end() || b != pending.end()) {
                if (b == pending.end() || (a != sorted.end() && a->start <= b->start)) {
                    push(*a++);
                } else {
                    push(*b++);
                }
            }
            std::swap(sorted, merged);
            pending.clear();
            // Merging costs the size of the large list, inserting the size of the small one, so
            // with the small one at twice the square root of the large one both stay small.
            pending_limit = std::max(
                MinPending, 2 * static_cast<size_t>(std::sqrt(static_cast<double>(sorted.size()))));
        }

        /// Disjoint and in order.
        std::vector<Interval> sorted;
        /// Added since the last merge, also disjoint and in order.
        std::vector<Interval> pending;
        size_t pending_limit = MinPending;
        /// Kept to reuse its memory.
        std::vector<Interval> merged;
    };

    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<VideoCore::BlitHelper> blit_helper;
    StagingBufferPool staging_pool;
    struct BufferBarriers {
        const VideoCore::Buffer* handle;
        AccessRanges read_ranges;
        AccessRanges write_ranges;
    };
    BufferBarriers* resource{};
    /// Entries past num_resources are unused, and kept so their lists keep their memory: they
    /// were freed and allocated again around every barrier, many times a frame.
    std::vector<BufferBarriers> resources;
    size_t num_resources{};
    VideoCore::Image::Barriers image_barriers;
    vk::MemoryBarrier2 memory_barrier{};
};

} // namespace Vulkan
