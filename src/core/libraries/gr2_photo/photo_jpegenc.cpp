// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <vector>
#include <stb_image_write.h>
#include "common/alignment.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/gr2_photo/photo_gallery.h"
#include "core/libraries/gr2_photo/photo_jpegenc.h"
#include "core/libraries/jpeg/jpeg_error.h"
#include "core/libraries/jpeg/jpegenc.h"
#include "core/libraries/libs.h"
#include "video_core/texture_cache/photo_readback.h"

namespace Libraries::Gr2Photo::JpegEnc {

using namespace Libraries::JpegEnc;

constexpr u32 ORBIS_JPEG_ENC_MAX_IMAGE_DIMENSION = 0xFFFF;
constexpr u32 ORBIS_JPEG_ENC_MAX_IMAGE_PITCH = 0xFFFFFFF;
constexpr u32 ORBIS_JPEG_ENC_MAX_IMAGE_SIZE = 0x7FFFFFFF;

// A photo that encodes to less than this is taken for an empty frame and is not kept.
constexpr size_t MinPhotoSize = 30000;

// The two validators are the ones of core/libraries/jpeg/jpegenc.cpp, which keeps them to itself.
static s32 ValidateJpegEncEncodeParam(const OrbisJpegEncEncodeParam* param) {

    // Validate addresses
    if (!param) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }
    if (!param->image || (param->pixel_format != ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8 &&
                          !Common::IsAligned(reinterpret_cast<VAddr>(param->image), 4))) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }
    if (!param->jpeg) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_ADDR;
    }

    // Validate sizes
    if (param->image_size == 0 || param->jpeg_size == 0) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_SIZE;
    }

    // Validate parameters
    if (param->image_width > ORBIS_JPEG_ENC_MAX_IMAGE_DIMENSION ||
        param->image_height > ORBIS_JPEG_ENC_MAX_IMAGE_DIMENSION) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->image_pitch == 0 || param->image_pitch > ORBIS_JPEG_ENC_MAX_IMAGE_PITCH ||
        (param->pixel_format != ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8 &&
         !Common::IsAligned(param->image_pitch, 4))) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    const auto calculated_size = param->image_height * param->image_pitch;
    if (calculated_size > ORBIS_JPEG_ENC_MAX_IMAGE_SIZE || calculated_size > param->image_size) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->encode_mode != ORBIS_JPEG_ENC_ENCODE_MODE_NORMAL &&
        param->encode_mode != ORBIS_JPEG_ENC_ENCODE_MODE_MJPEG) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_YCC &&
        param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_GRAYSCALE) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->sampling_type != ORBIS_JPEG_ENC_SAMPLING_TYPE_FULL &&
        param->sampling_type != ORBIS_JPEG_ENC_SAMPLING_TYPE_422 &&
        param->sampling_type != ORBIS_JPEG_ENC_SAMPLING_TYPE_420) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (param->restart_interval > ORBIS_JPEG_ENC_MAX_IMAGE_DIMENSION) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    switch (param->pixel_format) {
    case ORBIS_JPEG_ENC_PIXEL_FORMAT_R8G8B8A8:
    case ORBIS_JPEG_ENC_PIXEL_FORMAT_B8G8R8A8:
        if (param->image_pitch >> 2 < param->image_width ||
            param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_YCC ||
            param->sampling_type == ORBIS_JPEG_ENC_SAMPLING_TYPE_FULL) {
            return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
        }
        break;
    case ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8U8Y8V8:
        if (param->image_pitch >> 1 < Common::AlignUp(param->image_width, 2) ||
            param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_YCC ||
            param->sampling_type == ORBIS_JPEG_ENC_SAMPLING_TYPE_FULL) {
            return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
        }
        break;
    case ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8:
        if (param->image_pitch < param->image_width ||
            param->color_space != ORBIS_JPEG_ENC_COLOR_SPACE_GRAYSCALE ||
            param->sampling_type != ORBIS_JPEG_ENC_SAMPLING_TYPE_FULL) {
            return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
        }
        break;
    default:
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }

    return ORBIS_OK;
}

static s32 ValidateJpecEngHandle(OrbisJpegEncHandle handle) {
    if (!handle || !Common::IsAligned(reinterpret_cast<VAddr>(handle), 0x20) ||
        handle->handle != handle) {
        return ORBIS_JPEG_ENC_ERROR_INVALID_HANDLE;
    }
    return ORBIS_OK;
}

// The game hands over its own copy of the photo target, read from guest memory the GPU never
// wrote back. The pixels of the target itself take its place.
static void ReadPhotoTarget(const OrbisJpegEncEncodeParam* param) {
    VideoCore::PhotoReadback::Frame frame;
    if (!VideoCore::PhotoReadback::Download(frame)) {
        LOG_WARNING(Lib_Jpeg, "No photo target to read, encoding the guest buffer as it is");
        return;
    }
    // Rows of another length hold pixels of another size, which the encoder would misread.
    if (frame.pitch != param->image_pitch) {
        LOG_WARNING(Lib_Jpeg,
                    "Photo target rows are {} bytes, the encoder expects {}; encoding the guest "
                    "buffer as it is",
                    frame.pitch, param->image_pitch);
        return;
    }
    // The validated buffer holds image_height rows of image_pitch bytes.
    const u32 num_rows = std::min(frame.height, param->image_height);
    std::memcpy(param->image, frame.pixels.data(), u64{num_rows} * frame.pitch);
}

static u8 ClampToU8(int value) {
    return static_cast<u8>(std::clamp(value, 0, 255));
}

