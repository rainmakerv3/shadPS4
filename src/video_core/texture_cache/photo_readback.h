// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <vector>
#include "common/logging/log.h"
#include "common/types.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/types.h"

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore {

// Synchronous copy of a render target into host memory, for HLE code that has to hand a picture
// the guest just drew to a host encoder (the photo mode of Gravity Rush 2).
//
// Once armed with a size, the texture cache notes the last image of that size bound as a colour
// target or written as a storage image. Download() then runs the copy on the GPU command thread,
// behind every draw already recorded, and waits for it. Nothing is written to guest memory.
// Disarmed, which is the default, each hook is one load and a branch.
class PhotoReadback {
public:
    struct Frame {
        std::vector<u8> pixels; ///< Rows of pitch bytes, top row first, bytes as in guest memory
        u32 width{};
        u32 height{};
        u32 pitch{}; ///< Bytes per row
    };

    /// Starts tracking images of exactly this size. Call before the guest draws. Also turns on
    /// ClampLayers, whose answer the T# info memo keeps, so this runs once, before any T# is built.
    static void Arm(u32 width, u32 height) noexcept;

    static bool IsArmed(u32 width, u32 height) noexcept {
        return armed_extent.load(std::memory_order_relaxed) == PackExtent(width, height) &&
               width != 0;
    }

    /// GPU command thread: an image was bound as a colour target or written as a storage image.
    static void Track(const Image& image, ImageId image_id) noexcept {
        const u64 extent = armed_extent.load(std::memory_order_relaxed);
        if (extent == 0) [[likely]] {
            return;
        }
        if (PackExtent(image.info.size.width, image.info.size.height) == extent &&
            !image.info.props.is_tiled && image.info.guest_address != 0) {
            tracked_index.store(image_id.index, std::memory_order_relaxed);
        }
    }

    /// Any thread: the image is being deleted.
    static void Forget(ImageId image_id) noexcept {
        u32 index = image_id.index;
        if (tracked_index.load(std::memory_order_relaxed) == index) [[unlikely]] {
            tracked_index.compare_exchange_strong(index, ImageId::INVALID_INDEX);
        }
    }

    /// The album binds a T# of several thousand layers (4609 was seen), which cannot be
    /// allocated. While armed, a T# with more than 2048 layers is built with one.
    static u32 ClampLayers(u32 layers) noexcept {
        if (layers <= 2048 || armed_extent.load(std::memory_order_relaxed) == 0) [[likely]] {
            return layers;
        }
        LOG_WARNING(Render_Vulkan, "T# with {} layers is built with one layer", layers);
        return 1;
    }

    /// Keeps a view of such a T# on the one layer it was built with.
    static void ClampView(u32 layers, SubresourceRange& range) noexcept {
        if (layers <= 2048 || armed_extent.load(std::memory_order_relaxed) == 0) [[likely]] {
            return;
        }
        range.base.layer = 0;
        range.extent.layers = 1;
    }

    /// Guest thread: copies the tracked image out. Blocks until the GPU command thread has run
    /// the copy. False when no image of the armed size is tracked, or the tracked one is not a
    /// plain single-layer colour image the GPU has written.
    static bool Download(Frame& frame);

private:
    static constexpr u64 PackExtent(u32 width, u32 height) noexcept {
        return u64{width} << 32 | height;
    }

    static bool DownloadOnGpuThread(Vulkan::Rasterizer& rasterizer, Frame& frame);

    static inline std::atomic<u64> armed_extent{};
    static inline std::atomic<u32> tracked_index{ImageId::INVALID_INDEX};
};

} // namespace VideoCore
