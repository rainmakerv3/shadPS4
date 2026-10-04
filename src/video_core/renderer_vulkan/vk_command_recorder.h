// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstring>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include "common/assert.h"
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_command_stream.h"

namespace Vulkan {

/// Records commands into a command buffer, either right away or through a stream another thread
/// replays them from. It has the methods of vk::CommandBuffer that are used, taking the same
/// arguments, and copies whatever they point to when the commands are replayed later.
class CommandRecorder {
public:
    CommandRecorder() = default;

    /// Records straight into the command buffer.
    explicit CommandRecorder(vk::CommandBuffer cmdbuf_) noexcept : cmdbuf{cmdbuf_} {}

    /// Records into the stream, for the command buffer of the session the target names.
    CommandRecorder(CommandStream& stream_, CommandTarget target_) noexcept
        : stream{&stream_}, target{target_} {}

    /// The command buffer recorded into, for code that records into it on its own. Only commands
    /// recorded straight into it have one.
    [[nodiscard]] vk::CommandBuffer DirectHandle() const {
        ASSERT_MSG(!stream, "Command buffer is recorded on another thread");
        return cmdbuf;
    }

    vk::Result reset(vk::CommandBufferResetFlags flags = {}) const {
        if (!stream) {
            return cmdbuf.reset(flags);
        }
        Record([=](vk::CommandBuffer cmd) { static_cast<void>(cmd.reset(flags)); });
        return vk::Result::eSuccess;
    }

    void bindPipeline(vk::PipelineBindPoint bind_point, vk::Pipeline pipeline) const {
        Record([=](vk::CommandBuffer cmd) { cmd.bindPipeline(bind_point, pipeline); });
    }

    void pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages, u32 offset, u32 size,
                       const void* values) const {
        if (!stream) {
            cmdbuf.pushConstants(layout, stages, offset, size, values);
            return;
        }
        stream->Reserve(CommandStream::DataSize(size) + CommandStream::MaxCommandSize);
        const u8* const copy = stream->Copy(static_cast<const u8*>(values), size);
        Record(
            [=](vk::CommandBuffer cmd) { cmd.pushConstants(layout, stages, offset, size, copy); });
    }

