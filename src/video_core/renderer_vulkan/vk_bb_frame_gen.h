// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

// DLSS Frame Generation through NVIDIA Streamline (experiment). Streamline sits between the
// emulator and Vulkan: it adds what it needs to the instance and device and takes over the
// swapchain, presenting a generated frame between each pair of frames the emulator presents.
namespace Vulkan::FrameGen {

// What DLSS-G needs from an upscaled game frame. The images are copies owned by the upscaler
// that stay unchanged until the frame is presented.
struct FrameInputs {
    vk::Image depth, motion;
    vk::ImageView depth_view, motion_view;
    vk::Extent2D render{};
    // Row-major, row-vector matrices as Streamline expects.
    std::array<float, 16> view_to_clip{}, clip_to_view{}, clip_to_prev_clip{}, prev_clip_to_clip{};
    std::array<float, 3> position{}, up{}, right{}, forward{};
    float near_plane{}, far_plane{}, fov_y{}, aspect{};
    std::array<float, 2> jitter{};
    bool depth_inverted{};
    bool reset{};
};

// The window image without the emulator's overlays (F1 panel, fps counter, notifications) and
// a mask of where those overlays are, so generated frames keep them still instead of moving
// them with the scene. Both are the size of the window.
struct OverlayInputs {
    vk::Image hudless;
    vk::ImageView hudless_view;
    vk::Format hudless_format;
    vk::Image alpha;
    vk::ImageView alpha_view;
    vk::Extent2D extent;
};

// Whether dlss.ini asks for frame generation (read once, at launch).
bool Requested();

// Loads Streamline before the Vulkan instance is created. Returns its vkGetInstanceProcAddr,
// which hands out Streamline's versions of the functions it intercepts and the driver's for the
// rest, or null when frame generation is off or unavailable.
PFN_vkGetInstanceProcAddr Load();

// Streamline was loaded and initialized.
bool Active();

// Why frame generation is unavailable although dlss.ini asks for it (empty otherwise).
std::string Problem();

void Shutdown();

// Presenter thread, once per presented frame, in this order.
void BeginFrame();
// `inputs` is null for frames without them (menus, loading screens, repeated frames).
void SetFrame(const FrameInputs* inputs, const OverlayInputs* overlays, vk::CommandBuffer command,
              vk::Extent2D backbuffer, vk::Rect2D game_area);
void EndSubmit();
void BeginPresent();
void EndPresent();

// Frame generation stays off for the next frames: NVIDIA asks for it to be off while the
// window or swapchain changes.
void Pause();

// The dlss.ini switch, followed live once Streamline is loaded.
void SetEnabled(bool enabled);

struct Stats {
    bool generating;
    float base_fps;   // frames the emulator presents
    float output_fps; // frames reaching the display, generated ones included
};
// Over the last second, for the F1 panel and the corner counter.
Stats GetStats();

// The corner fps counter (dlss.ini fps_counter).
void SetCounterVisible(bool visible);
bool CounterVisible();

} // namespace Vulkan::FrameGen
