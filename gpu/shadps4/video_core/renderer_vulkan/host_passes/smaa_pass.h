// SPDX-License-Identifier: GPL-2.0-or-later
// resogun: SMAA 1x anti-aliasing of the game's picture (key smaa). Three full-screen passes at the
// game's render size (edge detection, blending weights, neighborhood blending) before the post
// process pass scales it to the window.
#pragma once

#include <array>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {
class Scheduler;
} // namespace Vulkan

namespace Vulkan::HostPasses {

class SmaaPass {
public:
    SmaaPass() = default;
    ~SmaaPass();

    void Create(vk::Device device, VmaAllocator allocator);

    /// Anti-aliases `input` (a `format` view of a `size` image in ShaderReadOnlyOptimal) and
    /// returns a view of the result, in ShaderReadOnlyOptimal with its writes visible to fragment
    /// reads. Images of a previous size are freed once the GPU is past `scheduler`'s tick.
    vk::ImageView Render(vk::CommandBuffer cmdbuf, Scheduler& scheduler, vk::ImageView input,
                         vk::Extent2D size, vk::Format format);

private:
    struct Target {
        vk::Image image{};
        vk::ImageView view{};
        VmaAllocation allocation{};
    };

    Target MakeImage(vk::Extent2D size, vk::Format format, vk::ImageUsageFlags usage,
                     const char* name);
    void Destroy(Target& target);
    void Upload(vk::CommandBuffer cmdbuf, Scheduler& scheduler);
    void Pass(vk::CommandBuffer cmdbuf, vk::Pipeline pipeline, const Target& target,
              vk::ImageView first, vk::ImageView second, const std::array<float, 8>& constants);

    vk::Device device{};
    VmaAllocator allocator{};
    vk::UniquePipelineLayout pipeline_layout{};
    vk::UniqueDescriptorSetLayout desc_set_layout{};
    vk::UniquePipeline edge_pipeline{}, weight_pipeline{}, blend_pipeline{};
    vk::Format blend_format{};
    vk::UniqueSampler linear{}, point{};
    Target area{}, search{}, edges{}, weights{}, output{};
    vk::Extent2D extent{};
    vk::Format output_format{};
    bool uploaded = false;
};

} // namespace Vulkan::HostPasses
