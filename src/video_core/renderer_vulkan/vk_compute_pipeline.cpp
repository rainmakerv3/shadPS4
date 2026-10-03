// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>

#include <boost/container/small_vector.hpp>

#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

ComputePipeline::ComputePipeline(const Instance& instance, Scheduler& scheduler,
                                 DescriptorHeap& desc_heap, const Shader::Profile& profile,
                                 vk::PipelineCache pipeline_cache, ComputePipelineKey compute_key_,
                                 const Shader::Info& info_, vk::ShaderModule module,
                                 SerializationSupport& sdata, bool preloading,
                                 PipelineCompiler* compiler)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache, true},
      compute_key{compute_key_} {
    auto& info = stages[int(Shader::SwStage::Compute)];
    info = &info_;
    preloaded = preloading;
    const auto debug_str = GetDebugString();

    u32 binding{};
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    for (const auto& buffer : info->buffers) {
        // During deserialization, we don't have access to the UD to fetch sharp data. To address
        // this properly we need to track shaprs or portion of them in `sdata`, but since we're
        // interested only in "is storage" flag (which is not even effective atm), we can take a
        // shortcut there.
        const auto sharp = preloading ? AmdGpu::Buffer{} : buffer.GetSharp(*info);
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }
    for (const auto& image : info->images) {
        const u32 num_bindings = image.NumBindings(*info);
        bindings.push_back({
            .binding = binding,
            .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                               : vk::DescriptorType::eSampledImage,
            .descriptorCount = num_bindings,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
        binding += num_bindings;
    }
    for (const auto& sampler : info->samplers) {
        bindings.push_back({
            .binding = binding++,
            .descriptorType = vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        });
    }

    const vk::PushConstantRange push_constants = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    const auto device = instance.GetDevice();
    auto [descriptor_set_result, descriptor_set] =
        device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(descriptor_set_result == vk::Result::eSuccess,
               "Failed to create compute descriptor set layout: {}",
               vk::to_string(descriptor_set_result));
    desc_layout = std::move(descriptor_set);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create compute pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Compute PipelineLayout {}", debug_str);

    // Runs on the compiler threads too, so it only reads what stays fixed after construction.
    const auto create = [this, module, debug_str](bool optimize) {
        const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
            .requiredSubgroupSize = 64,
        };
        const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
            .flags =
                optimize
                    ? vk::PipelineCreateFlags{}
                    : vk::PipelineCreateFlags{vk::PipelineCreateFlagBits::eDisableOptimization},
            .stage{
                .pNext = this->instance.IsSubgroupSize64Supported() ? &subgroup_size_ci : nullptr,
                .stage = vk::ShaderStageFlagBits::eCompute,
                .module = module,
                .pName = "main",
            },
            .layout = *pipeline_layout,
        };
        const auto start = std::chrono::steady_clock::now();
        auto [pipeline_result, pipe] = this->instance.GetDevice().createComputePipelineUnique(
            this->pipeline_cache, compute_pipeline_ci);
        ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
                   vk::to_string(pipeline_result));
        LogPipelineCreation(optimize ? "compute" : "unoptimized compute", debug_str, start);
        SetObjectName(this->instance.GetDevice(), *pipe, "Compute Pipeline {}", debug_str);
        return std::move(pipe);
    };

    if (compiler && preloading) {
        compile_job = compiler->Submit([this, create] { pipeline = create(true); });
    } else if (compiler) {
        // A dispatch is waiting on a pipeline met in game, and compute work can't be skipped.
        // The driver keeps optimized pipelines in its disk cache, so one it built before comes
        // quickly and is given a moment on a compiler thread. Building a new one took 64 ms at
        // the median and up to 400 in inFAMOUS Second Son, freezing fights as new effects showed
        // up, so then one is built without optimizations to use now, and the optimized one
        // replaces it once the compiler thread has it.
        static constexpr auto CachedPipelineWait = std::chrono::milliseconds{4};
        optimize_job = compiler->Submit([this, create] { optimized_pipeline = create(true); });
        if (optimize_job->WaitFor(CachedPipelineWait)) {
            optimize_job.reset();
            pipeline = std::move(optimized_pipeline);
        } else {
            pipeline = create(false);
        }
    } else {
        pipeline = create(true);
    }
}

ComputePipeline::~ComputePipeline() {
    CancelCompile();
}

} // namespace Vulkan
