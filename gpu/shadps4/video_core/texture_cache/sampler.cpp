// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdio>
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/sampler.h"

namespace VideoCore {

Sampler::Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                 const AmdGpu::BorderColorBuffer border_color_base, const bool is_depth, float extra_lod_bias, u32 aniso_override) {
    using namespace Vulkan;
    const bool game_aniso = instance.IsAnisotropicFilteringSupported() &&
                            (AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) ||
                             AmdGpu::IsAnisoFilter(sampler.xy_min_filter));
    // resogun: the anisotropy override (key anisotropy) raises samplers that filter linearly in
    // both directions to at least its level; point samplers, depth compares and samplers that ask
    // for the base level only (no mip filter) are left alone.
    const bool linear = LiverpoolToVK::Filter(sampler.xy_mag_filter) == vk::Filter::eLinear &&
                        LiverpoolToVK::Filter(sampler.xy_min_filter) == vk::Filter::eLinear;
    const bool override_aniso = aniso_override > 1 && instance.IsAnisotropicFilteringSupported() &&
                                linear && !is_depth && sampler.mip_filter != AmdGpu::MipFilter::None;
    const bool anisotropy_enable = game_aniso || override_aniso;
    float max_anisotropy = 1.0f;
    if (anisotropy_enable) {
        max_anisotropy = game_aniso ? sampler.MaxAniso() : 1.0f;
        if (override_aniso) max_anisotropy = std::max(max_anisotropy, float(aniso_override));
        max_anisotropy = std::clamp(max_anisotropy, 1.0f, instance.MaxSamplerAnisotropy());
    }
    auto border_color = LiverpoolToVK::BorderColor(sampler.border_color_type);
    if (border_color == vk::BorderColor::eFloatCustomEXT &&
        !instance.IsCustomBorderColorSupported()) {
        LOG_WARNING(Render_Vulkan, "Custom border color is not supported, falling back to black");
        border_color = vk::BorderColor::eFloatOpaqueBlack;
    }

    const auto custom_color = [&]() -> std::optional<vk::SamplerCustomBorderColorCreateInfoEXT> {
        if (border_color == vk::BorderColor::eFloatCustomEXT) {
            const auto border_color_index = sampler.border_color_ptr.Value();
            const auto border_color_buffer = border_color_base.Address<std::array<float, 4>*>();
            const auto custom_border_color_array = border_color_buffer[border_color_index];

            const vk::SamplerCustomBorderColorCreateInfoEXT ret{
                .customBorderColor =
                    vk::ClearColorValue{
                        .float32 = custom_border_color_array,
                    },
                .format = vk::Format::eR32G32B32A32Sfloat,
            };
            return ret;
        } else {
            return std::nullopt;
        }
    }();

    // resogun: MIP_FILTER None samples the base level only. Vulkan has no such mode; the spec's
    // equivalent is NEAREST mipmapping with the LOD clamped to [0, 0.25], which keeps the
    // min/mag filter choice. Mapping it to NEAREST over the whole range let Everybody's Golf's
    // face textures pick lower mips per UV island, drawing seams.
    const bool no_mips = sampler.mip_filter == AmdGpu::MipFilter::None;
    const float min_lod = no_mips ? 0.0f : sampler.MinLod();
    const float max_lod = no_mips ? 0.25f : sampler.MaxLod();
    // resogun: FILTER_MODE Min/Max takes the smallest or largest texel in the footprint instead
    // of the weighted average. Vulkan requires the average for compare samplers.
    const bool reduce = sampler.filter_mode != AmdGpu::FilterMode::Blend && !is_depth &&
                        instance.IsSamplerFilterMinmaxSupported();
    const vk::SamplerReductionModeCreateInfo reduction{
        .pNext = custom_color ? &*custom_color : nullptr,
        .reductionMode = sampler.filter_mode == AmdGpu::FilterMode::Min
                             ? vk::SamplerReductionMode::eMin
                             : vk::SamplerReductionMode::eMax,
    };
    const void* next = reduce ? static_cast<const void*>(&reduction)
                              : custom_color ? static_cast<const void*>(&*custom_color) : nullptr;
    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = next,
        .magFilter = LiverpoolToVK::Filter(sampler.xy_mag_filter),
        .minFilter = LiverpoolToVK::Filter(sampler.xy_min_filter),
        .mipmapMode = LiverpoolToVK::MipFilter(sampler.mip_filter),
        .addressModeU = LiverpoolToVK::ClampMode(sampler.clamp_x),
        .addressModeV = LiverpoolToVK::ClampMode(sampler.clamp_y),
        .addressModeW = LiverpoolToVK::ClampMode(sampler.clamp_z),
        .mipLodBias = std::clamp(sampler.LodBias() + extra_lod_bias, -instance.MaxSamplerLodBias(),
                                 instance.MaxSamplerLodBias()),
        .anisotropyEnable = anisotropy_enable,
        .maxAnisotropy = max_anisotropy,
        // GCN compares per instruction; a plain read of a compare sampler is undefined in Vulkan.
        .compareEnable = is_depth,
        .compareOp = LiverpoolToVK::DepthCompare(sampler.depth_compare_func),
        .minLod = min_lod,
        .maxLod = max_lod,
        .borderColor = border_color,
        .unnormalizedCoordinates = false, // Handled in shader due to Vulkan limitations.
    };
    auto [sampler_result, smplr] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(sampler_result));
    handle = std::move(smplr);
    if (sampler.filter_mode != AmdGpu::FilterMode::Blend) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::printf("Sampler: filter reduction %s requested (%s)\n",
                        sampler.filter_mode == AmdGpu::FilterMode::Min ? "min" : "max",
                        reduce ? "applied" : is_depth ? "compare sampler, averaged" : "unsupported");
        }
    }
}

Sampler::~Sampler() = default;

} // namespace VideoCore
