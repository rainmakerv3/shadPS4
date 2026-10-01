// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Gr2Online::Secure {

/// The two libSceSecure functions that would encrypt a request and decrypt a reply. The server
/// reads and writes plain text, so both leave the buffer alone.
void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Gr2Online::Secure
