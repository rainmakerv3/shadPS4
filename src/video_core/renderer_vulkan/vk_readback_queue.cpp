// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_readback_queue.h"

namespace Vulkan {

ReadbackQueue::ReadbackQueue(const Instance& instance)
    : device{instance.GetDevice()}, queue{instance.GetReadbackQueue()}, semaphore{instance},
      command_pool{instance, &semaphore} {}

ReadbackQueue::~ReadbackQueue() {
    // The pool can't go before the GPU is done with its command buffers, though a lost device
    // is not waited for.
    const u64 last_tick = semaphore.CurrentTick() - 1;
    if (last_tick == 0) {
        return;
    }
    const vk::Semaphore handle = semaphore.Handle();
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1U,
        .pSemaphores = &handle,
        .pValues = &last_tick,
    };
    static_cast<void>(device.waitSemaphores(wait_info, 1'000'000'000));
}

vk::CommandBuffer ReadbackQueue::Begin() {
    const vk::CommandBuffer cmdbuf = command_pool.Commit();
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };
    Check(cmdbuf.begin(begin_info));
    return cmdbuf;
}

u64 ReadbackQueue::Submit(vk::CommandBuffer cmdbuf, vk::Semaphore wait_semaphore, u64 wait_tick) {
    const vk::MemoryBarrier2 to_host = {
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1U,
        .pMemoryBarriers = &to_host,
    });
    Check(cmdbuf.end());

    const u64 signal_tick = semaphore.NextTick();
    const vk::Semaphore signal_semaphore = semaphore.Handle();
    const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = 1U,
        .pWaitSemaphoreValues = &wait_tick,
        .signalSemaphoreValueCount = 1U,
        .pSignalSemaphoreValues = &signal_tick,
    };
    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = 1U,
        .pWaitSemaphores = &wait_semaphore,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1U,
        .pCommandBuffers = &cmdbuf,
        .signalSemaphoreCount = 1U,
        .pSignalSemaphores = &signal_semaphore,
    };
    const auto result = queue.submit(submit_info);
    ASSERT_MSG(result != vk::Result::eErrorDeviceLost, "Device lost during readback submit");
    return signal_tick;
}

} // namespace Vulkan
