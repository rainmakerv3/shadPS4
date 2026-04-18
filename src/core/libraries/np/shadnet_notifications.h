// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "imgui/imgui_layer.h"
#include "imgui/imgui_texture.h"

namespace shadNet {

class NotificationsUI final : public ImGui::Layer {
public:
    NotificationsUI(std::string message);
    ~NotificationsUI() override;

    void Finish();

    void Draw() override;

private:
    std::string notifMessage;
    double notification_timer;

    // notification animation
    float fade_opacity = 0.0f;                 // Initial opacity (invisible)
    ImVec2 start_pos = ImVec2(1280.0f, 50.0f); // Starts off screen, right
    ImVec2 target_pos = ImVec2(0.0f, 50.0f);   // Final position
    float animation_duration = 0.5f;           // Animation duration
    float elapsed_time = 0.0f;                 // Animation time
    float fade_out_duration = 0.5f;            // Final fade duration
};

void QueueNotification(std::string message);
void Initialize();

}; // namespace shadNet
