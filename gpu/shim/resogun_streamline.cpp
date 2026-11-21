// SPDX-License-Identifier: GPL-2.0-or-later
// NVIDIA DLSS Frame Generation through Streamline: see resogun_streamline.h.
//
// The game makes no motion vectors, so they are made here from what it does give: the rasterizer
// finds the scene camera (the view-projection matrix the main pass's vertex shaders read) and the
// scene depth image, and at each flip RecordInputs copies that depth and runs dlssg_inputs.comp,
// which reprojects every pixel through this frame's and the previous frame's camera. That is
// camera motion only: a moving ball or character gets the motion of what is behind it, and
// DLSS-G's own optical flow has to make up the rest. Without a camera in a frame the inputs fall
// back to zero motion, far depth and identity matrices (DLSS-G then rests on its optical flow,
// like the driver's Smooth Motion).

#include "resogun_streamline.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
#include "video_core/host_shaders/dlssg_inputs_comp.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace ResogunStreamline {

bool LooksLikeCamera(const float* m) {
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1e7f) return false;
    }
    const auto len2 = [m](int row) {
        return m[row * 4] * m[row * 4] + m[row * 4 + 1] * m[row * 4 + 1] + m[row * 4 + 2] * m[row * 4 + 2];
    };
    const float w = len2(3);
    return w > 0.96f && w < 1.04f && len2(0) > 1e-6f && len2(1) > 1e-6f;
}

int VpTrace() {
    static const int level = [] {
        const char* v = std::getenv("RESOGUN_VP_TRACE");
        return v && *v ? std::atoi(v) : 0;
    }();
    return level;
}

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
std::atomic<int> supported{-1}; // -1 not checked yet (needs the device), then 0 or 1
bool generating = false;
sl::FrameToken* token = nullptr;
uint32_t frame_index = 0;

// resogun: one set of DLSS-G inputs per presenter frame (Frame::id): the draw thread fills a slot
// at the flip and the present thread tags it. The presenter hands a frame out again only once its
// present is done, so a slot is never rewritten while it is being read.
constexpr u32 MaxSlots = 8;
constexpr vk::Format InputFormats[2] = {vk::Format::eR32Sfloat, vk::Format::eR16G16Sfloat};
constexpr vk::ImageUsageFlags InputUsage = vk::ImageUsageFlagBits::eSampled |
                                           vk::ImageUsageFlagBits::eStorage |
                                           vk::ImageUsageFlagBits::eTransferDst |
                                           vk::ImageUsageFlagBits::eTransferSrc;

struct Slot {
    vk::Extent2D extent{};
    vk::Image image[2]; // [0] depth (R32F), [1] motion vectors (RG16F, in pixels)
    vk::DeviceMemory memory[2];
    vk::ImageView view[2];
    vk::Buffer depth_copy; // the scene depth as 32-bit floats, read by the compute pass
    vk::DeviceMemory depth_copy_memory;
    vk::DescriptorSet set;
    bool filled = false; // RecordInputs wrote it for the frame now in this slot
    bool has_camera = false;
    sl::Constants constants{};
};

struct Pass {
    vk::Device device;
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout layout;
    vk::Pipeline pipeline;
    vk::DescriptorPool pool;
    bool failed = false;
};

struct Push {
    float to_prev[16]; // column-major (GLSL mat4)
    u32 size[2];
    float far_depth;
    u32 has_camera;
};

Slot slots[MaxSlots];
Pass pass;

using Mat = std::array<double, 16>; // row-major, for column vectors (the game's convention)

// The previous flip's camera, for the motion of this one (draw thread).
struct PrevCamera {
    bool valid = false;
    u64 frame = 0;
    Mat vp{};
};
PrevCamera prev_camera;

std::atomic<uint64_t> presents{0}, game_frames_generated{0}, frames_camera{0}, frames_fallback{0};

Mat Mul(const Mat& a, const Mat& b) {
    Mat r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}

// Gauss-Jordan with partial pivoting.
bool Inverse(const Mat& m, Mat& out) {
    double a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-30) return false;
        if (p != c)
            for (int j = 0; j < 8; ++j) std::swap(a[p][j], a[c][j]);
        const double inv = 1.0 / a[c][c];
        for (int j = 0; j < 8; ++j) a[c][j] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r][c];
            for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = a[i][j + 4];
    return true;
}

