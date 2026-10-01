// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Gr2Online::NpWebApi {

/// libSceNpWebApi for the profile and friend list lookups of the game: they are answered by the
/// server of the game, not by PSN.
void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Gr2Online::NpWebApi
