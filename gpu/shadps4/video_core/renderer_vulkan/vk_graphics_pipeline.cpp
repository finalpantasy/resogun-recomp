// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <fmt/format.h>
#include <boost/container/small_vector.hpp>

#include "common/assert.h"
#include "common/thread.h"
#include "shader_recompiler/backend/spirv/emit_spirv_discard_frag.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_blend_rewrite.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "common/elf_info.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

using Shader::Backend::SPIRV::AuxShaderType;

// resogun: where pipeline construction time goes (reported when the background warm-up ends).
std::atomic<u64> g_resogun_pipeline_total_ns, g_resogun_pipeline_driver_ns;
std::atomic<u32> g_resogun_pipelines_built;
namespace {
using Clock = std::chrono::steady_clock;
u64 NsSince(Clock::time_point start) {
    return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}
} // namespace

static constexpr std::array LogicalStageToStageBit = {
    vk::ShaderStageFlagBits::eFragment,
    vk::ShaderStageFlagBits::eTessellationControl,
    vk::ShaderStageFlagBits::eTessellationEvaluation,
    vk::ShaderStageFlagBits::eVertex,
    vk::ShaderStageFlagBits::eGeometry,
    vk::ShaderStageFlagBits::eCompute,
};

GraphicsPipeline::GraphicsPipeline(
    const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
    const Shader::Profile& profile, const GraphicsPipelineKey& key_,
    vk::PipelineCache pipeline_cache, std::span<const Shader::Info*, MaxShaderStages> infos,
    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader_,
    std::span<const vk::ShaderModule> modules, SerializationSupport& sdata, bool preloading,
    bool defer_handles)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache}, key{key_},
      fetch_shader{std::move(fetch_shader_)} {
    const auto started = Clock::now();
    const vk::Device device = instance.GetDevice();
    std::ranges::copy(infos, stages.begin());
    BuildDescSetLayout(preloading);
    const auto debug_str = GetDebugString();

    const vk::PushConstantRange push_constants = {
        .stageFlags = AllGraphicsStageBits,
        .offset = 0,
        .size = sizeof(Shader::PushData),
    };

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    SetObjectName(device, *pipeline_layout, "Graphics PipelineLayout {}", debug_str);
    g_resogun_pipeline_total_ns += NsSince(started);
    if (defer_handles) {
        handles_ready.store(false, std::memory_order_relaxed);
        return;
    }
    CreateHandles(pipeline_cache, infos, runtime_infos, modules, sdata, preloading);
}

void GraphicsPipeline::StartHandles(AsyncCompiler& compiler, std::function<void()> create) {
    auto run = [this, create = std::move(create)] {
        try {
            create();
        } catch (...) {
            pipeline.reset();
            square_pipeline.reset();
            LOG_ERROR(Render_Vulkan, "resogun: deferred graphics pipeline failed");
        }
        handles_ready.store(true, std::memory_order_release);
        return true;
    };
    if (!handle_job.Start(compiler, run, true)) {
        run();
    }
}

// resogun: pipeline builds a draw may be waiting for (CreateHandles) and when the last one ended.
// Optimized builds behind fast-linked pipelines start only after 100 ms without one, so they never
// take CPU time from a load (the compile workers have the same thread priority).
namespace {
std::atomic<u32> g_foreground_builds{0};
std::atomic<s64> g_foreground_end_ns{0};
struct ForegroundBuild {
    ForegroundBuild() {
        g_foreground_builds.fetch_add(1, std::memory_order_relaxed);
    }
    ~ForegroundBuild() {
        g_foreground_end_ns.store(Clock::now().time_since_epoch().count(),
                                  std::memory_order_relaxed);
        g_foreground_builds.fetch_sub(1, std::memory_order_release);
    }
};
/// Waits until no foreground build ran for 100 ms; false when `stop` was set meanwhile.
bool WaitForQuiet(const std::atomic<bool>& stop, const std::atomic<bool>& abandoned) {
    using namespace std::chrono_literals;
    for (;;) {
        if (stop.load(std::memory_order_relaxed) || abandoned.load(std::memory_order_relaxed)) {
            return false;
        }
        const auto last = Clock::time_point{
            Clock::duration{g_foreground_end_ns.load(std::memory_order_relaxed)}};
        if (g_foreground_builds.load(std::memory_order_acquire) == 0 &&
            Clock::now() - last >= 100ms) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
}
} // namespace

// resogun: everything one monolithic vkCreateGraphicsPipelines call reads, owned in one place so a
// background thread can build the optimized pipeline after the fast-linked one is in use. The
// pointers in `info` point into this object, so it never moves (always held by shared_ptr).
struct PipelineCreateState {
    PipelineCreateState() = default;
    PipelineCreateState(const PipelineCreateState&) = delete;
    PipelineCreateState& operator=(const PipelineCreateState&) = delete;