// Streamline's matrices are for row vectors (v * M, translation in row 3): the transpose of ours.
sl::float4x4 ToSl(const Mat& m) {
    sl::float4x4 r;
    for (int i = 0; i < 4; ++i) {
        r[i] = sl::float4(float(m[0 * 4 + i]), float(m[1 * 4 + i]), float(m[2 * 4 + i]), float(m[3 * 4 + i]));
    }
    return r;
}

// The dummy constants: identity matrices, depth "far everywhere" (0, inverted), zero motion.
void DummyConstants(sl::Constants& c, vk::Extent2D extent) {
    c = sl::Constants{};
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
    c.cameraAspectRatio = extent.height ? float(extent.width) / float(extent.height) : 1.0f;
    c.motionVectorsInvalidValue = 0.0f;
    c.depthInverted = sl::Boolean::eTrue;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = sl::Boolean::eFalse;
}

// The projection split off a view-projection, assuming the usual form
// P = [sx 0 cx 0; 0 sy cy 0; 0 0 A B; 0 0 1 0] in the frame where view depth d is clip w: vp's w
// row is then the view's forward row, and the rest follows from it (view = P^-1 vp).
struct Derived {
    Mat proj{}, view{};
    double sx, sy, cx, cy, a, b, near_plane, far_plane, fov, aspect;
    bool inverted, ok;
};

Derived Derive(const Mat& vp, const SceneView& v) {
    Derived d{};
    const double fx = vp[12], fy = vp[13], fz = vp[14];
    const double ff = fx * fx + fy * fy + fz * fz;
    const auto along = [&](int row) {
        return (vp[row * 4] * fx + vp[row * 4 + 1] * fy + vp[row * 4 + 2] * fz) / ff;
    };
    d.cx = along(0);
    d.cy = along(1);
    d.a = along(2);
    d.b = vp[11] - d.a * vp[15];
    const auto rest = [&](int row, double c) {
        const double x = vp[row * 4] - c * fx, y = vp[row * 4 + 1] - c * fy, z = vp[row * 4 + 2] - c * fz;
        return std::sqrt((x * x + y * y + z * z) / ff);
    };
    d.sx = rest(0, d.cx);
    d.sy = rest(1, d.cy);
    d.proj = {d.sx, 0, d.cx, 0, 0, d.sy, d.cy, 0, 0, 0, d.a, d.b, 0, 0, 1, 0};
    Mat inv_proj{};
    d.ok = ff > 0 && d.sx > 0 && d.sy > 0 && std::fabs(d.b) > 1e-12 && Inverse(d.proj, inv_proj);
    if (d.ok) d.view = Mul(inv_proj, vp);
    // ndc z = A + B / d and depth = ndc z * zscale + zoffset: nearer is higher when zscale * B > 0.
    d.inverted = double(v.zscale) * d.b > 0;
    const double lo = v.clip_minus_w_to_w ? -1.0 : 0.0, hi = 1.0;
    const double z_near = d.b > 0 ? hi : lo, z_far = d.b > 0 ? lo : hi;
    d.near_plane = std::fabs(z_near - d.a) > 1e-12 ? d.b / (z_near - d.a) : 0.0;
    d.far_plane = std::fabs(z_far - d.a) > 1e-12 ? d.b / (z_far - d.a) : 1e6; // infinite far plane
    if (!(d.far_plane > 0) || d.far_plane > 1e6) d.far_plane = 1e6;
    d.fov = d.sy > 0 ? 2.0 * std::atan(1.0 / d.sy) : 1.0;
    d.aspect = d.sx > 0 ? d.sy / d.sx : 1.0;
    return d;
}

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

