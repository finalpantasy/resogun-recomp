#pragma once

#include <cstdint>
#include <cassert>
#include <deque>
#include <utility>
#include <vector>

namespace VideoCore {

// Copies retire in submission order, including overlapping guest ranges.
template <typename Payload>
class ReadbackQueue {
public:
    struct Entry {
        std::uint64_t address, size, tick;
        Payload payload;
        bool valid = true;
        bool watched = true; ///< still protects its pages (see ReleaseOutside)
        /// resogun: bit 0 the first page, bit 1 the last: a page shared with other bytes that a
        /// CPU write released (ReleaseEdge); the copy no longer protects or waits on it.
        std::uint8_t released_edges = 0;
        /// resogun (issue 3): the copy's bytes on pages it stopped protecting, as they were when
        /// it let go of them. CPU writes there no longer wait, and the GPU's older data must not
        /// land over them at retire (a heap's free-list links written into a freed render
        /// target): KeepCpuWrites keeps every byte that changed since.
        struct Snapshot {
            std::uint64_t address, size;
            std::vector<std::uint8_t> bytes;
        };
        std::vector<Snapshot> snapshots;
    };
    /// The pages an entry still protects: [begin, end), whole 4 KiB pages; empty when none.
    static std::pair<std::uint64_t, std::uint64_t> WatchedPages(const Entry& entry) {
        auto begin = entry.address & ~std::uint64_t{4095};
        auto end = (entry.address + entry.size + 4095) & ~std::uint64_t{4095};
        if (entry.released_edges & 1) begin += 4096;
        if ((entry.released_edges & 2) && end > begin) end -= 4096;
        return {begin, end > begin ? end : begin};
    }
    bool CanFit(std::uint64_t size) const {
        return entries.size() < 64 && size <= MaxBytes && bytes <= MaxBytes - size;
    }
    void Push(std::uint64_t address, std::uint64_t size, std::uint64_t tick, Payload payload) {
        assert(CanFit(size));
        entries.push_back(Entry{address, size, tick, std::move(payload), true, true, 0, {}});
        bytes += size;
    }
    /// `exact`: only copies sharing bytes with the range, not just a page (resogun: for consumers
    /// that read or drop exactly those bytes, e.g. an image upload).
    std::uint64_t RequiredTick(std::uint64_t address, std::uint64_t size, bool exact = false) const {
        std::uint64_t tick = 0;
        for (const auto& entry : entries) {
            if (entry.valid && entry.watched && Overlaps(entry, address, size, exact)) tick = entry.tick;
        }
        return tick;
    }
    template <typename Callback>
    void Cancel(std::uint64_t address, std::uint64_t size, Callback&& cancel, bool exact = false) {
        for (auto& entry : entries) {
            if (entry.valid && Overlaps(entry, address, size, exact)) {
                cancel(entry);
                entry.valid = false;
            }
        }
    }
    /// resogun: a CPU access to [address, address + size) that shares pages with small copies but
    /// not their bytes stops waiting for them: each such copy is unwatched (`release` drops its
    /// page protection) and still published when the GPU is done. Returns the copies released.
    template <typename Release>
    unsigned ReleaseOutside(std::uint64_t address, std::uint64_t size, std::uint64_t max_size,
                            Release&& release) {
        unsigned released = 0;
        for (auto& entry : entries) {
            if (entry.valid && entry.watched && entry.size <= max_size &&
                OverlapsPage(entry, address, size) &&
                !(address < entry.address + entry.size && entry.address < address + size)) {
                release(entry);
                entry.watched = false;
                Unprotected(entry, entry.address, entry.size);
                ++released;
            }
        }
        return released;
    }
    /// resogun: a CPU write to [address, address + size) on the first or last page of a copy but
    /// outside its bytes (a game's flag stored just past a render target it reads back) stops
    /// waiting for that copy: only that edge page is released (`release(begin, size)` drops its
    /// protection); the copy keeps protecting its other pages and is still published when the GPU
    /// is done. A CPU read of the copy's bytes on the released page sees the previous published
    /// version until then. Returns the copies released.
    template <typename Release>
    unsigned ReleaseEdge(std::uint64_t address, std::uint64_t size, Release&& release) {
        unsigned released = 0;
        if (!size || ((address ^ (address + size - 1)) & ~std::uint64_t{4095})) return 0;
        const std::uint64_t page = address & ~std::uint64_t{4095};
        for (auto& entry : entries) {
            if (!entry.valid || !entry.watched || !OverlapsPage(entry, address, size) ||
                (address < entry.address + entry.size && entry.address < address + size)) {
                continue;
            }
            const std::uint64_t first = entry.address & ~std::uint64_t{4095};
            const std::uint64_t last = (entry.address + entry.size - 1) & ~std::uint64_t{4095};
            const std::uint8_t edge = (page == first ? 1 : 0) | (page == last ? 2 : 0);
            if (!edge) continue; // an inner page is all copy bytes: not reachable
            release(page, std::uint64_t{4096});
            entry.released_edges |= edge;
            Unprotected(entry, page, std::uint64_t{4096});
            if (WatchedPages(entry).first == WatchedPages(entry).second) entry.watched = false;
            ++released;
        }
        return released;
    }
    /// Reads the bytes of every range a release just recorded (`read(address, dest, size)`, the
    /// guest's memory before the CPU write that released it goes on).
    template <typename Read>
    void FillSnapshots(Read&& read) {
        for (auto& entry : entries) {
            for (auto& snap : entry.snapshots) {
                if (snap.bytes.size() != snap.size) {
                    snap.bytes.resize(snap.size);
                    read(snap.address, snap.bytes.data(), snap.size);
                }
            }
        }
    }
    /// `data`: the copy's GPU bytes (entry.size). Puts back every byte of a snapshot range the
    /// CPU changed since (`read` reads the guest's bytes now). Returns the bytes kept.
    template <typename Read>
    static std::uint64_t KeepCpuWrites(const Entry& entry, std::uint8_t* data, Read&& read) {
        std::uint64_t kept = 0;
        std::vector<std::uint8_t> now;
        for (const auto& snap : entry.snapshots) {
            if (snap.bytes.size() != snap.size) continue;
            now.resize(snap.size);
            read(snap.address, now.data(), snap.size);
            std::uint8_t* out = data + (snap.address - entry.address);
            for (std::uint64_t i = 0; i < snap.size; ++i) {
                if (now[i] != snap.bytes[i]) {
                    out[i] = now[i];
                    ++kept;
                }
            }
        }
        return kept;
    }
    template <typename Ready, typename Complete>
    void Retire(Ready&& ready, Complete&& complete) {
        while (!entries.empty() && ready(entries.front().tick)) {
            complete(entries.front());
            bytes -= entries.front().size;
            entries.pop_front();
        }
    }
    std::uint64_t Bytes() const { return bytes; }
    const std::deque<Entry>& Entries() const { return entries; }

private:
    /// Records the copy's bytes within [begin, begin + size) as no longer protected.
    static void Unprotected(Entry& entry, std::uint64_t begin, std::uint64_t size) {
        const std::uint64_t lo = begin > entry.address ? begin : entry.address;
        const std::uint64_t end = begin + size, copy_end = entry.address + entry.size;
        const std::uint64_t hi = end < copy_end ? end : copy_end;
        if (hi <= lo) return;
        for (const auto& snap : entry.snapshots) {
            if (snap.address <= lo && hi <= snap.address + snap.size) return;
        }
        entry.snapshots.push_back({lo, hi - lo, {}});
    }
    static bool OverlapsPage(const Entry& entry, std::uint64_t address, std::uint64_t size) {
        if (!size) return false;
        const auto [begin, end] = WatchedPages(entry);
        return address < end && begin < address + size;
    }
    static bool Overlaps(const Entry& entry, std::uint64_t address, std::uint64_t size, bool exact) {
        const bool bytes = size && address < entry.address + entry.size && entry.address < address + size;
        return bytes || (!exact && OverlapsPage(entry, address, size));
    }
    static constexpr std::uint64_t MaxBytes = 64ull << 20;
    std::deque<Entry> entries;
    std::uint64_t bytes = 0;
};
} // namespace VideoCore
