// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include "core/libraries/error_codes.h"
#include "core/libraries/gr2_online/online_np_auth.h"
#include "core/libraries/libs.h"
#include "core/libraries/np/np_auth.h"

// The emulator's library defines these and keeps the requests; its header does not declare them.
namespace Libraries::Np::NpAuth {
s32 PS4_SYSV_ABI
sceNpAuthGetAuthorizationCode(s32 req_id, const OrbisNpAuthGetAuthorizationCodeParameter* param,
                              OrbisNpAuthorizationCode* auth_code, s32* issuer_id);
s32 PS4_SYSV_ABI
sceNpAuthGetAuthorizationCodeA(s32 req_id, const OrbisNpAuthGetAuthorizationCodeParameterA* param,
                               OrbisNpAuthorizationCode* auth_code, s32* issuer_id);
s32 PS4_SYSV_ABI
sceNpAuthGetAuthorizationCodeV3(s32 req_id, const OrbisNpAuthGetAuthorizationCodeParameterA* param,
                                OrbisNpAuthorizationCode* auth_code, s32* issuer_id);
} // namespace Libraries::Np::NpAuth

namespace Libraries::Gr2Online::NpAuth {

using Np::OrbisNpAuthorizationCode;
using Np::NpAuth::OrbisNpAuthGetAuthorizationCodeParameter;
using Np::NpAuth::OrbisNpAuthGetAuthorizationCodeParameterA;

namespace {

// The server never checks the code. With this one the game's toolkit goes on to the login
// request; with the emulator's six-character placeholder it stops before it.
constexpr char Code[] = "v3.shadps4_gr2_dummy_auth_code_AAAAAAAAAAAAAAAA";

// The emulator's library writes a code only when it hands one out.
s32 WithCode(s32 result, OrbisNpAuthorizationCode* auth_code) {
    if (result == ORBIS_OK && auth_code && auth_code->code[0] != '\0') {
        std::memset(auth_code, 0, sizeof(*auth_code));
        std::strncpy(auth_code->code, Code, sizeof(auth_code->code) - 1);
    }
    return result;
}

} // Anonymous namespace

s32 PS4_SYSV_ABI
sceNpAuthGetAuthorizationCode(s32 req_id, const OrbisNpAuthGetAuthorizationCodeParameter* param,
                              OrbisNpAuthorizationCode* auth_code, s32* issuer_id) {
    return WithCode(Np::NpAuth::sceNpAuthGetAuthorizationCode(req_id, param, auth_code, issuer_id),
                    auth_code);
}

s32 PS4_SYSV_ABI
sceNpAuthGetAuthorizationCodeA(s32 req_id, const OrbisNpAuthGetAuthorizationCodeParameterA* param,
                               OrbisNpAuthorizationCode* auth_code, s32* issuer_id) {
    return WithCode(Np::NpAuth::sceNpAuthGetAuthorizationCodeA(req_id, param, auth_code, issuer_id),
                    auth_code);
}

s32 PS4_SYSV_ABI
sceNpAuthGetAuthorizationCodeV3(s32 req_id, const OrbisNpAuthGetAuthorizationCodeParameterA* param,
                                OrbisNpAuthorizationCode* auth_code, s32* issuer_id) {
    return WithCode(
        Np::NpAuth::sceNpAuthGetAuthorizationCodeV3(req_id, param, auth_code, issuer_id),
        auth_code);
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("KxGkOrQJTqY", "libSceNpAuth", 1, "libSceNpAuth", sceNpAuthGetAuthorizationCode);
    LIB_FUNCTION("qAUXQ9GdWp8", "libSceNpAuth", 1, "libSceNpAuth", sceNpAuthGetAuthorizationCodeA);
    LIB_FUNCTION("KI4dHLlTNl0", "libSceNpAuth", 1, "libSceNpAuth", sceNpAuthGetAuthorizationCodeV3);
    LIB_FUNCTION("KxGkOrQJTqY", "libSceNpAuthCompat", 1, "libSceNpAuth",
                 sceNpAuthGetAuthorizationCode);
}

} // namespace Libraries::Gr2Online::NpAuth
