// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "common/types.h"

// Everything the online play of Gravity Rush 2 takes from the rest of the emulator: the running
// title, the local user, the shadNet session that NpHandler owns, two settings and the on-screen
// notices. No other file of this folder includes np_handler.h, user_settings.h, elf_info.h or
// emulator_settings.h.
namespace Libraries::Gr2Online::Host {

/// Version of the online protocol, sent as X-GR2-Version. The server turns away a client whose
/// version is below its minimum.
constexpr int OnlineVersion = 1;

/// The running title is Gravity Rush 2.
bool IsGravityRush2();

/// Serial of the running title. Its saves are in UserHome(user) / "savedata" / Serial().
std::string_view Serial();

/// The user the game plays as, player 1. -1 when there is none.
s32 UserId();

/// Online play is on: the "connected to network" setting is set and a server is configured. Read
/// on each call. While it is false no request leaves this folder.
bool Connected();

/// The user has a shadNet session right now. This is what the game is told.
bool SignedIn();

/// The user signed in to shadNet during this run. PlayerName and BearerToken answer with that
/// sign-in from then on, also while the session is down: the server of the game checks the token
/// itself and does not need the session.
bool HasIdentity();

/// The Online ID of the user's shadNet sign-in, kept for the run. The name of the local user when
/// there was no sign-in. Control bytes are removed, so the value can go into a header. Empty when
/// there is no user.
std::string PlayerName();

/// The bearer token of the user's shadNet sign-in, which the server checks against shadNet. It is
/// kept for the run and replaced when a new session hands out a new one. Empty when there was no
/// sign-in. A token that is still on its way after a sign-in is waited for, for at most 2.5 s
/// per sign-in.
std::string BearerToken();

/// X-GR2-Player, X-GR2-Auth and X-GR2-Version as the server reads them. A header with no value
/// is left out.
std::vector<std::pair<std::string, std::string>> IdentityHeaders();

struct Server {
    std::string host; ///< Empty when none is configured: online play is off
    int port;
};

/// Where the game's requests go: httpHostOverride and httpHostOverridePort of the "GR2Fork"
/// section, from the per-game file when it has them and from config.json otherwise. Read once.
const Server& GetServer();

/// Calls cb after every sign-in and sign-out of a user, on the thread that changed the state.
/// May be called before the session exists.
void OnSignInChange(std::function<void(s32 user_id, bool signed_in)> cb);

/// Shows a notice in the shadNet notification layer.
void Notify(std::string text);

/// Home directory of a user: gr2_avatar.png and savedata/ are in it.
std::filesystem::path UserHome(s32 user_id);

} // namespace Libraries::Gr2Online::Host
