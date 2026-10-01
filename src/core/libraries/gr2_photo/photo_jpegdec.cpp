// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <cstring>
#include "common/alignment.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/gr2_photo/photo_jpegdec.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/libraries/libs.h"

// A JPEG decoder private to this file: common/stb.cpp builds stb_image for PNG only.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include <stb_image.h>

namespace Libraries::Gr2Photo::JpegDec {

constexpr s32 MemorySize = 0x800;
constexpr size_t MaxJpegScan = 16_MB;

// A size of 0, or above 16 MiB, is taken as unknown: the stream then ends at its first
// end-of-image marker.
static size_t FindJpegSize(const u8* jpeg) {
    if (jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        return 0;
    }
    for (size_t i = 2; i + 1 < MaxJpegScan; ++i) {
        if (jpeg[i] == 0xFF && jpeg[i + 1] == 0xD9) {
            return i + 2;
        }
    }
    return 0;
}

static void FillImageInfo(OrbisJpegDecImageInfo* info, u32 width, u32 height, int components) {
    if (!info) {
        return;
    }
    std::memset(info, 0, sizeof(*info));
    info->img_width = width;
    info->img_height = height;
    info->out_img_width = width;
    info->out_img_height = height;
    if (components == 1) {
        info->color_space = ORBIS_JPEG_DEC_COLOR_SPACE_GRAYSCALE;
        info->sampling_type = ORBIS_JPEG_DEC_SAMPLING_TYPE_444;
        info->num_components = 1;
    } else {
        // stb does not report the subsampling; the encoder HLE writes 4:2:0.
        info->color_space = ORBIS_JPEG_DEC_COLOR_SPACE_YCBCR;
        info->sampling_type = ORBIS_JPEG_DEC_SAMPLING_TYPE_420;
        info->num_components = 3;
    }
}

s32 PS4_SYSV_ABI sceJpegDecQueryMemorySize(const OrbisJpegDecCreateParam* param) {
    return MemorySize;
}

s32 PS4_SYSV_ABI sceJpegDecCreate(const OrbisJpegDecCreateParam* param, void* memory,
                                  u32 memory_size, OrbisJpegDecHandle* handle) {
    if (!param || !memory || !handle) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    auto* internal = reinterpret_cast<OrbisJpegDecHandleInternal*>(
        Common::AlignUp(std::bit_cast<VAddr>(memory), 16));
    if (std::bit_cast<VAddr>(internal) + sizeof(*internal) >
        std::bit_cast<VAddr>(memory) + memory_size) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    internal->handle = internal;
    internal->handle_size = sizeof(*internal);
    internal->max_width = param->max_width;
    internal->reserved = 0;
    *handle = internal;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecDelete(OrbisJpegDecHandle handle) {
    return handle ? ORBIS_OK : ORBIS_KERNEL_ERROR_EINVAL;
}

s32 PS4_SYSV_ABI sceJpegDecParseHeader(const OrbisJpegDecParseParam* param,
                                       OrbisJpegDecImageInfo* info) {
    if (!param || !info || !param->jpeg) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    const auto* jpeg = static_cast<const u8*>(param->jpeg);
    size_t size = param->jpeg_size;
    if (size == 0 || size > MaxJpegScan) {
        size = FindJpegSize(jpeg);
    }
    int width = 0, height = 0, components = 0;
    if (size == 0 ||
        !stbi_info_from_memory(jpeg, static_cast<int>(size), &width, &height, &components)) {
        LOG_ERROR(Lib_Jpeg, "Not a JPEG stream, size = {}", param->jpeg_size);
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    FillImageInfo(info, width, height, components);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceJpegDecDecode(OrbisJpegDecHandle handle, const OrbisJpegDecDecodeParam* param,
                                  OrbisJpegDecImageInfo* info) {
    if (!handle || !param || !param->jpeg || !param->image) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    const auto* jpeg = static_cast<const u8*>(param->jpeg);
    size_t size = param->jpeg_size;
    if (size == 0 || size > MaxJpegScan) {
        size = FindJpegSize(jpeg);
    }
    int width = 0, height = 0, components = 0;
    stbi_uc* rgba = size == 0 ? nullptr
                              : stbi_load_from_memory(jpeg, static_cast<int>(size), &width, &height,
                                                      &components, 4);
    if (!rgba) {
        LOG_ERROR(Lib_Jpeg, "Decode failed, size = {}", param->jpeg_size);
        return ORBIS_KERNEL_ERROR_EINVAL;
    }

    // Rows go out as R, G, B, A bytes at the pitch of the texture the game decodes into.
    const u32 row_size = static_cast<u32>(width) * 4;
    u32 pitch = param->image_pitch;
    if (pitch < row_size || pitch > row_size + 0x10000 || pitch % 4 != 0) {
        pitch = row_size;
    }
    // An alpha of 0 asks for an opaque image.
    u8 alpha = static_cast<u8>(param->alpha <= 0xFF ? param->alpha : param->alpha >> 8);
    if (alpha == 0) {
        alpha = 0xFF;
    }
    auto* image = static_cast<u8*>(param->image);
    for (u32 y = 0; y < static_cast<u32>(height); ++y) {
        u8* row = image + u64{y} * pitch;
        std::memcpy(row, rgba + u64{y} * row_size, row_size);
        if (alpha != 0xFF) {
            for (u32 x = 0; x < static_cast<u32>(width); ++x) {
                row[x * 4 + 3] = alpha;
            }
        }
    }
    stbi_image_free(rgba);
    FillImageInfo(info, width, height, components);
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("uNAUmANZMEw", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecQueryMemorySize);
    LIB_FUNCTION("JPh3Zgg0Zwc", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecCreate);
    LIB_FUNCTION("Hwh11+m5KoI", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecDelete);
    LIB_FUNCTION("LSinoSQH790", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecParseHeader);
    LIB_FUNCTION("1kzQRoWEgSA", "libSceJpegDec", 1, "libSceJpegDec", sceJpegDecDecode);
}

} // namespace Libraries::Gr2Photo::JpegDec
