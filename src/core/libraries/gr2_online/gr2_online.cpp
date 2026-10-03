// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/logging/log.h"
#include "core/libraries/gr2_online/gr2_online.h"
#include "core/libraries/gr2_online/online_host.h"
#include "core/libraries/gr2_online/online_http.h"
#include "core/libraries/gr2_online/online_np_auth.h"
#include "core/libraries/gr2_online/online_np_manager.h"
#include "core/libraries/gr2_online/online_np_web_api.h"
#include "core/libraries/gr2_online/online_save.h"
#include "core/libraries/gr2_online/online_secure.h"
#include "core/libraries/gr2_online/online_session.h"
#include "core/libraries/gr2_online/online_stat.h"

namespace Libraries::Gr2Online {

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    if (!Host::IsGravityRush2()) {
        return;
    }
    // Registered ahead of every other library, so the game binds to these and not to the
    // emulator's own. What is registered for libSceSecure and libSceNpToolkit wins over the
    // modules the game ships, because the linker asks this table before the exports of a loaded
    // module. No user is logged in yet: what needs the user or the shadNet session waits for the
    // sign-in or for the first call of the game.
    if (const auto& server = Host::GetServer(); server.host.empty()) {
        LOG_WARNING(Lib_Http, "Gravity Rush 2: no server configured, online play is off "
                              "(GR2Fork.httpHostOverride)");
    } else {
        LOG_INFO(Lib_Http, "Gravity Rush 2: online play on its own libraries, server {}:{}",
                 server.host, server.port);
    }
    Session::Arm();
    Http::RegisterLib(sym);
    Secure::RegisterLib(sym);
    NpWebApi::RegisterLib(sym);
    NpManager::RegisterLib(sym);
    NpAuth::RegisterLib(sym);
    Save::RegisterLib(sym);
    Stat::RegisterLib(sym);
}

} // namespace Libraries::Gr2Online
