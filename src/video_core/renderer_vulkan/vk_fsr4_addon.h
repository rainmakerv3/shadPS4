// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <deque>
#include <memory>
#include <string>
#include <utility>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_dlss_ngx.h"
#include "video_core/renderer_vulkan/vk_fsr_upscaler.h"

namespace Vulkan {

class Instance;
class Scheduler;

// The optional FSR 4 add-on: fsr4/shadps4_fsr4.dll next to the executable, with the FSR 4 v07
// model files beside it. It needs INT8 dot products and compute shader derivatives, which the
// device enables only when the add-on is present.
class Fsr4Addon {
public:
    // True when fsr4/shadps4_fsr4.dll is present and FSR 4 is not disabled.
    static bool Present();
    // Null when the add-on cannot be loaded or the GPU lacks the features.
    static std::unique_ptr<Fsr4Addon> Create(const Instance& instance, Scheduler& scheduler);
    ~Fsr4Addon();

    struct ContextDesc {
        vk::Extent2D render, output;
    };
    bool HasContext(const ContextDesc& desc) const;
    // The caller must have drained the GPU if a context exists.
    bool CreateContext(const ContextDesc& desc);
    void ReleaseContextAfterGpuDrain();

    // Inputs in SHADER_READ_ONLY_OPTIMAL, output in GENERAL; same conventions as FSR 3.1.
    bool Evaluate(vk::CommandBuffer command, const DlssNgx::Resource& color,
                  const DlssNgx::Resource& depth, const DlssNgx::Resource& motion,
                  const DlssNgx::Resource& output, const DlssNgx::EvalDesc& eval,
                  const FsrUpscaler::Camera& camera);

private:
    Fsr4Addon(Scheduler& scheduler);
    void RetireFinished();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
