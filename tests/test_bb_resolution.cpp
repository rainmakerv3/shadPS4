// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/vk_bb_resolution.h"

namespace {
struct Case {
    vk::Extent2D render, target, expected;
};

constexpr std::array Cases{
    Case{{1920, 1080}, {3840, 2160}, {3840, 2160}}, Case{{1280, 720}, {1920, 1080}, {1920, 1080}},
    Case{{1280, 800}, {2560, 1600}, {2560, 1600}},  Case{{1920, 1200}, {2560, 1600}, {2560, 1600}},
    Case{{2560, 1080}, {3440, 1440}, {3413, 1440}}, Case{{3440, 1440}, {5120, 2160}, {5120, 2143}},
    Case{{3840, 1080}, {5120, 1440}, {5120, 1440}}, Case{{1920, 1080}, {3440, 1440}, {2560, 1440}},
    Case{{1920, 1080}, {1920, 1200}, {1920, 1080}}, Case{{1920, 1200}, {3840, 2160}, {3456, 2160}},
    Case{{3440, 1440}, {3840, 2160}, {3840, 1607}}, Case{{1440, 1080}, {3840, 2160}, {2880, 2160}},
    Case{{1001, 601}, {2561, 1601}, {2561, 1538}},
};
} // namespace

TEST(BbResolution, FitsPatchedAspectRatios) {
    for (const auto& test : Cases) {
        EXPECT_EQ(Vulkan::BbUpscaleOutput(test.render, test.target), test.expected);
        EXPECT_EQ(Vulkan::BbUpscaleOutput(test.render, test.target, true), test.expected);
    }
}

TEST(BbResolution, AutomaticOutputCapsUpscalingAtThreeTimes) {
    EXPECT_EQ(Vulkan::BbUpscaleOutput({640, 360}, {3840, 2160}), (vk::Extent2D{1920, 1080}));
    EXPECT_EQ(Vulkan::BbUpscaleOutput({1280, 800}, {7680, 4320}), (vk::Extent2D{3840, 2400}));
}

TEST(BbResolution, SmallerAutomaticWindowKeepsRenderSize) {
    EXPECT_EQ(Vulkan::BbUpscaleOutput({3440, 1440}, {1920, 1080}), (vk::Extent2D{3440, 1440}));
    EXPECT_EQ(Vulkan::BbUpscaleOutput({2560, 1600}, {1920, 1200}), (vk::Extent2D{2560, 1600}));
}

TEST(BbResolution, ExplicitOutputCanDownscaleOrExceedAutomaticLimit) {
    EXPECT_EQ(Vulkan::BbUpscaleOutput({3440, 1440}, {1920, 1080}, true), (vk::Extent2D{1920, 804}));
    EXPECT_EQ(Vulkan::BbUpscaleOutput({2560, 1600}, {1920, 1200}, true),
              (vk::Extent2D{1920, 1200}));
    EXPECT_EQ(Vulkan::BbUpscaleOutput({640, 360}, {3840, 2160}, true), (vk::Extent2D{3840, 2160}));
}

TEST(BbResolution, ZeroDimensionsDoNotDivideByZero) {
    EXPECT_EQ(Vulkan::BbUpscaleOutput({1920, 1080}, {0, 0}), (vk::Extent2D{1920, 1080}));
    EXPECT_EQ(Vulkan::BbUpscaleOutput({1920, 1080}, {2560, 0}, true), (vk::Extent2D{1920, 1080}));
    EXPECT_EQ(Vulkan::BbUpscaleOutput({0, 1080}, {2560, 1600}), (vk::Extent2D{0, 1080}));
}

TEST(BbResolution, OddSizesStayWithinBoundsAndPreserveAspectWithinOnePixel) {
    for (uint32_t width = 317; width < 2100; width += 113) {
        for (uint32_t height = 271; height < 1400; height += 97) {
            const vk::Extent2D input{width, height}, target{3441, 1441};
            const auto output = Vulkan::BbUpscaleOutput(input, target, true);
            EXPECT_LE(output.width, target.width);
            EXPECT_LE(output.height, target.height);
            EXPECT_GT(output.width, 0u);
            EXPECT_GT(output.height, 0u);
            const double width_error =
                std::abs(double(output.width) - double(output.height) * width / height);
            const double height_error =
                std::abs(double(output.height) - double(output.width) * height / width);
            EXPECT_TRUE(width_error <= 1.0 || height_error <= 1.0);
        }
    }
}
