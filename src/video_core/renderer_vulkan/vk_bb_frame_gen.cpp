// SPDX-FileCopyrightText: Copyright 2026 IFreemz
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_bb_frame_gen.h"
#include "video_core/renderer_vulkan/vk_bb_fsr_frame_gen.h"
#include "video_core/renderer_vulkan/vk_bb_temporal_dlss.h"
#include "video_core/renderer_vulkan/vk_hdr_mod.h"

#if defined(SHADPS4_BB_FRAME_GEN) && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <dxgi1_6.h>
#include <windows.h>
// Streamline's headers expect the Vulkan C types.
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_security.h>
#include <vulkan/vulkan.h>

// sl_pcl.h and sl_reflex.h do not compile as C++23 (`using to_underlying = std::to_underlying`);
// the few declarations used here, as in those headers.
namespace sl {
enum class PCLMarker : uint32_t {
    eSimulationStart = 0,
    eSimulationEnd = 1,
    eRenderSubmitStart = 2,
    eRenderSubmitEnd = 3,
    ePresentStart = 4,
    ePresentEnd = 5,
};
enum ReflexMode { eOff, eLowLatency, eLowLatencyWithBoost, ReflexMode_eCount };
// {F03AF81A-6D0B-4902-A651-C4965E215434}
SL_STRUCT_BEGIN(
    ReflexOptions,
    StructType({0xf03af81a, 0x6d0b, 0x4902, {0xa6, 0x51, 0xc4, 0x96, 0x5e, 0x21, 0x54, 0x34}}),
    kStructVersion1)
ReflexMode mode = ReflexMode::eOff;
uint32_t frameLimitUs = 0;
bool useMarkersToOptimize = false;
uint16_t virtualKey = 0;
uint32_t idThread = 0;
SL_STRUCT_END()
} // namespace sl
using PFun_slPCLSetMarker = sl::Result(sl::PCLMarker marker, const sl::FrameToken& frame);
using PFun_slReflexSleep = sl::Result(const sl::FrameToken& frame);
using PFun_slReflexSetOptions = sl::Result(const sl::ReflexOptions& options);
#define BB_FRAME_GEN 1
#endif

