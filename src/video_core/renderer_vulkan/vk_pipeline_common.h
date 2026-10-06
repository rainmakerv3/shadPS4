// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <memory>
#include <string_view>

#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_pipeline_compiler.h"

#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
struct PushData;
} // namespace Shader

namespace Vulkan {

static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Instance;
class Scheduler;
class DescriptorHeap;

class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
             bool is_compute = false);
    virtual ~Pipeline();

    /// Returns the pipeline to bind, waiting for it to finish compiling if needed.
    vk::Pipeline Handle() const {
        WaitReady();
        if (optimize_job && optimize_job->IsDone()) {
            SwapInOptimized();
        }
        return *pipeline;
    }

    /// Returns true once the pipeline can be bound without waiting for the compiler.
    [[nodiscard]] bool IsReady() const noexcept {
        return !compile_job || compile_job->IsDone();
    }

    /// Waits for the pipeline to finish compiling, compiling it on this thread if no compiler
    /// thread has started on it yet.
    void WaitReady() const;

    /// Returns true for pipelines loaded from the pipeline cache rather than met in game.
    [[nodiscard]] bool IsPreloaded() const noexcept {
        return preloaded;
    }

    vk::PipelineLayout GetLayout() const noexcept {
        return *pipeline_layout;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::SwStage::Compute) == Shader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::SwStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    bool IsCompute() const {
        return is_compute;
    }

    using DescriptorWrites = std::vector<vk::WriteDescriptorSet>;
    void BindResources(DescriptorWrites& set_writes, const Shader::PushData& push_data) const {
        BindResources(scheduler, set_writes, push_data);
    }

    /// Binds the resources for work recorded with the given scheduler, such as the one of the
    /// second queue.
    void BindResources(Scheduler& target, DescriptorWrites& set_writes,
                       const Shader::PushData& push_data) const;

protected:
    [[nodiscard]] std::string GetDebugString() const;

    /// Stops any compilation still referring to this pipeline. Called by the destructors, before
    /// the state a compile job reads goes away.
    void CancelCompile() noexcept;

    /// Logs a finished driver compile and accounts for it if it held up the thread.
    void LogPipelineCreation(std::string_view kind, std::string_view debug_str,
                             std::chrono::steady_clock::time_point start) const;

    /// Returns true if creating a pipeline should be tried again after it failed with the result
    /// on the attempt, counted from 1, having waited a little for the driver to recover.
    static bool RetryPipelineCreation(vk::Result result, u32 attempt, std::string_view kind,
                                      std::string_view debug_str);

    /// Replaces a pipeline built without optimizations with the optimized one built since.
    void SwapInOptimized() const;

    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    vk::PipelineCache pipeline_cache;
    /// Written by compile_job, so it may only be used once IsReady() returns true.
    mutable vk::UniquePipeline pipeline;
    std::shared_ptr<PipelineCompileJob> compile_job;
    /// Builds the optimized pipeline into optimized_pipeline, to replace one built without
    /// optimizations to be used at once.
    mutable std::shared_ptr<PipelineCompileJob> optimize_job;
    mutable vk::UniquePipeline optimized_pipeline;
    bool preloaded{};
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorSetLayout desc_layout;
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    bool uses_push_descriptors{};
    bool is_compute;
};

} // namespace Vulkan
