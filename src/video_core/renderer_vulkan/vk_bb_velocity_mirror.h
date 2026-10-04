// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <optional>
#include "video_core/renderer_vulkan/vk_common.h"
namespace VideoCore {
struct Image;
}
namespace Vulkan {
class Instance;
class Scheduler;
class Runtime;
class GraphicsPipeline;
struct RenderState;

// Replays the game's object-velocity draws at render resolution for DLSS motion vectors.
class BbVelocityMirror {
public:
    BbVelocityMirror();
    ~BbVelocityMirror();
    bool Requested() const;
    bool BeginDraw(const Instance& instance, Runtime& runtime, Scheduler& scheduler,
                   const GraphicsPipeline& pipeline, const RenderState& guest_state,
                   VideoCore::Image* guest_depth, u32 depth_layer);
    void EndDraw(Scheduler& scheduler);
    struct Frame {
        vk::ImageView view;
        vk::Extent2D extent;
    };
    // Temporal DLSS: hands over this frame's mirror (shader-read layout) and starts a new one.
    std::optional<Frame> ConsumeFrame(vk::CommandBuffer command);
    // Render resolution the mirror replays the game's 160x90 velocity draws at.
    void SetTargetSize(vk::Extent2D size);
    void Shutdown(Scheduler& scheduler);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Vulkan
