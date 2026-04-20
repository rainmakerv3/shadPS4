//  SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_common.h"

#include <SDL3/SDL_vulkan.h>
#include <cmrc/cmrc.hpp>
#include <imgui.h>
#include <stb_image.h>

#include "big_picture.h"
#include "common/logging/log.h"
#include "core/devtools/layer.h"
#include "core/file_format/psf.h"
#include "emulator.h"
#include "imgui/imgui_std.h"
#include "imgui/renderer/imgui_impl_sdl3_bpm.h"
#include "imgui/renderer/imgui_impl_vulkan_big_picture.h"
#include "imgui/renderer/texture_manager.h"
#include "settings_dialog_imgui.h"

#include "imgui_fonts/notosansjp_regular.ttf.g.cpp"
#include "imgui_fonts/proggyvector_regular.ttf.g.cpp"

CMRC_DECLARE(res);

namespace BigPictureMode {

const float gameImageSize = 200.f;

bool done = false;
bool showSettings = false;

std::filesystem::path runEbootPath = "";
std::vector<Game> gameVec = {};

float uiScale = 1.0f;

// Data
vk::Instance g_Instance = VK_NULL_HANDLE;
vk::PhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
vk::Device g_Device = VK_NULL_HANDLE;
uint32_t g_QueueFamily = (uint32_t)-1;
vk::Queue g_Queue = VK_NULL_HANDLE;
vk::DebugReportCallbackEXT g_DebugReport = VK_NULL_HANDLE;
vk::PipelineCache g_PipelineCache = VK_NULL_HANDLE;
vk::DescriptorPool g_DescriptorPool = VK_NULL_HANDLE;

ImGui_ImplVulkanH_Window g_MainWindowData;
uint32_t g_MinImageCount = 2;
bool g_SwapChainRebuild = false;

vk::detail::DynamicLoader dl;
vk::detail::DispatchLoaderDynamic loader;

static void check_vk_result(vk::Result err) {
    if (err == vk::Result::eSuccess)
        return;
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (static_cast<int>(err) < 0)
        abort();
}

static bool IsExtensionAvailable(const ImVector<vk::ExtensionProperties>& properties,
                                 const char* extension) {
    for (const vk::ExtensionProperties& p : properties)
        if (strcmp(p.extensionName, extension) == 0)
            return true;
    return false;
}

static void SetupVulkan(ImVector<const char*> instance_extensions) {
    vk::Result err;
    PFN_vkGetInstanceProcAddr getProc =
        dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    if (getProc == nullptr) {
        fprintf(stderr, "Failed to load vkGetInstanceProcAddr\n");
        return; // or handle error
    }
    // initialize dispatch with the global loader function so wrapper calls work
    loader.init(getProc);

    // Create Vulkan Instance
    {
        vk::InstanceCreateInfo create_info = {};
        create_info.sType = vk::StructureType::eInstanceCreateInfo;

        // Enumerate available extensions
        uint32_t properties_count;
        ImVector<vk::ExtensionProperties> properties;
        err = vk::enumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr, loader);
        check_vk_result(err);

        properties.resize(properties_count);
        err = vk::enumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data,
                                                       loader);
        check_vk_result(err);

        // Enable required extensions
        if (IsExtensionAvailable(properties,
                                 VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
        }
#endif

        // Create Vulkan Instance
        create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vk::createInstance(&create_info, nullptr, &g_Instance, loader);
        check_vk_result(err);
    }

    loader.init(g_Instance);

    bool funcsLoaded = ImGui_ImplVulkan_LoadFunctions(
        VK_API_VERSION_1_3,
        [](const char* function_name, void* user_data) -> PFN_vkVoidFunction {
            auto proc = loader.vkGetInstanceProcAddr;
            if (proc == nullptr)
                return nullptr;
            return (PFN_vkVoidFunction)proc((VkInstance)user_data, function_name);
        },
        (void*)g_Instance);
    if (!funcsLoaded) {
        fprintf(stderr, "ImGui_ImplVulkan_LoadFunctions failed\n");
    }

