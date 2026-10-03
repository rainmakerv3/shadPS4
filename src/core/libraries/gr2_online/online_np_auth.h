// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Gr2Online::NpAuth {

/// The libSceNpAuth functions that hand out an authorization code. They answer as the emulator's
/// library does, with a longer code: the game's toolkit builds no login request with the
/// emulator's short one.
void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Gr2Online::NpAuth
