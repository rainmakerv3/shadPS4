// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Gr2Photo::JpegDec {

// Layouts as Gravity Rush 2 fills and reads them.

enum OrbisJpegDecColorSpace : u16 {
    ORBIS_JPEG_DEC_COLOR_SPACE_UNKNOWN = 0,
    ORBIS_JPEG_DEC_COLOR_SPACE_YCBCR = 1,
    ORBIS_JPEG_DEC_COLOR_SPACE_GRAYSCALE = 2,
};

enum OrbisJpegDecSamplingType : u16 {
    ORBIS_JPEG_DEC_SAMPLING_TYPE_444 = 0,
    ORBIS_JPEG_DEC_SAMPLING_TYPE_422 = 1,
    ORBIS_JPEG_DEC_SAMPLING_TYPE_420 = 2,
};

struct OrbisJpegDecHandleInternal {
    OrbisJpegDecHandleInternal* handle;
    u32 handle_size;
    u32 max_width;
    u32 reserved;
};
static_assert(sizeof(OrbisJpegDecHandleInternal) == 0x18);

using OrbisJpegDecHandle = OrbisJpegDecHandleInternal*;

struct OrbisJpegDecCreateParam {
    u32 size;
    u32 attr;
    u32 max_width;
};
static_assert(sizeof(OrbisJpegDecCreateParam) == 0xC);

struct OrbisJpegDecParseParam {
    const void* jpeg;
    u32 jpeg_size;
    u16 unk3;
    u16 unk4;
};
static_assert(sizeof(OrbisJpegDecParseParam) == 0x10);

struct OrbisJpegDecDecodeParam {
    const void* jpeg;
    void* image;
    void* coef_buffer;
    u32 jpeg_size;
    u32 image_size;
    u32 coef_buffer_size;
    u16 output_mode;
    u16 color_space;
    u16 down_scale;
    u16 alpha;
    u32 image_pitch;
};
static_assert(sizeof(OrbisJpegDecDecodeParam) == 0x30);

struct OrbisJpegDecImageInfo {
    u32 img_width;
    u32 img_height;
    u16 color_space;
    u16 sampling_type;
    u32 num_components;
    u32 output_format;
    u32 coef_buffer_size;
    u32 reserved;
    u32 out_img_width;
    u32 out_img_height;
};
static_assert(sizeof(OrbisJpegDecImageInfo) == 0x24);

void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Gr2Photo::JpegDec
