// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
struct Buffer;
struct Image;
class TextureCache;
} // namespace VideoCore
namespace Vulkan {
class Instance;
class Runtime;
class Scheduler;
class BbVelocityMirror;

// Temporal DLSS Super Resolution for Bloodborne, active when shadps4_dlss.dll is present. Scene
// draws are jittered, the pre-HUD scene is upscaled with camera/object motion at the first
// Scaleform draw, and the game's display copy is re-done at output resolution with the HUD delta
// and the game's gamma LUT. Any missing input falls back to the stock presentation path for that
// frame.
bool BbTemporalDlssRequested();
// user/dlss.ini, shared by the renderer and the F1 panel.
std::filesystem::path BbDlssSettingsPath();

// Thread-safe status for the in-game panel.
struct BbDlssStatus {
    bool active{};
    std::string reason; // why DLSS is not active, in plain words
    std::string gpu;
    u32 render_width{}, render_height{}, output_width{}, output_height{};
};
BbDlssStatus BbTemporalDlssStatus();
void BbTemporalDlssReportGpu(std::string_view gpu);
void BbTemporalDlssReportProblem(std::string_view problem);

class BbTemporalDlss {
public:
    BbTemporalDlss();
    ~BbTemporalDlss();
    bool Requested() const;

    struct DrawInfo {
        u64 vs_hash;
        u64 ps_hash;
        VideoCore::ImageId color;
        VideoCore::ImageId depth;
        u32 num_indices;
        u32 num_instances;
        bool indirect;
    };
    // Called after resources are bound and before the guest render pass begins. Returns the
    // viewport offset (render pixels) for this draw.
    std::array<float, 2> OnDraw(const Instance& instance, Runtime& runtime, Scheduler& scheduler,
                                VideoCore::TextureCache& cache, BbVelocityMirror& mirror,
                                const DrawInfo& draw);
    void ObserveTexture(const VideoCore::Image& image, VideoCore::ImageId id,
                        const VideoCore::ImageViewInfo& view, u64 shader_hash, u32 slot);
    // CPU-visible bytes of any read-only constant binding; finds the game's scene constants.
    void ObserveSceneConstants(const void* data, u64 size);
    void ObserveConstants(Runtime& runtime, const Instance& instance,
                          const VideoCore::Buffer* source, u64 offset, u64 size, u64 shader_hash,
                          u32 binding);

    struct Presentation {
        vk::ImageView view;
        vk::Extent2D extent;
    };
    // Output for the VideoOut buffer at this address, if the latest display copy into it was
    // produced by this pass. The view yields sRGB-encoded values in the presenter's order.
    std::optional<Presentation> TakePresentation(VAddr address, vk::Format frame_view_format);
    // The presenter's frame size, which DLSS upscales to unless an output size is configured.
    void SetDisplaySize(u32 width, u32 height);
    // GPU command thread only.
    void Shutdown(const Instance& instance, Scheduler& scheduler);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace Vulkan
