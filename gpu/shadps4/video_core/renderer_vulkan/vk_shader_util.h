// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/**
 * @brief Creates a vulkan shader module from SPIR-V bytecode.
 * @param code The SPIR-V bytecode data.
 * @param device The vulkan device handle
 */
vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device);

/// resogun: XXH3 of the SPIR-V a module was made from by CompileSPV (0 if unknown), for the
/// pipeline fingerprints of RESOGUN_PIPELINE_FINGERPRINT.
u64 ShaderModuleHash(vk::ShaderModule module);

} // namespace Vulkan
