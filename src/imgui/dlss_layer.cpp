// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "common/path_util.h"
#include "imgui/dlss_layer.h"
#include "imgui/imgui_layer.h"
#include "imgui/renderer/imgui_core.h"
#include "video_core/renderer_vulkan/vk_bb_temporal_dlss.h"

namespace ImGui::Dlss {
namespace {

// The panel edits the same settings file the renderer polls, so it needs no renderer access.
class SettingsFile {
public:
    void Load() {
        lines.clear();
        std::ifstream file{Path()};
        for (std::string line; std::getline(file, line);)
            lines.push_back(line);
    }
    std::string Get(const std::string& key, const std::string& fallback) const {
        for (const auto& line : lines)
            if (line.rfind(key + "=", 0) == 0)
                return line.substr(key.size() + 1);
        return fallback;
    }
    void Set(const std::string& key, const std::string& value) {
        Load(); // keep edits made to the file while the panel was open
        for (auto& line : lines)
            if (line.rfind(key + "=", 0) == 0) {
                line = key + "=" + value;
                return Save();
            }
        lines.push_back(key + "=" + value);
        Save();
    }

private:
    static std::filesystem::path Path() {
        return Vulkan::BbDlssSettingsPath();
    }
    void Save() const {
        std::ofstream file{Path(), std::ios::trunc};
        for (const auto& line : lines)
            file << line << '\n';
    }
    std::vector<std::string> lines;
};

constexpr std::array RecommendedPatches{"Disable AA", "Disable Chromatic Aberration",
                                        "Disable DoF"};
// Removes the depth and motion data the upscaler needs; the full name ends in "(Perf Increase)".
constexpr char MotionBlurPatch[] = "Disable Motion Blur[^\"]*";

std::filesystem::path PatchFile() {
    return Common::FS::GetUserPath(Common::FS::PathType::PatchesDir) / "shadPS4" / "Bloodborne.xml";
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::regex PatchPattern(const char* name) {
    return std::regex{std::string{"(<Metadata [^>]*Name=\""} + name +
                      "\"[^>]*isEnabled=\")(true|false)(\")"};
}

// Patches in the shadPS4 patch file, for the recommended-visuals check.
struct PatchState {
    bool file_found{};
    std::array<bool, RecommendedPatches.size()> enabled{};
    bool motion_blur_disabled{};
};

PatchState ReadPatches() {
    PatchState state;
    const auto text = ReadAll(PatchFile());
    state.file_found = !text.empty();
    for (size_t i = 0; i < RecommendedPatches.size(); ++i) {
        std::smatch match;
        state.enabled[i] = std::regex_search(text, match, PatchPattern(RecommendedPatches[i])) &&
                           match[2] == "true";
    }
    std::smatch match;
    state.motion_blur_disabled =
        std::regex_search(text, match, PatchPattern(MotionBlurPatch)) && match[2] == "true";
    return state;
}

bool EnableRecommendedPatches() {
    auto text = ReadAll(PatchFile());
    if (text.empty())
        return false;
    for (const char* name : RecommendedPatches)
        text = std::regex_replace(text, PatchPattern(name), "$1true$3");
    text = std::regex_replace(text, PatchPattern(MotionBlurPatch), "$1false$3");
    std::ofstream file{PatchFile(), std::ios::binary | std::ios::trunc};
    file << text;
    return bool(file);
}

constexpr std::array UpscalerValues{"auto", "dlss", "fsr"};
constexpr std::array UpscalerNames{"Automatic", "DLSS", "FSR 3.1"};
constexpr std::array PresetValues{13, 11, 10, 12, 0};
constexpr std::array PresetNames{"M (recommended)", "K", "J", "L", "NVIDIA default"};

class DlssLayer final : public ImGui::Layer {
public:
    void Draw() override;
    bool ShouldKeepDrawing() override {
        return open || toggle_requested;
    }
    void Open() {
        settings.Load();
        patches = ReadPatches();
        patch_message.clear();
        open = true;
        focus = true;
        // Like shadPS4's other prompts: the game stops reading the controller while the panel
        // is open, and the mouse cursor is shown.
        ImGui::Core::AcquireGamepadInputCapture();
        GetIO().MouseDrawCursor = true;
    }
    void Close() {
        if (!open)
            return;
        open = false;
        ImGui::Core::ReleaseGamepadInputCapture();
        GetIO().MouseDrawCursor = false;
    }
    bool open{};
    bool focus{};
    bool registered{};
    std::atomic<bool> toggle_requested{}; // set by the input thread, handled while drawing

private:
    SettingsFile settings;
    PatchState patches;
    std::string patch_message;
};

DlssLayer layer;

void DlssLayer::Draw() {
    if (toggle_requested.exchange(false)) {
        if (open)
            Close();
        else
            Open();
    }
    if (!open)
        return;
    if (focus) {
        SetNextWindowFocus();
        focus = false;
    }
    SetNextWindowSize({460, 0}, ImGuiCond_Appearing);
    SetNextWindowPos(GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    bool keep_open = true;
    if (!Begin("Upscaling  (F1 / Esc to close)", &keep_open,
               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                   ImGuiWindowFlags_AlwaysAutoResize)) {
        End();
        return;
    }
    if (!keep_open || (IsWindowFocused() && IsKeyPressed(ImGuiKey_Escape, false))) {
        End();
        Close();
        return;
    }
    const auto status = Vulkan::BbTemporalDlssStatus();
    if (status.active) {
        TextColored({0.4f, 1.0f, 0.4f, 1.0f}, "Active");
        SameLine();
        Text("%s  %ux%u -> %ux%u", status.backend.c_str(), status.render_width,
             status.render_height, status.output_width, status.output_height);
    } else {
        TextColored({1.0f, 0.75f, 0.3f, 1.0f}, "Not active");
        if (!status.reason.empty())
            TextWrapped("%s", status.reason.c_str());
    }
    if (!status.gpu.empty())
        TextDisabled("GPU: %s", status.gpu.c_str());
    if (patches.motion_blur_disabled) {
        PushStyleColor(ImGuiCol_Text, ImVec4{1.0f, 0.4f, 0.4f, 1.0f});
        TextWrapped("The Disable Motion Blur patch is on. It removes the depth and motion data "
                    "the upscaler needs: turn it off below and restart the game.");
        PopStyleColor();
    }

    SeparatorText("Settings");
    bool enabled = settings.Get("enabled", "1") != "0";
    if (Checkbox("Upscaling enabled", &enabled))
        settings.Set("enabled", enabled ? "1" : "0");

    const auto upscaler = settings.Get("upscaler", "auto");
    int upscaler_index = 0;
    for (size_t i = 0; i < UpscalerValues.size(); ++i)
        if (upscaler.starts_with(UpscalerValues[i]))
            upscaler_index = int(i);
    if (Combo("Upscaler", &upscaler_index, UpscalerNames.data(), int(UpscalerNames.size())))
        settings.Set("upscaler", UpscalerValues[upscaler_index]);
    if (!status.dlss_problem.empty() && upscaler_index != 2)
        TextDisabled("%s", status.dlss_problem.c_str());

    const int preset = std::atoi(settings.Get("preset", "13").c_str());
    int preset_index = 0;
    for (size_t i = 0; i < PresetValues.size(); ++i)
        if (PresetValues[i] == preset)
            preset_index = int(i);
    BeginDisabled(status.backend == "FSR 3.1" || upscaler_index == 2);
    if (Combo("DLSS model", &preset_index, PresetNames.data(), int(PresetNames.size())))
        settings.Set("preset", std::to_string(PresetValues[preset_index]));
    EndDisabled();

    float sharpness = float(std::atof(settings.Get("sharpness", "0.6").c_str()));
    if (SliderFloat("Sharpness", &sharpness, 0.0f, 1.0f, "%.2f"))
        settings.Set("sharpness", fmt::format("{:.2f}", sharpness));

    SeparatorText("Resolution");
    TextWrapped("The game's render resolution is upscaled to your shadPS4 window size. The "
                "render resolution comes from the Resolution Patch you enable for Bloodborne "
                "(shadPS4 patches or BBLauncher): for a 4K window, the 1440p patch is Quality "
                "mode and the 1080p patch is Performance mode (faster, softer). A render "
                "resolution equal to the window only anti-aliases (DLAA / FSR Native AA).");

    SeparatorText("Recommended game patches");
    if (!patches.file_found) {
        TextWrapped("Bloodborne patch file not found. Download patches in shadPS4 first.");
    } else {
        bool all = true;
        for (size_t i = 0; i < RecommendedPatches.size(); ++i) {
            all &= patches.enabled[i];
            TextColored(patches.enabled[i] ? ImVec4{0.4f, 1.0f, 0.4f, 1.0f}
                                           : ImVec4{1.0f, 0.75f, 0.3f, 1.0f},
                        "%s  %s", patches.enabled[i] ? "on " : "off", RecommendedPatches[i]);
        }
        if (patches.motion_blur_disabled) {
            all = false;
            TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "on   Disable Motion Blur (must be off)");
        }
        if (!all) {
            TextWrapped("The game's own anti-aliasing, chromatic aberration and depth of field "
                        "blur the image before the upscaler sees it.");
            if (Button("Apply recommended patches")) {
                patch_message = EnableRecommendedPatches() ? "Done. Restart the game to apply."
                                                           : "Could not write the patch file.";
                patches = ReadPatches();
            }
        }
        if (!patch_message.empty())
            TextWrapped("%s", patch_message.c_str());
    }
    End();
}

} // namespace

void Register() {
    if (!layer.registered) {
        ImGui::Layer::AddLayer(&layer);
        layer.registered = true;
    }
}

void Unregister() {
    if (layer.registered) {
        ImGui::Layer::RemoveLayer(&layer);
        layer.registered = false;
    }
}

void Toggle() {
    layer.toggle_requested = true;
}

} // namespace ImGui::Dlss
