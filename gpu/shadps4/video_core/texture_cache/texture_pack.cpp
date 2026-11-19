// SPDX-License-Identifier: GPL-2.0-or-later
// resogun: texture dumps and replacement packs (see texture_pack.h).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <xxhash.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "resogun_png.h"
#include "resogun_toggles.h"
#include "resogun_settings.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_pack.h"
#include <vk_mem_alloc.h>

namespace VideoCore {

namespace {

struct FormatTraits {
    const char* name;
    u32 family;      ///< same for the UNORM and sRGB variants; 0: not eligible
    u32 dxgi;        ///< DDS DX10 format of the dump
    u32 block_bytes; ///< bytes per 4x4 block; 0: 4 bytes per pixel
    bool srgb;
};

FormatTraits Traits(vk::Format format) {
    switch (format) {
    case vk::Format::eBc1RgbaUnormBlock: return {"bc1", 1, 71, 8, false};
    case vk::Format::eBc1RgbaSrgbBlock: return {"bc1", 1, 72, 8, true};
    case vk::Format::eBc2UnormBlock: return {"bc2", 2, 74, 16, false};
    case vk::Format::eBc2SrgbBlock: return {"bc2", 2, 75, 16, true};
    case vk::Format::eBc3UnormBlock: return {"bc3", 3, 77, 16, false};
    case vk::Format::eBc3SrgbBlock: return {"bc3", 3, 78, 16, true};
    case vk::Format::eBc7UnormBlock: return {"bc7", 7, 98, 16, false};
    case vk::Format::eBc7SrgbBlock: return {"bc7", 7, 99, 16, true};
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eA8B8G8R8UnormPack32: return {"rgba8", 8, 28, 0, false};
    case vk::Format::eR8G8B8A8Srgb:
    case vk::Format::eA8B8G8R8SrgbPack32: return {"rgba8", 8, 29, 0, true};
    case vk::Format::eB8G8R8A8Unorm: return {"bgra8", 9, 87, 0, false};
    case vk::Format::eB8G8R8A8Srgb: return {"bgra8", 9, 91, 0, true};
    default: return {"", 0, 0, 0, false};
    }
}

/// The content key: the guest bytes, size and format family (sRGB and UNORM views share it).
/// 0 if the guest range cannot be read: an upload from GPU-side data can name memory the game
/// has already unmapped (seen in a course load), and the texture then keeps its own image.
u64 KeyOf(const Image& image) {
    const auto& info = image.info;
    const u64 seed = (u64(info.size.width) << 40) ^ (u64(info.size.height) << 16) ^
                     Traits(info.pixel_format).family;
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        static std::atomic<u32> unreadable{};
        if (unreadable++ < 4) {
            std::printf("Texture pack: %ux%u texture at %#llx not readable; it keeps its own image\n",
                        info.size.width, info.size.height,
                        static_cast<unsigned long long>(info.guest_address));
        }
        return 0;
    }
    runtime_fault_recover = &recover;
    const u64 key = XXH3_64bits_withSeed(reinterpret_cast<const void*>(info.guest_address),
                                         info.guest_size, seed);
    runtime_fault_recover = nullptr;
    return key;
}

/// A DDS file with a DX10 header holding one 2D mip level.
void WriteDds(const std::string& path, const FormatTraits& traits, u32 width, u32 height,
              const u8* data, u32 size) {
    u32 header[32 + 5] = {};
    header[0] = 0x20534444; // "DDS "
    header[1] = 124;
    header[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000; // caps, height, width, pixelformat, linearsize
    header[3] = height;
    header[4] = width;
    header[5] = size;
    header[7] = 1; // mip count
    header[19] = 32;
    header[20] = 0x4;        // DDPF_FOURCC
    header[21] = 0x30315844; // "DX10"
    header[27] = 0x1000;     // DDSCAPS_TEXTURE
    header[32] = traits.dxgi;
    header[33] = 3; // DDS_DIMENSION_TEXTURE2D
    header[35] = 1; // array size
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(header, 1, sizeof(header), f);
        std::fwrite(data, 1, size, f);
        std::fclose(f);
    }
}

vk::ImageViewType ViewType(AmdGpu::ImageType type) {
    switch (type) {
    case AmdGpu::ImageType::Color2D: return vk::ImageViewType::e2D;
    case AmdGpu::ImageType::Color2DArray: return vk::ImageViewType::e2DArray;
    default: return vk::ImageViewType(-1);
    }
}

} // namespace

