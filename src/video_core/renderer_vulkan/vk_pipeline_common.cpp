// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <boost/container/static_vector.hpp>

#include "common/perf_profiler.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache_,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      pipeline_cache{pipeline_cache_}, is_compute{is_compute_} {}

Pipeline::~Pipeline() {
    CancelCompile();
}

void Pipeline::CancelCompile() noexcept {
    if (compile_job) {
        compile_job->Cancel();
    }
    if (optimize_job) {
        optimize_job->Cancel();
    }
}

void Pipeline::SwapInOptimized() const {
    optimize_job.reset();
    if (!optimized_pipeline) {
        return;
    }
    // Commands recorded so far may still use the old one.
    scheduler.DeferOperation([old = std::move(pipeline)]() mutable { old.reset(); });
    pipeline = std::move(optimized_pipeline);
}

void Pipeline::WaitReady() const {
    if (!compile_job || compile_job->IsDone()) {
        return;
    }
    Common::Perf::ScopedStall stall{Common::Perf::Stall::PipelineWait};
    compile_job->Wait();
}

void Pipeline::LogPipelineCreation(std::string_view kind, std::string_view debug_str,
                                   std::chrono::steady_clock::time_point start) const {
    // Creation this slow is worth seeing in the log, the rest only clutters it.
    constexpr auto SlowCreation = std::chrono::milliseconds{5};

    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (!PipelineCompiler::IsCompilerThread()) {
        Common::Perf::Record(Common::Perf::Stall::PipelineCreate,
                             std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }
    const double elapsed_ms = std::chrono::duration<double, std::milli>(elapsed).count();
    if (elapsed >= SlowCreation && !preloaded) {
        LOG_INFO(Render_Vulkan, "Created {} pipeline {} in {:.1f} ms", kind, debug_str, elapsed_ms);
    } else {
        LOG_DEBUG(Render_Vulkan, "Created {} pipeline {} in {:.1f} ms", kind, debug_str,
                  elapsed_ms);
    }
}

bool Pipeline::RetryPipelineCreation(vk::Result result, u32 attempt, std::string_view kind,
                                     std::string_view debug_str) {
    // With the driver's cache cold, as after an update, the compiler threads build hundreds of
    // pipelines at once while the game loads, and the driver failed one of them now and then with
    // an unknown error, which ended the game. Building it again a little later can succeed.
    constexpr u32 MaxAttempts = 5;
    const bool may_recover = result == vk::Result::eErrorUnknown ||
                             result == vk::Result::eErrorOutOfHostMemory ||
                             result == vk::Result::eErrorOutOfDeviceMemory ||
                             result == vk::Result::eErrorInitializationFailed;
    if (!may_recover || attempt >= MaxAttempts) {
        return false;
    }
    LOG_WARNING(Render_Vulkan, "Creating {} pipeline {} failed with {}, trying again", kind,
                debug_str, vk::to_string(result));
    std::this_thread::sleep_for(std::chrono::milliseconds{100} * attempt);
    return true;
}

void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    const auto cmdbuf = scheduler.CommandBuffer();
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;

    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    scheduler.GetDynamicState().PushConstants(cmdbuf, *pipeline_layout, IsCompute(), stage_flags,
                                              &push_data, sizeof(push_data));

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        cmdbuf.pushDescriptorSetKHR(bind_point, *pipeline_layout, 0, set_writes);
        return;
    }

    const auto desc_set = desc_heap.Commit(*desc_layout);
    cmdbuf.updateAndBindDescriptorSet(instance.GetDevice(), bind_point, *pipeline_layout, desc_set,
                                      set_writes);
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