// Packs the image into the tight RGB or grey rows stb takes. Returns the component count.
static int PackImage(const OrbisJpegEncEncodeParam* param, std::vector<u8>& packed) {
    const u32 width = param->image_width;
    const u32 height = param->image_height;
    const auto* image = static_cast<const u8*>(param->image);
    if (param->pixel_format == ORBIS_JPEG_ENC_PIXEL_FORMAT_Y8) {
        packed.resize(u64{width} * height);
        for (u32 y = 0; y < height; ++y) {
            std::memcpy(packed.data() + u64{y} * width, image + u64{y} * param->image_pitch, width);
        }
        return 1;
    }
    packed.resize(u64{width} * height * 3);
    for (u32 y = 0; y < height; ++y) {
        const u8* row = image + u64{y} * param->image_pitch;
        u8* out = packed.data() + u64{y} * width * 3;
        switch (param->pixel_format) {
        case ORBIS_JPEG_ENC_PIXEL_FORMAT_R8G8B8A8:
            for (u32 x = 0; x < width; ++x) {
                out[x * 3 + 0] = row[x * 4 + 0];
                out[x * 3 + 1] = row[x * 4 + 1];
                out[x * 3 + 2] = row[x * 4 + 2];
            }
            break;
        case ORBIS_JPEG_ENC_PIXEL_FORMAT_B8G8R8A8:
            for (u32 x = 0; x < width; ++x) {
                out[x * 3 + 0] = row[x * 4 + 2];
                out[x * 3 + 1] = row[x * 4 + 1];
                out[x * 3 + 2] = row[x * 4 + 0];
            }
            break;
        default:
            // Y0 U Y1 V for each pair of pixels.
            for (u32 x = 0; x < width; ++x) {
                const u8* pair = row + (x & ~1u) * 2;
                const int c = pair[(x & 1) * 2] - 16;
                const int d = pair[1] - 128;
                const int e = pair[3] - 128;
                out[x * 3 + 0] = ClampToU8((298 * c + 409 * e + 128) >> 8);
                out[x * 3 + 1] = ClampToU8((298 * c - 100 * d - 208 * e + 128) >> 8);
                out[x * 3 + 2] = ClampToU8((298 * c + 516 * d + 128) >> 8);
            }
            break;
        }
    }
    return 3;
}

s32 PS4_SYSV_ABI sceJpegEncEncode(OrbisJpegEncHandle handle, const OrbisJpegEncEncodeParam* param,
                                  OrbisJpegEncOutputInfo* output_info) {
    if (auto handle_ret = ValidateJpecEngHandle(handle); handle_ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid handle");
        return handle_ret;
    }
    if (auto param_ret = ValidateJpegEncEncodeParam(param); param_ret != ORBIS_OK) {
        LOG_ERROR(Lib_Jpeg, "Invalid encode param");
        return param_ret;
    }

    const bool is_photo =
        VideoCore::PhotoReadback::IsArmed(param->image_width, param->image_height);
    if (is_photo) {
        ReadPhotoTarget(param);
    }

    std::vector<u8> packed;
    const int components = PackImage(param, packed);
    std::vector<u8> jpeg;
    const auto append = [](void* context, void* data, int size) {
        auto& out = *static_cast<std::vector<u8>*>(context);
        const auto* bytes = static_cast<const u8*>(data);
        out.insert(out.end(), bytes, bytes + size);
    };
    // The game asks for a compression ratio of 50; it is taken as the quality.
    const int quality = std::clamp<int>(param->compression_ratio, 1, 100);
    if (!stbi_write_jpg_to_func(append, &jpeg, param->image_width, param->image_height, components,
                                packed.data(), quality)) {
        LOG_ERROR(Lib_Jpeg, "Encoding {}x{} failed", param->image_width, param->image_height);
        return ORBIS_JPEG_ENC_ERROR_INVALID_PARAM;
    }
    if (jpeg.size() > param->jpeg_size) {
        LOG_ERROR(Lib_Jpeg, "Output buffer too small: need {} bytes, have {}", jpeg.size(),
                  param->jpeg_size);
        return ORBIS_JPEG_ENC_ERROR_INVALID_SIZE;
    }
    std::memcpy(param->jpeg, jpeg.data(), jpeg.size());
    if (output_info) {
        output_info->size = static_cast<u32>(jpeg.size());
        output_info->height = param->image_height;
    }

    if (is_photo) {
        if (jpeg.size() < MinPhotoSize) {
            LOG_WARNING(Lib_Jpeg, "Photo of {} bytes looks empty and is not saved", jpeg.size());
            Gallery::DropPending();
        } else {
            Gallery::Save(jpeg);
        }
    }

    // The firmware encoder returns the size of the stream, and the game takes anything that is
    // not positive as a failed photo.
    return static_cast<s32>(jpeg.size());
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("K+rocojkr-I", "libSceJpegEnc", 1, "libSceJpegEnc",
                 Libraries::JpegEnc::sceJpegEncCreate);
    LIB_FUNCTION("j1LyMdaM+C0", "libSceJpegEnc", 1, "libSceJpegEnc",
                 Libraries::JpegEnc::sceJpegEncDelete);
    LIB_FUNCTION("QbrU0cUghEM", "libSceJpegEnc", 1, "libSceJpegEnc", sceJpegEncEncode);
    LIB_FUNCTION("o6ZgXfFdWXQ", "libSceJpegEnc", 1, "libSceJpegEnc",
                 Libraries::JpegEnc::sceJpegEncQueryMemorySize);
}

} // namespace Libraries::Gr2Photo::JpegEnc
