// SPDX-License-Identifier: GPL-2.0-or-later
// NVIDIA DLSS Frame Generation through Streamline (third_party/streamline, MIT). With frame
// generation set at start, Vulkan is loaded from sl.interposer.dll, which adds what DLSS-G needs to
// the instance, device and swapchain; each presented game frame then gets a generated frame
// before it. Off while a menu or a loading screen is up.
#pragma once

#include <string>
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

/// Called on the present thread with the present command buffer, before it is submitted:
/// starts the frame for Streamline, tags the inputs and sets frame generation on for a game
/// frame without the overlay, off otherwise. Returns false when Streamline is not in use.
bool BeginFrame(vk::Device device, vk::PhysicalDevice physical, vk::CommandBuffer cmdbuf,
                vk::Image frame_image, vk::Format frame_format, vk::Extent2D extent, bool generate);

/// Around vkQueuePresentKHR for the frame BeginFrame started.
void PresentStart();
void PresentEnd();

/// Frames shown and the rate of generated ones, for the log once a second.
void LogStats();

} // namespace ResogunStreamline
