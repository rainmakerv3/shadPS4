// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Vulkan {

// RenoDX (a ReShade add-on) is loaded: it replaces the game's tonemapping and display-copy shaders
// and shadPS4's presentation shader to output HDR.
bool RenoDxLoaded();

// A RenoDX add-on sits where ReShade loads add-ons from, so it will be loaded with the Vulkan
// instance. For choices made before that.
bool RenoDxInstalled();

} // namespace Vulkan