    void pushDescriptorSetKHR(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout, u32 set,
                              vk::ArrayProxy<const vk::WriteDescriptorSet> writes) const {
        if (!stream) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, set, writes);
            return;
        }
        // Most draws push a dozen descriptors. The writes are kept without what goes unused in
        // them, which was most of the data recorded for a draw, and made again when replayed.
        size_t bytes = writes.size() * sizeof(PackedWrite);
        for (const auto& write : writes) {
            bytes += Align8(InfoSize(InfoOf(write.descriptorType), write.descriptorCount));
        }
        stream->Reserve(CommandStream::DataSize(bytes) + CommandStream::MaxCommandSize);
        u8* const data = stream->AllocateData(bytes);
        auto* const packed = reinterpret_cast<PackedWrite*>(data);
        u8* infos = data + writes.size() * sizeof(PackedWrite);
        for (u32 i = 0; i < writes.size(); ++i) {
            const auto& write = writes.data()[i];
            ASSERT_MSG(!write.pNext, "Descriptor write extensions are not copied");
            const DescriptorInfo info = InfoOf(write.descriptorType);
            packed[i] = PackedWrite{
                .binding = write.dstBinding,
                .array_element = write.dstArrayElement,
                .count = write.descriptorCount,
                .type = write.descriptorType,
                .info = info,
            };
            const size_t info_size = InfoSize(info, write.descriptorCount);
            if (info_size != 0) {
                std::memcpy(infos, InfoData(info, write), info_size);
            }
            infos += Align8(info_size);
        }
        const u32 count = writes.size();
        Record([=](vk::CommandBuffer cmd) {
            // Only the replaying thread uses it, and its writes keep their type and chain.
            thread_local std::vector<vk::WriteDescriptorSet> unpacked;
            if (unpacked.size() < count) {
                unpacked.resize(count);
            }
            const u8* next_info = data + count * sizeof(PackedWrite);
            for (u32 i = 0; i < count; ++i) {
                const PackedWrite& write = packed[i];
                auto& unpacked_write = unpacked[i];
                unpacked_write.dstBinding = write.binding;
                unpacked_write.dstArrayElement = write.array_element;
                unpacked_write.descriptorCount = write.count;
                unpacked_write.descriptorType = write.type;
                unpacked_write.pImageInfo =
                    write.info == DescriptorInfo::Image
                        ? reinterpret_cast<const vk::DescriptorImageInfo*>(next_info)
                        : nullptr;
                unpacked_write.pBufferInfo =
                    write.info == DescriptorInfo::Buffer
                        ? reinterpret_cast<const vk::DescriptorBufferInfo*>(next_info)
                        : nullptr;
                unpacked_write.pTexelBufferView =
                    write.info == DescriptorInfo::TexelBuffer
                        ? reinterpret_cast<const vk::BufferView*>(next_info)
                        : nullptr;
                next_info += Align8(InfoSize(write.info, write.count));
            }
            cmd.pushDescriptorSetKHR(bind_point, layout, set, count, unpacked.data());
        });
    }

    void bindDescriptorSets(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout,
                            u32 first_set, vk::ArrayProxy<const vk::DescriptorSet> sets,
                            vk::ArrayProxy<const u32> dynamic_offsets) const {
        if (!stream) {
            cmdbuf.bindDescriptorSets(bind_point, layout, first_set, sets, dynamic_offsets);
            return;
        }
        const auto copies = CopyArrays(Span(sets), Span(dynamic_offsets));
        const auto* const set_copies = std::get<0>(copies);
        const auto* const offset_copies = std::get<1>(copies);
        const u32 num_sets = sets.size();
        const u32 num_offsets = dynamic_offsets.size();
        Record([=](vk::CommandBuffer cmd) {
            cmd.bindDescriptorSets(bind_point, layout, first_set, num_sets, set_copies, num_offsets,
                                   offset_copies);
        });
    }

    void bindIndexBuffer(vk::Buffer buffer, vk::DeviceSize offset, vk::IndexType type) const {
        Record([=](vk::CommandBuffer cmd) { cmd.bindIndexBuffer(buffer, offset, type); });
    }

    void bindVertexBuffers(u32 first, u32 count, const vk::Buffer* buffers,
                           const vk::DeviceSize* offsets) const {
        if (!stream) {
            cmdbuf.bindVertexBuffers(first, count, buffers, offsets);
            return;
        }
        const auto copies = CopyArrays(std::span{buffers, count}, std::span{offsets, count});
        const auto* const buffer_copies = std::get<0>(copies);
        const auto* const offset_copies = std::get<1>(copies);
        Record([=](vk::CommandBuffer cmd) {
            cmd.bindVertexBuffers(first, count, buffer_copies, offset_copies);
        });
    }

    void bindVertexBuffers2(u32 first, u32 count, const vk::Buffer* buffers,
                            const vk::DeviceSize* offsets, const vk::DeviceSize* sizes,
                            const vk::DeviceSize* strides) const {
        if (!stream) {
            cmdbuf.bindVertexBuffers2(first, count, buffers, offsets, sizes, strides);
            return;
        }
        const auto copies = CopyArrays(std::span{buffers, count}, std::span{offsets, count},
                                       std::span{sizes, sizes ? count : 0},
                                       std::span{strides, strides ? count : 0});
        const auto* const buffer_copies = std::get<0>(copies);
        const auto* const offset_copies = std::get<1>(copies);
        const auto* const size_copies = std::get<2>(copies);
        const auto* const stride_copies = std::get<3>(copies);
        Record([=](vk::CommandBuffer cmd) {
            cmd.bindVertexBuffers2(first, count, buffer_copies, offset_copies, size_copies,
                                   stride_copies);
        });
    }

    void setVertexInputEXT(
        vk::ArrayProxy<const vk::VertexInputBindingDescription2EXT> bindings,
        vk::ArrayProxy<const vk::VertexInputAttributeDescription2EXT> attributes) const {
        if (!stream) {
            cmdbuf.setVertexInputEXT(bindings, attributes);
            return;
        }
        const auto copies = CopyArrays(Span(bindings), Span(attributes));
        const auto* const binding_copies = std::get<0>(copies);
        const auto* const attribute_copies = std::get<1>(copies);
        const u32 num_bindings = bindings.size();
        const u32 num_attributes = attributes.size();
        Record([=](vk::CommandBuffer cmd) {
            cmd.setVertexInputEXT(num_bindings, binding_copies, num_attributes, attribute_copies);
        });
    }

    void draw(u32 vertex_count, u32 instance_count, u32 first_vertex, u32 first_instance) const {
        Record([=](vk::CommandBuffer cmd) {
            cmd.draw(vertex_count, instance_count, first_vertex, first_instance);
        });
    }

    void drawIndexed(u32 index_count, u32 instance_count, u32 first_index, s32 vertex_offset,
                     u32 first_instance) const {
        Record([=](vk::CommandBuffer cmd) {
            cmd.drawIndexed(index_count, instance_count, first_index, vertex_offset,
                            first_instance);
        });
    }

    void drawIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 draw_count, u32 stride) const {
        Record(
            [=](vk::CommandBuffer cmd) { cmd.drawIndirect(buffer, offset, draw_count, stride); });
    }

    void drawIndexedIndirect(vk::Buffer buffer, vk::DeviceSize offset, u32 draw_count,
                             u32 stride) const {
        Record([=](vk::CommandBuffer cmd) {
            cmd.drawIndexedIndirect(buffer, offset, draw_count, stride);
        });
    }

    void drawIndirectCount(vk::Buffer buffer, vk::DeviceSize offset, vk::Buffer count_buffer,
                           vk::DeviceSize count_offset, u32 max_draw_count, u32 stride) const {
        Record([=](vk::CommandBuffer cmd) {
            cmd.drawIndirectCount(buffer, offset, count_buffer, count_offset, max_draw_count,
                                  stride);
        });
    }

    void drawIndexedIndirectCount(vk::Buffer buffer, vk::DeviceSize offset, vk::Buffer count_buffer,
                                  vk::DeviceSize count_offset, u32 max_draw_count,
                                  u32 stride) const {
        Record([=](vk::CommandBuffer cmd) {
            cmd.drawIndexedIndirectCount(buffer, offset, count_buffer, count_offset, max_draw_count,
                                         stride);
        });
    }

    void dispatch(u32 group_count_x, u32 group_count_y, u32 group_count_z) const {
        Record([=](vk::CommandBuffer cmd) {
            cmd.dispatch(group_count_x, group_count_y, group_count_z);
        });
    }

    void dispatchIndirect(vk::Buffer buffer, vk::DeviceSize offset) const {
        Record([=](vk::CommandBuffer cmd) { cmd.dispatchIndirect(buffer, offset); });
    }

    void beginRendering(const vk::RenderingInfo& info) const {
        if (!stream) {
            cmdbuf.beginRendering(info);
            return;
        }
        ASSERT_MSG(!info.pNext, "Rendering info extensions are not copied");
        const auto copies =
            CopyArrays(std::span{info.pColorAttachments, info.colorAttachmentCount},
                       std::span{info.pDepthAttachment, info.pDepthAttachment ? 1u : 0u},
                       std::span{info.pStencilAttachment, info.pStencilAttachment ? 1u : 0u});
        vk::RenderingInfo copy = info;
        copy.pColorAttachments = std::get<0>(copies);
        copy.pDepthAttachment = std::get<1>(copies);
        copy.pStencilAttachment = std::get<2>(copies);
        Record([=](vk::CommandBuffer cmd) { cmd.beginRendering(copy); });
    }

    void endRendering() const {
        Record([](vk::CommandBuffer cmd) { cmd.endRendering(); });
    }

    void pipelineBarrier2(const vk::DependencyInfo& info) const {
        if (!stream) {
            cmdbuf.pipelineBarrier2(info);
            return;
        }
        ASSERT_MSG(!info.pNext, "Dependency info extensions are not copied");
        const auto copies =
            CopyArrays(std::span{info.pMemoryBarriers, info.memoryBarrierCount},
                       std::span{info.pBufferMemoryBarriers, info.bufferMemoryBarrierCount},
                       std::span{info.pImageMemoryBarriers, info.imageMemoryBarrierCount});
        vk::DependencyInfo copy = info;
        copy.pMemoryBarriers = std::get<0>(copies);
        copy.pBufferMemoryBarriers = std::get<1>(copies);
        copy.pImageMemoryBarriers = std::get<2>(copies);
        Record([=](vk::CommandBuffer cmd) { cmd.pipelineBarrier2(copy); });
    }

    void pipelineBarrier(vk::PipelineStageFlags src_stages, vk::PipelineStageFlags dst_stages,
                         vk::DependencyFlags flags,
                         vk::ArrayProxy<const vk::MemoryBarrier> memory_barriers,
                         vk::ArrayProxy<const vk::BufferMemoryBarrier> buffer_barriers,
                         vk::ArrayProxy<const vk::ImageMemoryBarrier> image_barriers) const {
        if (!stream) {
            cmdbuf.pipelineBarrier(src_stages, dst_stages, flags, memory_barriers, buffer_barriers,
                                   image_barriers);
            return;
        }
        const auto copies =
            CopyArrays(Span(memory_barriers), Span(buffer_barriers), Span(image_barriers));
        const auto* const memory_copies = std::get<0>(copies);
        const auto* const buffer_copies = std::get<1>(copies);
        const auto* const image_copies = std::get<2>(copies);
        const u32 num_memory = memory_barriers.size();
        const u32 num_buffer = buffer_barriers.size();
        const u32 num_image = image_barriers.size();
        Record([=](vk::CommandBuffer cmd) {
            cmd.pipelineBarrier(src_stages, dst_stages, flags, num_memory, memory_copies,
                                num_buffer, buffer_copies, num_image, image_copies);
        });
    }

    void copyBuffer(vk::Buffer src, vk::Buffer dst,
                    vk::ArrayProxy<const vk::BufferCopy> regions) const {
        if (!stream) {
            cmdbuf.copyBuffer(src, dst, regions);
            return;
        }
        const auto* const copies = CopyArray(regions);
        const u32 count = regions.size();
        Record([=](vk::CommandBuffer cmd) { cmd.copyBuffer(src, dst, count, copies); });
    }

    void copyBufferToImage(vk::Buffer src, vk::Image dst, vk::ImageLayout layout,
                           vk::ArrayProxy<const vk::BufferImageCopy> regions) const {
        if (!stream) {
            cmdbuf.copyBufferToImage(src, dst, layout, regions);
            return;
        }
        const auto* const copies = CopyArray(regions);
        const u32 count = regions.size();
        Record(
            [=](vk::CommandBuffer cmd) { cmd.copyBufferToImage(src, dst, layout, count, copies); });
    }

    void copyImageToBuffer(vk::Image src, vk::ImageLayout layout, vk::Buffer dst,
                           vk::ArrayProxy<const vk::BufferImageCopy> regions) const {
        if (!stream) {
            cmdbuf.copyImageToBuffer(src, layout, dst, regions);
            return;
        }
        const auto* const copies = CopyArray(regions);
        const u32 count = regions.size();
        Record(
            [=](vk::CommandBuffer cmd) { cmd.copyImageToBuffer(src, layout, dst, count, copies); });
    }

    void copyImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                   vk::ImageLayout dst_layout, vk::ArrayProxy<const vk::ImageCopy> regions) const {
        if (!stream) {
            cmdbuf.copyImage(src, src_layout, dst, dst_layout, regions);
            return;
        }
        const auto* const copies = CopyArray(regions);
        const u32 count = regions.size();
        Record([=](vk::CommandBuffer cmd) {
            cmd.copyImage(src, src_layout, dst, dst_layout, count, copies);
        });
    }

    void resolveImage(vk::Image src, vk::ImageLayout src_layout, vk::Image dst,
                      vk::ImageLayout dst_layout,
                      vk::ArrayProxy<const vk::ImageResolve> regions) const {
        if (!stream) {
            cmdbuf.resolveImage(src, src_layout, dst, dst_layout, regions);
            return;
        }
        const auto* const copies = CopyArray(regions);
        const u32 count = regions.size();
        Record([=](vk::CommandBuffer cmd) {
            cmd.resolveImage(src, src_layout, dst, dst_layout, count, copies);
        });
    }

    void clearColorImage(vk::Image image, vk::ImageLayout layout, const vk::ClearColorValue& color,
                         vk::ArrayProxy<const vk::ImageSubresourceRange> ranges) const {
        if (!stream) {
            cmdbuf.clearColorImage(image, layout, color, ranges);
            return;
        }
        const auto* const copies = CopyArray(ranges);
        const u32 count = ranges.size();
        Record([=](vk::CommandBuffer cmd) {
            cmd.clearColorImage(image, layout, &color, count, copies);
        });
    }

    void fillBuffer(vk::Buffer buffer, vk::DeviceSize offset, vk::DeviceSize size,
                    u32 value) const {
        Record([=](vk::CommandBuffer cmd) { cmd.fillBuffer(buffer, offset, size, value); });
    }

    void updateBuffer(vk::Buffer buffer, vk::DeviceSize offset, vk::DeviceSize size,
                      const void* data) const {
        if (!stream) {
            cmdbuf.updateBuffer(buffer, offset, size, data);
            return;
        }
        stream->Reserve(CommandStream::DataSize(size) + CommandStream::MaxCommandSize);
        const u8* const copy = stream->Copy(static_cast<const u8*>(data), size);
        Record([=](vk::CommandBuffer cmd) { cmd.updateBuffer(buffer, offset, size, copy); });
    }

    void resetQueryPool(vk::QueryPool pool, u32 first, u32 count) const {
        Record([=](vk::CommandBuffer cmd) { cmd.resetQueryPool(pool, first, count); });
    }

    void writeTimestamp2(vk::PipelineStageFlags2 stage, vk::QueryPool pool, u32 query) const {
        Record([=](vk::CommandBuffer cmd) { cmd.writeTimestamp2(stage, pool, query); });
    }

    void beginDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const {
        if (!stream) {
            cmdbuf.beginDebugUtilsLabelEXT(label);
            return;
        }
        const vk::DebugUtilsLabelEXT copy = CopyLabel(label);
        Record([=](vk::CommandBuffer cmd) { cmd.beginDebugUtilsLabelEXT(copy); });
    }

    void insertDebugUtilsLabelEXT(const vk::DebugUtilsLabelEXT& label) const {
        if (!stream) {
            cmdbuf.insertDebugUtilsLabelEXT(label);
            return;
        }
        const vk::DebugUtilsLabelEXT copy = CopyLabel(label);
        Record([=](vk::CommandBuffer cmd) { cmd.insertDebugUtilsLabelEXT(copy); });
    }

    void endDebugUtilsLabelEXT() const {
        Record([](vk::CommandBuffer cmd) { cmd.endDebugUtilsLabelEXT(); });
    }

    void setViewport(u32 first, vk::ArrayProxy<const vk::Viewport> viewports) const {
        if (!stream) {
            cmdbuf.setViewport(first, viewports);
            return;
        }
        const auto* const copies = CopyArray(viewports);
        const u32 count = viewports.size();
        Record([=](vk::CommandBuffer cmd) { cmd.setViewport(first, count, copies); });
    }

    void setScissor(u32 first, vk::ArrayProxy<const vk::Rect2D> scissors) const {
        if (!stream) {
            cmdbuf.setScissor(first, scissors);
            return;
        }
        const auto* const copies = CopyArray(scissors);
        const u32 count = scissors.size();
        Record([=](vk::CommandBuffer cmd) { cmd.setScissor(first, count, copies); });
    }

    void setViewportWithCount(vk::ArrayProxy<const vk::Viewport> viewports) const {
        if (!stream) {
            cmdbuf.setViewportWithCount(viewports);
            return;
        }
        const auto* const copies = CopyArray(viewports);
        const u32 count = viewports.size();
        Record([=](vk::CommandBuffer cmd) { cmd.setViewportWithCount(count, copies); });
    }

    void setScissorWithCount(vk::ArrayProxy<const vk::Rect2D> scissors) const {
        if (!stream) {
            cmdbuf.setScissorWithCount(scissors);
            return;
        }
        const auto* const copies = CopyArray(scissors);
        const u32 count = scissors.size();
        Record([=](vk::CommandBuffer cmd) { cmd.setScissorWithCount(count, copies); });
    }

    void setDepthTestEnable(vk::Bool32 enable) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setDepthTestEnable(enable); });
    }

    void setDepthWriteEnable(vk::Bool32 enable) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setDepthWriteEnable(enable); });
    }

    void setDepthCompareOp(vk::CompareOp op) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setDepthCompareOp(op); });
    }

    void setDepthBoundsTestEnable(vk::Bool32 enable) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setDepthBoundsTestEnable(enable); });
    }

    void setDepthBounds(float min, float max) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setDepthBounds(min, max); });
    }

    void setDepthBiasEnable(vk::Bool32 enable) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setDepthBiasEnable(enable); });
    }

    void setDepthBias(float constant, float clamp, float slope) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setDepthBias(constant, clamp, slope); });
    }

    void setStencilTestEnable(vk::Bool32 enable) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setStencilTestEnable(enable); });
    }

    void setStencilOp(vk::StencilFaceFlags faces, vk::StencilOp fail_op, vk::StencilOp pass_op,
                      vk::StencilOp depth_fail_op, vk::CompareOp compare_op) const {
        Record([=](vk::CommandBuffer cmd) {
            cmd.setStencilOp(faces, fail_op, pass_op, depth_fail_op, compare_op);
        });
    }

    void setStencilReference(vk::StencilFaceFlags faces, u32 reference) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setStencilReference(faces, reference); });
    }

    void setStencilWriteMask(vk::StencilFaceFlags faces, u32 mask) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setStencilWriteMask(faces, mask); });
    }

    void setStencilCompareMask(vk::StencilFaceFlags faces, u32 mask) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setStencilCompareMask(faces, mask); });
    }

    void setPrimitiveRestartEnable(vk::Bool32 enable) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setPrimitiveRestartEnable(enable); });
    }

    void setRasterizerDiscardEnable(vk::Bool32 enable) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setRasterizerDiscardEnable(enable); });
    }

    void setCullMode(vk::CullModeFlags mode) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setCullMode(mode); });
    }

    void setFrontFace(vk::FrontFace face) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setFrontFace(face); });
    }

    void setBlendConstants(const float constants[4]) const {
        std::array<float, 4> values;
        std::memcpy(values.data(), constants, sizeof(values));
        Record([=](vk::CommandBuffer cmd) { cmd.setBlendConstants(values.data()); });
    }

    void setColorWriteMaskEXT(u32 first,
                              vk::ArrayProxy<const vk::ColorComponentFlags> masks) const {
        if (!stream) {
            cmdbuf.setColorWriteMaskEXT(first, masks);
            return;
        }
        const auto* const copies = CopyArray(masks);
        const u32 count = masks.size();
        Record([=](vk::CommandBuffer cmd) { cmd.setColorWriteMaskEXT(first, count, copies); });
    }

    void setLineWidth(float width) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setLineWidth(width); });
    }

    void setAttachmentFeedbackLoopEnableEXT(vk::ImageAspectFlags aspects) const {
        Record([=](vk::CommandBuffer cmd) { cmd.setAttachmentFeedbackLoopEnableEXT(aspects); });
    }

