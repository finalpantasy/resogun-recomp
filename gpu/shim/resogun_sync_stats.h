// SPDX-License-Identifier: GPL-2.0-or-later
// resogun: where the GPU command thread waits for the Vulkan recording thread, the copy threads
// or the GPU: calls, calls that blocked and wall time per call site, printed in the end-of-run
// report (resogun_gpu_sync_report). Cold course loads spend most of the GPU thread's blocked time
// here; the table says which waits to remove.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <source_location>
#include <string_view>

namespace ResogunSync {

/// RESOGUN_LOAD_SYNCS=0 turns off what cuts the GPU command thread's waits for the Vulkan
/// recording thread: asynchronous submits, host copies run by the thread that waits for them,
/// and readback processing without pending downloads skipping the host copy wait.
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("RESOGUN_LOAD_SYNCS");
        return !value || value[0] != '0';
    }();
    return enabled;
}

enum Kind : unsigned {
    Other,           ///< SyncRecording() from elsewhere (descriptor pushes too large for a chunk)
    Submit,          ///< recorder drained before a submission (Flush, Finish, Wait)
    Direct,          ///< CommandBuffer(): recorder drained to record on the calling thread
    LargeData,       ///< RecordData() block too large for a chunk
    HostCopies,      ///< guest memory copies before a submission or fence
    DeferredSignals, ///< deferred fence writes before a guest-visible write
    Tick,            ///< CPU wait for a GPU timeline tick
    Queued,          ///< wait for submissions the recorder makes (asynchronous submits)
    NumKinds,
};

inline constexpr std::array<const char*, NumKinds> kind_names = {
    "other recorder sync", "submit: recorder drain", "direct recording", "large data block",
    "host copies", "deferred signals", "GPU tick wait", "queued submits"};

/// Which thread waited: the GPU command thread, the draw recording thread, or another
/// (guest threads resolving a readback fault, the flip thread).
enum Role : unsigned { GpuCommand, DrawRecording, OtherThread };
inline constexpr std::array<const char*, 3> role_names = {"GPU", "DrawRec", "other"};

/// True on the GPU command thread (resogun_gpu.cpp; Windows only).
bool OnGpuCommandThread();

struct Site {
    const char* file{};
    unsigned line{};
    Kind kind{};
    Role role{};
    std::uint64_t calls{}, blocked{}, ns{};
};

class Table {
public:
    void Add(Kind kind, Role role, const std::source_location& where, std::uint64_t ns,
             bool blocked) {
        std::scoped_lock lock{mutex};
        Site* site = &sites.back();
        for (std::size_t i = 0; i + 1 < sites.size(); ++i) {
            if (!sites[i].file || (sites[i].kind == kind && sites[i].role == role &&
                                   sites[i].line == where.line() &&
                                   std::string_view{sites[i].file} == where.file_name())) {
                site = &sites[i];
                break;
            }
        }
        if (!site->file) {
            site->file = where.file_name();
            site->line = where.line();
            site->kind = kind;
            site->role = role;
        }
        ++site->calls;
        site->blocked += blocked;
        site->ns += ns;
    }

    void Report(std::FILE* out) {
        std::array<Site, 96> rows;
        {
            std::scoped_lock lock{mutex};
            rows = sites;
        }
        std::array<Site, NumKinds * 3> kinds{};
        for (const auto& row : rows) {
            if (!row.file) {
                continue;
            }
            auto& total = kinds[row.kind * 3 + row.role];
            total.calls += row.calls;
            total.blocked += row.blocked;
            total.ns += row.ns;
        }
        std::fprintf(out, "GPU syncs by reason and thread (calls, calls that blocked, wall ms):\n");
        for (unsigned k = 0; k < kinds.size(); ++k) {
            if (kinds[k].calls) {
                std::fprintf(out, "GPU syncs:   %-24s %-7s %9llu calls %9llu blocked %10.1f ms\n",
                             kind_names[k / 3], role_names[k % 3],
                             static_cast<unsigned long long>(kinds[k].calls),
                             static_cast<unsigned long long>(kinds[k].blocked), kinds[k].ns / 1e6);
            }
        }
        std::sort(rows.begin(), rows.end(),
                  [](const Site& a, const Site& b) { return a.ns > b.ns; });
        for (std::size_t i = 0; i < 20 && i < rows.size() && rows[i].file; ++i) {
            const std::string_view file{rows[i].file};
            const auto name = file.substr(file.find_last_of("/\\") + 1);
            std::fprintf(out, "GPU syncs:     %-22s %-7s %.*s:%u %llu calls %llu blocked %.1f ms\n",
                         kind_names[rows[i].kind], role_names[rows[i].role], int(name.size()),
                         name.data(), rows[i].line,
                         static_cast<unsigned long long>(rows[i].calls),
                         static_cast<unsigned long long>(rows[i].blocked), rows[i].ns / 1e6);
        }
    }

    static Table& Get() {
        static Table table;
        return table;
    }

private:
    std::mutex mutex;
    std::array<Site, 96> sites{};
};

/// `role`: from the caller, which knows the draw recording thread (DrawPipe::OnStageB).
inline void Add(Kind kind, Role role, const std::source_location& where, std::uint64_t ns,
                bool blocked) {
    Table::Get().Add(kind, role, where, ns, blocked);
}

} // namespace ResogunSync
