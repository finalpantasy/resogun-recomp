// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/host_passes/smaa_pass.h"

#include <cstring>

#include "common/assert.h"
#include "core/emulator_settings.h"
#include "video_core/renderer_vulkan/host_passes/smaa_spv.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#include <vk_mem_alloc.h>

namespace Vulkan::HostPasses {

namespace {

constexpr vk::ImageSubresourceRange kColor = {
    .aspectMask = vk::ImageAspectFlagBits::eColor,
    .levelCount = 1,
    .layerCount = 1,
};

// SMAA High, the reference preset: edge threshold 0.1, 16 search steps, 8 diagonal steps, corner
// rounding 25.
constexpr std::array<float, 4> kHigh = {0.1f, 16.0f, 8.0f, 25.0f};

constexpr vk::Extent2D kAreaSize{160, 560};
constexpr vk::Extent2D kSearchSize{64, 16};

void Barrier(vk::CommandBuffer cmdbuf, vk::Image image, vk::ImageLayout from, vk::ImageLayout to,
             vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access) {
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .image = image,
        .subresourceRange = kColor,
    };
    cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
}

vk::ShaderModule Module(vk::Device device, const uint32_t* code, size_t size) {
    return Check<"create smaa shader module">(
        device.createShaderModule({.codeSize = size, .pCode = code}));
}

} // namespace

SmaaPass::~SmaaPass() {
    for (Target* target : {&area, &search, &edges, &weights, &output}) {
        Destroy(*target);
    }
}

void SmaaPass::Destroy(Target& target) {
    if (target.view) {
        device.destroyImageView(target.view);
    }
    if (target.image) {
        vmaDestroyImage(allocator, target.image, target.allocation);
    }
    target = {};
}

SmaaPass::Target SmaaPass::MakeImage(vk::Extent2D size, vk::Format format,
                                     vk::ImageUsageFlags usage, const char* name) {
    const VkImageCreateInfo image_ci = static_cast<VkImageCreateInfo>(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {size.width, size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = usage,
    });
    const VmaAllocationCreateInfo alloc_ci{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage image{};
    Target target{};
    const VkResult result =
        vmaCreateImage(allocator, &image_ci, &alloc_ci, &image, &target.allocation, nullptr);
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating the SMAA {} image: {}", name,
               vk::to_string(vk::Result{result}));
    target.image = vk::Image{image};
    SetObjectName(device, target.image, "SMAA {}", name);
    target.view = Check<"create smaa image view">(device.createImageView({
        .image = target.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange = kColor,
    }));
    return target;
}

void SmaaPass::Create(vk::Device device_, VmaAllocator allocator_) {
    device = device_;
    allocator = allocator_;
    std::array<vk::DescriptorSetLayoutBinding, 6> bindings{};
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i] = {
            .binding = i,
            .descriptorType =
                i < 4 ? vk::DescriptorType::eSampledImage : vk::DescriptorType::eSampler,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eFragment,
        };
    }
    desc_set_layout = Check<"create smaa descriptor set layout">(device.createDescriptorSetLayoutUnique({
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    }));
    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = 8 * sizeof(float),
    };
    pipeline_layout = Check<"create smaa pipeline layout">(device.createPipelineLayoutUnique({
        .setLayoutCount = 1U,
        .pSetLayouts = &*desc_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    }));
    linear = Check<"create smaa sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    }));
    point = Check<"create smaa sampler">(device.createSamplerUnique({
        .magFilter = vk::Filter::eNearest,
        .minFilter = vk::Filter::eNearest,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
    }));
    area = MakeImage(kAreaSize, vk::Format::eR8G8Unorm,
                     vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst, "area");
    search = MakeImage(kSearchSize, vk::Format::eR8Unorm,
                       vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
                       "search");
}

