// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace ImGui::Dlss {

// In-game DLSS settings and status panel. Call Register() once after ImGui is initialized.
void Register();
void Unregister();
void Toggle();
// Where the game image is drawn this frame (ImGui coordinates), for the fps counter.
void SetGameArea(float x, float y);

} // namespace ImGui::Dlss
