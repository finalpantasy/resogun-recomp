// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <utility>
#include <vector>
#include <optional>
#include <span>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

namespace PipelineBinaryCache {
using Key = std::vector<u8>;
struct Binary {
    Key key;
    std::vector<u8> data;
    u64 content = 0; ///< version 2 files: XXH3 of `data`, which names its shared file
};
constexpr size_t MaxFileSize = 64 * 1024 * 1024;
constexpr u32 MaxBinaries = 32;

std::vector<u8> Encode(const Key& global, const Key& pipeline, u64 graphics_hash,
                       const std::vector<Binary>& binaries);
std::optional<std::vector<Binary>> Decode(std::span<const u8> bytes, const Key& global,
                                          const Key& pipeline, u64 graphics_hash);
bool Save(const std::filesystem::path& file, std::span<const u8> bytes);
std::optional<std::vector<Binary>> Load(const std::filesystem::path& file, const Key& global,
                                        const Key& pipeline, u64 graphics_hash);
/// resogun: version 2, the binary keys and content hashes only; the data is in SharedPath files
/// (StoreShared). The driver reuses a binary key for different data, so files are named by content.
std::vector<u8> EncodeShared(const Key& global, const Key& pipeline, u64 graphics_hash,
                             const std::vector<Binary>& binaries);
std::filesystem::path SharedPath(const std::filesystem::path& directory, const Key& global,
                                 const Key& binary, u64 content);
bool StoreShared(const std::filesystem::path& file, const Key& global, const Key& pipeline,
                 u64 graphics_hash, const std::vector<Binary>& binaries);
/// StoreShared, or the version 1 file with ResogunToggle::SharedBinaries off.
bool Store(const std::filesystem::path& file, const Key& global, const Key& pipeline,
           u64 graphics_hash, const std::vector<Binary>& binaries);
/// Rewrites every version 1 file of `directory` as version 2: {files, bytes freed}.
std::pair<u64, s64> ConvertToShared(const std::filesystem::path& directory,
                                    const std::function<bool()>& stop = {});
/// resogun: marks a file as used now for Prune; the time moves at most every 10 minutes.
void Touch(const std::filesystem::path& file);
/// resogun: nothing else ever removes a file, and every translator change leaves a new set behind.
/// Removes pipeline files of another driver (global key) or that no longer parse, shared files no
/// pipeline file names, and, while the folder is over `budget` bytes, the least recently used
/// pipeline files until it is under three quarters of it; a shared file goes with the last
/// pipeline file that names it. Files used in the last hour stay. {files removed, bytes freed}.
std::pair<u64, u64> Prune(const std::filesystem::path& directory, const Key& global, u64 budget,
                          const std::function<bool()>& stop = {});
} // namespace PipelineBinaryCache

/// resogun: with `restore_only`, only a stored binary is used: without one the result is
/// ePipelineCompileRequired and no pipeline (no compile, nothing counted).
vk::ResultValue<vk::UniquePipeline> CreateCachedGraphicsPipeline(
    vk::Device device, vk::PipelineCache cache, const vk::GraphicsPipelineCreateInfo& info,
    const GraphicsPipelineKey& key, const vk::PipelineBinaryKeyKHR& global_key,
    const std::filesystem::path& directory, bool restore_only = false);
/// resogun: set on threads building optimized pipelines behind fast-linked ones; their driver
/// time then goes to the pipeline library counters instead of the draw-path compile cost.
extern thread_local bool g_resogun_background_compile;
void ReportPipelineBinaryCache();

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata);
void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata);
void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, size_t perm_hash, size_t perm_idx);
void RegisterShaderBinary(std::vector<u32>&& spv, u64 pgm_hash, size_t perm_idx);

} // namespace Vulkan
