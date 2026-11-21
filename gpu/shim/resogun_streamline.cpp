// SPDX-License-Identifier: GPL-2.0-or-later
// NVIDIA DLSS Frame Generation through Streamline: see resogun_streamline.h.
//
// The game gives no motion vectors or camera, so for now DLSS-G gets zero motion vectors, a depth
// of "far everywhere" and identity matrices: it then rests on its own optical flow, like the
// driver's Smooth Motion. Real inputs (the game's depth and its camera) are the next step.

#include "resogun_streamline.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
#include <windows.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>
// windows.h names a macro MemoryBarrier, which hides vk::MemoryBarrier.
#undef MemoryBarrier
#endif

#include "resogun_settings.h"

namespace ResogunStreamline {

#ifdef _WIN32
namespace {

struct Api {
    PFun_slInit* init{};
    PFun_slIsFeatureSupported* is_supported{};
    PFun_slGetNewFrameToken* new_frame{};
    PFun_slSetTagForFrame* set_tags{};
    PFun_slSetConstants* set_constants{};
    PFun_slGetFeatureFunction* feature_function{};
    PFun_slDLSSGSetOptions* dlssg_set_options{};
    PFun_slDLSSGGetState* dlssg_get_state{};
    PFun_slPCLSetMarker* pcl_marker{};
    PFun_slReflexSetOptions* reflex_set_options{};
};

Api api;
bool loaded = false;
int supported = -1; // -1 not checked yet (needs the device), then 0 or 1
bool generating = false;
sl::FrameToken* token = nullptr;
uint32_t frame_index = 0;

struct Inputs {
    vk::Device device;
    vk::Extent2D extent{};
    vk::Image image[2];
    vk::DeviceMemory memory[2];
    vk::ImageView view[2];
    bool cleared = false;
};
Inputs inputs; // [0] depth (R32F), [1] motion vectors (RG16F)
constexpr vk::Format InputFormats[2] = {vk::Format::eR32Sfloat, vk::Format::eR16G16Sfloat};
constexpr vk::ImageUsageFlags InputUsage = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eTransferDst |
                                           vk::ImageUsageFlagBits::eTransferSrc;

std::atomic<uint64_t> presents{0}, game_frames_generated{0};

void LogMessage(sl::LogType type, const char* msg) {
    if (type == sl::LogType::eInfo) return;
    std::printf("Streamline %s: %s", type == sl::LogType::eError ? "error" : "warning", msg);
    const size_t n = std::strlen(msg);
    if (n == 0 || msg[n - 1] != '\n') std::printf("\n");
}

template <typename F>
bool Feature(sl::Feature feature, const char* name, F*& out) {
    void* fn = nullptr;
    if (api.feature_function(feature, name, fn) != sl::Result::eOk || !fn) {
        std::printf("Streamline: %s missing\n", name);
        return false;
    }
    out = reinterpret_cast<F*>(fn);
    return true;
}

void DestroyInputs() {
    if (!inputs.device) return;
    (void)inputs.device.waitIdle();
    for (int i = 0; i < 2; ++i) {
        if (inputs.view[i]) inputs.device.destroyImageView(inputs.view[i]);
        if (inputs.image[i]) inputs.device.destroyImage(inputs.image[i]);
        if (inputs.memory[i]) inputs.device.freeMemory(inputs.memory[i]);
        inputs.view[i] = nullptr;
        inputs.image[i] = nullptr;
        inputs.memory[i] = nullptr;
    }
    inputs.extent = vk::Extent2D{};
}

bool CreateInputs(vk::Device device, vk::PhysicalDevice physical, vk::Extent2D extent) {
    DestroyInputs();
    inputs.device = device;
    const auto props = physical.getMemoryProperties();
    for (int i = 0; i < 2; ++i) {
        const vk::ImageCreateInfo info{
            .imageType = vk::ImageType::e2D,
            .format = InputFormats[i],
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = InputUsage,
            .initialLayout = vk::ImageLayout::eUndefined,
        };
        auto [ir, image] = device.createImage(info);
        if (ir != vk::Result::eSuccess) return false;
        inputs.image[i] = image;
        const auto req = device.getImageMemoryRequirements(image);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < props.memoryTypeCount; ++t) {
            if ((req.memoryTypeBits >> t & 1) &&
                (props.memoryTypes[t].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
                type = t;
                break;
            }
        }
        if (type == UINT32_MAX) return false;
        auto [mr, memory] = device.allocateMemory({.allocationSize = req.size, .memoryTypeIndex = type});
        if (mr != vk::Result::eSuccess) return false;
        inputs.memory[i] = memory;
        if (device.bindImageMemory(image, memory, 0) != vk::Result::eSuccess) return false;
        auto [vr, view] = device.createImageView({
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = InputFormats[i],
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
        if (vr != vk::Result::eSuccess) return false;
        inputs.view[i] = view;
    }
    inputs.extent = extent;
    inputs.cleared = false;
    return true;
}

sl::Resource InputResource(int i) {
    sl::Resource r(sl::ResourceType::eTex2d, static_cast<VkImage>(inputs.image[i]),
                   static_cast<VkDeviceMemory>(inputs.memory[i]), static_cast<VkImageView>(inputs.view[i]),
                   static_cast<uint32_t>(VK_IMAGE_LAYOUT_GENERAL));
    r.width = inputs.extent.width;
    r.height = inputs.extent.height;
    r.nativeFormat = static_cast<uint32_t>(InputFormats[i]);
    r.mipLevels = 1;
    r.arrayLayers = 1;
    r.usage = static_cast<uint32_t>(static_cast<VkImageUsageFlags>(InputUsage));
    return r;
}

void Marker(sl::PCLMarker marker) {
    if (token) api.pcl_marker(marker, *token);
}

} // namespace

std::string VulkanLibrary() {
    if (!ResogunSettings::Get().startup_frame_generation) return {};
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    const std::filesystem::path dir = std::filesystem::path(exe).parent_path();
    const std::filesystem::path interposer = dir / L"sl.interposer.dll";
    if (!std::filesystem::exists(interposer)) {
        std::printf("Frame generation: sl.interposer.dll is not beside the program; off\n");
        return {};
    }
    if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
        std::printf("Frame generation: sl.interposer.dll is not signed by NVIDIA; off\n");
        return {};
    }
    HMODULE mod = LoadLibraryW(interposer.c_str());
    if (!mod) {
        std::printf("Frame generation: sl.interposer.dll did not load; off\n");
        return {};
    }
    const auto proc = [mod](auto& out, const char* name) {
        out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(GetProcAddress(mod, name));
        return out != nullptr;
    };
    if (!proc(api.init, "slInit") || !proc(api.is_supported, "slIsFeatureSupported") ||
        !proc(api.new_frame, "slGetNewFrameToken") || !proc(api.set_tags, "slSetTagForFrame") ||
        !proc(api.set_constants, "slSetConstants") || !proc(api.feature_function, "slGetFeatureFunction")) {
        std::printf("Frame generation: sl.interposer.dll lacks the Streamline API; off\n");
        FreeLibrary(mod);
        return {};
    }
    static const std::wstring plugin_dir = dir.wstring();
    static const wchar_t* plugin_paths[] = {plugin_dir.c_str()};
    static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences pref{};
    pref.showConsole = false;
    // RESOGUN_SL_LOG=<folder>: Streamline's verbose log files there.
    static std::wstring log_dir;
    if (const char* v = std::getenv("RESOGUN_SL_LOG"); v && *v) {
        log_dir = std::filesystem::path(v).wstring();
        pref.logLevel = sl::LogLevel::eVerbose;
        pref.pathToLogsAndData = log_dir.c_str();
    } else {
        pref.logLevel = sl::LogLevel::eDefault;
    }
    pref.pathsToPlugins = plugin_paths;
    pref.numPathsToPlugins = 1;
    pref.logMessageCallback = LogMessage;
    // No over-the-air plugin downloads: only the DLLs shipped beside the program.
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eDisableDebugText |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = uint32_t(std::size(features));
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = "6aa7f259-a329-4733-a6a2-8070a74312ae";
    pref.renderAPI = sl::RenderAPI::eVulkan;
    const sl::Result r = api.init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        std::printf("Frame generation: slInit failed (%d); off\n", int(r));
        return {};
    }
    loaded = true;
    std::printf("Frame generation: Streamline %s loaded\n", "2.14");
    const std::string path = interposer.string();
    return path;
}

bool Loaded() {
    return loaded;
}

const char* StatusText() {
    if (!loaded) return "Unavailable";
    if (supported == 0) return "Not supported";
    return generating ? "On" : "Off";
}

bool BeginFrame(vk::Device device, vk::PhysicalDevice physical, vk::CommandBuffer cmdbuf,
                vk::Image, vk::Format, vk::Extent2D extent, bool generate) {
    if (!loaded) return false;
    if (supported < 0) {
        sl::AdapterInfo adapter{};
        adapter.vkPhysicalDevice = static_cast<VkPhysicalDevice>(physical);
        const sl::Result r = api.is_supported(sl::kFeatureDLSS_G, adapter);
        supported = r == sl::Result::eOk ? 1 : 0;
        std::printf("Frame generation: DLSS-G %s (%d)\n", supported ? "supported" : "not supported", int(r));
        // The feature functions exist only once the device is made.
        if (!Feature(sl::kFeatureDLSS_G, "slDLSSGSetOptions", api.dlssg_set_options) ||
            !Feature(sl::kFeatureDLSS_G, "slDLSSGGetState", api.dlssg_get_state) ||
            !Feature(sl::kFeaturePCL, "slPCLSetMarker", api.pcl_marker) ||
            !Feature(sl::kFeatureReflex, "slReflexSetOptions", api.reflex_set_options)) {
            supported = 0;
            loaded = false;
            return false;
        }
        sl::ReflexOptions reflex{};
        reflex.mode = sl::ReflexMode::eLowLatency;
        api.reflex_set_options(reflex);
    }
    ++frame_index;
    if (api.new_frame(token, &frame_index) != sl::Result::eOk) {
        token = nullptr;
        return true;
    }
    Marker(sl::PCLMarker::eSimulationStart);
    Marker(sl::PCLMarker::eSimulationEnd);
    Marker(sl::PCLMarker::eRenderSubmitStart);

    const bool want = supported == 1 && generate && ResogunSettings::Get().frame_generation;
    if (want && (inputs.extent != extent || !inputs.image[0])) {
        if (!CreateInputs(device, physical, extent)) {
            std::printf("Frame generation: input images failed; off\n");
            supported = 0;
        }
    }
    const bool on = want && supported == 1;
    if (on && !inputs.cleared) {
        const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        vk::ImageMemoryBarrier to_general[2];
        for (int i = 0; i < 2; ++i) {
            to_general[i] = vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = inputs.image[i],
                .subresourceRange = range,
            };
        }
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer,
                               {}, {}, {}, to_general);
        const vk::ClearColorValue zero{std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}};
        for (int i = 0; i < 2; ++i) {
            cmdbuf.clearColorImage(inputs.image[i], vk::ImageLayout::eGeneral, zero, range);
        }
        const vk::MemoryBarrier done{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
                               {}, done, {}, {});
        inputs.cleared = true;
    }

    const sl::ViewportHandle viewport(0u);
    if (on) {
        sl::Constants c{};
        for (int i = 0; i < 4; ++i) {
            const sl::float4 row(i == 0, i == 1, i == 2, i == 3);
            c.cameraViewToClip[i] = row;
            c.clipToCameraView[i] = row;
            c.clipToLensClip[i] = row;
            c.clipToPrevClip[i] = row;
            c.prevClipToClip[i] = row;
        }
        c.jitterOffset = {0.0f, 0.0f};
        c.mvecScale = {1.0f, 1.0f};
        c.cameraPinholeOffset = {0.0f, 0.0f};
        c.cameraPos = {0.0f, 0.0f, 0.0f};
        c.cameraUp = {0.0f, 1.0f, 0.0f};
        c.cameraRight = {1.0f, 0.0f, 0.0f};
        c.cameraFwd = {0.0f, 0.0f, 1.0f};
        c.cameraNear = 0.1f;
        c.cameraFar = 10000.0f;
        c.cameraFOV = 1.0f;
        c.cameraAspectRatio = float(extent.width) / float(extent.height);
        c.motionVectorsInvalidValue = 0.0f;
        c.depthInverted = sl::Boolean::eTrue;
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.motionVectors3D = sl::Boolean::eFalse;
        c.reset = generating ? sl::Boolean::eFalse : sl::Boolean::eTrue;
        if (token) api.set_constants(c, *token, viewport);

        sl::Resource depth = InputResource(0), mvec = InputResource(1);
        const sl::Extent full{0, 0, extent.width, extent.height};
        const sl::ResourceTag tags[] = {
            sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &full),
            sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &full),
        };
        if (token) {
            api.set_tags(*token, viewport, tags, uint32_t(std::size(tags)),
                         reinterpret_cast<sl::CommandBuffer*>(static_cast<VkCommandBuffer>(cmdbuf)));
        }
    }
    if (on != generating) {
        sl::DLSSGOptions options{};
        options.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        options.numFramesToGenerate = 1;
        options.onErrorCallback = nullptr;
        const sl::Result r = api.dlssg_set_options(viewport, options);
        std::printf("Frame generation: %s (%d)\n", on ? "on" : "off", int(r));
        generating = on;
    }
    Marker(sl::PCLMarker::eRenderSubmitEnd);
    return true;
}

