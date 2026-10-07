// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_hdr_mod.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <windows.h>
// After windows.h.
#include <tlhelp32.h>
#endif

namespace Vulkan {

#ifdef _WIN32
namespace {
bool IsRenoDx(std::wstring name) {
    for (auto& c : name)
        c = static_cast<wchar_t>(std::towlower(c));
    return name.starts_with(L"renodx");
}
} // namespace
#endif

bool RenoDxLoaded() {
#ifdef _WIN32
    // ReShade loads its add-ons (renodx-*.addon64) while the Vulkan instance is created.
    static const bool loaded = [] {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return false;
        bool found = false;
        MODULEENTRY32W entry{.dwSize = sizeof(entry)};
        for (BOOL ok = Module32FirstW(snapshot, &entry); ok && !found;
             ok = Module32NextW(snapshot, &entry)) {
            found = IsRenoDx(entry.szModule);
        }
        CloseHandle(snapshot);
        return found;
    }();
    return loaded;
#else
    return false;
#endif
}

bool RenoDxInstalled() {
#ifdef _WIN32
    if (std::getenv("DISABLE_VK_LAYER_reshade_1"))
        return false;
    std::wstring exe(MAX_PATH, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size())));
    const auto dir = std::filesystem::path{exe}.parent_path();
    // ReShade.ini [ADDON] AddonPath, relative to the executable; its folder when unset.
    auto addons = dir;
    std::ifstream ini{dir / "ReShade.ini"};
    for (std::string line; std::getline(ini, line);) {
        if (!line.starts_with("AddonPath="))
            continue;
        auto value = line.substr(10);
        while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
            value.pop_back();
        if (!value.empty())
            addons = dir / std::filesystem::path{value};
    }
    std::error_code ec;
    for (const auto& file : std::filesystem::directory_iterator{addons, ec})
        if (file.path().extension().wstring().starts_with(L".addon") &&
            IsRenoDx(file.path().filename().wstring()))
            return true;
#endif
    return false;
}

} // namespace Vulkan
