// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <SDL3/SDL.h>

#include "imgui/imgui_layer.h"

namespace BigPictureMode::Settings {

class SettingsLayer final : public ImGui::Layer {
public:
    SettingsLayer();
    ~SettingsLayer() override;

    void Finish();
    void Draw() override;
};

void OpenSettings();

}; // namespace  BigPictureMode::Settings