namespace Vulkan::FrameGen {

namespace {
std::atomic<bool> counter_visible{};
std::string problem;
// Per-second counts of emulator presents and of frames shown for them.
std::chrono::steady_clock::time_point window_start{};
u32 window_presents{}, window_shown{}, windows{};
std::atomic<float> base_fps{}, output_fps{};
std::atomic<bool> generating{};
// Frames per rendered frame the user asked for, and the most this card and runtime allow.
std::atomic<u32> multiplier{2}, max_multiplier{2};

void CountPresent(u32 shown) {
    const auto now = std::chrono::steady_clock::now();
    ++window_presents;
    window_shown += shown;
    const float seconds = std::chrono::duration<float>(now - window_start).count();
    if (seconds >= 1.0f) {
        base_fps = float(window_presents) / seconds;
        output_fps = float(window_shown) / seconds;
        if (generating && ++windows % 5 == 0)
            LOG_INFO(Render_Vulkan, "[FRAME-GEN] base {:.1f} fps, shown {:.1f} fps",
                     base_fps.load(), output_fps.load());
        window_presents = window_shown = 0;
        window_start = now;
    }
}
} // namespace

void SetCounterVisible(bool visible) {
    counter_visible = visible;
}

bool CounterVisible() {
    return counter_visible;
}

Stats GetStats() {
    return {generating, base_fps, output_fps};
}

void SetMultiplier(u32 value) {
    multiplier = std::clamp<u32>(value, 2, 6);
}

u32 MaxMultiplier() {
    // FSR 3.1 generates one frame per rendered frame.
    return FsrFrameGen::Active() ? 2 : max_multiplier.load();
}

std::string Problem() {
    return FsrFrameGen::Problem().empty() ? problem : FsrFrameGen::Problem();
}

const char* BackendName() {
    return FsrFrameGen::Active() ? "FSR" : Active() ? "DLSS" : "";
}

namespace {
// A dlss.ini value as saved at launch (empty when missing).
std::string LaunchSetting(std::string_view key) {
    std::ifstream file{BbDlssSettingsPath()};
    std::string line;
    while (std::getline(file, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos || !line.starts_with(key) ||
            line.find_first_not_of(" \t", key.size()) != equals)
            continue;
        auto value = line.substr(equals + 1);
        std::erase_if(value, [](char c) { return c == ' ' || c == '\t' || c == '\r'; });
        return value;
    }
    return {};
}

// dlss.ini fg_backend: auto (DLSS-G when the card supports it, else FSR), dlss or fsr.
std::string Backend() {
    static const std::string backend = LaunchSetting("fg_backend");
    return backend.empty() ? "auto" : backend;
}
} // namespace

bool Requested() {
    static const bool requested = LaunchSetting("frame_gen").find('1') != std::string::npos;
    return requested;
}

#ifdef BB_FRAME_GEN

namespace {
std::filesystem::path ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
}

struct Api {
    HMODULE module{};
    PFun_slInit* init{};
    PFun_slShutdown* shutdown{};
    PFun_slGetNewFrameToken* new_frame{};
    PFun_slSetConstants* set_constants{};
    PFun_slSetTagForFrame* set_tags{};
    PFun_slGetFeatureFunction* feature_function{};
    PFun_slIsFeatureSupported* feature_supported{};
    PFun_slDLSSGSetOptions* dlssg_options{};
    PFun_slDLSSGGetState* dlssg_state{};
    PFun_slReflexSetOptions* reflex_options{};
    PFun_slReflexSleep* reflex_sleep{};
    PFun_slPCLSetMarker* marker{};
    bool feature_functions{};
};
Api api;
bool active{};
std::atomic<bool> enabled{true};
std::atomic<u32> paused_frames{};
// Reflex's sleep before each frame delays the emulator's vblank-paced presents, which costs
// the game frames above 60 fps; it is off unless dlss.ini asks for it.
std::atomic<bool> reflex_low_latency{true}, reflex_sleep{false};
std::chrono::steady_clock::time_point last_generated{};
bool reflex_applied{}, reflex_applied_mode{};

// Presenter thread state.
sl::FrameToken* token{};
bool frame_open{};
sl::DLSSGMode last_mode{sl::DLSSGMode::eOff};
u64 frames{};
u32 status_logged{};

const sl::ViewportHandle Viewport{0u};

void LogMessage(sl::LogType type, const char* message) {
    std::string text{message};
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    if (type == sl::LogType::eError)
        LOG_ERROR(Render_Vulkan, "[SL] {}", text);
    else if (type == sl::LogType::eWarn)
        LOG_WARNING(Render_Vulkan, "[SL] {}", text);
    else
        LOG_INFO(Render_Vulkan, "[SL] {}", text);
}

template <typename T>
bool Export(T*& function, const char* name) {
    function = reinterpret_cast<T*>(GetProcAddress(api.module, name));
    return function != nullptr;
}

// The DLSS-G, Reflex and PCL entry points exist once Streamline has a device, which it gets
// when the emulator creates its device through Streamline's vkCreateDevice.
bool FeatureFunctions() {
    if (api.feature_functions)
        return true;
    const auto get = [](sl::Feature feature, const char* name, auto*& function) {
        void* address{};
        if (api.feature_function(feature, name, address) != sl::Result::eOk || !address)
            return false;
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(address);
        return true;
    };
    if (!get(sl::kFeatureDLSS_G, "slDLSSGSetOptions", api.dlssg_options) ||
        !get(sl::kFeatureDLSS_G, "slDLSSGGetState", api.dlssg_state) ||
        !get(sl::kFeatureReflex, "slReflexSetOptions", api.reflex_options) ||
        !get(sl::kFeatureReflex, "slReflexSleep", api.reflex_sleep) ||
        !get(sl::kFeaturePCL, "slPCLSetMarker", api.marker)) {
        LOG_ERROR(Render_Vulkan, "[FRAME-GEN] Streamline feature functions are missing");
        active = false;
        return false;
    }
    api.feature_functions = true;
    return true;
}

void Marker(sl::PCLMarker marker) {
    if (frame_open && api.marker)
        api.marker(marker, *token);
}

sl::float4x4 Matrix(const std::array<float, 16>& m) {
    sl::float4x4 result{};
    for (u32 r = 0; r < 4; ++r)
        result.setRow(r, sl::float4(m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]));
    return result;
}

