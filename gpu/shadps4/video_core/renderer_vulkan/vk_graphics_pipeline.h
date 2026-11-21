// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>

#include <boost/container/static_vector.hpp>
#include <xxhash.h>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/regs_primitive.h"
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
#include "video_core/renderer_vulkan/vk_async_compiler.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "resogun_toggles.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Vulkan {

static constexpr u32 MaxShaderStages = static_cast<u32>(Shader::SwStage::NumLogicalStages);
static constexpr u32 MaxVertexBufferCount = 32;

class Instance;
class Scheduler;
class DescriptorHeap;

template <typename T>
using VertexInputs = boost::container::static_vector<T, MaxVertexBufferCount>;

struct GraphicsPipelineKey {
    std::array<size_t, MaxShaderStages> stage_hashes;
    std::array<vk::Format, MaxVertexBufferCount> vertex_buffer_formats;
    u32 patch_control_points;
    u32 num_color_attachments;
    std::array<Shader::PsColorBuffer, AmdGpu::NUM_COLOR_BUFFERS> color_buffers;
    std::array<AmdGpu::BlendControl, AmdGpu::NUM_COLOR_BUFFERS> blend_controls;
    std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS> write_masks;
    AmdGpu::ColorBufferMask cb_shader_mask;
    AmdGpu::ColorControl::LogicOp logic_op;
    u8 num_samples;
    u8 depth_samples;
    std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    u32 mrt_mask;
    struct {
        AmdGpu::DepthBuffer::ZFormat z_format : 2;
        AmdGpu::DepthBuffer::StencilFormat stencil_format : 1;
        u32 depth_clamp_enable : 1;
    };
    struct {
        AmdGpu::PrimitiveType prim_type : 5;
        AmdGpu::PolygonMode polygon_mode : 2;
        AmdGpu::ClipSpace clip_space : 1;
        AmdGpu::ProvokingVtxLast provoking_vtx_last : 1;
        u32 depth_clip_enable : 1;
    };

    GraphicsPipelineKey() {
        std::memset(this, 0, sizeof(*this));
    }