uint32_t DeviceLocalType(vk::PhysicalDevice physical, uint32_t bits) {
    const auto props = physical.getMemoryProperties();
    for (uint32_t t = 0; t < props.memoryTypeCount; ++t) {
        if ((bits >> t & 1) && (props.memoryTypes[t].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
            return t;
        }
    }
    return UINT32_MAX;
}

bool CreatePass(vk::Device device) {
    if (pass.pipeline) return true;
    if (pass.failed) return false;
    pass.failed = true; // until everything below is made
    pass.device = device;
    const vk::DescriptorSetLayoutBinding bindings[] = {
        {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute},
        {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
        {2, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute},
    };
    auto [lr, set_layout] = device.createDescriptorSetLayout(
        {.bindingCount = uint32_t(std::size(bindings)), .pBindings = bindings});
    if (lr != vk::Result::eSuccess) return false;
    pass.set_layout = set_layout;
    const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, sizeof(Push)};
    auto [pr, layout] = device.createPipelineLayout({
        .setLayoutCount = 1,
        .pSetLayouts = &pass.set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    });
    if (pr != vk::Result::eSuccess) return false;
    pass.layout = layout;
    const vk::ShaderModule module = Vulkan::CompileSPV(DLSSG_INPUTS_COMP, device);
    if (!module) return false;
    const vk::ComputePipelineCreateInfo info{
        .stage = {.stage = vk::ShaderStageFlagBits::eCompute, .module = module, .pName = "main"},
        .layout = pass.layout,
    };
    auto [cr, pipeline] = device.createComputePipeline({}, info);
    device.destroyShaderModule(module);
    if (cr != vk::Result::eSuccess) return false;
    pass.pipeline = pipeline;
    const vk::DescriptorPoolSize sizes[] = {
        {vk::DescriptorType::eStorageBuffer, MaxSlots},
        {vk::DescriptorType::eStorageImage, 2 * MaxSlots},
    };
    auto [dr, pool] = device.createDescriptorPool({
        .maxSets = MaxSlots,
        .poolSizeCount = uint32_t(std::size(sizes)),
        .pPoolSizes = sizes,
    });
    if (dr != vk::Result::eSuccess) return false;
    pass.pool = pool;
    pass.failed = false;
    return true;
}

// Only for a slot whose frame is not in flight (see Slot); the descriptor set stays.
void DestroySlot(Slot& slot) {
    const vk::Device device = pass.device;
    for (int i = 0; i < 2; ++i) {
        if (slot.view[i]) device.destroyImageView(slot.view[i]);
        if (slot.image[i]) device.destroyImage(slot.image[i]);
        if (slot.memory[i]) device.freeMemory(slot.memory[i]);
        slot.view[i] = nullptr;
        slot.image[i] = nullptr;
        slot.memory[i] = nullptr;
    }
    if (slot.depth_copy) device.destroyBuffer(slot.depth_copy);
    if (slot.depth_copy_memory) device.freeMemory(slot.depth_copy_memory);
    slot.depth_copy = nullptr;
    slot.depth_copy_memory = nullptr;
    slot.extent = vk::Extent2D{};
}

bool CreateSlot(Slot& slot, vk::Device device, vk::PhysicalDevice physical, vk::CommandBuffer cmdbuf,
                vk::Extent2D extent) {
    DestroySlot(slot);
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
        slot.image[i] = image;
        const auto req = device.getImageMemoryRequirements(image);
        const uint32_t type = DeviceLocalType(physical, req.memoryTypeBits);
        if (type == UINT32_MAX) return false;
        auto [mr, memory] = device.allocateMemory({.allocationSize = req.size, .memoryTypeIndex = type});
        if (mr != vk::Result::eSuccess) return false;
        slot.memory[i] = memory;
        if (device.bindImageMemory(image, memory, 0) != vk::Result::eSuccess) return false;
        auto [vr, view] = device.createImageView({
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = InputFormats[i],
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        });
        if (vr != vk::Result::eSuccess) return false;
        slot.view[i] = view;
    }
    auto [br, buffer] = device.createBuffer({
        .size = vk::DeviceSize(extent.width) * extent.height * 4,
        .usage = vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst,
    });
    if (br != vk::Result::eSuccess) return false;
    slot.depth_copy = buffer;
    const auto breq = device.getBufferMemoryRequirements(buffer);
    const uint32_t btype = DeviceLocalType(physical, breq.memoryTypeBits);
    if (btype == UINT32_MAX) return false;
    auto [bmr, bmemory] = device.allocateMemory({.allocationSize = breq.size, .memoryTypeIndex = btype});
    if (bmr != vk::Result::eSuccess) return false;
    slot.depth_copy_memory = bmemory;
    if (device.bindBufferMemory(buffer, bmemory, 0) != vk::Result::eSuccess) return false;
    if (!slot.set) {
        auto [sr, sets] = device.allocateDescriptorSets({
            .descriptorPool = pass.pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &pass.set_layout,
        });
        if (sr != vk::Result::eSuccess) return false;
        slot.set = sets[0];
    }
    const vk::DescriptorBufferInfo buffer_info{slot.depth_copy, 0, VK_WHOLE_SIZE};
    const vk::DescriptorImageInfo image_info[2] = {
        {{}, slot.view[0], vk::ImageLayout::eGeneral},
        {{}, slot.view[1], vk::ImageLayout::eGeneral},
    };
    const vk::WriteDescriptorSet writes[] = {
        {.dstSet = slot.set, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageBuffer, .pBufferInfo = &buffer_info},
        {.dstSet = slot.set, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &image_info[0]},
        {.dstSet = slot.set, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = vk::DescriptorType::eStorageImage, .pImageInfo = &image_info[1]},
    };
    device.updateDescriptorSets(writes, {});
    // The images stay in General from here on (the tags tell DLSS-G so).
    vk::ImageMemoryBarrier to_general[2];
    for (int i = 0; i < 2; ++i) {
        to_general[i] = vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eShaderWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = slot.image[i],
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
    }
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eComputeShader,
                           {}, {}, {}, to_general);
    slot.extent = extent;
    return true;
}