sl::Resource ImageResource(vk::Image image, vk::ImageView view, vk::Format format,
                           vk::Extent2D extent) {
    sl::Resource resource{sl::ResourceType::eTex2d, static_cast<VkImage>(image), nullptr,
                          static_cast<VkImageView>(view),
                          static_cast<u32>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
    resource.width = extent.width;
    resource.height = extent.height;
    resource.nativeFormat = static_cast<u32>(format);
    resource.mipLevels = 1;
    resource.arrayLayers = 1;
    resource.flags = 0;
    resource.usage = static_cast<u32>(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    return resource;
}
} // namespace

// The first NVIDIA GPU's LUID, which Streamline checks DLSS-G support for.
std::optional<LUID> NvidiaAdapter() {
    IDXGIFactory1* factory{};
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))))
        return std::nullopt;
    std::optional<LUID> luid;
    IDXGIAdapter1* adapter{};
    for (UINT i = 0; !luid && factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10DE &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            luid = desc.AdapterLuid;
        adapter->Release();
    }
    factory->Release();
    return luid;
}

PFN_vkGetInstanceProcAddr Load() {
    if (!Requested())
        return nullptr;
    if (Backend() == "fsr") {
        FsrFrameGen::Select();
        return nullptr;
    }
    // RenoDX makes the swapchain scRGB, where DLSS-G does not generate frames.
    if (RenoDxInstalled()) {
        LOG_INFO(Render_Vulkan, "[FRAME-GEN] RenoDX found: FSR frame generation");
        FsrFrameGen::Select();
        return nullptr;
    }
    // Without DLSS-G, FSR frame generation unless dlss.ini asks for DLSS-G only.
    const auto fail = [](std::string reason) -> PFN_vkGetInstanceProcAddr {
        LOG_WARNING(Render_Vulkan, "[FRAME-GEN] DLSS-G off: {}", reason);
        if (Backend() == "dlss")
            problem = std::move(reason);
        else
            FsrFrameGen::Select();
        return nullptr;
    };
    auto luid = NvidiaAdapter();
    if (!luid)
        return fail("DLSS Frame generation needs an NVIDIA RTX 40 or 50 series GPU.");
    const auto interposer = ExecutableDirectory() / "sl.interposer.dll";
    if (!std::filesystem::is_regular_file(interposer))
        return fail("The frame generation files (sl.*.dll) are missing next to shadPS4.exe.");
    if (!sl::security::verifyEmbeddedSignature(interposer.c_str()))
        return fail("sl.interposer.dll is not signed by NVIDIA.");
    api.module = LoadLibraryW(interposer.c_str());
    if (!api.module) {
        LOG_ERROR(Render_Vulkan, "[FRAME-GEN] sl.interposer.dll could not be loaded");
        return nullptr;
    }
    PFN_vkGetInstanceProcAddr get_instance_proc_addr{};
    get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(api.module, "vkGetInstanceProcAddr"));
    if (!Export(api.init, "slInit") || !Export(api.shutdown, "slShutdown") ||
        !Export(api.new_frame, "slGetNewFrameToken") ||
        !Export(api.set_constants, "slSetConstants") || !Export(api.set_tags, "slSetTagForFrame") ||
        !Export(api.feature_function, "slGetFeatureFunction") ||
        !Export(api.feature_supported, "slIsFeatureSupported") || !get_instance_proc_addr) {
        LOG_ERROR(Render_Vulkan, "[FRAME-GEN] sl.interposer.dll is missing entry points");
        FreeLibrary(api.module);
        api = {};
        return nullptr;
    }

    static const std::wstring plugin_path = ExecutableDirectory().wstring();
    static const std::wstring log_path = (BbDlssSettingsPath().parent_path() / "log").wstring();
    static const wchar_t* plugin_paths[] = {plugin_path.c_str()};
    static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences preferences{};
    preferences.showConsole = false;
    preferences.logLevel = sl::LogLevel::eDefault;
    preferences.pathsToPlugins = plugin_paths;
    preferences.numPathsToPlugins = 1;
    preferences.pathToLogsAndData = log_path.c_str();
    preferences.logMessageCallback = LogMessage;
    // No over-the-air downloads; command buffer state is restored by the emulator.
    preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                        sl::PreferenceFlags::eUseManualHooking |
                        sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = u32(std::size(features));
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = "shadPS4";
    preferences.projectId = "6a1d7c55-3b8e-4f0a-9d62-bb0f5e1c7a21";
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    const auto result = api.init(preferences, sl::kSDKVersion);
    if (result != sl::Result::eOk) {
        FreeLibrary(api.module);
        api = {};
        return fail(fmt::format("NVIDIA Streamline did not start (error {}).", int(result)));
    }
    sl::AdapterInfo adapter{};
    adapter.deviceLUID = reinterpret_cast<uint8_t*>(&*luid);
    adapter.deviceLUIDSizeInBytes = sizeof(LUID);
    const auto supported = api.feature_supported(sl::kFeatureDLSS_G, adapter);
    if (supported != sl::Result::eOk) {
        api.shutdown();
        FreeLibrary(api.module);
        api = {};
        switch (supported) {
        case sl::Result::eErrorDriverOutOfDate:
            return fail("DLSS Frame generation needs a newer NVIDIA driver.");
        case sl::Result::eErrorOSOutOfDate:
        case sl::Result::eErrorOSDisabledHWS:
            return fail("DLSS Frame generation needs Windows 10 20H1 or newer with "
                        "Hardware-accelerated GPU scheduling turned on.");
        default:
            return fail("DLSS Frame generation needs an NVIDIA RTX 40 or 50 series GPU.");
        }
    }
    active = true;
    LOG_INFO(Render_Vulkan, "[FRAME-GEN] Streamline loaded");
    return get_instance_proc_addr;
}

