// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace Vulkan {

class Instance;

/**
 * Copies memory the GPU wrote back for the CPU on a queue of its own, each batch starting once
 * the work that wrote it is done. On the queue the game's work goes to, a copy waited for all
 * work submitted before it too, while the game thread that wanted the memory waited for the copy.
 */
class ReadbackQueue {
public:
    explicit ReadbackQueue(const Instance& instance);
    ~ReadbackQueue();

    ReadbackQueue(const ReadbackQueue&) = delete;
    ReadbackQueue& operator=(const ReadbackQueue&) = delete;

    /// Whether the device has a queue to spare for this.
    [[nodiscard]] bool IsAvailable() const noexcept {
        return static_cast<bool>(queue);
    }

    /// Returns a command buffer to record copies back into, ended by Submit.
    [[nodiscard]] vk::CommandBuffer Begin();

    /// Submits the copies, to start once wait_semaphore reaches wait_tick. Returns the tick of
    /// GetSemaphore() that is signalled once they are done and the host can read them.
    u64 Submit(vk::CommandBuffer cmdbuf, vk::Semaphore wait_semaphore, u64 wait_tick);

    [[nodiscard]] Semaphore& GetSemaphore() noexcept {
        return semaphore;
    }

private:
    vk::Device device;
    vk::Queue queue;
    Semaphore semaphore;
    CommandPool command_pool;
};

} // namespace Vulkan
