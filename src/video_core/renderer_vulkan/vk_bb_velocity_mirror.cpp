// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later
#include <bit>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <utility>
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_bb_temporal_dlss.h"
#include "video_core/renderer_vulkan/vk_bb_velocity_mirror.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {
namespace {
constexpr vk::ImageSubresourceRange Range{
    .aspectMask = vk::ImageAspectFlagBits::eColor, .levelCount = 1, .layerCount = 1};
constexpr vk::ImageSubresourceRange DepthRange{.aspectMask = vk::ImageAspectFlagBits::eDepth |
                                                             vk::ImageAspectFlagBits::eStencil,
                                               .levelCount = 1,
                                               .layerCount = 1};
// The game renders object velocity into a coarse 160x90 target.
constexpr u32 SourceWidth = 160, SourceHeight = 90;
} // namespace
struct BbVelocityMirror::Impl {
    bool requested{}, stopped{}, failed{}, frame_drawn{};
    u64 draws{};
    std::unique_ptr<VideoCore::UniqueImage> image;
    std::unique_ptr<VideoCore::UniqueImage> depth;
    vk::UniqueImageView view;
    vk::UniqueImageView depth_view;
    vk::ImageLayout layout{vk::ImageLayout::eUndefined};
    vk::ImageLayout depth_layout{vk::ImageLayout::eUndefined};
    u64 source_depth_uid{};
    vk::Extent2D size{2560, 1440}; // replay resolution (the scene's render size)
    vk::Extent2D target{2560, 1440};
    Impl() {
        requested = BbTemporalDlssRequested();
    }
    void Transit(vk::CommandBuffer command, vk::ImageLayout next, vk::PipelineStageFlags2 stage,
                 vk::AccessFlags2 access) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask =
                layout == vk::ImageLayout::eUndefined
                    ? vk::AccessFlagBits2::eNone
                    : vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = stage,
            .dstAccessMask = access,
            .oldLayout = layout,
            .newLayout = next,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image->image,
            .subresourceRange = Range};
        command.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        layout = next;
    }
    void TransitDepth(vk::CommandBuffer command, vk::ImageLayout next,
                      vk::PipelineStageFlags2 stage, vk::AccessFlags2 access) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask =
                depth_layout == vk::ImageLayout::eUndefined
                    ? vk::AccessFlagBits2::eNone
                    : vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = stage,
            .dstAccessMask = access,
            .oldLayout = depth_layout,
            .newLayout = next,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = depth->image,
            .subresourceRange = DepthRange};
        command.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        depth_layout = next;
    }
};
BbVelocityMirror::BbVelocityMirror() : impl{std::make_unique<Impl>()} {}
BbVelocityMirror::~BbVelocityMirror() = default;
bool BbVelocityMirror::Requested() const {
    return impl->requested;
}
bool BbVelocityMirror::BeginDraw(const Instance& instance, Runtime& runtime, Scheduler& scheduler,
                                 const GraphicsPipeline& pipeline, const RenderState& guest_state,
                                 VideoCore::Image* guest_depth, u32 depth_layer) {
    if (!impl->requested || impl->stopped || impl->failed || !pipeline.VelocityMirrorHandle() ||
        guest_state.width != SourceWidth || guest_state.height != SourceHeight ||
        guest_state.num_layers != 1 || guest_state.num_color_attachments != 1 ||
        !guest_state.depth_stencil_attachment.has_depth ||
        !guest_state.depth_stencil_attachment.has_stencil || !guest_depth ||
        !guest_depth->backing || depth_layer != 0)
        return false;
    const auto& depth_ci = guest_depth->backing->image.image_ci;
    if (depth_ci.format != vk::Format::eD32SfloatS8Uint ||
        depth_ci.samples != vk::SampleCountFlagBits::e1 ||
        depth_ci.extent.width < guest_state.width || depth_ci.extent.height < guest_state.height ||
        !(depth_ci.usage & vk::ImageUsageFlagBits::eTransferSrc) ||
        !instance.IsFormatSupported(depth_ci.format,
                                    vk::FormatFeatureFlagBits2::eBlitSrc |
                                        vk::FormatFeatureFlagBits2::eBlitDst |
                                        vk::FormatFeatureFlagBits2::eDepthStencilAttachment))
        return false;
    const auto& state = scheduler.GetDynamicState();
    if (state.viewports.size() != 1 || state.scissors.size() != 1 ||
        state.rasterizer_discard_enable)
        return false;
    auto viewports = state.viewports;
    auto scissors = state.scissors;
    auto& viewport = viewports[0];
    static u32 viewport_diagnostics{};
    if (viewport_diagnostics++ < 4)
        LOG_INFO(Render_Vulkan, "[BB-VELOCITY-MIRROR] Viewport {}x{}, xy={},{}; target={}x{}",
                 viewport.width, viewport.height, viewport.x, viewport.y, guest_state.width,
                 guest_state.height);
    // Screen-space/clipping-disabled fullscreen draws are intentionally unsupported.
    if (std::abs(viewport.width) != float(SourceWidth) ||
        std::abs(viewport.height) != float(SourceHeight))
        return false;
    if (impl->image && impl->size != impl->target) {
        scheduler.Finish(); // the old images may still be in flight
        impl->image.reset();
        impl->depth.reset();
        impl->view.reset();
        impl->depth_view.reset();
        impl->layout = impl->depth_layout = vk::ImageLayout::eUndefined;
        impl->frame_drawn = false;
    }
    impl->size = impl->target;
    const u32 Width = impl->size.width, Height = impl->size.height;
    const float fx = float(Width) / SourceWidth, fy = float(Height) / SourceHeight;
    viewport.x *= fx;
    viewport.y *= fy;
    viewport.width *= fx;
    viewport.height *= fy;
    scissors[0].offset.x = s32(std::lround(scissors[0].offset.x * fx));
    scissors[0].offset.y = s32(std::lround(scissors[0].offset.y * fy));
    scissors[0].extent.width = u32(std::lround(scissors[0].extent.width * fx));
    scissors[0].extent.height = u32(std::lround(scissors[0].extent.height * fy));
    scheduler.EndRendering();
    try {
        if (!impl->image) {
            impl->image = std::make_unique<VideoCore::UniqueImage>(instance.GetDevice(),
                                                                   instance.GetAllocator());
            impl->image->Create(vk::ImageCreateInfo{
                .imageType = vk::ImageType::e2D,
                .format = vk::Format::eR16G16B16A16Sfloat,
                .extent = {Width, Height, 1},
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eColorAttachment |
                         vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc});
            auto result = instance.GetDevice().createImageViewUnique(
                vk::ImageViewCreateInfo{.image = impl->image->image,
                                        .viewType = vk::ImageViewType::e2D,
                                        .format = vk::Format::eR16G16B16A16Sfloat,
                                        .subresourceRange = Range});
            if (result.result != vk::Result::eSuccess)
                throw std::runtime_error("velocity mirror image view unavailable");
            impl->view = std::move(result.value);
            impl->depth = std::make_unique<VideoCore::UniqueImage>(instance.GetDevice(),
                                                                   instance.GetAllocator());
            impl->depth->Create(
                vk::ImageCreateInfo{.imageType = vk::ImageType::e2D,
                                    .format = depth_ci.format,
                                    .extent = {Width, Height, 1},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = vk::SampleCountFlagBits::e1,
                                    .tiling = vk::ImageTiling::eOptimal,
                                    .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                             vk::ImageUsageFlagBits::eTransferDst});
            auto result_depth = instance.GetDevice().createImageViewUnique(
                vk::ImageViewCreateInfo{.image = impl->depth->image,
                                        .viewType = vk::ImageViewType::e2D,
                                        .format = depth_ci.format,
                                        .subresourceRange = DepthRange});
            if (result_depth.result != vk::Result::eSuccess)
                throw std::runtime_error("velocity mirror depth view unavailable");
            impl->depth_view = std::move(result_depth.value);
        }
        const auto command = scheduler.CommandBuffer();
        if (!impl->frame_drawn) {
            // Seed the exact original render-area depth/stencil BEFORE the first guest draw.
            // Nearest scaling preserves its visibility tests; the original image is read only.
            runtime.Transit(guest_depth, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eBlit, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            impl->TransitDepth(command, vk::ImageLayout::eTransferDstOptimal,
                               vk::PipelineStageFlagBits2::eBlit,
                               vk::AccessFlagBits2::eTransferWrite);
            std::array<vk::ImageBlit, 2> blits;
            for (u32 i = 0; i < blits.size(); ++i) {
                const auto aspect =
                    i == 0 ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eStencil;
                blits[i] = vk::ImageBlit{
                    .srcSubresource = {.aspectMask = aspect,
                                       .baseArrayLayer = depth_layer,
                                       .layerCount = 1},
                    .srcOffsets = std::array{vk::Offset3D{}, vk::Offset3D{guest_state.width,
                                                                          guest_state.height, 1}},
                    .dstSubresource = {.aspectMask = aspect, .layerCount = 1},
                    .dstOffsets =
                        std::array{vk::Offset3D{}, vk::Offset3D{s32(Width), s32(Height), 1}}};
            }
            command.blitImage(guest_depth->GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                              impl->depth->image, vk::ImageLayout::eTransferDstOptimal, blits,
                              vk::Filter::eNearest);
            runtime.Transit(guest_depth, guest_state.depth_stencil_attachment.image_layout,
                            vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                                vk::PipelineStageFlagBits2::eLateFragmentTests,
                            vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                                vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
            runtime.FlushBarriers();
            impl->source_depth_uid = guest_depth->image_uid;
        }
        impl->TransitDepth(command, vk::ImageLayout::eDepthStencilAttachmentOptimal,
                           vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                               vk::PipelineStageFlagBits2::eLateFragmentTests,
                           vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                               vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
        impl->Transit(command, vk::ImageLayout::eColorAttachmentOptimal,
                      vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                      vk::AccessFlagBits2::eColorAttachmentRead |
                          vk::AccessFlagBits2::eColorAttachmentWrite);
        RenderState mirror{};
        mirror.width = Width;
        mirror.height = Height;
        mirror.num_layers = 1;
        mirror.num_color_attachments = 1;
        mirror.depth_stencil_attachment = guest_state.depth_stencil_attachment;
        mirror.depth_stencil_attachment.image_view = *impl->depth_view;
        mirror.depth_stencil_attachment.image_layout =
            vk::ImageLayout::eDepthStencilAttachmentOptimal;
        mirror.depth_stencil_attachment.depth_clear = false;
        mirror.depth_stencil_attachment.stencil_clear = false;
        mirror.color_attachments[0] = {
            .image_view = *impl->view,
            .image_layout = vk::ImageLayout::eColorAttachmentOptimal,
            .clear_value = {std::bit_cast<u32>(.5f), std::bit_cast<u32>(.5f), 0, 0},
            .is_clear = impl->frame_drawn ? 0u : 1u};
        command.setViewportWithCount(viewports);
        command.setScissorWithCount(scissors);
        scheduler.BeginRendering(mirror);
        command.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.VelocityMirrorHandle());
        impl->frame_drawn = true;
        ++impl->draws;
        return true;
    } catch (const std::exception& error) {
        impl->failed = true;
        scheduler.EndRendering();
        scheduler.GetDynamicState().Invalidate();
        LOG_WARNING(Render_Vulkan, "[BB-VELOCITY-MIRROR] Disabled: {}", error.what());
        return false;
    }
}
void BbVelocityMirror::EndDraw(Scheduler& scheduler) {
    scheduler.EndRendering();
    scheduler.GetDynamicState().Invalidate();
}
std::optional<BbVelocityMirror::Frame> BbVelocityMirror::ConsumeFrame(vk::CommandBuffer command) {
    if (!impl->requested || impl->stopped || impl->failed || !impl->frame_drawn || !impl->image)
        return {};
    impl->frame_drawn = false;
    impl->Transit(command, vk::ImageLayout::eShaderReadOnlyOptimal,
                  vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
    return Frame{*impl->view, impl->size};
}
void BbVelocityMirror::SetTargetSize(vk::Extent2D size) {
    impl->target = size;
}
void BbVelocityMirror::Shutdown(Scheduler& scheduler) {
    if (!impl->requested || impl->stopped)
        return;
    impl->stopped = true;
    scheduler.Finish();
    impl->view.reset();
    impl->depth_view.reset();
    impl->image.reset();
    impl->depth.reset();
    LOG_INFO(Render_Vulkan,
             "[BB-VELOCITY-MIRROR] Orderly GPU-thread teardown after{} mirrored draws",
             impl->draws);
}
} // namespace Vulkan
