// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <imgui.h>

#include "imgui/imgui_std.h"
#include "imgui/settings_dialog_imgui.h"
#include "settings_dialog_layer.h"

static bool running = false;

namespace BigPictureMode::Settings {

SettingsLayer::SettingsLayer() {
    AddLayer(this);
}

SettingsLayer::~SettingsLayer() {}

void SettingsLayer::Finish() {
    RemoveLayer(this);
}

void SettingsLayer::Draw() {
    ImGui::PushFont(ImGui::GetIO().Fonts->Fonts[IMGUI_FONT_BIGPIGTURE]);
    BigPictureMode::Settings::DrawSettings(&running, true);
    ImGui::PopFont();

    if (!running) {
        Finish();
    }
}

void OpenSettings() {
    if (running) {
        return;
    }

    running = true;
    SettingsLayer* m_settings = new SettingsLayer();
}

} // namespace BigPictureMode::Settings
