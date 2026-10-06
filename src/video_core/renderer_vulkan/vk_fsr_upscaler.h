// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_dlss_ngx.h"

namespace Vulkan {

class Instance;

// AMD FSR 3.1 upscaling through amd_fidelityfx_vk.dll next to the executable. Works on any GPU;
// without the DLL rendering stays unchanged. Callers own synchronization and must drain recorded
// work before destroying a context.
class FsrUpscaler {
public:
    // True when amd_fidelityfx_vk.dll is present and FSR is not disabled.
    static bool Present();
    // Null when the DLL cannot be loaded.
    static std::unique_ptr<FsrUpscaler> Create(const Instance& instance);
    // The vkGetDeviceProcAddr the FidelityFX backend gets (PFN_vkGetDeviceProcAddr), with
    // stand-ins for functions it calls that the device may lack.
    static void* FfxDeviceProcAddr();
    ~FsrUpscaler();

    struct ContextDesc {
        vk::Extent2D render, output;
        bool depth_inverted;
        bool hdr;           // linear HDR colour instead of sRGB-encoded LDR
        bool auto_exposure; // with hdr: exposure computed by FSR instead of 1
        bool operator==(const ContextDesc&) const = default;
    };
    bool HasContext(const ContextDesc& desc) const;
    // Destroys an existing context first; the caller must have drained the GPU if one exists.
    bool CreateContext(const ContextDesc& desc);
    void DestroyContextAfterGpuDrain();

    struct Camera {
        float near_plane, far_plane, fov_y; // view-space units and radians
    };
    // Same conventions as DLSS: jitter and motion vectors in render pixels, motion vectors from
    // the current to the previous frame.
    bool Evaluate(vk::CommandBuffer command, const DlssNgx::Resource& color,
                  const DlssNgx::Resource& depth, const DlssNgx::Resource& motion,
                  const DlssNgx::Resource& output, const DlssNgx::EvalDesc& eval,
                  const Camera& camera);

private:
    FsrUpscaler();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Vulkan
