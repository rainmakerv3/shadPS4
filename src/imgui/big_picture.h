// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <vulkan/vulkan.hpp>

#include "common/types.h"

namespace BigPictureMode {

struct TextureData {
    vk::DescriptorSet DS; // Descriptor set: this is what you'll pass to Image()
    int Width;
    int Height;
    int Channels;

    // Need to keep track of these to properly cleanup
    vk::ImageView ImageView;
    vk::Image Image;
    vk::DeviceMemory ImageMemory;
    vk::Buffer UploadBuffer;
    vk::DeviceMemory UploadBufferMemory;
    vk::Sampler Sampler;

    TextureData() {
        memset(this, 0, sizeof(*this));
    }
};

struct Game {
    TextureData iconTexture;
    std::filesystem::path ebootPath;
    std::string title;
    std::string serial;
    bool focusState;
};

void Launch(char* exeName);
void SetGameIcons(std::vector<Game>& games);
void GetGameInfo(std::vector<Game>& games, bool isSettingsInfo, TextureData texture = {});
std::filesystem::path UpdateChecker(const std::string sceItem, std::filesystem::path game_folder);

bool LoadTextureFromData(std::vector<u8> data, TextureData* tex_data);
std::vector<std::string> GetGpuNames();

} // namespace BigPictureMode
