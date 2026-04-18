// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmrc/cmrc.hpp>
#include <imgui.h>
#include <queue>

#include "imgui/imgui_std.h"
#include "shadnet_notifications.h"

CMRC_DECLARE(res);
namespace shadNet {

static ImGui::RefCountedTexture shadnetIcon;

std::optional<NotificationsUI> current_notif = std::nullopt;
std::queue<std::string> notif_queue;
std::mutex queueMtx;

NotificationsUI::NotificationsUI(std::string message) : notifMessage(message) {
    AddLayer(this);

    if (shadnetIcon.GetTexture().im_id == nullptr) {
        Initialize();
    }

    notification_timer = 5.0f;
}

NotificationsUI::~NotificationsUI() {
    Finish();
}

void NotificationsUI::Finish() {
    RemoveLayer(this);
}

void NotificationsUI::Draw() {
    const auto& io = ImGui::GetIO();
    notification_timer -= io.DeltaTime;

    float AdjustWidth = io.DisplaySize.x / 1920;
    float AdjustHeight = io.DisplaySize.y / 1080;
    const ImVec2 window_size{
        std::min(io.DisplaySize.x, (350 * AdjustWidth)),
        std::min(io.DisplaySize.y, (70 * AdjustHeight)),
    };

    elapsed_time += io.DeltaTime;
    float progress = std::min(elapsed_time / animation_duration, 1.0f);
    float final_pos_x, start_x;

    start_x = io.DisplaySize.x;
    final_pos_x = io.DisplaySize.x - window_size.x - 20 * AdjustWidth;
    ImVec2 current_pos = ImVec2(start_x + (final_pos_x - start_x) * progress, 50 *
    AdjustHeight); ImGui::SetNextWindowPos(current_pos);

    // If the remaining time of the trophy is less than or equal to 1 second, the fade-ouy begins.
    if (notification_timer <= 1.0f) {
        float fade_out_time = 1.0f - (notification_timer / 1.0f);
        fade_opacity = 1.0f - fade_out_time;
    } else {
        // Fade in , 0 to 1
        fade_opacity = 1.0f;
    }

    fade_opacity = std::max(0.0f, std::min(fade_opacity, 1.0f));

    ImGui::SetNextWindowSize(window_size);
    ImGui::SetNextWindowPos(current_pos);
    ImGui::SetNextWindowCollapsed(false);
    ImGui::KeepNavHighlight();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade_opacity);

    if (ImGui::Begin("Notification Window", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoInputs)) {

        ImGui::GetColorU32(ImVec4{0.7f});

        ImGui::Image(shadnetIcon.GetTexture().im_id,
                     ImVec2((50 * AdjustWidth), (50 * AdjustHeight)));
        ImGui::SameLine();

        ImGui::Text("%s", notifMessage.c_str());
    }

    ImGui::PopStyleVar();
    ImGui::End();

    if (notification_timer <= 0) {
        std::lock_guard<std::mutex> lock(queueMtx);
        if (!notif_queue.empty()) {
            std::string next = notif_queue.front();
            notif_queue.pop();
            current_notif.emplace(next);
        } else {
            current_notif.reset();
        }

        // Resetting the animation
        elapsed_time = 0.0f;                // Resetting animation time
        fade_opacity = 0.0f;                // Starts invisible
        start_pos = ImVec2(1280.0f, 50.0f); // Starts off screen, right
    }
}

void Initialize() {
    auto resource = cmrc::res::get_filesystem();
    auto file = resource.open("src/images/shadps4.png");
    std::vector<u8> imgdata = std::vector<u8>(file.begin(), file.end());
    shadnetIcon = ImGui::RefCountedTexture::DecodePngTexture(imgdata);
}

void QueueNotification(std::string message) {
    std::lock_guard<std::mutex> lock(queueMtx);

    if (!current_notif.has_value()) {
        current_notif.emplace(message);
    } else {
        notif_queue.push(message);
    }
}

} // namespace shadNet