private:
    /// Records a command, a function of the command buffer, right away or into the stream.
    template <typename Func>
    void Record(Func&& func) const {
        if (!stream) {
            func(cmdbuf);
            return;
        }
        stream->Emit([func = std::forward<Func>(func), target = target](RecordingContext& context) {
            func(context.Get(target));
        });
    }

    [[nodiscard]] static constexpr size_t Align8(size_t bytes) noexcept {
        return (bytes + 7) & ~size_t{7};
    }

    template <typename T>
    [[nodiscard]] static std::span<const T> Span(const vk::ArrayProxy<const T>& array) noexcept {
        return {array.data(), array.size()};
    }

    /// Copies arrays the next command points to into one piece of the stream, with room for the
    /// command after it. Empty arrays come back null.
    template <typename... Ts>
    [[nodiscard]] std::tuple<const Ts*...> CopyArrays(std::span<const Ts>... arrays) const {
        static_assert(((alignof(Ts) <= 8) && ...));
        const size_t bytes = (Align8(arrays.size_bytes()) + ... + 0);
        stream->Reserve(CommandStream::DataSize(bytes) + CommandStream::MaxCommandSize);
        u8* data = bytes != 0 ? stream->AllocateData(bytes) : nullptr;
        const auto copy = [&data]<typename T>(std::span<const T> array) -> const T* {
            if (array.empty()) {
                return nullptr;
            }
            auto* const destination = reinterpret_cast<T*>(data);
            std::memcpy(static_cast<void*>(destination), array.data(), array.size_bytes());
            data += Align8(array.size_bytes());
            return destination;
        };
        // Copied in order, as braced lists evaluate their elements in order.
        return std::tuple<const Ts*...>{copy(arrays)...};
    }

    /// Copies an array the next command points to, with room for the command after it.
    template <typename T>
    [[nodiscard]] const T* CopyArray(const vk::ArrayProxy<const T>& array) const {
        return std::get<0>(CopyArrays(Span(array)));
    }

    [[nodiscard]] vk::DebugUtilsLabelEXT CopyLabel(const vk::DebugUtilsLabelEXT& label) const {
        ASSERT_MSG(!label.pNext, "Label extensions are not copied");
        const size_t length = label.pLabelName ? std::strlen(label.pLabelName) + 1 : 0;
        stream->Reserve(CommandStream::DataSize(length) + CommandStream::MaxCommandSize);
        vk::DebugUtilsLabelEXT copy = label;
        copy.pLabelName = stream->Copy(label.pLabelName, length);
        return copy;
    }

    /// What a descriptor write points to, which only its type says.
    enum class DescriptorInfo : u32 {
        Image,
        Buffer,
        TexelBuffer,
    };

    [[nodiscard]] static DescriptorInfo InfoOf(vk::DescriptorType type) {
        // The types besides these have values the compiler can't make a table of, and a
        // switch over all of them was half of what recording a draw took.
        static constexpr std::array<DescriptorInfo, 11> Infos = {
            DescriptorInfo::Image,       // Sampler
            DescriptorInfo::Image,       // CombinedImageSampler
            DescriptorInfo::Image,       // SampledImage
            DescriptorInfo::Image,       // StorageImage
            DescriptorInfo::TexelBuffer, // UniformTexelBuffer
            DescriptorInfo::TexelBuffer, // StorageTexelBuffer
            DescriptorInfo::Buffer,      // UniformBuffer
            DescriptorInfo::Buffer,      // StorageBuffer
            DescriptorInfo::Buffer,      // UniformBufferDynamic
            DescriptorInfo::Buffer,      // StorageBufferDynamic
            DescriptorInfo::Image,       // InputAttachment
        };
        static_assert(static_cast<u32>(vk::DescriptorType::eInputAttachment) == Infos.size() - 1);
        const auto index = static_cast<u32>(type);
        if (index >= Infos.size()) [[unlikely]] {
            UnsupportedDescriptorType(type);
        }
        return Infos[index];
    }

    /// Kept out of line, so that what is called for every descriptor stays a table lookup.
    [[noreturn]] SHAD_NO_INLINE static void UnsupportedDescriptorType(vk::DescriptorType type) {
        UNREACHABLE_MSG("Descriptor type {} is not copied", vk::to_string(type));
    }

    [[nodiscard]] static size_t InfoSize(DescriptorInfo info, u32 count) {
        switch (info) {
        case DescriptorInfo::Image:
            return count * sizeof(vk::DescriptorImageInfo);
        case DescriptorInfo::Buffer:
            return count * sizeof(vk::DescriptorBufferInfo);
        case DescriptorInfo::TexelBuffer:
            return count * sizeof(vk::BufferView);
        }
        return 0;
    }

    [[nodiscard]] static const void* InfoData(DescriptorInfo info,
                                              const vk::WriteDescriptorSet& write) {
        switch (info) {
        case DescriptorInfo::Image:
            return write.pImageInfo;
        case DescriptorInfo::Buffer:
            return write.pBufferInfo;
        case DescriptorInfo::TexelBuffer:
            return write.pTexelBufferView;
        }
        return nullptr;
    }

    /// A descriptor write as recorded, followed by what it points to.
    struct PackedWrite {
        u32 binding;
        u32 array_element;
        u32 count;
        vk::DescriptorType type;
        DescriptorInfo info;
    };

    vk::CommandBuffer cmdbuf{};
    CommandStream* stream{};
    CommandTarget target{CommandTarget::Primary};
};

} // namespace Vulkan
