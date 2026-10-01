// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Code patches to the executable of Gravity Rush 2 that the served online content needs: without
// them a served challenge or notice ends in a double free, and the player's own name is missing
// from two screens.
namespace Libraries::Gr2Online::Patches {

/// Installs every patch that is not installed yet. Cheap once all are in. Called from the HTTP
/// library, which the game initialises before it asks the server for anything.
void Apply();

} // namespace Libraries::Gr2Online::Patches
