// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "core/libraries/content_export/content_export.h"
#include "core/libraries/content_export/content_export_error.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/gr2_photo/photo_content.h"
#include "core/libraries/gr2_photo/photo_gallery.h"
#include "core/libraries/libs.h"

// What Gravity Rush 2 calls to save a photo and to list, show and delete the album. The game
// hands the JPEG to the export library, asks the search library for the row of what it exported
// and later for the whole album, and opens the paths in those rows itself. The firmware modules
// do this through a system service and its database; here the album is a host folder, and the
// photo is already in it when the export call comes, written by the encoder HLE.
namespace Libraries::Gr2Photo::Content {

using ContentExport::OrbisContentExportCallbackParam;
using ContentExport::OrbisContentExportDataProvideFunction;
using ContentExport::OrbisContentExportParam;

// One row of sceContentSearchSearchContent, as the game reads it.
constexpr u64 RowSize = 0x960;
constexpr u64 RowHandle = 0x000;    // u64
constexpr u64 RowType = 0x00C;      // u32
constexpr u64 RowPath = 0x018;      // char[0x401]
constexpr u64 RowTitle = 0x424;     // char[0x101]
constexpr u64 RowThumbnail = 0x52B; // char[0x401]

struct OrbisContentSearchMetadataValue {
    u32 size;
    u32 type;
    char* buffer;
};

// The exported photo is the one the encoder just saved: its id goes back to the game, which
// looks it up in the album right away.
static s32 ExportPending(char* out, u64 out_size) {
    const std::string id = Gallery::TakePending();
    if (id.empty()) {
        LOG_WARNING(Lib_ContentExport, "No photo to export");
        return ORBIS_CONTENT_EXPORT_ERROR_NOTACCEPT;
    }
    Gallery::Add(id);
    if (out && out_size != 0) {
        std::snprintf(out, out_size, "%s", id.c_str());
    }
    LOG_INFO(Lib_ContentExport, "Exported photo {}", id);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentExportInit(void* init_param) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentExportStart() {
    // The return value is the export handle.
    return 0;
}

s32 PS4_SYSV_ABI sceContentExportFromData(s32 handle, const OrbisContentExportParam* param,
                                          u64 content_length,
                                          OrbisContentExportDataProvideFunction func,
                                          void* userdata, char* out, u64 out_size) {
    return ExportPending(out, out_size);
}

s32 PS4_SYSV_ABI sceContentExportFromDataWithThumbnail(
    s32 handle, const OrbisContentExportParam* param,
    const OrbisContentExportCallbackParam* content, const char* thumbnail_type,
    const OrbisContentExportCallbackParam* thumbnail, char* out, u64 out_size) {
    return ExportPending(out, out_size);
}

s32 PS4_SYSV_ABI sceContentExportFinish(s32 handle) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentExportTerm() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentSearchInit(void* init_param) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentSearchTerm() {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentSearchGetMyApplicationIndex(s32* index) {
    if (index) {
        *index = 0;
    }
    return ORBIS_OK;
}

// The two update counters tell the game whether the album changed between its count and its
// search: it starts over when they differ. The album cannot change inside one query here.
s32 PS4_SYSV_ABI sceContentSearchGetNumOfContent(const void* conditions, u32 num_conditions,
                                                 u32* num, u64* update_counter) {
    if (num) {
        *num = Gallery::Count();
    }
    if (update_counter) {
        *update_counter = 0;
    }
    return ORBIS_OK;
}

// The conditions and the sort key are not read: every query gets the album in name order.
s32 PS4_SYSV_ABI sceContentSearchSearchContent(const void* conditions, u32 num_conditions,
                                               const void* sort, u32 num_sort, u32 offset,
                                               u32 limit, u64* hits, u8* rows,
                                               u64* update_counter) {
    if (!hits || !rows) {
        return ORBIS_FAIL;
    }
    const auto photos = Gallery::List(offset, limit);
    // The game adds the hit count to its offset until it has every row, so a page with no rows
    // must not look like a success.
    if (photos.empty()) {
        return ORBIS_FAIL;
    }
    std::memset(rows, 0, photos.size() * RowSize);
    for (u64 i = 0; i < photos.size(); ++i) {
        u8* row = rows + i * RowSize;
        // Absolute, or the game looks for the file in its own archive.
        const std::string path = fmt::format("{}/{}.jpg", Gallery::GuestDir, photos[i].id);
        const u32 type = 1;
        std::memcpy(row + RowHandle, &photos[i].handle, sizeof(u64));
        std::memcpy(row + RowType, &type, sizeof(u32));
        std::snprintf(reinterpret_cast<char*>(row + RowPath), 0x401, "%s", path.c_str());
        std::snprintf(reinterpret_cast<char*>(row + RowTitle), 0x101, "%s", photos[i].id.c_str());
        std::snprintf(reinterpret_cast<char*>(row + RowThumbnail), 0x401, "%s", path.c_str());
    }
    *hits = photos.size();
    if (update_counter) {
        *update_counter = 0;
    }
    return ORBIS_OK;
}

// Photos carry no metadata, so a comment always reads back empty.
s32 PS4_SYSV_ABI sceContentSearchOpenMetadataByContentId(u64 content_id, u32* handle) {
    LOG_INFO(Lib_ContentExport, "No metadata is kept for photo {:#x}", content_id);
    if (handle) {
        *handle = 0;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentSearchGetMetadataValue(u32 handle, const char* key,
                                                  OrbisContentSearchMetadataValue* value) {
    if (!value || !value->buffer || value->size == 0) {
        return ORBIS_FAIL;
    }
    std::memset(value->buffer, 0, std::min<u32>(value->size, 0x101));
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentSearchCloseMetadata(u32 handle) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentDeleteInitialize(void* init_param) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentDeleteById(u64 content_id) {
    if (!Gallery::Delete(content_id)) {
        LOG_WARNING(Lib_ContentExport, "Unknown content id {:#x}", content_id);
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceContentDeleteTerminate() {
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("FzEWeYnAFlI", "libSceContentExport", 1, "libSceContentExport",
                 sceContentExportInit);
    LIB_FUNCTION("FCygF4Ec4so", "libSceContentExport", 1, "libSceContentExport",
                 sceContentExportStart);
    LIB_FUNCTION("AOWqIYsgVHs", "libSceContentExport", 1, "libSceContentExport",
                 sceContentExportFromData);
    LIB_FUNCTION("uZTQHI50WpY", "libSceContentExport", 1, "libSceContentExport",
                 sceContentExportFromDataWithThumbnail);
    LIB_FUNCTION("tb3cZTCl8Ps", "libSceContentExport", 1, "libSceContentExport",
                 sceContentExportFinish);
    LIB_FUNCTION("+KDWny9Y-6k", "libSceContentExport", 1, "libSceContentExport",
                 sceContentExportTerm);

    LIB_FUNCTION("dPj4ZtRcIWk", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchInit);
    LIB_FUNCTION("1xSZodB2geA", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchTerm);
    LIB_FUNCTION("FRT4EYtZU1Y", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchGetMyApplicationIndex);
    LIB_FUNCTION("o-RBPV0qr8c", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchGetNumOfContent);
    LIB_FUNCTION("TEW3IKxYfXc", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchSearchContent);
    LIB_FUNCTION("bjAlYWwRTJA", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchOpenMetadataByContentId);
    LIB_FUNCTION("ruNe-FgCzO8", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchGetMetadataValue);
    LIB_FUNCTION("-YbpaF0XS-I", "libSceContentSearch", 1, "libSceContentSearch",
                 sceContentSearchCloseMetadata);

    LIB_FUNCTION("zoxb0wEChEM", "libSceContentDelete", 1, "libSceContentDelete",
                 sceContentDeleteInitialize);
    LIB_FUNCTION("pXJh3aVk8Ks", "libSceContentDelete", 1, "libSceContentDelete",
                 sceContentDeleteById);
    LIB_FUNCTION("5XLSih32qHA", "libSceContentDelete", 1, "libSceContentDelete",
                 sceContentDeleteTerminate);
}

} // namespace Libraries::Gr2Photo::Content