sl::Resource InputResource(const Slot& slot, int i) {
    sl::Resource r(sl::ResourceType::eTex2d, static_cast<VkImage>(slot.image[i]),
                   static_cast<VkDeviceMemory>(slot.memory[i]), static_cast<VkImageView>(slot.view[i]),
                   static_cast<uint32_t>(VK_IMAGE_LAYOUT_GENERAL));
    r.width = slot.extent.width;
    r.height = slot.extent.height;
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

bool WatchCamera() {
    if (VpTrace() > 0) return true;
    return loaded && supported.load(std::memory_order_relaxed) != 0 &&
           ResogunSettings::Get().frame_generation.load(std::memory_order_relaxed);
}

void RecordInputs(vk::Device device, vk::PhysicalDevice physical, vk::CommandBuffer cmdbuf, u32 slot_id,
                  const SceneView* view, vk::Image depth, vk::Extent2D extent, u64 frame) {
    // Without Streamline (RESOGUN_VP_TRACE alone) only the camera is worked out, for the trace.
    const bool record = loaded && supported.load(std::memory_order_relaxed) != 0 && slot_id < MaxSlots;
    Slot* slot = record ? &slots[slot_id] : nullptr;
    if (slot) slot->filled = false;
    if (extent.width == 0 || extent.height == 0) return;

    sl::Constants c;
    DummyConstants(c, extent);
    Push push{};
    push.size[0] = extent.width;
    push.size[1] = extent.height;
    push.far_depth = 0.0f; // far in the dummy's inverted depth
    bool has_camera = false;
    if (view && depth) {
        Mat vp;
        for (int i = 0; i < 16; ++i) vp[i] = view->vp[i];
        const Derived d = Derive(vp, *view);
        Mat inv_vp{};
        if (d.ok && Inverse(vp, inv_vp)) {
            const bool reset = !prev_camera.valid || prev_camera.frame + 1 != frame;
            const Mat& prev = reset ? vp : prev_camera.vp;
            const Mat clip_to_prev = Mul(prev, inv_vp);
            Mat prev_to_clip{}, inv_prev{};
            if (!Inverse(prev, inv_prev)) inv_prev = inv_vp;
            prev_to_clip = Mul(vp, inv_prev);
            // Viewport: ndc (w = 1) to (screen x, screen y, depth value, 1); the same both frames.
            const Mat screen = {view->xscale, 0, 0, view->xoffset, 0, view->yscale, 0, view->yoffset,
                                0, 0, view->zscale, view->zoffset, 0, 0, 0, 1};
            Mat inv_screen{};
            if (Inverse(screen, inv_screen)) {
                const Mat to_prev = Mul(screen, Mul(clip_to_prev, inv_screen));
                for (int r = 0; r < 4; ++r)
                    for (int col = 0; col < 4; ++col) push.to_prev[col * 4 + r] = float(to_prev[r * 4 + col]);
                has_camera = true;
                Mat inv_proj{};
                Inverse(d.proj, inv_proj);
                c.cameraViewToClip = ToSl(d.proj);
                c.clipToCameraView = ToSl(inv_proj);
                c.clipToPrevClip = ToSl(clip_to_prev);
                c.prevClipToClip = ToSl(prev_to_clip);
                // The view's rows are right, up and forward; the camera sits at -R^T t.
                const Mat& v = d.view;
                c.cameraRight = {float(v[0]), float(v[1]), float(v[2])};
                c.cameraUp = {float(v[4]), float(v[5]), float(v[6])};
                c.cameraFwd = {float(v[8]), float(v[9]), float(v[10])};
                c.cameraPos = {float(-(v[0] * v[3] + v[4] * v[7] + v[8] * v[11])),
                               float(-(v[1] * v[3] + v[5] * v[7] + v[9] * v[11])),
                               float(-(v[2] * v[3] + v[6] * v[7] + v[10] * v[11]))};
                c.cameraNear = float(d.near_plane);
                c.cameraFar = float(d.far_plane);
                c.cameraFOV = float(d.fov);
                c.cameraAspectRatio = float(d.aspect);
                // An off-center projection (cx, cy) is in cameraViewToClip already; the game
                // has no temporal jitter (cx, cy stay put frame to frame, see the trace).
                c.jitterOffset = {0.0f, 0.0f};
                // dlssg_inputs.comp writes pixels of the depth image: scaled to [-1, 1] here.
                c.mvecScale = {1.0f / float(extent.width), 1.0f / float(extent.height)};
                // Only read when camera motion is not included; every pixel has a value.
                c.motionVectorsInvalidValue = 0.0f;
                c.depthInverted = d.inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
                c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
                prev_camera = {true, frame, vp};
                if (VpTrace() > 0) {
                    std::printf("VP trace: flip %llu camera near %.3f far %.1f%s depth %s fov %.1f deg "
                                "aspect %.3f offset %.4f,%.4f (%.2f,%.2f px) pos %.2f,%.2f,%.2f%s\n",
                                static_cast<unsigned long long>(frame), d.near_plane, d.far_plane,
                                d.far_plane >= 1e6 ? " (infinite)" : "", d.inverted ? "reversed" : "forward",
                                d.fov * 57.29577951308232, d.aspect, d.cx, d.cy, d.cx * view->xscale,
                                d.cy * view->yscale, c.cameraPos.x, c.cameraPos.y, c.cameraPos.z,
                                reset ? " (reset)" : "");
                }
            }
        }
    }
    if (!has_camera) {
        prev_camera.valid = false;
        if (VpTrace() > 0) {
            std::printf("VP trace: flip %llu no camera; dummy inputs\n", static_cast<unsigned long long>(frame));
        }
    }
    ++(has_camera ? frames_camera : frames_fallback);
    if (!slot || !CreatePass(device)) return;
    if (slot->extent != extent && !CreateSlot(*slot, device, physical, cmdbuf, extent)) {
        std::printf("Frame generation: input images failed\n");
        DestroySlot(*slot);
        return;
    }
    if (has_camera) {
        // The previous flip's compute pass read this buffer (same queue, earlier).
        const vk::MemoryBarrier before_copy{
            .srcAccessMask = vk::AccessFlagBits::eShaderRead,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eTransfer,
                               {}, before_copy, {}, {});
        const vk::BufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {vk::ImageAspectFlagBits::eDepth, 0, 0, 1},
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent.width, extent.height, 1},
        };
        cmdbuf.copyImageToBuffer(depth, vk::ImageLayout::eTransferSrcOptimal, slot->depth_copy, region);
    }
    // Copy done before the pass reads it; the last reader of the images (DLSS-G at this slot's
    // previous present) done before the pass writes them.
    const vk::MemoryBarrier before_pass{
        .srcAccessMask = vk::AccessFlagBits::eTransferWrite | vk::AccessFlagBits::eShaderRead,
        .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eComputeShader,
                           {}, before_pass, {}, {});
    push.has_camera = has_camera ? 1u : 0u;
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pass.pipeline);
    cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pass.layout, 0, slot->set, {});
    cmdbuf.pushConstants(pass.layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
    cmdbuf.dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
    const vk::MemoryBarrier after_pass{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eAllCommands,
                           {}, after_pass, {}, {});
    slot->constants = c;
    slot->has_camera = has_camera;
    slot->filled = true;
}

