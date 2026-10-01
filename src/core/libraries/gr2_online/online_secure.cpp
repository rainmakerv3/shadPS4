// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/types.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/gr2_online/online_secure.h"
#include "core/libraries/libs.h"

namespace Libraries::Gr2Online::Secure {

// The game encrypts the body of a request before it sends it and decrypts an answer after it
// read it, both in place and both as (context, pointer to {data, size}, context). The buffer
// stays as it is, so the request leaves in plain text and a plain answer is read as it came.
// The other functions of the library are left to the module the game ships.
s32 PS4_SYSV_ABI sceLibSecureCryptographyEncrypt(void* ctx_a, void* buf, void* ctx_b) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceLibSecureCryptographyDecrypt(void* ctx_a, void* buf, void* ctx_b) {
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("aEoi0u2FOiQ", "libSceSecure", 1, "libSceSecure", sceLibSecureCryptographyEncrypt);
    LIB_FUNCTION("hMYgMP-Vuno", "libSceSecure", 1, "libSceSecure", sceLibSecureCryptographyDecrypt);
}

} // namespace Libraries::Gr2Online::Secure
