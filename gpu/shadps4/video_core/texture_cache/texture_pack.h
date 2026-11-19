// SPDX-License-Identifier: GPL-2.0-or-later
// resogun: texture dumps and replacement packs, keyed by the guest texture's contents.
//
// texture_dump=<dir> (settings.ini; RESOGUN_TEXTURE_DUMP) writes every eligible texture the game uploads (2D, one layer,
// never written by the GPU, BC1/BC2/BC3/BC7 or 8-bit RGBA) once, as <key>_<w>x<h>_<format>.dds.
// texture_pack=<dir> (RESOGUN_TEXTURE_PACK) samples <key>*.png from that folder (searched recursively) instead
// of the game's texture, at whatever size the PNG has, with a full mip chain. A worker thread
// decodes the PNGs; until one is uploaded, the game's own texture shows. tools/texture_pack.py
// turns dumps into PNGs and upscales them. Packs are made from the player's own game: none ship.
#pragma once

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/types.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"

namespace Vulkan {
class Instance;
class Scheduler;
class Runtime;
} // namespace Vulkan

namespace VideoCore {

struct TextureReplacement {
    TextureReplacement(vk::Device device, VmaAllocator allocator) : image{device, allocator} {}
    UniqueImage image;
    u32 width = 0, height = 0, levels = 0;
    /// Its formats for UNORM and sRGB views (RGBA8 for a PNG, the BCn pair for a DDS).
    vk::Format unorm = vk::Format::eR8G8B8A8Unorm, srgb = vk::Format::eR8G8B8A8Srgb;
};

class TexturePack {
public:
    TexturePack(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                Vulkan::Runtime& runtime);
    ~TexturePack();

    bool Active() const {
        return !dump_dir.empty() || !index.empty();
    }
    /// Called after `image` was uploaded from guest memory: dumps it and keys it to its
    /// replacement (or to none). Main thread.
    void OnUpload(Image& image);
    /// Uploads the replacements the worker decoded since the last call. Main thread.
    void Tick();
    /// The replacement view to sample in place of `view`; null when there is none or it is
    /// still loading. Any thread.
    vk::ImageView ViewFor(const Image& image, const ImageView& view);

private:
    struct Decoded {
        u64 key = 0;
        std::vector<std::vector<u8>> mips;
        std::vector<vk::Extent2D> extents;
        u64 bytes = 0;
        vk::Format unorm = vk::Format::eR8G8B8A8Unorm, srgb = vk::Format::eR8G8B8A8Srgb;
        u32 block_bytes = 0; ///< 0 for RGBA8, else the bytes of one 4x4 block
    };
    static constexpr u64 MaxDecoded = 256ull << 20; ///< decoded and not yet uploaded
    static bool Eligible(const Image& image);
    void Dump(Image& image, u64 key);
    void Work(std::stop_token stop);
    /// PNG to RGBA8 with a mip chain, or a DDS's BC1/BC3/BC7 mips as they are; `out.mips` stays
    /// empty when the file cannot be decoded.
    static void Decode(const std::string& path, Decoded& out);
    static void DecodeDds(const std::string& path, Decoded& out);
    void Upload(Decoded& decoded);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    std::string dump_dir;
    std::unordered_set<u64> dumped;
    std::unordered_map<u64, std::string> index; ///< pack PNGs by key
    std::unordered_set<u64> requested; ///< main thread
    std::mutex queue_mutex;
    std::condition_variable_any queue_cv;
    std::deque<u64> queue;       ///< keys to decode
    std::deque<Decoded> decoded; ///< decoded, to upload
    u64 decoded_bytes = 0;
    std::mutex views_mutex;      ///< guards loaded and views
    std::unordered_map<u64, std::unique_ptr<TextureReplacement>> loaded;
    struct ViewKey {
        const TextureReplacement* owner;
        vk::Format format;
        vk::ImageViewType type;
        vk::ComponentMapping mapping;
        bool operator==(const ViewKey&) const = default;
    };
    std::unordered_multimap<u64, std::pair<ViewKey, vk::UniqueImageView>> views;
    u64 replaced = 0, replaced_bytes = 0, dumps = 0;
    std::jthread worker; ///< last: stops before the members it uses go
};

} // namespace VideoCore
