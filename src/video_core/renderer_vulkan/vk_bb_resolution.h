// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

inline vk::Extent2D BbUpscaleOutput(vk::Extent2D render, vk::Extent2D target,
                                    bool explicit_output = false) {
    if (!render.width || !render.height || !target.width || !target.height)
        return render;
    const double scale =
        std::min(double(target.width) / render.width, double(target.height) / render.height);
    if (!explicit_output && scale <= 1.0)
        return render;
    const double upscale = explicit_output ? scale : std::min(scale, 3.0);
    return {std::min(target.width, std::max(1u, uint32_t(std::lround(render.width * upscale)))),
            std::min(target.height, std::max(1u, uint32_t(std::lround(render.height * upscale))))};
}

} // namespace Vulkan