void ClearSlot(u32 slot_id) {
    if (slot_id < MaxSlots) slots[slot_id].filled = false;
}

bool BeginFrame(vk::Device, vk::PhysicalDevice physical, vk::CommandBuffer cmdbuf, vk::Image, vk::Format,
                vk::Extent2D extent, bool generate, u32 slot_id, vk::Rect2D game_rect) {
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

    // resogun: the frame's inputs come from RecordInputs (draw thread, before this frame's flip
    // was queued for presenting). A frame without them (frame generation just turned on) is shown
    // without a generated frame before it.
    const Slot* slot = slot_id < MaxSlots && slots[slot_id].filled ? &slots[slot_id] : nullptr;
    const bool on = supported == 1 && generate && ResogunSettings::Get().frame_generation && slot;

    const sl::ViewportHandle viewport(0u);
    if (on) {
        sl::Constants c = slot->constants;
        if (!generating) c.reset = sl::Boolean::eTrue;
        if (token) api.set_constants(c, *token, viewport);

        sl::Resource depth = InputResource(*slot, 0), mvec = InputResource(*slot, 1);
        const sl::Extent full{0, 0, slot->extent.width, slot->extent.height};
        // The game frame is letterboxed into the swapchain image: frame generation runs on its rect.
        const sl::Extent game{uint32_t(game_rect.offset.y), uint32_t(game_rect.offset.x), game_rect.extent.width,
                              game_rect.extent.height};
        const bool subrect = game_rect.extent.width != 0 &&
                             (game_rect.extent.width != extent.width || game_rect.extent.height != extent.height);
        const sl::ResourceTag tags[] = {
            sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &full),
            sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &full),
            sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle{}, &game),
        };
        if (token) {
            const sl::Result r = api.set_tags(*token, viewport, tags, subrect ? 3u : 2u,
                                              reinterpret_cast<sl::CommandBuffer*>(static_cast<VkCommandBuffer>(cmdbuf)));
            static sl::Result last_result = sl::Result::eOk;
            if (r != last_result) {
                std::printf("Frame generation: tagging inputs returned %d\n", int(r));
                last_result = r;
            }
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
        std::printf("Frame generation: %s, status %u, %u frames shown per game frame, %.0f MB VRAM; "
                    "inputs: %llu flips with the camera, %llu without\n",
                    generating ? "on" : "off", unsigned(state.status), state.numFramesActuallyPresented,
                    double(state.estimatedVRAMUsageInBytes) / (1024.0 * 1024.0),
                    static_cast<unsigned long long>(frames_camera.exchange(0)),
                    static_cast<unsigned long long>(frames_fallback.exchange(0)));
    }
}

#else

bool WatchCamera() { return VpTrace() > 0; }
void RecordInputs(vk::Device, vk::PhysicalDevice, vk::CommandBuffer, u32, const SceneView*, vk::Image,
                  vk::Extent2D, u64) {}
void ClearSlot(u32) {}
std::string VulkanLibrary() { return {}; }
bool Loaded() { return false; }
const char* StatusText() { return "Unavailable"; }
bool BeginFrame(vk::Device, vk::PhysicalDevice, vk::CommandBuffer, vk::Image, vk::Format, vk::Extent2D, bool, u32,
                vk::Rect2D) {
    return false;
}
void PresentStart() {}
void PresentEnd() {}
void LogStats() {}

#endif

} // namespace ResogunStreamline