    bool operator==(const GraphicsPipelineKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

struct PipelineCreateState;
struct OptimizedPipeline;

/// resogun: VK_EXT_graphics_pipeline_library (RESOGUN_PIPELINE_LIBRARY=1). A first-visit graphics
/// pipeline is fast-linked from four cached parts (vertex input, pre-rasterization, fragment
/// shader, fragment output), each keyed by only the Vulkan state it takes; the optimized
/// monolithic pipeline is then built on a low-priority thread and replaces the linked one
/// (GraphicsPipeline::Handle). The PipelineCache owns it; null when libraries are off.
class GraphicsPipelineLibraries {
public:
    explicit GraphicsPipelineLibraries(const Instance& instance);
    ~GraphicsPipelineLibraries();

    /// The active instance, or null when pipeline libraries are off.
    static GraphicsPipelineLibraries* Get() noexcept;

    /// Drops queued optimizations and joins the optimizer threads (before pipelines go).
    void StopOptimizer();

    /// Prints the part, link and optimization counts and times since the last call.
    static void Report();

    struct Impl;
    std::unique_ptr<Impl> impl;
};

class GraphicsPipeline : public Pipeline {
public:
    struct SerializationSupport {
        VertexInputs<vk::VertexInputAttributeDescription> vertex_attributes{};
        VertexInputs<vk::VertexInputBindingDescription> vertex_bindings{};
        VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors{};
        vk::PipelineMultisampleStateCreateInfo multisampling{};
        std::vector<u32> tcs{};
        std::vector<u32> tes{};
        std::vector<u32> fragment{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    GraphicsPipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                     const Shader::Profile& profile, const GraphicsPipelineKey& key,
                     vk::PipelineCache pipeline_cache,
                     std::span<const Shader::Info*, MaxShaderStages> stages,
                     std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                     std::optional<const Shader::Gcn::FetchShaderData> fetch_shader,
                     std::span<const vk::ShaderModule> modules, SerializationSupport& sdata,
                     bool preloading, bool defer_handles = false);
    ~GraphicsPipeline();

    /// resogun: creates the Vulkan pipelines (the layout already exists). The constructor calls
    /// it unless `defer_handles`; then StartHandles() runs it on a compiler thread. Only reads
    /// whether each of `infos` is set.
    void CreateHandles(vk::PipelineCache pipeline_cache,
                       std::span<const Shader::Info* const, MaxShaderStages> infos,
                       std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                       std::span<const vk::ShaderModule> modules, SerializationSupport& sdata,
                       bool preloading);

    /// resogun: queues `create` (a CreateHandles call) for a deferred pipeline, or runs it here
    /// when the queue is full. A failure leaves the handles null.
    void StartHandles(AsyncCompiler& compiler, std::function<void()> create);

    /// resogun: whether the Vulkan pipelines exist (or failed); false while a deferred build runs.
    bool HandlesReady() const noexcept {
        return handles_ready.load(std::memory_order_acquire);
    }

    /// resogun: waits for a deferred build, or runs it here if no compiler thread has started it.
    /// Draws record this on the recording thread, so the GPU thread can go on queueing builds.
    void WaitHandles() const {
        if (!HandlesReady()) {
            ResogunStats::WaitTimer timer{ResogunStats::handle_wait_ns};
            ResogunStats::handle_waits.fetch_add(1, std::memory_order_relaxed);
            if (handle_job.Wait()) {
                ResogunStats::handle_runs.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // resogun: switch from compiler-owned snapshots to live stage data on publication.
    void PublishStages(std::span<const Shader::Info*, MaxShaderStages> infos) {
        std::ranges::copy(infos, stages.begin());
    }

    const std::optional<const Shader::Gcn::FetchShaderData>& GetFetchShader() const noexcept {
        return fetch_shader;
    }

    const GraphicsPipelineKey& GetGraphicsKey() const {
        return key;
    }

    /// resogun: second draw squaring a self-scaled MIN/MAX blend, or null (vk_blend_rewrite.h).
    vk::Pipeline SquarePassHandle() const noexcept {
        return *square_pipeline;
    }

    /// resogun: the optimized pipeline once it replaced a fast-linked one, else the pipeline.
    /// Hides Pipeline::Handle for graphics pipelines; the linked pipeline stays alive, since
    /// recorded command buffers may still use it.
    vk::Pipeline Handle() const noexcept;

    /// Gets the attributes and bindings for vertex inputs.
    template <typename Attribute, typename Binding>
    void GetVertexInputs(VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
                         VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
                         VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0,
                         u32 step_rate_1) const;

private:
    void BuildDescSetLayout(bool preloading);
    /// resogun: fills `state` with the monolithic create info CreateHandles used to build inline.
    void FillCreateState(PipelineCreateState& state,
                         std::span<const Shader::Info* const, MaxShaderStages> infos,
                         std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                         std::span<const vk::ShaderModule> modules, SerializationSupport& sdata,
                         bool preloading);
    /// resogun: the pipeline-library path of CreateHandles; false to build it the usual way.
    bool CreateFromLibraries(GraphicsPipelineLibraries& libraries, vk::PipelineCache pipeline_cache,
                             std::shared_ptr<PipelineCreateState> state);

private:
    GraphicsPipelineKey key;
    /// resogun: the descriptor set layout's bindings and flags (pipeline library layouts).
    std::vector<vk::DescriptorSetLayoutBinding> layout_bindings;
    vk::DescriptorSetLayoutCreateFlags layout_flags;
    std::shared_ptr<OptimizedPipeline> optimized;
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader{};
    vk::UniquePipeline square_pipeline;
    std::atomic<bool> handles_ready{true};
    AsyncJob<bool> handle_job;
};

struct ClipDistanceShaderKey {
    std::array<std::tuple<u8, u8>, 8> clip_locations;

    bool operator==(const ClipDistanceShaderKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::GraphicsPipelineKey> {
    std::size_t operator()(const Vulkan::GraphicsPipelineKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};

template <>
struct std::hash<Vulkan::ClipDistanceShaderKey> {
    std::size_t operator()(const Vulkan::ClipDistanceShaderKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};
