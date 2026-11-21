// SPDX-License-Identifier: GPL-2.0-or-later
// NVIDIA DLSS Frame Generation through Streamline (third_party/streamline, MIT). With frame
// generation set at start, Vulkan is loaded from sl.interposer.dll, which adds what DLSS-G needs to
// the instance, device and swapchain; each presented game frame then gets a generated frame
// before it. Off while a menu or a loading screen is up.
#pragma once

#include <string>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace ResogunStreamline {

/// Before the Vulkan loader: when frame generation is set at start and sl.interposer.dll (signed
/// by NVIDIA) sits beside the program, initializes Streamline and returns the interposer's path to
/// load Vulkan from; otherwise an empty string (plain vulkan-1.dll, no frame generation).
std::string VulkanLibrary();

/// Streamline is in use (Vulkan came from the interposer).
bool Loaded();

/// Short state for the menu: "Unavailable" before a start with it, "Not supported", "On", "Off".
const char* StatusText();

/// The scene camera of a frame, as one draw of the main pass saw it: the vertex shaders compute
/// clip = vp * world (column vector, `vp` row-major), and the draw's viewport registers map clip
/// space to the scene depth image (screen = ndc * scale + offset, depth = ndc.z * zscale + zoffset).
struct SceneView {
    float vp[16];
    float xscale, xoffset, yscale, yoffset, zscale, zoffset;
    bool clip_minus_w_to_w; ///< ndc z in [-1, 1] (else [0, 1])
};

/// Whether a 4x4 row-major matrix looks like a perspective view-projection with a rigid view:
/// finite, its w row a unit forward vector, x and y rows not empty. (Shadow maps are orthographic
/// and fail the w test.)
bool LooksLikeCamera(const float* m);

/// Draw thread: whether the draws should be watched for the scene camera (frame generation is
/// on, or RESOGUN_VP_TRACE is set). False keeps the camera search out of the draw path.
bool WatchCamera();

/// RESOGUN_VP_TRACE level (0 off, 1 a line per frame, 2 also every candidate draw).
int VpTrace();

/// Draw thread, in the flip's command buffer after the frame's draws: fills frame slot `slot`'s
/// depth and motion vectors for DLSS-G from the scene depth (`depth`, D32 level 0, in
/// TransferSrcOptimal) and the cameras of this and the previous frame. `view` null (no camera
/// this frame, or no usable depth): the slot falls back to zero motion and far depth.
void RecordInputs(vk::Device device, vk::PhysicalDevice physical, vk::CommandBuffer cmdbuf, u32 slot,
                  const SceneView* view, vk::Image depth, vk::Extent2D depth_extent, u64 frame);

/// Draw thread: frame slot `slot` got a frame without inputs (frame generation off).
void ClearSlot(u32 slot);

/// Called on the present thread with the present command buffer, before it is submitted:
/// starts the frame for Streamline, tags the inputs and sets frame generation on for a game
/// frame without the overlay, off otherwise. Returns false when Streamline is not in use.
/// `slot` is the presenter frame whose inputs RecordInputs filled (~0u: none); `game_rect` is
/// where that frame lands in the swapchain image (it is letterboxed).
bool BeginFrame(vk::Device device, vk::PhysicalDevice physical, vk::CommandBuffer cmdbuf,
                vk::Image frame_image, vk::Format frame_format, vk::Extent2D extent, bool generate,
                u32 slot = ~0u, vk::Rect2D game_rect = {});

/// Around vkQueuePresentKHR for the frame BeginFrame started.
void PresentStart();
void PresentEnd();

/// Frames shown and the rate of generated ones, for the log once a second.
void LogStats();

} // namespace ResogunStreamline