    // Select Physical Device (GPU)
    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);

    // Select graphics queue family
    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
    IM_ASSERT(g_QueueFamily != (uint32_t)-1);

    // Create Logical Device (with 1 queue)
    {
        ImVector<const char*> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");

        // Enumerate physical device extension
        uint32_t properties_count;
        ImVector<vk::ExtensionProperties> properties;
        err = g_PhysicalDevice.enumerateDeviceExtensionProperties(nullptr, &properties_count,
                                                                  nullptr, loader);
        check_vk_result(err);

        properties.resize(properties_count);
        err = g_PhysicalDevice.enumerateDeviceExtensionProperties(nullptr, &properties_count,
                                                                  properties.Data, loader);
        check_vk_result(err);

#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

        const float queue_priority[] = {1.0f};
        vk::DeviceQueueCreateInfo queue_info[1] = {};
        queue_info[0].sType = vk::StructureType::eDeviceQueueCreateInfo;
        queue_info[0].queueFamilyIndex = g_QueueFamily;
        queue_info[0].queueCount = 1;
        queue_info[0].pQueuePriorities = queue_priority;
        vk::DeviceCreateInfo create_info = {};
        create_info.sType = vk::StructureType::eDeviceCreateInfo;
        create_info.queueCreateInfoCount = sizeof(queue_info) / sizeof(queue_info[0]);
        create_info.pQueueCreateInfos = queue_info;
        create_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
        create_info.ppEnabledExtensionNames = device_extensions.Data;
        err = g_PhysicalDevice.createDevice(&create_info, nullptr, &g_Device, loader);
        check_vk_result(err);

        loader.init(g_Device);

        g_Device.getQueue(g_QueueFamily, 0, &g_Queue, loader);
    }

    // Create Descriptor Pool
    // If you wish to load e.g. additional textures you may need to alter pools sizes and maxSets.
    {
        vk::DescriptorPoolSize pool_sizes[] = {
            {vk::DescriptorType::eCombinedImageSampler,
             IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE},
        };
        vk::DescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = vk::StructureType::eDescriptorPoolCreateInfo;
        pool_info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
        pool_info.maxSets = 0;
        for (vk::DescriptorPoolSize& pool_size : pool_sizes)
            pool_info.maxSets += pool_size.descriptorCount;
        pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        err = g_Device.createDescriptorPool(&pool_info, nullptr, &g_DescriptorPool, loader);
        check_vk_result(err);
    }
}

// All the ImGui_ImplVulkanH_XXX structures/functions are optional helpers used by the demo.
// Your real engine/app may not use them.
static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width,
                              int height) {
    wd->Surface = surface;

    // Check for WSI support
    vk::Bool32 res;
    vk::Result err =
        g_PhysicalDevice.getSurfaceSupportKHR(g_QueueFamily, wd->Surface, &res, loader);
    check_vk_result(err);

    if (res != VK_TRUE) {
        fprintf(stderr, "Error no WSI support on physical device 0\n");
        exit(-1);
    }

    // Select Surface Format
    std::vector<vk::Format> requestSurfaceImageFormat = {
        vk::Format::eB8G8R8A8Unorm, vk::Format::eR8G8B8A8Unorm, vk::Format::eB8G8R8Unorm,
        vk::Format::eR8G8B8Unorm};

    const vk::ColorSpaceKHR requestSurfaceColorSpace =
        vk::ColorSpaceKHR::eVkColorspaceSrgbNonlinear;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        g_PhysicalDevice, wd->Surface,
        reinterpret_cast<const VkFormat*>(requestSurfaceImageFormat.data()),
        requestSurfaceImageFormat.size(), static_cast<VkColorSpaceKHR>(requestSurfaceColorSpace));

    // Select Present Mode
#ifdef APP_USE_UNLIMITED_FRAME_RATE
    VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR,
                                        VK_PRESENT_MODE_FIFO_KHR};
#else
    VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
#endif
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, wd->Surface,
                                                          present_modes, sizeof(present_modes));
    // printf("[vulkan] Selected PresentMode = %d\n", wd->PresentMode);

    // Create SwapChain, RenderPass, Framebuffer, etc.
    IM_ASSERT(g_MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd,
                                           g_QueueFamily, nullptr, width, height, g_MinImageCount);
}

