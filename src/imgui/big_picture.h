// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <SDL3/SDL.h>
#include <imgui.h>

#include "common/types.h"
#include "imgui/imgui_texture.h"

namespace BigPictureMode {

struct Game {
    ImGui::RefCountedTexture iconTexture;
    std::filesystem::path ebootPath;
    std::string title;
    std::string serial;
    bool focusState;
};

void Launch(char* exeName);
void SetGameIcons(std::vector<Game>& games);
void GetGameInfo(std::vector<Game>& games, bool isSettingsInfo,
                 ImGui::RefCountedTexture texture = {});
std::filesystem::path UpdateChecker(const std::string sceItem, std::filesystem::path game_folder);

void LoadTextureFromFile(std::filesystem::path filePath, ImGui::RefCountedTexture& textureId);
void LoadTexture(std::vector<u8> data, ImGui::RefCountedTexture& textureId);

} // namespace BigPictureMode