TexturePack::TexturePack(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_} {
    const auto& settings = ResogunSettings::Get();
    if (const char* dir = settings.texture_dump.c_str(); *dir) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        dump_dir = dir;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            const auto stem = entry.path().stem().string();
            if (stem.size() >= 16) {
                dumped.insert(std::strtoull(stem.substr(0, 16).c_str(), nullptr, 16));
            }
        }
        std::printf("Texture dump: new textures go to %s (%zu already there)\n", dir,
                    dumped.size());
    }
    if (const char* dir = settings.texture_pack.c_str(); *dir) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".png") {
                continue;
            }
            const auto stem = entry.path().stem().string();
            if (stem.size() < 16 || stem.find_first_not_of("0123456789abcdef") < 16) {
                continue;
            }
            index[std::strtoull(stem.substr(0, 16).c_str(), nullptr, 16)] =
                entry.path().string();
        }
        std::printf("Texture pack: %zu replacement textures in %s\n", index.size(), dir);
        if (!index.empty()) {
            worker = std::jthread([this](std::stop_token stop) { Work(stop); });
        }
    }
}

TexturePack::~TexturePack() = default;

bool TexturePack::Eligible(const Image& image) {
    const auto& info = image.info;
    return Traits(info.pixel_format).family != 0 && info.type == AmdGpu::ImageType::Color2D &&
           info.resources.layers == 1 && !info.props.is_volume && !info.props.is_depth &&
           info.num_samples <= 1 && info.size.width >= 16 && info.size.height >= 16 &&
           info.guest_address != 0 && False(image.flags & ImageFlagBits::GpuModified);
}

void TexturePack::OnUpload(Image& image) {
    image.replacement_key = 0;
    if (!Eligible(image)) {
        return;
    }
    const u64 key = KeyOf(image);
    if (!key) {
        return;
    }
    if (!dump_dir.empty() && dumped.insert(key).second) {
        Dump(image, key);
    }
    if (!index.contains(key)) {
        return;
    }
    image.replacement_key = key;
    if (requested.insert(key).second) {
        {
            std::scoped_lock lock{queue_mutex};
            queue.push_back(key);
        }
        queue_cv.notify_one();
    }
}

void TexturePack::Work(std::stop_token stop) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
    while (true) {
        u64 key;
        {
            std::unique_lock lock{queue_mutex};
            // The decoded backlog stays under MaxDecoded: uploads go at the render thread's pace.
            if (!queue_cv.wait(lock, stop, [this] {
                    return !queue.empty() && decoded_bytes < MaxDecoded;
                })) {
                return;
            }
            key = queue.front();
            queue.pop_front();
        }
        const std::string& path = index.at(key);
        Decoded out{.key = key};
        try {
            Decode(path, out);
        } catch (const std::bad_alloc&) {
            out.mips.clear();
        }
        if (out.mips.empty()) {
            std::printf("Texture pack: cannot decode %s\n", path.c_str());
            continue;
        }
        for (const auto& mip : out.mips) {
            out.bytes += mip.size();
        }
        std::scoped_lock lock{queue_mutex};
        decoded_bytes += out.bytes;
        decoded.push_back(std::move(out));
    }
}

