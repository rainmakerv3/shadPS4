// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

// Debug readout of how bright the game image is on an HDR swapchain (SHADPS4_HDR_METER=1 with
// RenoDX): the game image is drawn again the way it reaches the screen and its luminance is
// measured, ignoring the emulator's overlays.
namespace Vulkan::HdrMeter {

bool Enabled();

// Presenter thread, after ImGui has drawn the frame: measures it into an scRGB image.
void Record(const Instance& instance, vk::CommandBuffer command, vk::Extent2D extent,
            vk::Format format);

// Over the last second, in nits.
struct Reading {
    bool valid;
    float peak, p999, p99, median;
};
Reading Get();

// Before the device goes away.
void Shutdown();

} // namespace Vulkan::HdrMeter