static void CleanupVulkan() {
    g_Device.destroyDescriptorPool(g_DescriptorPool, nullptr, loader);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    // Remove the debug report callback
    auto f_vkDestroyDebugReportCallbackEXT =
        (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(
            g_Instance, "vkDestroyDebugReportCallbackEXT");
    f_vkDestroyDebugReportCallbackEXT(g_Instance, g_DebugReport, g_Allocator);
#endif // APP_USE_VULKAN_DEBUG_REPORT

    g_Device.destroy(nullptr, loader);
    g_Instance.destroy(nullptr, loader);
}

static void CleanupVulkanWindow() {
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, nullptr);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data) {
    vk::Semaphore image_acquired_semaphore =
        wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    vk::Semaphore render_complete_semaphore =
        wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    vk::Result err =
        g_Device.acquireNextImageKHR(wd->Swapchain, UINT64_MAX, image_acquired_semaphore,
                                     VK_NULL_HANDLE, &wd->FrameIndex, loader);
    if (err == vk::Result::eErrorOutOfDateKHR || err == vk::Result::eSuboptimalKHR)
        g_SwapChainRebuild = true;
    if (err == vk::Result::eErrorOutOfDateKHR)
        return;
    if (err != vk::Result::eSuboptimalKHR)
        check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    vk::CommandBuffer cmdbuf = fd->CommandBuffer;
    {
        err = g_Device.waitForFences(1, reinterpret_cast<const vk::Fence*>(&fd->Fence), VK_TRUE,
                                     UINT64_MAX, loader);
        check_vk_result(err);

        err = g_Device.resetFences(1, reinterpret_cast<const vk::Fence*>(&fd->Fence), loader);
        check_vk_result(err);
    }
    {
        err = g_Device.resetCommandPool(fd->CommandPool, {}, loader);
        check_vk_result(err);

        vk::CommandBufferBeginInfo info = {};
        info.sType = vk::StructureType::eCommandBufferBeginInfo;
        info.flags |= vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        err = cmdbuf.begin(&info, loader);
        check_vk_result(err);
    }
    {
        vk::RenderPassBeginInfo info = {};
        info.sType = vk::StructureType::eRenderPassBeginInfo;
        info.renderPass = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues = reinterpret_cast<const vk::ClearValue*>(&wd->ClearValue);
        cmdbuf.beginRenderPass(&info, vk::SubpassContents::eInline, loader);
    }

    // Record dear imgui primitives into command buffer
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    // Submit command buffer
    cmdbuf.endRenderPass(loader);
    {
        vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        vk::SubmitInfo info = {};
        info.sType = vk::StructureType::eSubmitInfo;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &cmdbuf;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = cmdbuf.end(loader);
        check_vk_result(err);
        err = g_Queue.submit(1, &info, fd->Fence, loader);
        check_vk_result(err);
    }
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd) {
    if (g_SwapChainRebuild)
        return;
    vk::Semaphore render_complete_semaphore =
        wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    vk::SwapchainKHR hppSwapchain(wd->Swapchain);

    vk::PresentInfoKHR info = {};
    info.sType = vk::StructureType::ePresentInfoKHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &hppSwapchain;
    info.pImageIndices = &wd->FrameIndex;
    vk::Result err = g_Queue.presentKHR(&info, loader);
    if (err == vk::Result::eErrorOutOfDateKHR || err == vk::Result::eSuboptimalKHR)
        g_SwapChainRebuild = true;
    if (err == vk::Result::eErrorOutOfDateKHR)
        return;
    if (err != vk::Result::eSuboptimalKHR)
        check_vk_result(err);
    wd->SemaphoreIndex =
        (wd->SemaphoreIndex + 1) % wd->SemaphoreCount; // Now we can use the next set of semaphores
}