void TexturePack::Decode(const std::string& path, Decoded& out) {
    ResogunPng::Image png;
    if (!ResogunPng::LoadRgba(path.c_str(), png) || png.width <= 0 || png.height <= 0) {
        return;
    }
    {
        auto& mips = out.mips;
        auto& extents = out.extents;
        mips.push_back(std::move(png.rgba));
        extents.push_back({u32(png.width), u32(png.height)});
        // Mip chain: 2x2 box filter down to 1x1.
        while (extents.back().width > 1 || extents.back().height > 1) {
            const auto [pw, ph] = extents.back();
            const u32 w = std::max(pw / 2, 1u), h = std::max(ph / 2, 1u);
            const auto& src = mips.back();
            std::vector<u8> dst(size_t(w) * h * 4);
            for (u32 y = 0; y < h; ++y) {
                const u32 y0 = std::min(y * 2, ph - 1), y1 = std::min(y * 2 + 1, ph - 1);
                for (u32 x = 0; x < w; ++x) {
                    const u32 x0 = std::min(x * 2, pw - 1), x1 = std::min(x * 2 + 1, pw - 1);
                    for (u32 ch = 0; ch < 4; ++ch) {
                        const u32 sum = src[(size_t(y0) * pw + x0) * 4 + ch] +
                                        src[(size_t(y0) * pw + x1) * 4 + ch] +
                                        src[(size_t(y1) * pw + x0) * 4 + ch] +
                                        src[(size_t(y1) * pw + x1) * 4 + ch];
                        dst[(size_t(y) * w + x) * 4 + ch] = u8((sum + 2) / 4);
                    }
                }
            }
            mips.push_back(std::move(dst));
            extents.push_back({w, h});
        }
    }
}

void TexturePack::Tick() {
    // At most 16 MiB of uploads a submit, so a burst of new textures spreads over frames.
    u64 budget = 16ull << 20;
    while (budget) {
        Decoded next;
        {
            std::scoped_lock lock{queue_mutex};
            if (decoded.empty()) {
                return;
            }
            next = std::move(decoded.front());
            decoded.pop_front();
            decoded_bytes -= next.bytes;
        }
        queue_cv.notify_one();
        const u64 bytes = u64(next.extents[0].width) * next.extents[0].height * 4;
        Upload(next);
        budget = bytes >= budget ? 0 : budget - bytes;
    }
}

void TexturePack::Dump(Image& image, u64 key) {
    const auto traits = Traits(image.info.pixel_format);
    const u32 width = image.info.size.width, height = image.info.size.height;
    const u32 size = traits.block_bytes
                         ? ((width + 3) / 4) * ((height + 3) / 4) * traits.block_bytes
                         : width * height * 4;
    const auto download =
        runtime.GetStagingPool().Request(size, MemoryType::HostCached, 16, true);
    const vk::BufferImageCopy copy = {
        .bufferOffset = download.offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };
    runtime.DownloadImage(&image, download.buffer, std::span{&copy, 1});
    char name[96];
    std::snprintf(name, sizeof(name), "%016llx_%ux%u_%s%s.dds",
                  static_cast<unsigned long long>(key), width, height, traits.name,
                  traits.srgb ? "_srgb" : "");
    std::string path = dump_dir + "/" + name;
    ++dumps;
    scheduler.DeferPriorityOperation([this, download, size, traits, width, height,
                                      path = std::move(path)] {
        download.Invalidate();
        WriteDds(path, traits, width, height, download.mapped, size);
        runtime.GetStagingPool().FreeDeferred(download);
    });
}

