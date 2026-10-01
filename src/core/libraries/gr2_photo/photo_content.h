// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Gr2Photo::Content {

/// libSceContentExport, libSceContentSearch and libSceContentDelete over the photo gallery.
void RegisterLib(Core::Loader::SymbolsResolver* sym);

} // namespace Libraries::Gr2Photo::Content
