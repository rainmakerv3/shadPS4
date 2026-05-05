// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <variant>
#include <vector>
#include <SDL3/SDL.h>
#include <imgui.h>

#include "big_picture.h"

namespace BigPictureMode::Settings {

enum class SettingsCategory {
    Profiles,
    General,
    Graphics,
    Input,
    Trophy,
    Folders,
    Log,
    Experimental,
};

void Init();
void DeInit();

void SetProfileIcons(std::vector<Game>& games);
TextureData LoadEmbeddedTexture(std::string resourcePath);
void AddCategory(std::string name, TextureData texture, SettingsCategory category);

void DrawSettings(bool* open, bool gameRunning);
void SaveSettings(std::string profile);
void LoadSettings(std::string profile);
void LoadCategory(SettingsCategory);
void SaveInstallDirs();

void AddSettingBool(std::string name, bool& value);
void AddSettingSliderInt(std::string name, int& value, int min, int max);
void AddSettingSliderFloat(std::string name, float& value, int min, int max, int precision);
void AddSettingCombo(std::string name, int& value, std::vector<std::string> options);
int GetComboIndex(std::string selection, std::vector<std::string> options);

} // namespace  BigPictureMode::Settings