void PresentStart() {
    if (!loaded) return;
    Marker(sl::PCLMarker::ePresentStart);
}

void PresentEnd() {
    if (!loaded) return;
    Marker(sl::PCLMarker::ePresentEnd);
    ++presents;
    if (generating) ++game_frames_generated;
}

void LogStats() {
    if (!loaded || supported != 1) return;
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) return;
    last = now;
    sl::DLSSGState state{};
    const sl::ViewportHandle viewport(0u);
    if (api.dlssg_get_state(viewport, state, nullptr) == sl::Result::eOk) {
        std::printf("Frame generation: %s, status %u, %u frames shown per game frame, %.0f MB VRAM\n",
                    generating ? "on" : "off", unsigned(state.status), state.numFramesActuallyPresented,
                    double(state.estimatedVRAMUsageInBytes) / (1024.0 * 1024.0));
    }
}

#else

std::string VulkanLibrary() { return {}; }
bool Loaded() { return false; }
const char* StatusText() { return "Unavailable"; }
bool BeginFrame(vk::Device, vk::PhysicalDevice, vk::CommandBuffer, vk::Image, vk::Format, vk::Extent2D, bool) {
    return false;
}
void PresentStart() {}
void PresentEnd() {}
void LogStats() {}

#endif

} // namespace ResogunStreamline