void TexturePack::Upload(Decoded& decoded) {
    const u64 key = decoded.key;
    const auto& mips = decoded.mips;
    const auto& extents = decoded.extents;
    const auto& path = index.at(key);
    auto replacement =
        std::make_unique<TextureReplacement>(instance.GetDevice(), instance.GetAllocator());
    replacement->width = extents[0].width;
    replacement->height = extents[0].height;
    replacement->levels = u32(mips.size());
    const vk::ImageCreateInfo image_ci = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8G8B8A8Unorm,
        .extent = {replacement->width, replacement->height, 1},
        .mipLevels = replacement->levels,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    const VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    const VkImageCreateInfo raw_ci = static_cast<VkImageCreateInfo>(image_ci);
    VkImage raw_image{};
    if (vmaCreateImage(instance.GetAllocator(), &raw_ci, &alloc_ci, &raw_image,
                       &replacement->image.allocation, nullptr) != VK_SUCCESS) {
        std::printf("Texture pack: no GPU memory for %s\n", path.c_str());
        return;
    }
    replacement->image.image = vk::Image{raw_image};
    replacement->image.image_ci = image_ci;

    u64 total = 0;
    for (const auto& mip : mips) {
        total += mip.size();
    }
    const vk::Image image = replacement->image.image;
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, replacement->levels,
                                          0, 1};
    const auto barrier = [this, image, range](vk::ImageLayout from, vk::ImageLayout to,
                                              vk::PipelineStageFlags2 src_stage,
                                              vk::AccessFlags2 src_access,
                                              vk::PipelineStageFlags2 dst_stage,
                                              vk::AccessFlags2 dst_access) {
        scheduler.Record([=](vk::CommandBuffer cmd) {
            const vk::ImageMemoryBarrier2 info = {
                .srcStageMask = src_stage,
                .srcAccessMask = src_access,
                .dstStageMask = dst_stage,
                .dstAccessMask = dst_access,
                .oldLayout = from,
                .newLayout = to,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = range,
            };
            cmd.pipelineBarrier2(
                vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &info});
        });
    };
    scheduler.EndRendering();
    barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal,
            vk::PipelineStageFlagBits2::eNone, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite);
    // Bands of rows through the staging ring (reused once the GPU is done with them): a
    // dedicated buffer per texture would hold pinned memory for each until it is trimmed.
    constexpr u64 Band = 8ull << 20;
    for (u32 m = 0; m < mips.size(); ++m) {
        const u32 width = extents[m].width, height = extents[m].height;
        const u64 row = u64(width) * 4;
        const u32 rows = u32(std::max<u64>(1, Band / row));
        for (u32 y = 0; y < height; y += rows) {
            const u32 count = std::min(rows, height - y);
            const auto staging =
                runtime.GetStagingPool().Request(row * count, MemoryType::HostUncached, 16);
            std::memcpy(staging.mapped, mips[m].data() + row * y, row * count);
            staging.Flush();
            const vk::BufferImageCopy copy = {
                .bufferOffset = staging.offset,
                .imageSubresource = {vk::ImageAspectFlagBits::eColor, m, 0, 1},
                .imageOffset = {0, s32(y), 0},
                .imageExtent = {width, count, 1},
            };
            scheduler.Record([buffer = staging.buffer->Handle(), image, copy](vk::CommandBuffer cmd) {
                cmd.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, copy);
            });
        }
    }
    barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::PipelineStageFlagBits2::eCopy, vk::AccessFlagBits2::eTransferWrite,
            vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eShaderRead);

    replaced_bytes += total;
    if ((++replaced & (replaced - 1)) == 0) {
        std::printf("Texture pack: %llu textures replaced, %llu MiB (latest %s, %ux%u)\n",
                    static_cast<unsigned long long>(replaced),
                    static_cast<unsigned long long>(replaced_bytes >> 20), path.c_str(),
                    replacement->width, replacement->height);
    }
    std::scoped_lock lock{views_mutex};
    loaded.emplace(key, std::move(replacement));
}

vk::ImageView TexturePack::ViewFor(const Image& image, const ImageView& view) {
    if (!image.replacement_key || True(image.flags & ImageFlagBits::GpuModified)) {
        return {};
    }
    const auto& info = view.info;
    const auto type = ViewType(info.type);
    const auto traits = Traits(info.format);
    if (info.is_storage || info.range.base.level != 0 || info.range.base.layer != 0 ||
        type == vk::ImageViewType(-1) ||
        traits.family != Traits(image.info.pixel_format).family) {
        return {};
    }
    std::scoped_lock lock{views_mutex};
    const auto found = loaded.find(image.replacement_key);
    if (found == loaded.end()) {
        return {};
    }
    const TextureReplacement* owner = found->second.get();
    const ViewKey view_key{owner,
                           traits.srgb ? vk::Format::eR8G8B8A8Srgb : vk::Format::eR8G8B8A8Unorm,
                           type, info.mapping};
    const u64 hash = XXH3_64bits(&view_key, sizeof(view_key));
    for (auto [it, end] = views.equal_range(hash); it != end; ++it) {
        if (it->second.first == view_key) {
            return *it->second.second;
        }
    }
    const vk::ImageViewCreateInfo view_ci = {
        .image = owner->image.image,
        .viewType = type,
        .format = view_key.format,
        .components = info.mapping,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, owner->levels, 0, 1},
    };
    auto [result, created] = instance.GetDevice().createImageViewUnique(view_ci);
    if (result != vk::Result::eSuccess) {
        return {};
    }
    const vk::ImageView handle = *created;
    views.emplace(hash, std::make_pair(view_key, std::move(created)));
    return handle;
}

} // namespace VideoCore