bool Active() {
    return active || FsrFrameGen::Active();
}

void Shutdown() {
    FsrFrameGen::Shutdown();
    if (!active)
        return;
    active = false;
    api.shutdown();
}

void BeginFrame() {
    if (!active || !FeatureFunctions())
        return;
    if (api.new_frame(token, nullptr) != sl::Result::eOk || !token)
        return;
    frame_open = true;
    if (!reflex_applied || reflex_applied_mode != reflex_low_latency) {
        sl::ReflexOptions reflex{};
        reflex.mode = reflex_low_latency ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
        if (api.reflex_options(reflex) != sl::Result::eOk)
            LOG_WARNING(Render_Vulkan, "[FRAME-GEN] Reflex options were not accepted");
        reflex_applied = true;
        reflex_applied_mode = reflex_low_latency;
        LOG_INFO(Render_Vulkan, "[FRAME-GEN] Reflex low latency {}, sleep {}",
                 reflex_low_latency.load(), reflex_sleep.load());
    }
    if (reflex_sleep)
        api.reflex_sleep(*token);
    Marker(sl::PCLMarker::eSimulationStart);
    Marker(sl::PCLMarker::eSimulationEnd);
    Marker(sl::PCLMarker::eRenderSubmitStart);
}

void SetFrame(const FrameInputs* inputs, const OverlayInputs* overlays, vk::CommandBuffer command,
              vk::Extent2D backbuffer, vk::Rect2D game_area) {
    if (FsrFrameGen::Active()) {
        if (!enabled || paused_frames > 0)
            inputs = nullptr;
        if (paused_frames > 0)
            --paused_frames;
        FsrFrameGen::SetFrame(inputs, overlays, command, backbuffer, game_area);
        generating = inputs != nullptr;
        return;
    }
    if (!frame_open)
        return;
    const auto cmd = static_cast<VkCommandBuffer>(command);
    sl::DLSSGOptions options{};
    if (!enabled)
        inputs = nullptr;
    if (paused_frames > 0) {
        --paused_frames;
        inputs = nullptr;
    }
    options.mode = inputs ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    // Up to what Streamline reports for this card: 1 below RTX 50 series.
    options.numFramesToGenerate = std::min(multiplier.load(), max_multiplier.load()) - 1;
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    // Let DLSS-G run beside the emulator's queue instead of holding it until the generated frame
    // is done. Its inputs are copies in a ring of four, rewritten four frames later.
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockNoClientQueues;
    if (inputs) {
        // The formats and sizes of everything DLSS-G gets.
        options.mvecDepthWidth = inputs->render.width;
        options.mvecDepthHeight = inputs->render.height;
        options.colorWidth = backbuffer.width;
        options.colorHeight = backbuffer.height;
        options.mvecBufferFormat = u32(vk::Format::eR16G16Sfloat);
        options.depthBufferFormat = u32(vk::Format::eR32Sfloat);
        if (overlays) {
            options.colorBufferFormat = u32(overlays->hudless_format);
            options.hudLessBufferFormat = u32(overlays->hudless_format);
            options.uiBufferFormat = u32(vk::Format::eR16Sfloat);
        }
        sl::Constants constants{};
        constants.cameraViewToClip = Matrix(inputs->view_to_clip);
        constants.clipToCameraView = Matrix(inputs->clip_to_view);
        constants.clipToLensClip = Matrix({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
        constants.clipToPrevClip = Matrix(inputs->clip_to_prev_clip);
        constants.prevClipToClip = Matrix(inputs->prev_clip_to_clip);
        constants.jitterOffset = {inputs->jitter[0], inputs->jitter[1]};
        // Motion is in render pixels.
        constants.mvecScale = {1.0f / float(inputs->render.width),
                               1.0f / float(inputs->render.height)};
        constants.cameraPinholeOffset = {0.0f, 0.0f};
        constants.cameraPos = {inputs->position[0], inputs->position[1], inputs->position[2]};
        constants.cameraUp = {inputs->up[0], inputs->up[1], inputs->up[2]};
        constants.cameraRight = {inputs->right[0], inputs->right[1], inputs->right[2]};
        constants.cameraFwd = {inputs->forward[0], inputs->forward[1], inputs->forward[2]};
        constants.cameraNear = inputs->near_plane;
        constants.cameraFar = inputs->far_plane;
        constants.cameraFOV = inputs->fov_y;
        constants.cameraAspectRatio = inputs->aspect;
        constants.depthInverted = inputs->depth_inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.cameraMotionIncluded = sl::Boolean::eTrue;
        constants.motionVectors3D = sl::Boolean::eFalse;
        constants.reset = inputs->reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        constants.orthographicProjection = sl::Boolean::eFalse;
        constants.motionVectorsDilated = sl::Boolean::eFalse;
        constants.motionVectorsJittered = sl::Boolean::eFalse;
        api.set_constants(constants, *token, Viewport);

        auto depth = ImageResource(inputs->depth, inputs->depth_view, vk::Format::eR32Sfloat,
                                   inputs->render);
        auto motion = ImageResource(inputs->motion, inputs->motion_view, vk::Format::eR16G16Sfloat,
                                    inputs->render);
        const sl::Extent render_extent{0, 0, inputs->render.width, inputs->render.height};
        // Where the game image sits in the window (letterboxing, overlays aside).
        const sl::Extent game_extent{u32(game_area.offset.y), u32(game_area.offset.x),
                                     game_area.extent.width, game_area.extent.height};
        const bool subrect =
            game_area.offset.x != 0 || game_area.offset.y != 0 || game_area.extent != backbuffer;
        sl::ResourceTag tags[] = {
            sl::ResourceTag{&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent,
                            &render_extent},
            sl::ResourceTag{&motion, sl::kBufferTypeMotionVectors,
                            sl::ResourceLifecycle::eValidUntilPresent, &render_extent},
            sl::ResourceTag{nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{},
                            &game_extent},
        };
        api.set_tags(*token, Viewport, tags, subrect ? 3u : 2u,
                     reinterpret_cast<sl::CommandBuffer*>(cmd));
        if (overlays) {
            // Copied now: the frame image and the mask are reused before DLSS-G is done.
            auto hudless = ImageResource(overlays->hudless, overlays->hudless_view,
                                         overlays->hudless_format, overlays->extent);
            auto alpha = ImageResource(overlays->alpha, overlays->alpha_view,
                                       vk::Format::eR16Sfloat, overlays->extent);
            const sl::Extent full{0, 0, overlays->extent.width, overlays->extent.height};
            sl::ResourceTag overlay_tags[] = {
                sl::ResourceTag{&hudless, sl::kBufferTypeHUDLessColor,
                                sl::ResourceLifecycle::eOnlyValidNow, &full},
                sl::ResourceTag{&alpha, sl::kBufferTypeUIAlpha,
                                sl::ResourceLifecycle::eOnlyValidNow, &full},
            };
            api.set_tags(*token, Viewport, overlay_tags, 2u,
                         reinterpret_cast<sl::CommandBuffer*>(cmd));
            options.enableUserInterfaceRecomposition = sl::Boolean::eTrue;
        }
    } else {
        sl::ResourceTag tags[] = {
            sl::ResourceTag{nullptr, sl::kBufferTypeDepth,
                            sl::ResourceLifecycle::eValidUntilPresent},
            sl::ResourceTag{nullptr, sl::kBufferTypeMotionVectors,
                            sl::ResourceLifecycle::eValidUntilPresent},
            sl::ResourceTag{nullptr, sl::kBufferTypeHUDLessColor,
                            sl::ResourceLifecycle::eOnlyValidNow},
            sl::ResourceTag{nullptr, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eOnlyValidNow},
        };
        api.set_tags(*token, Viewport, tags, 4u, reinterpret_cast<sl::CommandBuffer*>(cmd));
    }
    if (options.mode != last_mode)
        LOG_INFO(Render_Vulkan, "[FRAME-GEN] {}", inputs ? "On" : "Off");
    last_mode = options.mode;
    api.dlssg_options(Viewport, options);
    generating = inputs != nullptr;
}

void EndSubmit() {
    Marker(sl::PCLMarker::eRenderSubmitEnd);
}

void BeginPresent() {
    Marker(sl::PCLMarker::ePresentStart);
}

void EndPresent() {
    if (FsrFrameGen::Active()) {
        if (generating)
            last_generated = std::chrono::steady_clock::now();
        CountPresent(FsrFrameGen::TakeShown());
        return;
    }
    Marker(sl::PCLMarker::ePresentEnd);
    if (!frame_open) {
        generating = false;
        CountPresent(1);
        return;
    }
    frame_open = false;
    // Frames shown since the last query, generated ones included.
    u32 shown = 1;
    sl::DLSSGState state{};
    if (api.dlssg_state(Viewport, state, nullptr) == sl::Result::eOk) {
        const u32 max = std::clamp<u32>(state.numFramesToGenerateMax, 1, 5) + 1;
        if (max != max_multiplier.exchange(max))
            LOG_INFO(Render_Vulkan, "[FRAME-GEN] Up to {}x", max);
        if (last_mode == sl::DLSSGMode::eOn) {
            shown = state.numFramesActuallyPresented;
            if ((state.status != sl::DLSSGStatus::eOk && ++frames % 600 == 1) ||
                status_logged < 3) {
                LOG_INFO(Render_Vulkan,
                         "[FRAME-GEN] status={} presented={} maxGenerated={} minSize={}",
                         u32(state.status), state.numFramesActuallyPresented,
                         state.numFramesToGenerateMax, state.minWidthOrHeight);
                ++status_logged;
            }
        }
    }
    if (generating)
        last_generated = std::chrono::steady_clock::now();
    CountPresent(shown);
}

void SetEnabled(bool value) {
    enabled = value;
}

void Pause() {
    paused_frames = 30;
}

bool SkipRepeatedFrame() {
    return generating &&
           std::chrono::steady_clock::now() - last_generated < std::chrono::milliseconds{100};
}

void SetReflex(bool low_latency, bool sleep) {
    if (low_latency != reflex_low_latency || sleep != reflex_sleep)
        reflex_applied = false;
    reflex_low_latency = low_latency;
    reflex_sleep = sleep;
}

#else

PFN_vkGetInstanceProcAddr Load() {
    if (Requested() && Backend() != "dlss")
        FsrFrameGen::Select();
    return nullptr;
}
bool Active() {
    return FsrFrameGen::Active();
}
void Shutdown() {
    FsrFrameGen::Shutdown();
}
void BeginFrame() {}
void SetFrame(const FrameInputs* inputs, const OverlayInputs* overlays, vk::CommandBuffer command,
              vk::Extent2D backbuffer, vk::Rect2D game_area) {
    FsrFrameGen::SetFrame(inputs, overlays, command, backbuffer, game_area);
    generating = FsrFrameGen::Active() && inputs != nullptr;
}
void EndSubmit() {}
void BeginPresent() {}
void EndPresent() {
    CountPresent(FsrFrameGen::TakeShown());
}
void SetEnabled(bool) {}
void Pause() {}
void SetReflex(bool, bool) {}
bool SkipRepeatedFrame() {
    return false;
}

#endif

} // namespace Vulkan::FrameGen
