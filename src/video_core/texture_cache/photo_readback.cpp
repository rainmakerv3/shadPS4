// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <mutex>
#include <span>
#include "common/logging/log.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/photo_readback.h"
#include "video_core/texture_cache/texture_cache.h"

extern std::unique_ptr<Vulkan::Presenter> presenter;
extern std::unique_ptr<AmdGpu::Liverpool> liverpool;

namespace VideoCore {

void PhotoReadback::Arm(u32 width, u32 height) noexcept {
    armed_extent.store(PackExtent(width, height), std::memory_order_relaxed);
}

bool PhotoReadback::Download(Frame& frame) {
    // Both are null when libSceGnmDriver is LLE. SendCommand has no shortcut for the GPU command
    // thread and would wait on itself there.
    if (!presenter || !liverpool || liverpool->OnGpuThread()) {
        return false;
    }
    bool downloaded = false;
    liverpool->SendCommand<true>(
        [&] { downloaded = DownloadOnGpuThread(presenter->GetRasterizer(), frame); });
    return downloaded;
}

bool PhotoReadback::DownloadOnGpuThread(Vulkan::Rasterizer& rasterizer, Frame& frame) {
    TextureCache& texture_cache = rasterizer.GetTextureCache();
    Vulkan::Runtime& runtime = rasterizer.GetRuntime();
    Vulkan::StagingBufferRef download;
    u64 download_size;
    // Only the draw command buffer drops the guest-copy hold before it runs queued commands.
    rasterizer.DropCopyHoldForCommands();
    {
        // Guest threads free images under this mutex. Once the copy is recorded the image
        // outlives it: a slot is erased on this thread, after the tick that used it.
        std::scoped_lock lk{texture_cache.mutex};
        const ImageId image_id{tracked_index.load(std::memory_order_relaxed)};
        if (!image_id || !texture_cache.slot_images.IsAllocated(image_id)) {
            LOG_WARNING(Render_Vulkan, "No photo target has been bound");
            return false;
        }
        Image& image = texture_cache.slot_images[image_id];
        const ImageInfo& info = image.info;
        // Once per photo: tells from a log which image was taken for the photo target.
        LOG_INFO(Render_Vulkan,
                 "Photo target {:#x}: {}x{}, pitch {}, {} bits, {} layers, {} samples",
                 info.guest_address, info.size.width, info.size.height, info.pitch, info.num_bits,
                 info.resources.layers, info.num_samples);
        // The copy below takes one colour layer, and copies into a buffer need a single-sample
        // image.
        if (False(image.flags & ImageFlagBits::Registered) || !image.backing ||
            False(image.flags & ImageFlagBits::GpuModified) ||
            !IsArmed(info.size.width, info.size.height) || info.num_samples > 1 ||
            info.props.is_depth || info.props.is_block || info.size.depth != 1 ||
            info.resources.layers != 1) {
            LOG_WARNING(Render_Vulkan, "Photo target is not readable");
            return false;
        }
        const u32 pitch_bytes = info.pitch * (info.num_bits / 8);
        download_size = u64{pitch_bytes} * info.size.height;
        constexpr u64 MaxDownloadSize = 64_MB;
        if (download_size == 0 || download_size > MaxDownloadSize) {
            return false;
        }
        // Same copy as the synchronous arm of TextureCache::DownloadImageMemory, one layer only.
        download = runtime.GetStagingPool().Request(download_size, MemoryType::HostCached, 16);
        const vk::BufferImageCopy image_download = {
            .bufferOffset = download.offset,
            .bufferRowLength = info.pitch,
            .bufferImageHeight = info.size.height,
            .imageSubresource =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .imageOffset = {0, 0, 0},
            .imageExtent = {info.size.width, info.size.height, 1},
        };
        runtime.DownloadImage(&image, download.buffer, std::span{&image_download, 1});
        frame.width = info.size.width;
        frame.height = info.size.height;
        frame.pitch = pitch_bytes;
    }
    // The wait for the GPU is outside the lock, so guest threads freeing images do not wait too.
    rasterizer.Finish();
    download.Invalidate();
    frame.pixels.assign(download.mapped, download.mapped + download_size);
    return true;
}

} // namespace VideoCore