    GraphicsPipelineKey key;
    VertexInputs<vk::VertexInputAttributeDescription> vertex_attributes;
    VertexInputs<vk::VertexInputBindingDescription> vertex_bindings;
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    vk::PipelineVertexInputDivisorStateCreateInfo divisor_state;
    vk::PipelineVertexInputStateCreateInfo vertex_input;
    vk::PipelineInputAssemblyStateCreateInfo input_assembly;
    vk::PipelineTessellationStateCreateInfo tessellation;
    vk::StructureChain<vk::PipelineRasterizationStateCreateInfo,
                       vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT,
                       vk::PipelineRasterizationDepthClipStateCreateInfoEXT>
        raster_chain;
    bool has_provoking_vertex = false;
    bool has_depth_clip = false;
    vk::PipelineMultisampleStateCreateInfo multisampling;
    vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control;
    vk::PipelineViewportStateCreateInfo viewport;
    boost::container::static_vector<vk::DynamicState, 32> dynamic_states;
    vk::PipelineDynamicStateCreateInfo dynamic;
    boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages> stages;
    vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size;
    bool uses_subgroup_size = false;
    /// Generated tessellation or fragment shaders (rect and quad lists, clip distance discard).
    bool aux_shaders = false;
    vk::Format depth_format = vk::Format::eUndefined;
    std::array<vk::Format, Shader::IR::NumRenderTargets> color_formats{};
    std::array<vk::SampleCountFlagBits, AmdGpu::NUM_COLOR_BUFFERS> color_samples{};
    vk::AttachmentSampleCountInfoAMD mixed_samples;
    bool uses_mixed_samples = false;
    vk::PipelineRenderingCreateInfo rendering;
    std::array<vk::PipelineColorBlendAttachmentState, AmdGpu::NUM_COLOR_BUFFERS> attachments{};
    std::array<vk::PipelineColorBlendAttachmentState, AmdGpu::NUM_COLOR_BUFFERS>
        square_attachments{};
    bool needs_square_pass = false;
    vk::PipelineColorBlendStateCreateInfo color_blending;
    // Required by spec unless VK_EXT_extended_dynamic_state3 is supported.
    // In practice, we use dynamic state for all of it.
    vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    vk::GraphicsPipelineCreateInfo info;
};

// resogun: an optimized pipeline built behind a fast-linked one; shared with its build task.
struct OptimizedPipeline {
    std::atomic<VkPipeline> handle{VK_NULL_HANDLE};
    std::atomic<bool> abandoned{false};
    vk::UniquePipeline pipeline;
};

vk::Pipeline GraphicsPipeline::Handle() const noexcept {
    if (optimized) {
        if (const VkPipeline handle = optimized->handle.load(std::memory_order_acquire)) {
            return handle;
        }
    }
    return *pipeline;
}

void GraphicsPipeline::FillCreateState(PipelineCreateState& state,
                                       std::span<const Shader::Info* const, MaxShaderStages> infos,
                                       std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                                       std::span<const vk::ShaderModule> modules,
                                       SerializationSupport& sdata, bool preloading) {
    state.key = key;
    if (!preloading) {
        VertexInputs<AmdGpu::Buffer> guest_buffers;
        if (!instance.IsVertexInputDynamicState()) {
            const auto& vs_info = runtime_infos[u32(Shader::SwStage::Vertex)].sw.vs;
            GetVertexInputs(sdata.vertex_attributes, sdata.vertex_bindings, sdata.divisors,
                            guest_buffers, vs_info.step_rate_0, vs_info.step_rate_1);
        }
    }
    state.vertex_attributes = sdata.vertex_attributes;
    state.vertex_bindings = sdata.vertex_bindings;
    state.divisors = sdata.divisors;

    state.divisor_state = vk::PipelineVertexInputDivisorStateCreateInfo{
        .vertexBindingDivisorCount = static_cast<u32>(state.divisors.size()),
        .pVertexBindingDivisors = state.divisors.data(),
    };

    state.vertex_input = vk::PipelineVertexInputStateCreateInfo{
        .pNext = state.divisors.empty() ? nullptr : &state.divisor_state,
        .vertexBindingDescriptionCount = static_cast<u32>(state.vertex_bindings.size()),
        .pVertexBindingDescriptions = state.vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(state.vertex_attributes.size()),
        .pVertexAttributeDescriptions = state.vertex_attributes.data(),
    };

    const auto topology = LiverpoolToVK::PrimitiveType(key.prim_type);
    state.input_assembly = vk::PipelineInputAssemblyStateCreateInfo{
        .topology = topology,
    };

    const bool is_rect_list = key.prim_type == AmdGpu::PrimitiveType::RectList;
    const bool is_quad_list = key.prim_type == AmdGpu::PrimitiveType::QuadList;
    state.tessellation = vk::PipelineTessellationStateCreateInfo{
        .patchControlPoints = is_rect_list ? 3U : (is_quad_list ? 4U : key.patch_control_points),
    };

    state.raster_chain = {
        vk::PipelineRasterizationStateCreateInfo{
            .depthClampEnable = key.depth_clamp_enable &&
                                (!key.depth_clip_enable || instance.IsDepthClipEnableSupported()),
            .rasterizerDiscardEnable = false,
            .polygonMode = LiverpoolToVK::PolygonMode(key.polygon_mode),
            .lineWidth = 1.0f,
        },
        vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT{
            .provokingVertexMode = key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First
                                       ? vk::ProvokingVertexModeEXT::eFirstVertex
                                       : vk::ProvokingVertexModeEXT::eLastVertex,
        },
        vk::PipelineRasterizationDepthClipStateCreateInfoEXT{
            .depthClipEnable = key.depth_clip_enable,
        },
    };
    state.has_provoking_vertex = instance.IsProvokingVertexSupported();
    state.has_depth_clip = instance.IsDepthClipEnableSupported();
    if (!state.has_provoking_vertex) {
        state.raster_chain.unlink<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>();
    }
    if (!state.has_depth_clip) {
        state.raster_chain.unlink<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>();
    }

    if (!preloading) {
        const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
        sdata.multisampling = {
            .rasterizationSamples = LiverpoolToVK::NumSamples(
                key.num_samples, instance.GetColorSampleCounts() & instance.GetDepthSampleCounts()),
            .sampleShadingEnable =
                fs_info.addr_flags.persp_sample_ena || fs_info.addr_flags.linear_sample_ena,
        };
    }
    state.multisampling = sdata.multisampling;
    state.multisampling.pNext = nullptr;

    state.clip_control = vk::PipelineViewportDepthClipControlCreateInfoEXT{
        .negativeOneToOne = key.clip_space == AmdGpu::ClipSpace::MinusWToW,
    };

    state.viewport = vk::PipelineViewportStateCreateInfo{
        .pNext = instance.IsDepthClipControlSupported() ? &state.clip_control : nullptr,
    };

    state.dynamic_states = {
        vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
        vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
        vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,          vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::eLineWidth,          vk::DynamicState::ePrimitiveRestartEnable,
    };

    if (instance.IsDepthBoundsSupported()) {
        state.dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
        state.dynamic_states.push_back(vk::DynamicState::eDepthBounds);
    }
    if (instance.IsDynamicColorWriteMaskSupported()) {
        state.dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (instance.IsVertexInputDynamicState()) {
        state.dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);
    } else if (!state.vertex_bindings.empty()) {
        state.dynamic_states.push_back(vk::DynamicState::eVertexInputBindingStride);
    }

    state.dynamic = vk::PipelineDynamicStateCreateInfo{
        .dynamicStateCount = static_cast<u32>(state.dynamic_states.size()),
        .pDynamicStates = state.dynamic_states.data(),
    };

    auto& shader_stages = state.stages;
    auto stage = u32(Shader::SwStage::Vertex);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::Geometry);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eGeometry,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::SwStage::TessellationControl);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        const auto type = is_quad_list ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS;
        if (!preloading) {
            const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
            sdata.tcs = Shader::Backend::SPIRV::EmitAuxilaryTessShader(type, fs_info);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = CompileSPV(sdata.tcs, instance.GetDevice()),
            .pName = "main",
        });
        state.aux_shaders = true;
    }
    stage = u32(Shader::SwStage::TessellationEval);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        if (!preloading) {
            const auto& fs_info = runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs;
            sdata.tes = Shader::Backend::SPIRV::EmitAuxilaryTessShader(
                AuxShaderType::PassthroughTES, fs_info);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = CompileSPV(sdata.tes, instance.GetDevice()),
            .pName = "main",
        });
        state.aux_shaders = true;
    }
    stage = u32(Shader::SwStage::Fragment);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (runtime_infos[u32(Shader::SwStage::Fragment)].hw.fs.clip_distance_emulation) {
        if (!preloading) {
            const auto& vs = runtime_infos[static_cast<u32>(Shader::SwStage::Vertex)].hw.vs;

            sdata.fragment = Shader::Backend::SPIRV::EmitDiscardFragmentShader(vs.outputs);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = CompileSPV(sdata.fragment, instance.GetDevice()),
            .pName = "main",
        });
        state.aux_shaders = true;
    }

    state.subgroup_size = vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo{
        .requiredSubgroupSize = 64,
    };
    state.uses_subgroup_size = instance.IsSubgroupSize64Supported();
    for (auto& shader_stage : shader_stages) {
        shader_stage.pNext = state.uses_subgroup_size ? &state.subgroup_size : nullptr;
    }

    state.depth_format =
        instance.GetSupportedFormat(LiverpoolToVK::DepthFormat(key.z_format, key.stencil_format),
                                    vk::FormatFeatureFlagBits2::eDepthStencilAttachment);
    for (s32 i = 0; i < key.num_color_attachments; ++i) {
        const auto& col_buf = key.color_buffers[i];
        const auto format = LiverpoolToVK::SurfaceFormat(col_buf.data_format, col_buf.num_format);
        const auto color_format =
            instance.GetSupportedFormat(format, vk::FormatFeatureFlagBits2::eColorAttachment);
        if (!instance.IsFormatSupported(color_format,
                                        vk::FormatFeatureFlagBits2::eColorAttachment)) {
            LOG_WARNING(Render_Vulkan,
                        "color buffer format {} does not support COLOR_ATTACHMENT_BIT",
                        vk::to_string(color_format));
        }
        state.color_formats[i] = color_format;
    }

    std::ranges::transform(key.color_samples, state.color_samples.begin(), [this](u8 num_samples) {
        return num_samples ? LiverpoolToVK::NumSamples(num_samples, instance.GetColorSampleCounts())
                           : vk::SampleCountFlagBits::e1;
    });
    state.mixed_samples = vk::AttachmentSampleCountInfoAMD{
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentSamples = state.color_samples.data(),
        .depthStencilAttachmentSamples =
            LiverpoolToVK::NumSamples(key.depth_samples, instance.GetDepthSampleCounts()),
    };
    state.uses_mixed_samples = instance.IsMixedDepthSamplesSupported();

    state.rendering = vk::PipelineRenderingCreateInfo{
        .pNext = state.uses_mixed_samples ? &state.mixed_samples : nullptr,
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentFormats = state.color_formats.data(),
        .depthAttachmentFormat = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid
                                     ? state.depth_format
                                     : vk::Format::eUndefined,
        .stencilAttachmentFormat = key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid
                                       ? state.depth_format
                                       : vk::Format::eUndefined,
    };

    auto& attachments = state.attachments;
    // resogun: GNM scales MIN/MAX operands, Vulkan does not (vk_blend_rewrite.h). A self-scaled
    // MIN/MAX is drawn twice: native, then this pipeline squares the destination.
    auto& square_attachments = state.square_attachments;
    bool needs_square_pass = false;
    for (u32 i = 0; i < key.num_color_attachments; i++) {
        const auto& control = key.blend_controls[i];
        const auto target_format = key.color_buffers[i].num_format;
        BlendEquation color_equation{control.color_src_factor, control.color_func,
                                     control.color_dst_factor};
        BlendEquation alpha_equation{
            control.separate_alpha_blend ? control.alpha_src_factor : control.color_src_factor,
            control.separate_alpha_blend ? control.alpha_func : control.color_func,
            control.separate_alpha_blend ? control.alpha_dst_factor : control.color_dst_factor};
        const auto color_rewrite =
            RewriteScaledMinMaxBlend(color_equation, target_format, BlendChannel::Color);
        const auto alpha_rewrite =
            RewriteScaledMinMaxBlend(alpha_equation, target_format, BlendChannel::Alpha);
        const bool writes_color = bool(key.write_masks[i] & (vk::ColorComponentFlagBits::eR |
                                                             vk::ColorComponentFlagBits::eG |
                                                             vk::ColorComponentFlagBits::eB));
        const bool writes_alpha = bool(key.write_masks[i] & vk::ColorComponentFlagBits::eA);
        if (control.enable &&
            ((writes_color && color_rewrite == BlendRewriteResult::Unsupported) ||
             (writes_alpha && alpha_rewrite == BlendRewriteResult::Unsupported))) {
            LOG_WARNING(Render_Vulkan,
                        "Scaled MIN/MAX blend on attachment {} has no Vulkan equivalent "
                        "(format {}, color {}/{}/{}, alpha {}/{}/{})",
                        i, u32(target_format), u32(color_equation.src_factor),
                        u32(color_equation.function), u32(color_equation.dst_factor),
                        u32(alpha_equation.src_factor), u32(alpha_equation.function),
                        u32(alpha_equation.dst_factor));
        }
        const bool square_color =
            control.enable && writes_color && color_rewrite == BlendRewriteResult::Squared;
        const bool square_alpha =
            control.enable && writes_alpha && alpha_rewrite == BlendRewriteResult::Squared;
        needs_square_pass |= square_color || square_alpha;

        const auto src_color = LiverpoolToVK::BlendFactor(color_equation.src_factor);
        const auto dst_color = LiverpoolToVK::BlendFactor(color_equation.dst_factor);

        attachments[i] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = control.enable,
            .srcColorBlendFactor = src_color,
            .dstColorBlendFactor = dst_color,
            .colorBlendOp = LiverpoolToVK::BlendOp(color_equation.function),
            .srcAlphaBlendFactor = LiverpoolToVK::BlendFactor(alpha_equation.src_factor),
            .dstAlphaBlendFactor = LiverpoolToVK::BlendFactor(alpha_equation.dst_factor),
            .alphaBlendOp = LiverpoolToVK::BlendOp(alpha_equation.function),
            .colorWriteMask =
                instance.IsDynamicColorWriteMaskSupported()
                    ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
                    : key.write_masks[i],
        };
        square_attachments[i] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = true,
            .srcColorBlendFactor = vk::BlendFactor::eZero,
            .dstColorBlendFactor = square_color ? vk::BlendFactor::eDstColor : vk::BlendFactor::eOne,
            .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eZero,
            .dstAlphaBlendFactor = square_alpha ? vk::BlendFactor::eDstAlpha : vk::BlendFactor::eOne,
            .alphaBlendOp = vk::BlendOp::eAdd,
            .colorWriteMask = attachments[i].colorWriteMask,
        };

        // On GCN GPU there is an additional mask which allows to control color components exported
        // from a pixel shader. A situation possible, when the game may mask out the alpha channel,
        // while it is still need to be used in blending ops. For such cases, HW will default alpha
        // to 1 and perform the blending, while shader normally outputs 0 in the last component.
        // Unfortunatelly, Vulkan doesn't provide any control on blend inputs, so below we detecting
        // such cases and override alpha value in order to emulate HW behaviour.
        const auto has_alpha_masked_out =
            (key.cb_shader_mask.GetMask(i) & AmdGpu::ColorBufferMask::ComponentA) == 0;
        const auto has_src_alpha_in_src_blend = src_color == vk::BlendFactor::eSrcAlpha ||
                                                src_color == vk::BlendFactor::eOneMinusSrcAlpha;
        const auto has_src_alpha_in_dst_blend = dst_color == vk::BlendFactor::eSrcAlpha ||
                                                dst_color == vk::BlendFactor::eOneMinusSrcAlpha;
        if (has_alpha_masked_out && has_src_alpha_in_src_blend) {
            attachments[i].srcColorBlendFactor = src_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
        if (has_alpha_masked_out && has_src_alpha_in_dst_blend) {
            attachments[i].dstColorBlendFactor = dst_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
    }
    state.needs_square_pass = needs_square_pass;

    state.color_blending = vk::PipelineColorBlendStateCreateInfo{
        .logicOpEnable =
            instance.IsLogicOpSupported() && key.logic_op != AmdGpu::ColorControl::LogicOp::Copy,
        .logicOp = LiverpoolToVK::LogicOp(key.logic_op),
        .attachmentCount = key.num_color_attachments,
        .pAttachments = attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    state.info = vk::GraphicsPipelineCreateInfo{
        .pNext = &state.rendering,
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = !instance.IsVertexInputDynamicState() ? &state.vertex_input : nullptr,
        .pInputAssemblyState = &state.input_assembly,
        .pTessellationState = &state.tessellation,
        .pViewportState = &state.viewport,
        .pRasterizationState = &state.raster_chain.get(),
        .pMultisampleState = &state.multisampling,
        .pDepthStencilState =
            !instance.IsExtendedDynamicState3Supported() ? &state.depth_stencil : nullptr,
        .pColorBlendState = &state.color_blending,
        .pDynamicState = &state.dynamic,
        .layout = *pipeline_layout,
    };
}

void GraphicsPipeline::CreateHandles(vk::PipelineCache pipeline_cache,
                                     std::span<const Shader::Info* const, MaxShaderStages> infos,
                                     std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                                     std::span<const vk::ShaderModule> modules,
                                     SerializationSupport& sdata, bool preloading) {
    const auto started = Clock::now();
    const vk::Device device = instance.GetDevice();
    const auto debug_str = GetDebugString();
    ForegroundBuild foreground;

    auto state = std::make_shared<PipelineCreateState>();
    FillCreateState(*state, infos, runtime_infos, modules, sdata, preloading);

    // resogun: warm-up (preloading) keeps building the optimized pipeline directly.
    if (auto* libraries = preloading ? nullptr : GraphicsPipelineLibraries::Get();
        libraries && CreateFromLibraries(*libraries, pipeline_cache, state)) {
        SetObjectName(device, *pipeline, "Graphics Pipeline {}", debug_str);
        g_resogun_pipeline_total_ns += NsSince(started);
        ++g_resogun_pipelines_built;
        return;
    }

    const auto binary_directory = Common::FS::GetShaderCachePath(Common::ElfInfo::Instance().GameSerial()) /
        "pipeline-binaries";
    auto [pipeline_result, pipe] =
        CreateCachedGraphicsPipeline(device, pipeline_cache, state->info, key,
                                     instance.GetPipelineBinaryGlobalKey(), binary_directory);
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Graphics Pipeline {}", debug_str);
    RecordWaveSizes();

    if (state->needs_square_pass) {
        auto square_blending = state->color_blending;
        square_blending.logicOpEnable = false;
        square_blending.pAttachments = state->square_attachments.data();
        auto square_info = state->info;
        square_info.pColorBlendState = &square_blending;
        auto [square_result, square] =
            CreateCachedGraphicsPipeline(device, pipeline_cache, square_info, key,
                                         instance.GetPipelineBinaryGlobalKey(), binary_directory);
        ASSERT_MSG(square_result == vk::Result::eSuccess,
                   "Failed to create the squaring pipeline: {}", vk::to_string(square_result));
        square_pipeline = std::move(square);
        SetObjectName(device, *square_pipeline, "Graphics Pipeline {} squaring pass", debug_str);
    }
    g_resogun_pipeline_total_ns += NsSince(started);
    ++g_resogun_pipelines_built;
}

// ---------------------------------------------------------------------------------------------
// resogun: graphics pipeline libraries.

namespace {
GraphicsPipelineLibraries* g_pipeline_libraries = nullptr;

enum PartKind : u32 { VertexInputPart, PreRasterPart, FragmentPart, OutputPart, NumPartKinds };
constexpr std::array<const char*, NumPartKinds> PartNames = {"vertex input", "pre-raster",
                                                             "fragment", "output"};

struct LibraryStats {
    std::array<std::atomic<u32>, NumPartKinds> built{}, reused{}, failed{};
    std::array<std::atomic<u64>, NumPartKinds> build_ns{};
    std::atomic<u32> links{}, link_failures{}, restored{}, ineligible{};
    std::atomic<u64> link_ns{}, restore_ns{};
    std::atomic<u32> optimized{}, optimize_failed{}, optimize_dropped{};
    std::atomic<u64> optimize_ns{};
    std::atomic<u64> part_wait_ns{};
};
LibraryStats g_library_stats;

/// Byte string of the Vulkan state one part takes; the part cache key.
struct PartKey {
    std::string bytes;
    template <typename T>
    void Add(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
    }
};
} // namespace

struct GraphicsPipelineLibraries::Impl {
    struct Part {
        std::mutex mutex;
        std::condition_variable cv;
        bool ready = false;
        vk::UniquePipeline pipeline;
    };
    struct Layout {
        vk::UniqueDescriptorSetLayout set_layout;
        vk::UniquePipelineLayout pipeline_layout;
    };

    const Instance& instance;
    std::mutex parts_mutex;
    std::array<std::unordered_map<std::string, std::shared_ptr<Part>>, NumPartKinds> parts;
    std::mutex layouts_mutex;
    std::unordered_map<std::string, Layout> layouts;
    vk::UniquePipelineCache optimizer_cache;
    std::unique_ptr<AsyncCompiler> optimizer;
    std::atomic<bool> stopping{false};

    explicit Impl(const Instance& instance_) : instance{instance_} {
        auto [result, cache] = instance.GetDevice().createPipelineCacheUnique({});
        ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create optimizer pipeline cache");
        optimizer_cache = std::move(cache);
        // Low priority, so the optimized builds never take the CPU from draw-path compiles.
        const char* threads_env = std::getenv("RESOGUN_PIPELINE_OPTIMIZE_THREADS");
        const unsigned threads =
            threads_env ? std::clamp(unsigned(std::atoi(threads_env)), 0u, 8u) : 2u;
        if (threads) {
            optimizer = std::make_unique<AsyncCompiler>(threads, 16384, [](unsigned index) {
                Common::SetCurrentThreadName(
                    ("resogun:PipelineOptimize" + std::to_string(index)).c_str());
                Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
            });
        }
    }

    /// A pipeline layout identical to a pipeline's own (one descriptor set, push constants), shared
    /// by every library of that layout: pre-rasterization and fragment libraries linked together
    /// need identically defined layouts.
    vk::PipelineLayout GetLayout(const std::vector<vk::DescriptorSetLayoutBinding>& bindings,
                                 vk::DescriptorSetLayoutCreateFlags flags) {
        PartKey key;
        key.Add(u32(flags));
        for (const auto& b : bindings) {
            key.Add(b.binding);
            key.Add(b.descriptorType);
            key.Add(b.descriptorCount);
            key.Add(u32(b.stageFlags));
        }
        std::scoped_lock lock{layouts_mutex};
        if (const auto it = layouts.find(key.bytes); it != layouts.end()) {
            return *it->second.pipeline_layout;
        }
        const vk::Device device = instance.GetDevice();
        auto [set_result, set_layout] = device.createDescriptorSetLayoutUnique({
            .flags = flags,
            .bindingCount = static_cast<u32>(bindings.size()),
            .pBindings = bindings.data(),
        });
        if (set_result != vk::Result::eSuccess) {
            return {};
        }
        const vk::PushConstantRange push_constants = {
            .stageFlags = AllGraphicsStageBits,
            .offset = 0,
            .size = sizeof(Shader::PushData),
        };
        const vk::DescriptorSetLayout set = *set_layout;
        auto [layout_result, layout] = device.createPipelineLayoutUnique({
            .setLayoutCount = 1U,
            .pSetLayouts = &set,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constants,
        });
        if (layout_result != vk::Result::eSuccess) {
            return {};
        }
        const vk::PipelineLayout handle = *layout;
        layouts.emplace(std::move(key.bytes), Layout{std::move(set_layout), std::move(layout)});
        return handle;
    }

    /// The part for `key`, built here by `build` when no thread has; a thread that finds it being
    /// built waits for that build. Null when it failed.
    template <typename Build>
    vk::Pipeline GetPart(PartKind kind, std::string&& key, Build&& build) {
        std::shared_ptr<Part> part;
        bool owner = false;
        {
            std::scoped_lock lock{parts_mutex};
            auto [it, inserted] = parts[kind].try_emplace(std::move(key));
            if (inserted) {
                it->second = std::make_shared<Part>();
                owner = true;
            }
            part = it->second;
        }
        if (owner) {
            const auto started = Clock::now();
            vk::Result result = vk::Result::eErrorUnknown;
            vk::UniquePipeline created;
            try {
                auto built = build();
                result = built.result;
                created = std::move(built.value);
            } catch (...) {
                // Waiting threads must still be released; the part stays failed.
            }
            if (result == vk::Result::eSuccess) {
                ++g_library_stats.built[kind];
                g_library_stats.build_ns[kind] += NsSince(started);
            } else {
                ++g_library_stats.failed[kind];
                LOG_ERROR(Render_Vulkan, "resogun: {} pipeline library failed: {}",
                          PartNames[kind], vk::to_string(result));
            }
            std::scoped_lock lock{part->mutex};
            part->pipeline = std::move(created);
            part->ready = true;
            part->cv.notify_all();
            return *part->pipeline;
        }
        ++g_library_stats.reused[kind];
        std::unique_lock lock{part->mutex};
        if (!part->ready) {
            const auto started = Clock::now();
            part->cv.wait(lock, [&] { return part->ready; });
            g_library_stats.part_wait_ns += NsSince(started);
        }
        return *part->pipeline;
    }
};

GraphicsPipelineLibraries::GraphicsPipelineLibraries(const Instance& instance)
    : impl{std::make_unique<Impl>(instance)} {
    g_pipeline_libraries = this;
}

GraphicsPipelineLibraries::~GraphicsPipelineLibraries() {
    StopOptimizer();
    g_pipeline_libraries = nullptr;
}

GraphicsPipelineLibraries* GraphicsPipelineLibraries::Get() noexcept {
    return g_pipeline_libraries;
}

void GraphicsPipelineLibraries::StopOptimizer() {
    impl->stopping = true;
    if (impl->optimizer) {
        impl->optimizer->Stop(); // queued tasks see `stopping` and return at once
    }
}

void GraphicsPipelineLibraries::Report() {
    auto& s = g_library_stats;
    std::string parts;
    u64 total_ns = 0;
    u32 total = 0;
    for (u32 i = 0; i < NumPartKinds; ++i) {
        const u32 built = s.built[i].exchange(0);
        const u64 ns = s.build_ns[i].exchange(0);
        total += built;
        total_ns += ns;
        parts += fmt::format("{}{} {} built ({:.1f} ms) {} reused", parts.empty() ? "" : ", ",
                             PartNames[i], built, ns / 1e6, s.reused[i].exchange(0));
        if (const u32 failed = s.failed[i].exchange(0)) {
            parts += fmt::format(" {} failed", failed);
        }
    }
    const u32 links = s.links.exchange(0), restored = s.restored.exchange(0);
    const u32 optimized = s.optimized.exchange(0);
    if (!total && !links && !restored && !optimized) {
        return;
    }
    std::printf("Pipeline libraries: %u parts %.1f ms (%s); part waits %.1f ms; %u linked %.1f ms "
                "(%u failed); %u restored from binaries %.1f ms; %u not eligible; optimized %u "
                "%.1f ms in background (%u failed, %u dropped)\n",
                total, total_ns / 1e6, parts.c_str(), s.part_wait_ns.exchange(0) / 1e6, links,
                s.link_ns.exchange(0) / 1e6, s.link_failures.exchange(0), restored,
                s.restore_ns.exchange(0) / 1e6, s.ineligible.exchange(0), optimized,
                s.optimize_ns.exchange(0) / 1e6, s.optimize_failed.exchange(0),
                s.optimize_dropped.exchange(0));
}

bool GraphicsPipeline::CreateFromLibraries(GraphicsPipelineLibraries& libraries,
                                           vk::PipelineCache pipeline_cache,
                                           std::shared_ptr<PipelineCreateState> state) {
    auto& lib = *libraries.impl;
    // Shapes the libraries do not cover take the monolithic path: generated shaders (keyed by
    // more than their stage), the squaring pass (a second pipeline), tessellation, and static
    // vertex input or depth-stencil state.
    const bool tessellation = std::ranges::any_of(state->stages, [](const auto& stage) {
        return stage.stage == vk::ShaderStageFlagBits::eTessellationControl ||
               stage.stage == vk::ShaderStageFlagBits::eTessellationEvaluation;
    });
    if (state->aux_shaders || state->needs_square_pass || tessellation ||
        !instance.IsVertexInputDynamicState() || !instance.IsExtendedDynamicState3Supported()) {
        ++g_library_stats.ineligible;
        return false;
    }
    const vk::Device device = instance.GetDevice();
    const auto binary_directory =
        Common::FS::GetShaderCachePath(Common::ElfInfo::Instance().GameSerial()) /
        "pipeline-binaries";

    // A stored binary of the optimized pipeline restores faster than any link.
    {
        const auto started = Clock::now();
        auto [result, restored] =
            CreateCachedGraphicsPipeline(device, pipeline_cache, state->info, key,
                                         instance.GetPipelineBinaryGlobalKey(), binary_directory,
                                         true);
        if (result == vk::Result::eSuccess && restored) {
            pipeline = std::move(restored);
            ++g_library_stats.restored;
            g_library_stats.restore_ns += NsSince(started);
            return true;
        }
    }

    const vk::PipelineLayout layout = lib.GetLayout(layout_bindings, layout_flags);
    if (!layout) {
        return false;
    }
    const auto add_dynamic = [&](PartKey& part_key) {
        part_key.Add(u32(state->dynamic_states.size()));
        for (const auto dynamic_state : state->dynamic_states) {
            part_key.Add(dynamic_state);
        }
    };
    const auto add_multisampling = [&](PartKey& part_key) {
        part_key.Add(state->multisampling.rasterizationSamples);
        part_key.Add(state->multisampling.sampleShadingEnable);
        part_key.Add(state->multisampling.minSampleShading);
        part_key.Add(state->multisampling.alphaToCoverageEnable);
        part_key.Add(state->multisampling.alphaToOneEnable);
    };
    const auto library_info = [](vk::GraphicsPipelineLibraryFlagsEXT flags) {
        return vk::GraphicsPipelineLibraryCreateInfoEXT{.flags = flags};
    };
    const auto create = [&](const vk::GraphicsPipelineCreateInfo& info) {
        return device.createGraphicsPipelineUnique(pipeline_cache, info);
    };

    // Vertex input interface: the topology (vertex input and primitive restart are dynamic).
    PartKey vertex_key;
    vertex_key.Add(state->input_assembly.topology);
    add_dynamic(vertex_key);
    const vk::Pipeline vertex_part = lib.GetPart(VertexInputPart, std::move(vertex_key.bytes), [&] {
        const auto part = library_info(vk::GraphicsPipelineLibraryFlagBitsEXT::eVertexInputInterface);
        return create({
            .pNext = &part,
            .flags = vk::PipelineCreateFlagBits::eLibraryKHR,
            .pInputAssemblyState = &state->input_assembly,
            .pDynamicState = &state->dynamic,
        });
    });

    // Pre-rasterization: the vertex (and geometry) modules, the layout and rasterization state.
    const vk::PipelineRenderingCreateInfo no_attachments{};
    boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages> pre_stages;
    const vk::PipelineShaderStageCreateInfo* fragment_stage = nullptr;
    for (const auto& stage : state->stages) {
        if (stage.stage == vk::ShaderStageFlagBits::eFragment) {
            fragment_stage = &stage;
        } else {
            pre_stages.push_back(stage);
        }
    }
    // Module handles plus the stage hashes (hash and permutation): a destroyed module's handle
    // may come back for another shader.
    PartKey pre_key;
    pre_key.Add(key.stage_hashes[u32(Shader::SwStage::Vertex)]);
    pre_key.Add(key.stage_hashes[u32(Shader::SwStage::Geometry)]);
    for (const auto& stage : pre_stages) {
        pre_key.Add(u32(stage.stage));
        pre_key.Add(static_cast<VkShaderModule>(stage.module));
    }
    pre_key.Add(state->uses_subgroup_size);
    pre_key.Add(static_cast<VkPipelineLayout>(layout));
    const auto& raster = state->raster_chain.get<vk::PipelineRasterizationStateCreateInfo>();
    pre_key.Add(raster.depthClampEnable);
    pre_key.Add(raster.rasterizerDiscardEnable);
    pre_key.Add(raster.polygonMode);
    pre_key.Add(raster.lineWidth);
    pre_key.Add(state->has_provoking_vertex
                    ? u32(state->raster_chain
                              .get<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>()
                              .provokingVertexMode)
                    : ~0u);
    pre_key.Add(state->has_depth_clip
                    ? u32(state->raster_chain
                              .get<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>()
                              .depthClipEnable)
                    : ~0u);
    pre_key.Add(state->viewport.pNext ? u32(state->clip_control.negativeOneToOne) : ~0u);
    add_dynamic(pre_key);
    const vk::Pipeline pre_part = lib.GetPart(PreRasterPart, std::move(pre_key.bytes), [&] {
        auto part = library_info(vk::GraphicsPipelineLibraryFlagBitsEXT::ePreRasterizationShaders);
        part.pNext = &no_attachments;
        return create({
            .pNext = &part,
            .flags = vk::PipelineCreateFlagBits::eLibraryKHR,
            .stageCount = static_cast<u32>(pre_stages.size()),
            .pStages = pre_stages.data(),
            .pViewportState = &state->viewport,
            .pRasterizationState = &state->raster_chain.get(),
            .pDynamicState = &state->dynamic,
            .layout = layout,
        });
    });

    // Fragment shader: the module, the layout, multisampling and the depth-stencil formats.
    const vk::PipelineRenderingCreateInfo depth_only{
        .depthAttachmentFormat = state->rendering.depthAttachmentFormat,
        .stencilAttachmentFormat = state->rendering.stencilAttachmentFormat,
    };
    PartKey fragment_key;
    fragment_key.Add(key.stage_hashes[u32(Shader::SwStage::Fragment)]);
    fragment_key.Add(fragment_stage ? static_cast<VkShaderModule>(fragment_stage->module)
                                    : VkShaderModule{});
    fragment_key.Add(state->uses_subgroup_size);
    fragment_key.Add(static_cast<VkPipelineLayout>(layout));
    add_multisampling(fragment_key);
    fragment_key.Add(depth_only.depthAttachmentFormat);
    fragment_key.Add(depth_only.stencilAttachmentFormat);
    add_dynamic(fragment_key);
    const vk::Pipeline fragment_part = lib.GetPart(FragmentPart, std::move(fragment_key.bytes), [&] {
        auto part = library_info(vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentShader);
        part.pNext = &depth_only;
        return create({
            .pNext = &part,
            .flags = vk::PipelineCreateFlagBits::eLibraryKHR,
            .stageCount = fragment_stage ? 1u : 0u,
            .pStages = fragment_stage,
            .pMultisampleState = &state->multisampling,
            .pDepthStencilState = &state->depth_stencil,
            .pDynamicState = &state->dynamic,
            .layout = layout,
        });
    });

    // Fragment output: attachment formats and samples, blending, logic op.
    PartKey output_key;
    output_key.Add(state->rendering.colorAttachmentCount);
    for (u32 i = 0; i < state->rendering.colorAttachmentCount; ++i) {
        output_key.Add(state->color_formats[i]);
        output_key.Add(state->attachments[i]);
    }
    output_key.Add(state->rendering.depthAttachmentFormat);
    output_key.Add(state->rendering.stencilAttachmentFormat);
    output_key.Add(state->uses_mixed_samples);
    if (state->uses_mixed_samples) {
        for (u32 i = 0; i < state->mixed_samples.colorAttachmentCount; ++i) {
            output_key.Add(state->color_samples[i]);
        }
        output_key.Add(state->mixed_samples.depthStencilAttachmentSamples);
    }
    output_key.Add(state->color_blending.logicOpEnable);
    output_key.Add(state->color_blending.logicOp);
    output_key.Add(state->color_blending.blendConstants);
    add_multisampling(output_key);
    add_dynamic(output_key);
    const vk::Pipeline output_part = lib.GetPart(OutputPart, std::move(output_key.bytes), [&] {
        auto part = library_info(vk::GraphicsPipelineLibraryFlagBitsEXT::eFragmentOutputInterface);
        part.pNext = &state->rendering;
        return create({
            .pNext = &part,
            .flags = vk::PipelineCreateFlagBits::eLibraryKHR,
            .pMultisampleState = &state->multisampling,
            .pColorBlendState = &state->color_blending,
            .pDynamicState = &state->dynamic,
        });
    });

    if (!vertex_part || !pre_part || !fragment_part || !output_part) {
        return false;
    }

    // Fast link: no link-time optimization, so it costs about a millisecond.
    const std::array part_handles{vertex_part, pre_part, fragment_part, output_part};
    const vk::PipelineLibraryCreateInfoKHR linked_parts{
        .libraryCount = static_cast<u32>(part_handles.size()),
        .pLibraries = part_handles.data(),
    };
    const auto link_started = Clock::now();
    auto [link_result, linked] = create({.pNext = &linked_parts, .layout = layout});
    if (link_result != vk::Result::eSuccess) {
        ++g_library_stats.link_failures;
        LOG_ERROR(Render_Vulkan, "resogun: pipeline library link failed: {}",
                  vk::to_string(link_result));
        return false;
    }
    ++g_library_stats.links;
    g_library_stats.link_ns += NsSince(link_started);
    pipeline = std::move(linked);

    // The optimized pipeline (and its stored binary) follows on a low-priority thread. It uses the
    // shared layout, which outlives this pipeline; binding with this pipeline's identical layout
    // stays valid.
    if (!lib.optimizer) {
        return true;
    }
    state->info.layout = layout;
    auto slot = std::make_shared<OptimizedPipeline>();
    optimized = slot;
    const Instance* inst = &instance;
    auto* impl = &lib;
    const bool queued = lib.optimizer->Submit([impl, inst, state, slot, binary_directory] {
        if (!WaitForQuiet(impl->stopping, slot->abandoned)) {
            ++g_library_stats.optimize_dropped;
            return;
        }
        const auto started = Clock::now();
        g_resogun_background_compile = true;
        vk::Result result = vk::Result::eErrorUnknown;
        vk::UniquePipeline built;
        try {
            auto created = CreateCachedGraphicsPipeline(
                inst->GetDevice(), *impl->optimizer_cache, state->info, state->key,
                inst->GetPipelineBinaryGlobalKey(), binary_directory);
            result = created.result;
            built = std::move(created.value);
        } catch (...) {
        }
        g_resogun_background_compile = false;
        if (result != vk::Result::eSuccess || !built) {
            ++g_library_stats.optimize_failed;
            return;
        }
        g_library_stats.optimize_ns += NsSince(started);
        ++g_library_stats.optimized;
        slot->pipeline = std::move(built);
        slot->handle.store(*slot->pipeline, std::memory_order_release);
    });
    if (!queued) {
        ++g_library_stats.optimize_dropped;
    }
    return true;
}

GraphicsPipeline::~GraphicsPipeline() {
    WaitHandles(); // a deferred build writes into this object
    if (optimized) {
        optimized->abandoned.store(true, std::memory_order_relaxed);
    }
}

template <typename Attribute, typename Binding>
void GraphicsPipeline::GetVertexInputs(
    VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    if (!fetch_shader || fetch_shader->attributes.empty()) {
        return;
    }
    const auto& vs_info = GetStage(Shader::SwStage::Vertex);
    for (const auto& attrib : fetch_shader->attributes) {
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = attrib.GetSharp(vs_info);
        attributes.push_back(Attribute{
            .location = attrib.semantic,
            .binding = attrib.semantic,
            .format = LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt()),
            .offset = 0,
        });
        bindings.push_back(Binding{
            .binding = attrib.semantic,
            .stride = buffer.GetStride(),
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
        });
        const u32 divisor = step_rate == InstanceIdType::OverStepRate0
                                ? step_rate_0
                                : (step_rate == InstanceIdType::OverStepRate1 ? step_rate_1 : 1);
        if constexpr (std::is_same_v<Binding, vk::VertexInputBindingDescription2EXT>) {
            bindings.back().divisor = divisor;
        } else if (step_rate != InstanceIdType::None) {
            divisors.push_back(vk::VertexInputBindingDivisorDescriptionEXT{
                .binding = attrib.semantic,
                .divisor = divisor,
            });
        }
        guest_buffers.emplace_back(buffer);
    }
}