void Launch(char* exeName) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR(ImGui, "SDL_INIT_VIDEO Error: {}", SDL_GetError());
        SDL_Quit();
        return;
    }

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        LOG_ERROR(ImGui, "SDL_INIT_GAMEPAD Error: {}", SDL_GetError());
        SDL_Quit();
        return;
    }

    // Create window with Vulkan graphics context
    SDL_WindowFlags window_flags =
        (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                          SDL_WINDOW_HIDDEN);
    SDL_Window* window = SDL_CreateWindow("shadPS4 Big Picture Mode", 1280, 720, window_flags);
    if (window == nullptr) {
        // printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return;
    }

    ImVector<const char*> extensions;
    {
        uint32_t sdl_extensions_count = 0;
        const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_extensions_count);
        for (uint32_t n = 0; n < sdl_extensions_count; n++)
            extensions.push_back(sdl_extensions[n]);
    }
    SetupVulkan(extensions);

    // Create Window Surface
    VkSurfaceKHR rawSurface;
    if (SDL_Vulkan_CreateSurface(window, g_Instance, nullptr, &rawSurface) == 0) {
        printf("Failed to create Vulkan surface.\n");
        return;
    }

    vk::SurfaceKHR surface = vk::SurfaceKHR(rawSurface);

    // Create Framebuffers
    int w, h;
    SDL_GetWindowSize(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVulkanWindow(wd, surface, w, h);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigNavCursorVisibleAlways = true;

    ImFontConfig configBase;
    configBase.OversampleH = 3;
    configBase.OversampleV = 3;

    ImFontConfig configMerge;
    configMerge.OversampleH = 3;
    configMerge.OversampleV = 3;
    configMerge.MergeMode = true;

    // tm symbol
    const ImWchar icon_ranges[] = {0x2122, 0x2122, 0};
    ImFont* myFont = io.Fonts->AddFontFromMemoryCompressedTTF(
        imgui_font_notosansjp_regular_compressed_data,
        imgui_font_notosansjp_regular_compressed_size, 32.0f, &configBase, icon_ranges);

    io.Fonts->AddFontFromMemoryCompressedTTF(imgui_font_notosansjp_regular_compressed_data,
                                             imgui_font_notosansjp_regular_compressed_size, 32.0f,
                                             &configMerge, io.Fonts->GetGlyphRangesDefault());

    io.Fonts->Build();

    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);         // black
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.40f, 0.70f, 1.00f);           // blue
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.50f, 0.85f, 1.00f);    // lighter blue
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.26f, 0.59f, 0.98f, 0.80f); // another light  blue
    colors[ImGuiCol_SliderGrab] = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);       // another light  blue

    style.WindowRounding = 0.0f;
    style.FrameRounding = 5.0f * uiScale;
    style.ItemSpacing = ImVec2(10.0f * uiScale, 10.0f * uiScale);
    style.FramePadding = ImVec2(10.0f * uiScale, 10.0f * uiScale);
    style.FrameBorderSize = 2.5f * uiScale;
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = ImVec2(20.0f * uiScale, 20.0f * uiScale);
    style.GrabMinSize = 20.0f * uiScale;

    // Setup Platform/Renderer backends
    ImGui_ImplSDL3_InitForVulkan(window);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion = VK_API_VERSION_1_3;
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.PipelineCache = g_PipelineCache;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.RenderPass = wd->RenderPass;
    init_info.Subpass = 0;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.Allocator = nullptr;
    init_info.CheckVkResultFn = nullptr;
    ImGui_ImplVulkan_Init(&init_info);
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    uiScale = static_cast<float>(EmulatorSettings.GetBigPictureScale() / 1000.f);

    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                done = true;
            }
        }

        int fb_width, fb_height;
        SDL_GetWindowSize(window, &fb_width, &fb_height);
        if (fb_width > 0 && fb_height > 0 &&
            (g_SwapChainRebuild || g_MainWindowData.Width != fb_width ||
             g_MainWindowData.Height != fb_height)) {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device,
                                                   &g_MainWindowData, g_QueueFamily, nullptr,
                                                   fb_width, fb_height, g_MinImageCount);
            g_MainWindowData.FrameIndex = 0;
            g_SwapChainRebuild = false;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::PushFont(myFont);

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGuiWindowFlags window_flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::Begin("Game Window", &done, window_flags);

        if (ImGui::IsWindowAppearing()) {
            GetGameInfo(gameVec, false);
        }

        ImGui::DrawPrettyBackground();
        ImGui::SetWindowFontScale(uiScale);

        ImGuiWindowFlags child_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NavFlattened;

        if (ImGui::IsWindowAppearing()) {
            ImGui::SetNextWindowFocus();
        }

        ImGui::BeginChild("ContentRegion", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true,
                          child_flags);
        Overlay::TextCentered("Select Game");
        ImGui::Dummy(ImVec2(0.0f, 10.f * uiScale));

        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }

        SetGameIcons(gameVec);
        ImGui::EndChild();
        ImGui::Separator();

        ImGui::SetNextItemWidth(300.0f * uiScale);

        static float sliderScale = 1.0f;
        if (ImGui::IsWindowAppearing()) {
            sliderScale = uiScale;
        }
        ImGui::SliderFloat("UI Scale", &sliderScale, 0.25f, 3.0f);

        // Only update when user is not interacting with slider
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            uiScale = sliderScale;
        }

        ImGui::SameLine();

        // Align buttons right
        float buttonsWidth = ImGui::CalcTextSize("Settings").x + ImGui::CalcTextSize("Exit").x +
                             ImGui::GetStyle().FramePadding.x * 4.0f +
                             ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - buttonsWidth);

        if (ImGui::Button("Settings")) {
            showSettings = true;
        }

        ImGui::SameLine();

        if (ImGui::Button("Exit")) {
            ImGui::OpenPopup("Confirm Exit");
        }

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal("Confirm Exit", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("This will exit shadPS4!\nAre you sure?");
            ImGui::Separator();

            if (ImGui::Button("OK", ImVec2(120 * uiScale, 0))) {
                ImGui::CloseCurrentPopup();
                done = true;
            }
            ImGui::SameLine();

            if (ImGui::Button("Cancel", ImVec2(120 * uiScale, 0))) {
                ImGui::CloseCurrentPopup();
            }

            if (ImGui::IsWindowAppearing()) {
                ImGui::SetItemDefaultFocus();
            }

            ImGui::EndPopup();
        }

        if (showSettings) {
            EmulatorSettings.SetBigPictureScale(static_cast<int>(uiScale * 1000));
            EmulatorSettings.Save();
            Settings::DrawSettings(&showSettings, false);

            // update when settings dialog closed
            if (!showSettings) {
                uiScale = static_cast<float>(EmulatorSettings.GetBigPictureScale() / 1000.f);
                sliderScale = uiScale;
                GetGameInfo(gameVec, false);
            }
        }

        ImGui::PopFont();
        ImGui::End();

        ImGui::Render();

        ImDrawData* main_draw_data = ImGui::GetDrawData();
        const bool main_is_minimized =
            (main_draw_data->DisplaySize.x <= 0.0f || main_draw_data->DisplaySize.y <= 0.0f);
        wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
        wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
        wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
        wd->ClearValue.color.float32[3] = clear_color.w;
        if (!main_is_minimized)
            FrameRender(wd, main_draw_data);

        // Update and Render additional Platform Windows
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        // Present Main Platform Window
        if (!main_is_minimized)
            FramePresent(wd);
    }

    vk::Result err = g_Device.waitIdle(loader);
    check_vk_result(err);

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();

    ImGui::DestroyContext();
    CleanupVulkanWindow();
    CleanupVulkan();
    SDL_DestroyWindow(window);
    SDL_Quit();

    EmulatorSettings.SetBigPictureScale(static_cast<int>(uiScale * 1000));
    EmulatorSettings.Save();

    if (runEbootPath != "") {
        auto* emulator = Common::Singleton<Core::Emulator>::Instance();
        emulator->executableName = exeName;
        emulator->Run(runEbootPath);
    } else {
        std::quick_exit(0);
    }
}

