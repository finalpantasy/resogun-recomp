// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <unordered_map>

#include <xxhash.h>

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace {
std::mutex module_hash_mutex;
std::unordered_map<VkShaderModule, u64> module_hashes;
} // namespace

u64 ShaderModuleHash(vk::ShaderModule module) {
    std::scoped_lock lock{module_hash_mutex};
    const auto it = module_hashes.find(static_cast<VkShaderModule>(module));
    return it == module_hashes.end() ? 0 : it->second;
}

vk::ShaderModule CompileSPV(std::span<const u32> code, vk::Device device) {
    const vk::ShaderModuleCreateInfo shader_info = {
        .codeSize = code.size() * sizeof(u32),
        .pCode = code.data(),
    };

    auto [module_result, module] = device.createShaderModule(shader_info);
    ASSERT_MSG(module_result == vk::Result::eSuccess, "Failed to compile SPIR-V shader: {}",
               vk::to_string(module_result));
    const u64 hash = XXH3_64bits(code.data(), code.size_bytes());
    std::scoped_lock lock{module_hash_mutex};
    module_hashes[static_cast<VkShaderModule>(module)] = hash;
    return module;
}

} // namespace Vulkan