// Declare templated GetVertexInputs for necessary types.
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription>& attributes,
    VertexInputs<vk::VertexInputBindingDescription>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription2EXT>& attributes,
    VertexInputs<vk::VertexInputBindingDescription2EXT>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;

void GraphicsPipeline::BuildDescSetLayout(bool preloading) {
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    u32 binding{};

    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const auto stage_bit = LogicalStageToStageBit[u32(stage->sw_stage)];
        for (const auto& buffer : stage->buffers) {
            const auto sharp =
                preloading ? AmdGpu::Buffer{}
                           : buffer.GetSharp(*stage); // See for the comment in compute PL creation
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
        for (const auto& image : stage->images) {
            const u32 num_bindings = image.NumBindings(*stage);
            bindings.push_back({
                .binding = binding,
                .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                .descriptorCount = num_bindings,
                .stageFlags = stage_bit,
            });
            binding += num_bindings;
        }
        for (const auto& sampler : stage->samplers) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eSampler,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
    }
    uses_push_descriptors = binding < instance.MaxPushDescriptors();
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    layout_bindings.assign(bindings.begin(), bindings.end());
    layout_flags = flags;
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto [layout_result, layout] =
        instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics descriptor set layout: {}", vk::to_string(layout_result));
    desc_layout = std::move(layout);
}

} // namespace Vulkan
