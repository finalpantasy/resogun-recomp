#include <cstdint>
#include <cassert>
#include <vector>
#include "video_core/texture_cache/readback_queue.h"

int main() {
    VideoCore::ReadbackQueue<int> queue;
    std::vector<int> published;
    auto complete = [&](auto& entry) {
        if (entry.valid) published.push_back(entry.payload);
    };
    queue.Push(0x1000, 4096, 3, 1);
    queue.Push(0x1000, 4096, 4, 2);
    assert(queue.RequiredTick(0x1fff, 8) == 4);
    assert(queue.RequiredTick(0x2000, 8) == 0);
    queue.Retire([](auto tick) { return tick <= 2; }, complete);
    assert(published.empty());
    queue.Retire([](auto tick) { return tick <= 3; }, complete);
    assert((published == std::vector<int>{1}));
    unsigned canceled = 0;
    queue.Cancel(0x1001, 1, [&](auto&) { ++canceled; });
    queue.Cancel(0x1001, 1, [&](auto&) { ++canceled; });
    assert(canceled == 1);
    queue.Push(0x1000, 4096, 5, 3);
    queue.Retire([](auto) { return true; }, complete);
    assert((published == std::vector<int>{1, 3}));
    assert(queue.Bytes() == 0);
    assert(!queue.CanFit(65ull << 20));
    for (int i = 0; i < 64; ++i) queue.Push(0x1000, 1, 6, i);
    assert(!queue.CanFit(1));
    assert(queue.RequiredTick(0x1ff0, 1) == 6);
    assert(queue.RequiredTick(0x3000, 1) == 0);
    queue.Retire([](auto) { return true; }, [](auto&) {});
    queue.Push(0x1000, 64ull << 20, 7, 4);
    assert(!queue.CanFit(1));
    assert(queue.RequiredTick(0x1000, 0) == 0);
    queue.Retire([](auto) { return true; }, [](auto&) {});
    assert(queue.CanFit(64ull << 20));

    // Byte rule: a CPU write next to a small pending copy, on the same page but outside its
    // bytes, releases that copy's page protection instead of waiting for the GPU.
    std::vector<std::uint64_t> released;
    auto release = [&](auto& entry) { released.push_back(entry.address); };
    queue.Push(0x10000c00, 4, 8, 5);            // 1x1 image
    queue.Push(0x10001000, 4096, 9, 6);         // a page fully covered by a copy
    queue.Push(0x20000000, 1u << 20, 10, 7);    // large copy
    assert(queue.ReleaseOutside(0x10000100, 8, 64 << 10, release) == 1);
    assert((released == std::vector<std::uint64_t>{0x10000c00}));
    assert(queue.RequiredTick(0x10000100, 8) == 0);   // nothing left to wait for there
    assert(queue.RequiredTick(0x10000c00, 4) == 0);   // released copies never block again
    assert(queue.ReleaseOutside(0x10001010, 8, 64 << 10, release) == 0);  // inside the bytes
    assert(queue.RequiredTick(0x10001010, 8) == 9);
    assert(queue.ReleaseOutside(0x20000008, 8, 64 << 10, release) == 0);  // too large
    assert(queue.RequiredTick(0x20000008, 8) == 10);
    unsigned watched = 0, published_released = 0;
    queue.Retire([](auto) { return true; }, [&](auto& entry) {
        watched += entry.watched;
        published_released += entry.valid && !entry.watched;  // still published, no watcher left
    });
    assert(watched == 2 && published_released == 1);

    // Exact overlap: an image upload or free next to a pending copy, on a shared page but not
    // its bytes, neither waits for nor cancels it.
    queue.Push(0x30000c00, 256, 11, 8);         // 8x8 image at the end of a page
    assert(queue.RequiredTick(0x30000000, 0xc00) == 11);
    assert(queue.RequiredTick(0x30000000, 0xc00, true) == 0);
    assert(queue.RequiredTick(0x30000000, 0xc01, true) == 11);
    canceled = 0;
    queue.Cancel(0x30000000, 0xc00, [&](auto&) { ++canceled; }, true);
    assert(canceled == 0);
    published.clear();
    queue.Retire([](auto) { return true; }, complete);
    assert((published == std::vector<int>{8}));

    // Edge rule: a game flag stored 0x20 past a 128 KiB render target that is read back. The
    // write releases only the last page; the copy keeps its other pages and its bytes.
    using Queue = VideoCore::ReadbackQueue<int>;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pages;
    auto release_page = [&](std::uint64_t page, std::uint64_t size) { pages.emplace_back(page, size); };
    queue.Push(0x10650ec300, 0x20000, 12, 10);
    assert(queue.RequiredTick(0x106510c320, 8) == 12);                     // waited before
    assert(queue.ReleaseEdge(0x106510c320, 8, release_page) == 1);
    assert((pages == std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0x106510c000, 4096}}));
    assert(queue.RequiredTick(0x106510c320, 8) == 0);                      // flag write: no wait
    assert(queue.RequiredTick(0x106510c2f8, 8) == 12);                     // copy bytes still wait
    assert(queue.RequiredTick(0x10650f0000, 8) == 12);                     // inner pages still wait
    assert(queue.ReleaseEdge(0x106510c320, 8, release_page) == 0);         // already released
    assert(queue.ReleaseEdge(0x10650f0010, 8, release_page) == 0);         // inside the bytes
    assert(queue.ReleaseEdge(0x10650ec100, 8, release_page) == 1);         // the first page too
    {
        const auto [begin, end] = Queue::WatchedPages(queue.Entries().front());
        assert(begin == 0x10650ed000 && end == 0x106510c000);
    }
    assert(queue.ReleaseEdge(0x10650ecffc, 8, release_page) == 0);         // spans two pages
    // A one-page copy released from its only page is no longer watched at all.
    queue.Push(0x30000100, 0x100, 13, 11);
    assert(queue.ReleaseEdge(0x30000000, 8, release_page) == 1);
    assert(!queue.Entries().back().watched);
    assert(queue.RequiredTick(0x30000000, 8) == 0);
    unsigned done = 0;
    queue.Retire([](auto) { return true; }, [&](auto& entry) { done += entry.valid; });
    assert(done == 2);

    // Issue 3: a heap's free writes a chunk header just before a freed render target (releasing
    // its first page), then its free-list links into the target's first bytes, which no longer
    // fault. The copy retiring later must not put the GPU's older bytes over those links.
    {
        Queue heap;
        std::vector<std::uint8_t> ram(0x3000, 0xaa); // guest bytes at 0x1065bfd000
        const std::uint64_t base = 0x1065bfd000;
        auto read = [&](std::uint64_t address, std::uint8_t* dest, std::uint64_t size) {
            for (std::uint64_t i = 0; i < size; ++i) dest[i] = ram[address - base + i];
        };
        heap.Push(base + 0x100, 0x2000, 20, 1);                          // the render target
        assert(heap.ReleaseEdge(base + 0xf0, 16, release_page) == 1);    // chunk header write
        heap.FillSnapshots(read);
        assert(heap.Entries().front().snapshots.size() == 1);
        assert(heap.Entries().front().snapshots[0].address == base + 0x100);
        assert(heap.Entries().front().snapshots[0].size == 0xf00);
        for (int i = 0; i < 16; ++i) ram[0x100 + i] = std::uint8_t(0x10 + i); // free-list links
        std::vector<std::uint8_t> gpu(0x2000, 0x55);                     // the copy's pixels
        assert(Queue::KeepCpuWrites(heap.Entries().front(), gpu.data(), read) == 16);
        for (int i = 0; i < 16; ++i) assert(gpu[i] == std::uint8_t(0x10 + i));  // links kept
        assert(gpu[16] == 0x55 && gpu[0xeff] == 0x55 && gpu[0xf00] == 0x55);    // pixels land
        // A small copy whose page another write released whole (stale mode) keeps CPU bytes too.
        heap.Push(base + 0x2100, 0x40, 21, 2);
        assert(heap.ReleaseOutside(base + 0x2000, 8, 64 << 10, [](auto&) {}) == 1);
        heap.FillSnapshots(read);
        ram[0x2120] = 0x01;
        std::vector<std::uint8_t> small(0x40, 0x77);
        assert(Queue::KeepCpuWrites(heap.Entries().back(), small.data(), read) == 1);
        assert(small[0x20] == 0x01 && small[0x21] == 0x77);
    }
}
