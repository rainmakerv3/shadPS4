// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/enum.h"
#include "common/types.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::PngDec {

enum class OrbisPngDecColorSpace : u16 {
    Grayscale = 2,
    Rgb,
    Clut,
    GrayscaleAlpha = 18,
    Rgba,
};

enum class OrbisPngDecImageFlag : u32 {
    None = 0,
    Adam7Interlace = 1,
    TrnsChunkExist = 2,
};
DECLARE_ENUM_FLAG_OPERATORS(OrbisPngDecImageFlag)

enum class OrbisPngDecPixelFormat : u16 {
    R8G8B8A8 = 0,
    B8G8R8A8,
};

enum class OrbisPngDecAttribute {
    None = 0,
    BitDepth16,
};

struct OrbisPngDecParseParam {
    const u8* png_mem_addr;
    u32 png_mem_size;
    u32 reserved;
};

struct OrbisPngDecImageInfo {
    u32 image_width;
    u32 image_height;
    OrbisPngDecColorSpace color_space;
    u16 bit_depth;
    OrbisPngDecImageFlag image_flag;
};

struct OrbisPngDecCreateParam {
    u32 this_size;
    u32 attribute;
    u32 max_image_width;
};

struct OrbisPngEncCreateParam {
    uint32_t cbSize;
    uint32_t attribute;
    uint32_t maxImageWidth;
    uint32_t maxFilterNumber;
};

struct OrbisPngEncEncodeParam {
    const void* imageAddr;
    void* pngAddr;
    uint32_t imageSize;
    uint32_t pngSize;
    uint32_t imageWidth;
    uint32_t imageHeight;
    uint32_t imagePitch;
    uint16_t pixelFormat;
    uint16_t colorSpace;
    uint16_t bitDepth;
    uint16_t clutNumber;
    uint16_t filterType;
    uint16_t compressionLevel;
};

struct OrbisPngEncImageInfo {
    uint32_t dataSize;
    uint32_t processedHeight;
};

using OrbisPngDecHandle = void*;
using OrbisPngEncHandle = void*;

struct OrbisPngDecDecodeParam {
    const u8* png_mem_addr;
    u8* image_mem_addr;
    u32 png_mem_size;
    u32 image_mem_size;
    OrbisPngDecPixelFormat pixel_format;
    u16 alpha_value;
    u32 image_pitch;
};

s32 PS4_SYSV_ABI scePngDecCreate(const OrbisPngDecCreateParam* param, void* memoryAddress,
                                 u32 memorySize, OrbisPngDecHandle* handle);
s32 PS4_SYSV_ABI scePngDecDecode(OrbisPngDecHandle handle, const OrbisPngDecDecodeParam* param,
                                 OrbisPngDecImageInfo* imageInfo);
s32 PS4_SYSV_ABI scePngDecDecodeWithInputControl();
s32 PS4_SYSV_ABI scePngDecDelete(OrbisPngDecHandle handle);
s32 PS4_SYSV_ABI scePngDecParseHeader(const OrbisPngDecParseParam* param,
                                      OrbisPngDecImageInfo* imageInfo);
s32 PS4_SYSV_ABI scePngDecQueryMemorySize(const OrbisPngDecCreateParam* param);

s32 PS4_SYSV_ABI scePngEncCreate(const OrbisPngEncCreateParam* param, void* memoryAddress,
                                 uint32_t size, OrbisPngEncHandle* handle);
s32 PS4_SYSV_ABI scePngEncEncode(OrbisPngEncHandle handle, const OrbisPngEncEncodeParam* param,
                                 OrbisPngEncImageInfo* imageInfo);
s32 PS4_SYSV_ABI scePngEncDelete(OrbisPngEncHandle handle);
s32 PS4_SYSV_ABI scePngEncQueryMemorySize(const OrbisPngEncCreateParam* param);

void RegisterlibScePngDec(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::PngDec
