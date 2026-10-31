// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_command_recorder.h"
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

    /// Copies data the CPU wrote into staging memory to a buffer. Unless something recorded in
    /// the command buffer touched the ranges copied to, the copy is recorded ahead of all of it,
    /// so it neither ends the render pass nor needs a barrier before the next draw.
    void UploadBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                      std::span<const vk::BufferCopy> copies);

    /// Notes a read barriers aren't kept for, which uploads mustn't be recorded ahead of.
    void NoteBufferRead(const VideoCore::Buffer* handle, u64 offset, u64 size);

    /// Notes that the command buffer reads memory in ways not reported, such as through device
    /// addresses, so no upload is recorded ahead of what it recorded so far.
    void NoteUntrackedAccess();

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

    /// Returns true if anything recorded in the command buffer so far touched a buffer range, or
    /// read memory in ways not reported.
    bool IsTouchedInSession(const VideoCore::Buffer* handle, u64 offset, u64 size) const;

    void FlushBarriers();

    /// Sets the scheduler of the second queue the game's compute rings run on, if any. Uploads
    /// nothing the graphics queue still has in flight touches go there too.
    void SetAsyncScheduler(Scheduler* async) {
        async_scheduler = async;
    }

    [[nodiscard]] Scheduler* AsyncScheduler() const noexcept {
        return async_scheduler;
    }

    /// Sets whether what is recorded from here on is for work on the second queue, so the
    /// uploads it needs go there too, after the graphics work submitted they have to wait for.
    void SetRecordingForAsync(bool for_async) noexcept {
        recording_for_async = for_async;
    }

    /// Returns the command buffer of the second queue, after a barrier making it wait for all
    /// recorded there before. Everything on that queue runs in order.
    CommandRecorder AsyncCommandBuffer();

    /// Submits what was recorded for the second queue, returning the last tick of it submitted,
    /// which the graphics queue waits for before it runs anything recorded since.
    u64 FlushAsync();

    /// Returns the last tick of graphics work, possibly not submitted yet, that work on the
    /// second queue touching a buffer range has to wait for: graphics work touching it, or
    /// writing it if only writes count, work reading memory in ways not reported, and memory
    /// bound to buffers. Zero if there is none.
    [[nodiscard]] u64 GraphicsDependency(const VideoCore::Buffer* handle, u64 offset, u64 size,
                                         bool only_written) const;

    /// Makes the next submit of the second queue wait for the graphics tick, which has to be
    /// submitted already.
    void WaitOnGraphics(u64 tick);

    /// Returns the graphics tick the next submit of the second queue waits for.
    [[nodiscard]] u64 AsyncGraphicsWait() const;

    /// Copies buffer ranges on the second queue, if no graphics work not submitted yet writes
    /// what they copy from. Returns false if they have to be copied on the graphics queue.
    bool CopyBufferOnAsync(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                           std::span<const vk::BufferCopy> copies);

    /// Notes memory bound to buffers by the next graphics submit, which work on the second queue
    /// has to wait for.
    void NoteResidencyChange() noexcept {
        residency_pending = true;
    }

    /// Called as the graphics queue submits the tick.
    void OnGraphicsSubmit(u64 tick) noexcept {
        if (std::exchange(residency_pending, false)) {
            residency_tick = tick;
        }
    }

private:
    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<VideoCore::BlitHelper> blit_helper;
    StagingBufferPool staging_pool;
    /// Buffer accesses are kept with the epoch they were made in, and the barrier that makes
    /// them visible starts the next one.
    u64 barrier_epoch{1};
    /// The command buffer in which memory was read in ways not reported, if any, and its tick.
    u64 untracked_session{};
    u64 untracked_tick{};
    /// The scheduler of the second queue, whether anything was recorded for it since it was last
    /// submitted, or ever, and the last tick of it submitted.
    Scheduler* async_scheduler{};
    bool async_pending{};
    bool async_since_barrier{};
    u64 async_submitted_tick{};
    /// The graphics tick the next submit of the second queue waits for.
    u64 async_graphics_wait{};
    bool recording_for_async{};
    /// Whether the next graphics submit binds memory to buffers, and the last one that did.
    bool residency_pending{};
    u64 residency_tick{};
    VideoCore::Image::Barriers image_barriers;
    vk::MemoryBarrier2 memory_barrier{};
};

} // namespace Vulkan
