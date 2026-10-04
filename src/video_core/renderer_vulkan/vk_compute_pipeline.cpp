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
    // Returns no pipeline if one can only come from the driver's cache and isn't there.
    const auto create = [this, module,
                         debug_str](vk::PipelineCreateFlags flags) -> vk::UniquePipeline {
        const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
            .requiredSubgroupSize = 64,
        };
        const vk::ComputePipelineCreateInfo compute_pipeline_ci = {
            .flags = flags,
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
        if (pipeline_result == vk::Result::ePipelineCompileRequired) {
            return vk::UniquePipeline{};
        }
        ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create compute pipeline: {}",
                   vk::to_string(pipeline_result));
        const bool optimized = !(flags & vk::PipelineCreateFlagBits::eDisableOptimization);
        LogPipelineCreation(optimized ? "compute" : "unoptimized compute", debug_str, start);
        SetObjectName(this->instance.GetDevice(), *pipe, "Compute Pipeline {}", debug_str);
        return std::move(pipe);
    };

    if (compiler && preloading) {
        compile_job = compiler->Submit([this, create] { pipeline = create({}); });
        return;
    }
    // A dispatch is waiting on a pipeline met in game, and compute work can't be skipped. One the
    // driver built before comes from its disk cache at once.
    const bool cache_control = compiler && this->instance.IsPipelineCreationCacheControlSupported();
    if (cache_control) {
        pipeline = create(vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired);
        if (pipeline) {
            return;
        }
    }
    if (compiler && scheduler.RecordsOnThread()) {
        // A new one took 60-190 ms to build in inFAMOUS Second Son, and a new effect brings a few
        // dozen, which froze the game for up to 2.8 s built one after another on the GPU thread.
        // Its commands are recorded into Vulkan on a thread of its own, which binds the pipeline
        // only when it gets to the dispatch, so a compiler thread builds it meanwhile, and the
        // GPU thread goes on to find the next ones to build at the same time.
        compile_job = compiler->Submit([this, create] { pipeline = create({}); }, true);
    } else if (cache_control) {
        // Otherwise one built without optimizations, which is quicker on some drivers, is used
        // now, and the optimized one replaces it once a compiler thread has it.
        optimize_job = compiler->Submit([this, create] { optimized_pipeline = create({}); });
        pipeline = create(vk::PipelineCreateFlagBits::eDisableOptimization);
    } else {
        pipeline = create({});
    }
}

ComputePipeline::~ComputePipeline() {
    CancelCompile();
}

} // namespace Vulkan
