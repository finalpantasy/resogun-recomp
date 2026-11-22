// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <span>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "common/path_util.h"
#include "video_core/renderer_vulkan/vk_wave_stats.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace Vulkan {

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;


void Pipeline::BindResources(DescriptorWrites& set_writes,
                             const Shader::PushData& push_data) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    scheduler.Record([layout, stage_flags, push_data](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    // resogun: with split stage sets the fragment shader's writes (bindings below split_binding,
    // written first) go to set 0 and the other stages' to set 1.
    std::span<vk::WriteDescriptorSet> writes0{set_writes};
    std::span<vk::WriteDescriptorSet> writes1{};
    if (split_sets) {
        const auto in_set0 = [this](const vk::WriteDescriptorSet& write) {
            return write.dstBinding < split_binding;
        };
        auto mid = std::ranges::find_if_not(set_writes, in_set0);
        if (std::any_of(mid, set_writes.end(), in_set0)) {
            mid = std::stable_partition(set_writes.begin(), set_writes.end(), in_set0);
        }
        const size_t count0 = static_cast<size_t>(mid - set_writes.begin());
        writes0 = std::span{set_writes}.first(count0);
        writes1 = std::span{set_writes}.subspan(count0);
    }

    if (uses_push_descriptors && !writes0.empty()) {
        PushDescriptors(bind_point, layout, writes0);
    }
    const bool heap0 = !uses_push_descriptors && !writes0.empty();
    if (!heap0 && writes1.empty()) {
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    std::array<vk::DescriptorSet, 2> sets{};
    if (heap0) {
        sets[0] = desc_heap.Commit(*desc_layout);
        for (auto& set_write : writes0) {
            set_write.dstSet = sets[0];
        }
    }
    if (!writes1.empty()) {
        sets[1] = desc_heap.Commit(*stage_desc_layout);
        for (auto& set_write : writes1) {
            set_write.dstSet = sets[1];
        }
    }
    if (heap0 && !writes1.empty()) {
        instance.GetDevice().updateDescriptorSets(set_writes, {});
    } else if (heap0) {
        instance.GetDevice().updateDescriptorSets(
            vk::ArrayProxy<const vk::WriteDescriptorSet>(u32(writes0.size()), writes0.data()), {});
    } else {
        instance.GetDevice().updateDescriptorSets(
            vk::ArrayProxy<const vk::WriteDescriptorSet>(u32(writes1.size()), writes1.data()), {});
    }
    const u32 first = heap0 ? 0u : 1u;
    const u32 count = heap0 && !writes1.empty() ? 2u : 1u;
    scheduler.Record([bind_point, layout, sets, first, count](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, first, count, sets.data() + first, 0,
                                  nullptr);
    });
}

void Pipeline::PushDescriptors(vk::PipelineBindPoint bind_point, vk::PipelineLayout layout,
                               std::span<const vk::WriteDescriptorSet> set_writes) const {
    const vk::ArrayProxy<const vk::WriteDescriptorSet> all(u32(set_writes.size()),
                                                           set_writes.data());
    if (!scheduler.IsRecordingDeferred()) {
        scheduler.Record([&](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, all);
        });
        return;
    }
    // Writes and the infos they point to are laid out in the recording chunk, with the
    // pointers already aimed at those copies: the command only carries a span.
    size_t bytes = set_writes.size() * sizeof(vk::WriteDescriptorSet) + 64;
    for (const auto& write : set_writes) {
        bytes += write.descriptorCount *
                     (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                      sizeof(vk::BufferView)) +
                 32;
    }
    // resogun: a push too large for a recording chunk (Everybody's Golf binds thousands of
    // descriptors at once) goes into the command buffer directly, after what was recorded.
    if (bytes + 1024 > RecordChunk::Capacity) {
        {
            ResogunStats::WaitTimer timer{ResogunStats::sync_large_ns};
            scheduler.SyncRecording();
        }
        scheduler.CommandBuffer().pushDescriptorSetKHR(bind_point, layout, 0, all);
        return;
    }
    scheduler.ReserveRecordData(bytes);
    const auto writes = scheduler.RecordData(set_writes);
    auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
    for (size_t i = 0; i < writes.size(); ++i) {
        auto& write = patched[i];
        if (write.pBufferInfo) {
            write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
        }
        if (write.pImageInfo) {
            write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
        }
        if (write.pTexelBufferView) {
            write.pTexelBufferView =
                scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
        }
    }
    scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
        cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

// resogun: one histogram for every pipeline of the process, reported to last_run.log on news and
// every 256 pipelines (vk_wave_stats.h). AMD reports width 0 for graphics executables, so on
// AMD the key also carries the physical VGPR count from VK_AMD_shader_info (it differs between
// wave32 and wave64), and the ISA of the first executables of each stage goes to
// <shaders>/wave_dumps/ (wave32 code manipulates EXEC with the _b32 forms).
void Pipeline::RecordWaveSizes() const {
    if (!instance.IsPipelineExecutableInfoEnabled() || !pipeline) {
        return;
    }
    const auto [result, executables] = instance.GetDevice().getPipelineExecutablePropertiesKHR(
        vk::PipelineInfoKHR{.pipeline = *pipeline});
    if (result != vk::Result::eSuccess) {
        return;
    }
    static std::mutex mutex;
    static WaveSizeHistogram histogram;
    std::scoped_lock lock{mutex};
    bool news = false;
    for (const vk::PipelineExecutablePropertiesKHR& executable : executables) {
        const std::string stages = vk::to_string(executable.stages);
        std::string key = stages;
        // One stage bit per query: the last stage of the executable (vk_wave_stats.h).
        const std::uint32_t stage_bit = AmdShaderInfoStageBit(
            is_compute ? static_cast<std::uint32_t>(vk::ShaderStageFlagBits::eCompute)
                       : static_cast<std::uint32_t>(executable.stages));
        if (instance.IsAmdShaderInfoEnabled() && stage_bit != 0) {
            const auto stage = static_cast<vk::ShaderStageFlagBits>(stage_bit);
            const auto [stats_result, stats] = instance.GetDevice().getShaderInfoAMD(
                *pipeline, stage, vk::ShaderInfoTypeAMD::eStatistics);
            std::string stats_line = "stats unavailable";
            std::optional<std::uint32_t> vgprs;
            if (stats_result == vk::Result::eSuccess &&
                stats.size() >= sizeof(vk::ShaderStatisticsInfoAMD)) {
                vk::ShaderStatisticsInfoAMD info{};
                std::memcpy(&info, stats.data(), sizeof(info));
                vgprs = info.numPhysicalVgprs;
                key += "/vgpr" + std::to_string(info.numPhysicalVgprs);
                stats_line = "physical vgprs " + std::to_string(info.numPhysicalVgprs) +
                             ", available " + std::to_string(info.numAvailableVgprs) +
                             ", used " + std::to_string(info.resourceUsage.numUsedVgprs) +
                             "; physical sgprs " + std::to_string(info.numPhysicalSgprs) +
                             ", used " + std::to_string(info.resourceUsage.numUsedSgprs) +
                             "; lds " + std::to_string(info.resourceUsage.ldsUsageSizeInBytes) +
                             "; workgroup " + std::to_string(info.computeWorkGroupSize[0]) +
                             "x" + std::to_string(info.computeWorkGroupSize[1]) + "x" +
                             std::to_string(info.computeWorkGroupSize[2]);
            }
            if (histogram.ShouldDump(key, 3)) {
                const auto [isa_result, isa] = instance.GetDevice().getShaderInfoAMD(
                    *pipeline, stage, vk::ShaderInfoTypeAMD::eDisassembly);
                if (isa_result == vk::Result::eSuccess && !isa.empty()) {
                    const auto dir =
                        Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) / "wave_dumps";
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    const std::string name = WaveDumpFileName(
                        stages, vgprs, histogram.DumpsOf(key), executable.subgroupSize);
                    std::ofstream out(dir / name, std::ios::binary);
                    out << "key " << key << "\n" << stats_line << "\n";
                    out.write(reinterpret_cast<const char*>(isa.data()),
                              static_cast<std::streamsize>(isa.size()));
                }
            }
        }
        news |= histogram.Add(std::move(key), executable.subgroupSize);
    }
    if (histogram.EndPipeline(news)) {
        std::printf("GPU: wave sizes after %u pipelines: %s\n", histogram.Pipelines(),
                    histogram.Summary().c_str());
    }
}

} // namespace Vulkan
