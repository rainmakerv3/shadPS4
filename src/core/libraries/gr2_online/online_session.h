// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// What the emulator itself asks the Gravity Rush 2 server, outside the game's own requests: the
// launch gate, the avatar upload and the Dusty Token check.
namespace Libraries::Gr2Online::Session {

/// Subscribes to the sign-in of the user. The first sign-in asks the server whether this build
/// and this account may play (GET /clientgate). Called once, from RegisterLib.
void Arm();

/// Called for every request the game creates. The first call made with online play on and the
/// user signed in uploads the avatar of the user and brings a Dusty Token count above the
/// server's back down to it. Returns at once; the work is on its own threads.
void OnRequest();

} // namespace Libraries::Gr2Online::Session