// A full-screen triangle drawn by one of the SMAA passes into a `format` attachment.
static vk::UniquePipeline MakePipeline(vk::Device device, vk::PipelineLayout layout,
                                       const uint32_t* vs, size_t vs_size, const uint32_t* fs,
                                       size_t fs_size, vk::Format format) {
    const vk::ShaderModule vs_module = Module(device, vs, vs_size);
    const vk::ShaderModule fs_module = Module(device, fs, fs_size);
    const std::array stages{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex, .module = vs_module, .pName = "main"},
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment, .module = fs_module, .pName = "main"},
    };
    const vk::PipelineRenderingCreateInfo rendering_ci{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::PipelineViewportStateCreateInfo viewport_info{.viewportCount = 1, .scissorCount = 1};
    const vk::PipelineRasterizationStateCreateInfo raster{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineColorBlendAttachmentState attachment{
        .blendEnable = false,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo blending{
        .attachmentCount = 1,
        .pAttachments = &attachment,
    };
    const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = dynamic_states.size(),
        .pDynamicStates = dynamic_states.data(),
    };
    auto pipeline = Check<"create smaa pipeline">(device.createGraphicsPipelineUnique(
        {}, vk::GraphicsPipelineCreateInfo{
                .pNext = &rendering_ci,
                .stageCount = stages.size(),
                .pStages = stages.data(),
                .pVertexInputState = &vertex_input,
                .pInputAssemblyState = &input_assembly,
                .pViewportState = &viewport_info,
                .pRasterizationState = &raster,
                .pMultisampleState = &multisampling,
                .pColorBlendState = &blending,
                .pDynamicState = &dynamic_info,
                .layout = layout,
            }));
    device.destroyShaderModule(vs_module);
    device.destroyShaderModule(fs_module);
    return pipeline;
}

// The lookup textures go up once, through a staging buffer freed when the GPU is done with it.
void SmaaPass::Upload(vk::CommandBuffer cmdbuf, Scheduler& scheduler) {
    using namespace Smaa;
    const VkBufferCreateInfo buffer_ci = static_cast<VkBufferCreateInfo>(vk::BufferCreateInfo{
        .size = sizeof(k_smaa_area) + sizeof(k_smaa_search),
        .usage = vk::BufferUsageFlagBits::eTransferSrc,
    });
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO,
    };
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    const VkResult result =
        vmaCreateBuffer(allocator, &buffer_ci, &alloc_ci, &buffer, &allocation, &info);
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating the SMAA staging buffer: {}",
               vk::to_string(vk::Result{result}));
    std::memcpy(info.pMappedData, k_smaa_area, sizeof(k_smaa_area));
    std::memcpy(static_cast<u8*>(info.pMappedData) + sizeof(k_smaa_area), k_smaa_search,
                sizeof(k_smaa_search));
    vmaFlushAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
    for (const Target* target : {&area, &search}) {
        Barrier(cmdbuf, target->image, vk::ImageLayout::eUndefined,
                vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eNone,
                vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferWrite);
    }
    const vk::BufferImageCopy area_copy{
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {kAreaSize.width, kAreaSize.height, 1},
    };
    const vk::BufferImageCopy search_copy{
        .bufferOffset = sizeof(k_smaa_area),
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageExtent = {kSearchSize.width, kSearchSize.height, 1},
    };
    cmdbuf.copyBufferToImage(vk::Buffer{buffer}, area.image, vk::ImageLayout::eTransferDstOptimal,
                             area_copy);
    cmdbuf.copyBufferToImage(vk::Buffer{buffer}, search.image,
                             vk::ImageLayout::eTransferDstOptimal, search_copy);
    for (const Target* target : {&area, &search}) {
        Barrier(cmdbuf, target->image, vk::ImageLayout::eTransferDstOptimal,
                vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferWrite, vk::PipelineStageFlagBits2::eFragmentShader,
                vk::AccessFlagBits2::eShaderRead);
    }
    scheduler.DeferOperation([allocator = allocator, buffer, allocation] {
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
    uploaded = true;
}

void SmaaPass::Pass(vk::CommandBuffer cmdbuf, vk::Pipeline pipeline, const Target& target,
                    vk::ImageView first, vk::ImageView second,
                    const std::array<float, 8>& constants) {
    // The target is drawn over whole: what it held is not needed, but the last frame's reads of
    // it must be done.
    Barrier(cmdbuf, target.image, vk::ImageLayout::eUndefined,
            vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eFragmentShader,
            vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite);
    const std::array<vk::DescriptorImageInfo, 6> infos{
        vk::DescriptorImageInfo{.imageView = first,
                                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{.imageView = second,
                                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{.imageView = area.view,
                                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{.imageView = search.view,
                                .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal},
        vk::DescriptorImageInfo{.sampler = *linear},
        vk::DescriptorImageInfo{.sampler = *point},
    };
    std::array<vk::WriteDescriptorSet, 6> writes{};
    for (u32 i = 0; i < writes.size(); ++i) {
        writes[i] = {
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = i < 4 ? vk::DescriptorType::eSampledImage : vk::DescriptorType::eSampler,
            .pImageInfo = &infos[i],
        };
    }
    const vk::RenderingAttachmentInfo attachment{
        .imageView = target.view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{vk::ClearColorValue{std::array<float, 4>{0, 0, 0, 0}}},
    };
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    cmdbuf.setViewport(0, vk::Viewport{.width = float(extent.width),
                                       .height = float(extent.height),
                                       .minDepth = 0.0f,
                                       .maxDepth = 1.0f});
    cmdbuf.setScissor(0, vk::Rect2D{.extent = extent});
    cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eGraphics, *pipeline_layout, 0, writes);
    cmdbuf.pushConstants(*pipeline_layout,
                         vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment, 0,
                         sizeof(constants), constants.data());
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {.extent = extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    cmdbuf.draw(3, 1, 0, 0);
    cmdbuf.endRendering();
    Barrier(cmdbuf, target.image, vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eFragmentShader,
            vk::AccessFlagBits2::eShaderRead);
}

vk::ImageView SmaaPass::Render(vk::CommandBuffer cmdbuf, Scheduler& scheduler, vk::ImageView input,
                               vk::Extent2D size, vk::Format format) {
    using namespace Smaa;
    if (!uploaded) {
        Upload(cmdbuf, scheduler);
        edge_pipeline = MakePipeline(device, *pipeline_layout, k_smaa_vs_edge, sizeof(k_smaa_vs_edge),
                                     k_smaa_ps_edge, sizeof(k_smaa_ps_edge), vk::Format::eR8G8Unorm);
        weight_pipeline = MakePipeline(device, *pipeline_layout, k_smaa_vs_weights,
                                       sizeof(k_smaa_vs_weights), k_smaa_ps_weights,
                                       sizeof(k_smaa_ps_weights), vk::Format::eR8G8B8A8Unorm);
    }
    if (!blend_pipeline || blend_format != format) {
        if (blend_pipeline) { // the last frame may still use it
            scheduler.DeferOperation([pipeline = blend_pipeline.release(), device = device] {
                device.destroyPipeline(pipeline);
            });
        }
        blend_pipeline = MakePipeline(device, *pipeline_layout, k_smaa_vs_blend, sizeof(k_smaa_vs_blend),
                                      k_smaa_ps_blend, sizeof(k_smaa_ps_blend), format);
        blend_format = format;
    }
    if (extent != size || output_format != format || !output.image) {
        // The previous frame may still read the old images.
        scheduler.DeferOperation([device = device, allocator = allocator,
                                  old = std::array{edges, weights, output}] {
            for (const Target& target : old) {
                if (target.view) {
                    device.destroyImageView(target.view);
                }
                if (target.image) {
                    vmaDestroyImage(allocator, target.image, target.allocation);
                }
            }
        });
        const auto attachment = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
        edges = MakeImage(size, vk::Format::eR8G8Unorm, attachment, "edges");
        weights = MakeImage(size, vk::Format::eR8G8B8A8Unorm, attachment, "weights");
        output = MakeImage(size, format, attachment, "output");
        extent = size;
        output_format = format;
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{.pLabelName = "Host/SMAA"});
    }
    const float w = float(size.width), h = float(size.height);
    const std::array<float, 8> constants{1.0f / w, 1.0f / h, w, h, kHigh[0], kHigh[1], kHigh[2], kHigh[3]};
    Pass(cmdbuf, *edge_pipeline, edges, input, area.view, constants);
    Pass(cmdbuf, *weight_pipeline, weights, input, edges.view, constants);
    Pass(cmdbuf, *blend_pipeline, output, input, weights.view, constants);
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
    return output.view;
}

} // namespace Vulkan::HostPasses
