// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: MIT
//
// C interface between shadPS4 and the optional FSR 4 add-on
// (fsr4/shadps4_fsr4.dll). The add-on runs the FSR 4 v07 INT8 model through
// FireBurn's FSR-Vulkan; its model files sit next to the DLL. shadPS4 loads it
// at runtime when present.

#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHADPS4_FSR4_ABI 2

typedef void (*ShadFsr4LogFn)(int warning, const char* message);

typedef struct ShadFsr4Image {
    VkImage image;
    VkImageView view;
    VkImageLayout layout; // current layout, restored after the dispatch
    uint32_t width;
    uint32_t height;
    uint32_t ffx_format; // FFX surface format of the view
} ShadFsr4Image;

typedef struct ShadFsr4Context {
    uint32_t render_width;
    uint32_t render_height;
    uint32_t output_width;
    uint32_t output_height;
} ShadFsr4Context;

typedef struct ShadFsr4Evaluate {
    ShadFsr4Image color;  // sampled
    ShadFsr4Image depth;  // sampled, hardware depth
    ShadFsr4Image motion; // sampled, render pixels, current to previous
    ShadFsr4Image output; // storage, general layout
    float jitter_x, jitter_y;
    float frame_ms;
    float camera_near, camera_far, fov_y;
    int32_t reset;
    int32_t hdr; // 0: LDR, 1: linear HDR with automatic exposure, 2: linear HDR, exposure 1
} ShadFsr4Evaluate;

enum {
    SHADPS4_FSR4_FAILED = 0,   // nothing recorded; the frame id was not used
    SHADPS4_FSR4_OK = 1,       // recorded; retire the frame id once the GPU finished it
    SHADPS4_FSR4_BUSY = 2,     // no free frame slot: retire finished frames and try again
    SHADPS4_FSR4_ABANDONED = 3 // begun but not recorded; still retire the frame id
};

typedef struct ShadFsr4Api {
    uint32_t abi;
    // directory holds the model files (UTF-16).
    int32_t (*Configure)(const wchar_t* directory, ShadFsr4LogFn log);
    int32_t (*Initialize)(VkPhysicalDevice physical, VkDevice device);
    int32_t (*HasContext)(const ShadFsr4Context* context);
    // The caller must have drained the GPU if a context exists.
    int32_t (*CreateContext)(const ShadFsr4Context* context);
    int32_t (*Evaluate)(VkCommandBuffer command, const ShadFsr4Evaluate* evaluate,
                        uint64_t frame_id);
    void (*Retire)(uint64_t completed_frame_id);
    // The caller must have drained the GPU.
    void (*ReleaseContext)(void);
    const char* (*Problem)(void);
} ShadFsr4Api;

typedef const ShadFsr4Api* (*ShadFsr4GetApiFn)(void);

#ifdef __cplusplus
}
#endif