void SetGameIcons(std::vector<Game>& games) {
    ImGuiStyle& style = ImGui::GetStyle();
    const float maxAvailableWidth = ImGui::GetContentRegionAvail().x;
    const float itemSpacing = style.ItemSpacing.x; // already scaled
    const float padding = 10.0f * uiScale;
    float rowContentWidth = gameImageSize * uiScale + itemSpacing;

    for (int i = 0; i < games.size(); i++) {
        ImGui::BeginGroup();
        std::string ButtonName = "Button" + std::to_string(i);
        const char* ButtonNameChar = ButtonName.c_str();

        bool isNextItemFocused = (ImGui::GetID(ButtonNameChar) == ImGui::GetFocusID());
        bool popColor = false;
        if (isNextItemFocused) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
            popColor = true;
        }

        if (games[i].iconTexture.GetTexture().im_id != nullptr) {
            if (ImGui::ImageButton(ButtonNameChar, games[i].iconTexture.GetTexture().im_id,
                                   ImVec2(gameImageSize * uiScale, gameImageSize * uiScale))) {
                done = true;
                runEbootPath = games[i].ebootPath;
            }
        } else {
            if (ImGui::Button(ButtonNameChar)) {
                done = true;
                runEbootPath = games[i].ebootPath;
            }
        }

        if (popColor) {
            ImGui::PopStyleColor();
        }

        // Scroll to item only when newly-focused
        if (ImGui::IsItemFocused() && !games[i].focusState) {
            ImGui::SetScrollHereY(0.5f);
        }

        if (ImGui::IsWindowFocused())
            games[i].focusState = ImGui::IsItemFocused();

        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + gameImageSize * uiScale);
        ImGui::TextWrapped("%s", games[i].title.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();

        // Use same line if content fits horizontally, move to next line if not
        rowContentWidth += (gameImageSize * uiScale + itemSpacing * 2 + padding);
        if (rowContentWidth < maxAvailableWidth) {
            ImGui::SameLine(0.0f, padding);
        } else {
            ImGui::Dummy(ImVec2(0.0f, padding));
            rowContentWidth = gameImageSize * uiScale + itemSpacing;
        }
    }
}

