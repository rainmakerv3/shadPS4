// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <string_view>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "core/libraries/gr2_photo/gr2_photo.h"
#include "core/libraries/gr2_photo/photo_content.h"
#include "core/libraries/gr2_photo/photo_gallery.h"
#include "core/libraries/gr2_photo/photo_jpegdec.h"
#include "core/libraries/gr2_photo/photo_jpegenc.h"
#include "video_core/texture_cache/photo_readback.h"

namespace Libraries::Gr2Photo {

constexpr std::array<std::string_view, 7> Serials = {
    "CUSA03694", "CUSA04943", "CUSA04934", "CUSA00547", "PCJS50010", "PCAS00079", "CUSA04935",
};

// The game renders a photo into a target of this size before it encodes it.
constexpr u32 PhotoSize = 1024;

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    if (!std::ranges::contains(Serials, Common::ElfInfo::Instance().GameSerial())) {
        return;
    }
    // The linker asks the HLE table before the exports of a loaded module and takes the first
    // match, so registering here, ahead of every other library, decides what the game calls:
    // firmware modules in sys_modules may still load, and nothing binds to them.
    LOG_INFO(Lib_Jpeg, "Gravity Rush 2: photo mode runs on HLE");
    VideoCore::PhotoReadback::Arm(PhotoSize, PhotoSize);
    Gallery::Setup();
    JpegEnc::RegisterLib(sym);
    JpegDec::RegisterLib(sym);
    Content::RegisterLib(sym);
}

} // namespace Libraries::Gr2Photo
