// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Gr2Online::NpManager {

/// The libSceNpManager functions Gravity Rush 2 needs in another form than the emulator's own:
/// the name of the player, the delivery of the sign-in state to the game's toolkit, and the
/// avatar address the toolkit never produces here.
void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Gr2Online::NpManager