std::filesystem::path UpdateChecker(const std::string sceItem, std::filesystem::path game_folder) {
    std::filesystem::path outputPath;
    auto update_folder = game_folder;
    update_folder += "-UPDATE";

    auto patch_folder = game_folder;
    patch_folder += "-patch";

    if (std::filesystem::exists(update_folder / "sce_sys" / sceItem)) {
        outputPath = update_folder / "sce_sys" / sceItem;
    } else if (std::filesystem::exists(patch_folder / "sce_sys" / sceItem)) {
        outputPath = patch_folder / "sce_sys" / sceItem;
    } else {
        outputPath = game_folder / "sce_sys" / sceItem;
    }

    return outputPath;
}

void GetGameInfo(std::vector<Game>& games, bool isSettingsInfo, ImGui::RefCountedTexture texture) {
    games.clear();
    if (isSettingsInfo) {
        Game global;
        global.title = "Global";
        global.iconTexture = texture;
        global.focusState = false;
        games.push_back(global);
    }

    for (const auto& installLoc : EmulatorSettings.GetAllGameInstallDirs()) {
        if (installLoc.enabled && std::filesystem::exists(installLoc.path)) {
            for (const auto& entry : std::filesystem::directory_iterator(installLoc.path)) {
                if (entry.path().filename().string().ends_with("-UPDATE") ||
                    entry.path().filename().string().ends_with("-patch") || !entry.is_directory()) {
                    continue;
                }

                Game game;
                PSF psf;
                const std::string sfoFileName = "param.sfo";
                std::filesystem::path sfoPath = UpdateChecker(sfoFileName, entry.path());

                if (psf.Open(sfoPath)) {
                    if (const auto title = psf.GetString("TITLE"); title.has_value()) {
                        game.title = *title;
                    }

                    if (const auto title_id = psf.GetString("TITLE_ID"); title_id.has_value()) {
                        game.serial = *title_id;
                    }
                } else {
                    continue;
                }

                const std::string iconFileName = "icon0.png";
                std::filesystem::path iconPath = UpdateChecker(iconFileName, entry.path());
                LoadTextureFromFile(iconPath, game.iconTexture);

                game.ebootPath = entry.path() / "eboot.bin";
                game.focusState = false;
                games.push_back(game);
            }
        }
    }

    // Keep global settings at the start if it's added
    auto start = isSettingsInfo ? games.begin() + 1 : (games.begin());
    std::sort(start, games.end(), [](const Game& a, const Game& b) {
        return a.title < b.title; // Alphabetical order
    });
}

void LoadTextureFromFile(std::filesystem::path filePath, ImGui::RefCountedTexture& texture) {
    std::ifstream file(filePath, std::ios::binary);
    std::vector<u8> data =
        std::vector<u8>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    LoadTexture(data, texture);
}

void LoadTexture(std::vector<u8> data, ImGui::RefCountedTexture& texture) {
    texture = ImGui::RefCountedTexture::DecodePngTexture(data);
}

} // namespace BigPictureMode
