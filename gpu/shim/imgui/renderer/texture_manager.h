// resogun: no ImGui overlay in the port; hooks used by the renderer are no-ops.
#pragma once
#include "video_core/renderer_vulkan/vk_common.h"
namespace Frontend { class WindowSDL; }
namespace Vulkan { class Instance; }
namespace ResogunOverlay { bool HoldsGame(); }
namespace ImGui::Core {
inline void Initialize(const Vulkan::Instance&, const Frontend::WindowSDL&, u32, vk::Format) {}
inline void OnSurfaceFormatChange(vk::Format) {}
inline void Shutdown(vk::Device) {}
// resogun: the last frame is shown again while the game is held (the Cheats page), with the
// overlay drawn over it each time.
inline bool MustKeepDrawing() { return ResogunOverlay::HoldsGame(); }
struct TextureManager { static void Submit() {} };
} // namespace ImGui::Core
