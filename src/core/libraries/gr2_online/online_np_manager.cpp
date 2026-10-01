// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/gr2_online/online_host.h"
#include "core/libraries/gr2_online/online_np_manager.h"
#include "core/libraries/libs.h"
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_manager.h"
#include "core/linker.h"

namespace Libraries::Gr2Online::NpManager {

using Np::OrbisNpId;
using Np::OrbisNpOnlineId;
using Np::NpManager::OrbisNpReachabilityState;
using Np::NpManager::OrbisNpReachabilityStateCallback;
using Np::NpManager::OrbisNpState;
using Np::NpManager::OrbisNpStateCallbackForNpToolkit;
using UserService::OrbisUserServiceUserId;

namespace {

// The toolkit of the game registers one state callback and the game one reachability callback,
// and both then wait to be told the state: they never ask for it. The emulator's own library
// hands a sign-in change to the callbacks that are registered when the pump next runs, and
// nothing while there is no change, so a callback that registers after that pump, or while the
// user stays signed out, is never told. Here a callback is owed the current state from the
// moment it registers, and again after every change.
std::mutex mutex;
OrbisNpStateCallbackForNpToolkit state_cb{};
void* state_userdata{};
bool state_owed{};
OrbisNpReachabilityStateCallback reach_cb{};
void* reach_userdata{};
bool reach_owed{};

void OweState() {
    std::scoped_lock lk{mutex};
    state_owed = true;
    reach_owed = true;
}

void Deliver() {
    OrbisNpStateCallbackForNpToolkit state_func{};
    void* state_arg{};
    OrbisNpReachabilityStateCallback reach_func{};
    void* reach_arg{};
    {
        std::scoped_lock lk{mutex};
        if (state_cb && state_owed) {
            state_owed = false;
            state_func = state_cb;
            state_arg = state_userdata;
        }
        if (reach_cb && reach_owed) {
            reach_owed = false;
            reach_func = reach_cb;
            reach_arg = reach_userdata;
        }
    }
    if (!state_func && !reach_func) {
        return;
    }
    // Called with the lock released: a callback may register or unregister.
    const s32 user_id = Host::UserId();
    const bool signed_in = Host::SignedIn();
    if (state_func) {
        LOG_INFO(Lib_NpManager, "Gravity Rush 2: toolkit is told {}",
                 signed_in ? "SignedIn" : "SignedOut");
        state_func(user_id, signed_in ? OrbisNpState::SignedIn : OrbisNpState::SignedOut,
                   state_arg);
    }
    // The toolkit waits for both: signed in alone does not start it.
    if (reach_func) {
        reach_func(user_id,
                   signed_in ? OrbisNpReachabilityState::Reachable
                             : OrbisNpReachabilityState::Unavailable,
                   reach_arg);
    }
}

} // Anonymous namespace

s32 PS4_SYSV_ABI sceNpRegisterStateCallbackForToolkit(OrbisNpStateCallbackForNpToolkit callback,
                                                      void* userdata) {
    std::scoped_lock lk{mutex};
    state_cb = callback;
    state_userdata = userdata;
    state_owed = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpUnregisterStateCallbackForToolkit() {
    std::scoped_lock lk{mutex};
    if (!state_cb) {
        return ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED;
    }
    state_cb = nullptr;
    state_userdata = nullptr;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpRegisterNpReachabilityStateCallback(OrbisNpReachabilityStateCallback callback,
                                                          void* userdata) {
    if (!callback) {
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    }
    std::scoped_lock lk{mutex};
    reach_cb = callback;
    reach_userdata = userdata;
    reach_owed = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpUnregisterNpReachabilityStateCallback() {
    std::scoped_lock lk{mutex};
    if (!reach_cb) {
        return ORBIS_NP_ERROR_CALLBACK_NOT_REGISTERED;
    }
    reach_cb = nullptr;
    reach_userdata = nullptr;
    return ORBIS_OK;
}

// Both pumps, sceNpCheckCallback and sceNpCheckCallbackForLib: the callbacks run here, on the
// thread of the game that asks for them. The emulator's own pump is therefore never called for
// this game, and nothing it delivers reaches the game: the state callbacks registered with the
// emulator's library and whatever is attached with RegisterNpCallback.
s32 PS4_SYSV_ABI sceNpCheckCallback() {
    Deliver();
    return ORBIS_OK;
}

// The game makes its user object when the user logs in, reads the name out of these two without
// looking at the return value, and puts that name on ghosts, photos and score rows. So the name
// is written even when the answer is "signed out".
s32 PS4_SYSV_ABI sceNpGetNpId(OrbisUserServiceUserId user_id, OrbisNpId* np_id) {
    const s32 ret = Np::NpManager::sceNpGetNpId(user_id, np_id);
    if (ret != ORBIS_OK && np_id) {
        *np_id = {};
        const std::string name = Host::PlayerName();
        std::strncpy(np_id->handle.data, name.c_str(), sizeof(np_id->handle.data));
    }
    return ret;
}

s32 PS4_SYSV_ABI sceNpGetOnlineId(OrbisUserServiceUserId user_id, OrbisNpOnlineId* online_id) {
    const s32 ret = Np::NpManager::sceNpGetOnlineId(user_id, online_id);
    if (ret != ORBIS_OK && online_id) {
        *online_id = {};
        const std::string name = Host::PlayerName();
        std::strncpy(online_id->data, name.c_str(), sizeof(online_id->data));
    }
    return ret;
}

// UserProfile::Interface::getAvatarUrl of the toolkit the game ships. That one never completes
// here, which leaves every player icon a placeholder; this one answers at once with the address
// the server of the game keeps the avatar under. The result is written the way the toolkit lays
// out a Future<string>: the state at +0x18 (0 is done), the string at +0x230 with the pointer to
// its text at +0x08, its length at +0x18 and its capacity at +0x20. The address is always longer
// than the 15 bytes such a string holds in place, so the text comes from the toolkit's own
// allocator, which also frees it.
constexpr u64 FutureState = 0x18;
constexpr u64 FutureString = 0x230;
constexpr u64 ToolkitAllocator = 0xb0258;

// Found once and kept: the module list is not walked again while the game loads more modules.
static u64 ToolkitBase() {
    static std::atomic<u64> found{};
    if (const u64 base = found) {
        return base;
    }
    const auto* linker = Common::Singleton<Core::Linker>::Instance();
    for (s32 i = 0; const auto* module = linker->GetModule(i); ++i) {
        if (module->name.contains("libSceNpToolkit")) {
            found = module->GetBaseAddress();
            return found;
        }
    }
    return 0;
}

static bool IsOnlineId(const char* text) {
    if (!text) {
        return false;
    }
    for (s32 i = 0; i <= Np::ORBIS_NP_ONLINEID_MAX_LENGTH; ++i) {
        const u8 c = static_cast<u8>(text[i]);
        if (c == 0) {
            return i > 0;
        }
        if (c < 0x21 || c > 0x7e) {
            return false;
        }
    }
    return false;
}

s32 PS4_SYSV_ABI GetAvatarUrl(u8* future, const u8* request, bool async) {
    if (!future) {
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    }
    // The request names the player at +0x30. Where that is no Online ID, the avatar asked for is
    // the player's own.
    const char* target = request ? *reinterpret_cast<const char* const*>(request + 0x30) : nullptr;
    const std::string who = IsOnlineId(target)
                                ? std::string{target}
                                : Host::PlayerName().substr(0, Np::ORBIS_NP_ONLINEID_MAX_LENGTH);
    // The HTTP library sends every address to the server of the game, whatever host it names.
    const std::string url = "http://gr2.local:8443/avatar.png?u=" + who;

    using Alloc = PS4_SYSV_ABI void* (*)(void* self, u64 size);
    const u64 base = ToolkitBase();
    void* allocator = base ? *reinterpret_cast<void**>(base + ToolkitAllocator) : nullptr;
    if (!allocator) {
        LOG_ERROR(Lib_NpManager, "No toolkit allocator, avatar of {} stays empty", who);
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    }
    const auto alloc = reinterpret_cast<Alloc>((*reinterpret_cast<void***>(allocator))[0]);
    char* text = static_cast<char*>(alloc(allocator, url.size() + 1));
    if (!text) {
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    }
    std::memcpy(text, url.c_str(), url.size() + 1);

    u8* str = future + FutureString;
    *reinterpret_cast<char**>(str + 0x08) = text;
    *reinterpret_cast<u64*>(str + 0x18) = url.size();
    *reinterpret_cast<u64*>(str + 0x20) = url.size();
    *reinterpret_cast<volatile s32*>(future + FutureState) = 0;
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    Host::OnSignInChange([](s32 user_id, bool) {
        if (user_id == Host::UserId()) {
            OweState();
        }
    });

    LIB_FUNCTION("p-o74CnoNzY", "libSceNpManager", 1, "libSceNpManager", sceNpGetNpId);
    LIB_FUNCTION("XDncXQIJUSk", "libSceNpManager", 1, "libSceNpManager", sceNpGetOnlineId);
    LIB_FUNCTION("3Zl8BePTh9Y", "libSceNpManager", 1, "libSceNpManager", sceNpCheckCallback);
    LIB_FUNCTION("JELHf4xPufo", "libSceNpManager", 1, "libSceNpManager", sceNpCheckCallback);
    LIB_FUNCTION("hw5KNqAAels", "libSceNpManager", 1, "libSceNpManager",
                 sceNpRegisterNpReachabilityStateCallback);
    LIB_FUNCTION("cRILAEvn+9M", "libSceNpManager", 1, "libSceNpManager",
                 sceNpUnregisterNpReachabilityStateCallback);
    LIB_FUNCTION("JELHf4xPufo", "libSceNpManagerForToolkit", 1, "libSceNpManager",
                 sceNpCheckCallback);
    LIB_FUNCTION("0c7HbXRKUt4", "libSceNpManagerForToolkit", 1, "libSceNpManager",
                 sceNpRegisterStateCallbackForToolkit);
    LIB_FUNCTION("YIvqqvJyjEc", "libSceNpManagerForToolkit", 1, "libSceNpManager",
                 sceNpUnregisterStateCallbackForToolkit);

    LIB_FUNCTION("3jeBu0QFXWU", "libSceNpToolkit", 1, "libSceNpToolkit", GetAvatarUrl);
}

} // namespace Libraries::Gr2Online::NpManager
