// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <functional>

#include "resogun_copy.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "resogun_toggles.h"
#include "video_core/renderer_vulkan/vk_wait_diagnostics.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "resogun_threads.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;
std::mutex Scheduler::registry_mutex;
std::array<Scheduler*, 8> Scheduler::registry{};

namespace {
u64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

ResogunSync::Role SyncRole() {
    if (DrawPipe::OnStageB()) {
        return ResogunSync::DrawRecording;
    }
    return ResogunSync::OnGpuCommandThread() ? ResogunSync::GpuCommand : ResogunSync::OtherThread;
}

u64 ElapsedNs(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                start)
        .count();
}
} // namespace

Scheduler::Scheduler(const Instance& instance, bool threaded_recording)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore} {
    // resogun: RESOGUN_VK_RECORD_THREAD=0 records on the calling thread.
    const char* env = std::getenv("RESOGUN_VK_RECORD_THREAD");
    if (threaded_recording && !(env && env[0] == '0')) {
        record_chunk = AcquireChunk();
        recorder_thread = std::jthread(std::bind_front(&Scheduler::RecorderThread, this));
        recording_threaded = true;
    }
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    AllocateWorkerCommandBuffers(true);
    {
        std::scoped_lock lk{registry_mutex};
        for (auto& slot : registry) {
            if (!slot) {
                slot = this;
                break;
            }
        }
    }
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    {
        std::scoped_lock lk{registry_mutex};
        std::replace(registry.begin(), registry.end(), this, static_cast<Scheduler*>(nullptr));
    }
    if (recorder_thread.joinable()) {
        SyncRecording();
        recorder_thread.request_stop();
        recorder_cv.notify_all();
        recorder_thread.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

bool Scheduler::FullBarriers() {
    static const bool enabled = [] {
        const char* env = std::getenv("RESOGUN_FULL_BARRIERS");
        const bool on = env && env[0] == '1';
        if (on) {
            // In last_run.log whatever the GPU log level, so a tester's log shows the mode.
            std::printf("GPU: full barriers on (RESOGUN_FULL_BARRIERS=1)\n");
        }
        return on;
    }();
    return enabled;
}

void Scheduler::RecordFullBarrier(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    if (!recording_threaded) {
        if (FullBarriers()) {
            RecordFullBarrier(current_cmdbuf);
        }
        current_cmdbuf.beginRendering(rendering_info);
        return;
    }
    // The attachment infos live on this stack frame: the recorded closure keeps copies.
    Record([info = rendering_info, color_attachments, depth_attachment,
            stencil_attachment](vk::CommandBuffer cmdbuf) mutable {
        info.pColorAttachments = color_attachments.data();
        if (info.pDepthAttachment) {
            info.pDepthAttachment = &depth_attachment;
        }
        if (info.pStencilAttachment) {
            info.pStencilAttachment = &stencil_attachment;
        }
        if (FullBarriers()) {
            RecordFullBarrier(cmdbuf);
        }
        cmdbuf.beginRendering(info);
    });
}

void Scheduler::EndRendering() {
    if (!is_rendering) {
        return;
    }
    is_rendering = false;
    Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.endRendering();
        if (FullBarriers()) {
            RecordFullBarrier(cmdbuf);
        }
    });
}

std::unique_ptr<RecordChunk> Scheduler::AcquireChunk() {
    std::scoped_lock lk{recorder_mutex};
    if (free_chunks.empty()) {
        return std::make_unique<RecordChunk>();
    }
    auto chunk = std::move(free_chunks.back());
    free_chunks.pop_back();
    return chunk;
}

void Scheduler::SignalAfterHostCopies(std::function<void()> signal) {
    if (!IsRecordingDeferred()) {
        WaitHostCopies();
        signal();
        return;
    }
    ResogunCopy::FlushBatch();
    deferred_signals_issued.fetch_add(1, std::memory_order_relaxed);
    auto deferred = [signal = std::move(signal), done = deferred_signals_done]() mutable {
        ResogunCopy::AfterCopies([signal = std::move(signal), done = std::move(done)] {
            signal();
            done->fetch_add(1, std::memory_order_release);
        });
    };
    if (!ResogunSync::On(ResogunSync::HostQueue)) {
        Record([deferred = std::move(deferred)](vk::CommandBuffer) mutable { deferred(); });
    } else {
        // In the host work queue: after the copies issued before it, and runnable by a thread
        // that waits for it without waiting for the recording thread.
        const u64 seq = ++host_copies_issued;
        {
            std::scoped_lock lk{host_work_mutex};
            host_work.push_back({seq, {}, std::move(deferred)});
        }
        Record([this, seq](vk::CommandBuffer) { RunHostWork(seq); });
        // The guest waits for these fences. A recording thread stuck on a pipeline build
        // would hold them back with it, though nothing in them needs it: they run here.
        if (ResogunSync::On(ResogunSync::EagerSignals) && RecorderBehind()) {
            RunHostWork(seq);
        }
    }
    KickRecording(true);
}

bool Scheduler::RecorderBehind() const noexcept {
    if (queued_chunks.load(std::memory_order_relaxed) >= 2) {
        return true;
    }
    const u64 since = recorder_chunk_start.load(std::memory_order_relaxed);
    return since && NowNs() - since > 1'000'000;
}

void Scheduler::RecordHostCopy(const ResogunCopy::Item& item) {
    const u64 seq = ++host_copies_issued;
    if (!ResogunSync::On(ResogunSync::HostQueue)) {
        Record([item, this, seq](vk::CommandBuffer) {
            item.run(item);
            host_copies_done.store(seq, std::memory_order_release);
        });
        return;
    }
    {
        std::scoped_lock lk{host_work_mutex};
        host_work.push_back({seq, item, {}});
    }
    Record([this, seq](vk::CommandBuffer) { RunHostWork(seq); });
}

void Scheduler::RunHostWork(u64 upto) {
    if (host_copies_done.load(std::memory_order_acquire) >= upto) {
        return;
    }
    std::scoped_lock run{host_work_run_mutex};
    while (host_copies_done.load(std::memory_order_relaxed) < upto) {
        HostWork work;
        {
            std::scoped_lock lk{host_work_mutex};
            work = std::move(host_work.front());
            host_work.pop_front();
        }
        if (work.signal) {
            work.signal();
        } else {
            work.item.run(work.item);
        }
        host_copies_done.store(work.seq, std::memory_order_release);
    }
}

void Scheduler::WaitDeferredSignals(std::source_location caller) {
    const u64 issued = deferred_signals_issued.load(std::memory_order_relaxed);
    if (deferred_signals_done->load(std::memory_order_acquire) >= issued ||
        ResogunToggle::Disabled(ResogunToggle::OrderedGuestWrites)) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    {
        ResogunStats::WaitTimer timer{ResogunStats::host_copies_wait_ns};
        KickRecording(true);
        if (ResogunSync::On(ResogunSync::HostQueue)) {
            // Hands the signals still queued to the copy threads here.
            RunHostWork(host_copies_issued);
        }
        while (deferred_signals_done->load(std::memory_order_acquire) < issued) {
            // Helps the copy threads the signals wait for.
            ResogunCopy::WaitAsync();
            std::this_thread::yield();
        }
    }
    ResogunSync::Add(ResogunSync::DeferredSignals, SyncRole(), caller, ElapsedNs(start), true);
}

void Scheduler::WaitHostCopies(std::source_location caller) {
    const auto start = std::chrono::steady_clock::now();
    bool blocked = false;
    if (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
        ResogunStats::WaitTimer timer{ResogunStats::host_copies_wait_ns};
        ResogunStats::host_copy_waits.fetch_add(1, std::memory_order_relaxed);
        KickRecording(true);
        blocked = true;
        if (ResogunSync::On(ResogunSync::HostQueue)) {
            RunHostWork(host_copies_issued);
        }
        while (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
            std::this_thread::yield();
        }
    }
    {
        ResogunStats::WaitTimer timer{ResogunStats::copy_threads_wait_ns};
        ResogunCopy::WaitAsync();
    }
    const u64 ns = ElapsedNs(start);
    // Only waits worth a line: most calls find nothing pending.
    if (blocked || ns > 20'000) {
        ResogunSync::Add(ResogunSync::HostCopies, SyncRole(), caller, ns, true);
    }
}

void Scheduler::KickRecording(bool force) {
    if (!recording_threaded) {
        return;
    }
    // Callers kick where nobody holds the raw command buffer: deferral resumes.
    direct_mode = false;
    // Batches of tens of KiB keep the queue handoff cheap relative to the work it carries.
    if (!force && full_chunks.empty() && record_chunk->Size() < 32 * 1024) {
        return;
    }
    if (full_chunks.empty() && record_chunk->Empty()) {
        return;
    }
    bool wake;
    {
        std::scoped_lock lk{recorder_mutex};
        for (auto& chunk : full_chunks) {
            recorder_queue.push_back(std::move(chunk));
        }
        if (!record_chunk->Empty()) {
            recorder_queue.push_back(std::move(record_chunk));
        }
        wake = recorder_sleeping;
        queued_chunks.store(recorder_queue.size(), std::memory_order_release);
    }
    full_chunks.clear();
    // A busy recorder picks the new chunks up by itself: waking it is a syscall per draw.
    if (wake) {
        recorder_cv.notify_one();
    }
    if (!record_chunk) {
        record_chunk = AcquireChunk();
    }
}

void Scheduler::SyncRecording(ResogunSync::Kind kind, std::source_location caller) {
    if (!recording_threaded) {
        return;
    }
    KickRecording(true);
    const auto start = std::chrono::steady_clock::now();
    bool blocked;
    {
        ResogunStats::WaitTimer timer{ResogunStats::sync_recording_ns};
        std::unique_lock lk{recorder_mutex};
        const auto idle = [this] { return recorder_queue.empty() && !recorder_busy; };
        blocked = !idle();
        recorder_idle_cv.wait(lk, idle);
    }
    ResogunSync::Add(kind, SyncRole(), caller, ElapsedNs(start), blocked);
}

void Scheduler::RecorderThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("resogun:VkRecorder");
    while (true) {
        // Spin briefly before sleeping: the next chunk usually follows within microseconds,
        // and a sleeping recorder costs the GPU thread a wake-up syscall per kick. With few
        // hardware threads (Steam Deck: 8) the spin would take time from guest threads.
        static const auto spin_time = std::chrono::microseconds(
            ResogunThreads::Available() >= 12 ? 200 : 20);
        const auto spin_until = std::chrono::steady_clock::now() + spin_time;
        for (u32 spins = 1; queued_chunks.load(std::memory_order_acquire) == 0; ++spins) {
            __builtin_ia32_pause();
            // The clock is read every 256 pauses, not per iteration.
            if (!(spins & 255) &&
                (stoken.stop_requested() || std::chrono::steady_clock::now() >= spin_until)) {
                break;
            }
        }
        std::unique_ptr<RecordChunk> chunk;
        {
            std::unique_lock lk{recorder_mutex};
            recorder_sleeping = true;
            recorder_cv.wait(lk, stoken, [this] { return !recorder_queue.empty(); });
            recorder_sleeping = false;
            if (recorder_queue.empty()) {
                return; // stop requested
            }
            chunk = std::move(recorder_queue.front());
            recorder_queue.pop_front();
            queued_chunks.store(recorder_queue.size(), std::memory_order_release);
            recorder_busy = true;
        }
        // current_cmdbuf changes on this thread (an asynchronous submit, always the last
        // command of its chunk) or after SyncRecording(), which waits for this thread.
        const u64 queued_before = queued_tick.load(std::memory_order_relaxed);
        recorder_chunk_start.store(NowNs(), std::memory_order_relaxed);
        chunk->Execute(current_cmdbuf);
        recorder_chunk_start.store(0, std::memory_order_relaxed);
        {
            std::scoped_lock lk{recorder_mutex};
            free_chunks.push_back(std::move(chunk));
            recorder_busy = false;
            // WaitQueued() waits for submissions made here.
            if (recorder_queue.empty() ||
                queued_tick.load(std::memory_order_relaxed) != queued_before) {
                recorder_idle_cv.notify_all();
            }
        }
    }
}

void Scheduler::Flush(SubmitInfo& info, std::source_location caller) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info, caller);
}

void Scheduler::Flush(std::source_location caller) {
    SubmitInfo info{};
    Flush(info, caller);
}

void Scheduler::Finish(std::source_location caller) {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    const bool diagnose = WaitDiagnostics::Enabled();
    const auto start = diagnose ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    SubmitInfo info{};
    SubmitExecution(info, caller);
    const u64 submit_ns = diagnose ? std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count() : 0;
    Wait(presubmit_tick, caller, submit_ns);
}

void Scheduler::WaitSubmitted(std::source_location caller) {
    const u64 tick = CurrentTick();
    if (tick > 1) {
        Wait(tick - 1, caller);
    }
}

void Scheduler::Wait(u64 tick, std::source_location caller, u64 submit_ns) {
    const bool diagnose = WaitDiagnostics::Enabled();
    auto start = diagnose ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info, caller);
    }
    const auto submitted = std::chrono::steady_clock::now();
    const bool blocked = !IsFree(tick);
    {
        ResogunStats::WaitTimer timer{ResogunStats::tick_wait_ns};
        work_semaphore.Wait(tick);
    }
    ResogunSync::Add(ResogunSync::Tick, SyncRole(), caller, ElapsedNs(submitted), blocked);
    if (diagnose) {
        const auto completed = std::chrono::steady_clock::now();
        WaitDiagnostics::Get().Add(caller.file_name(), caller.line(),
            submit_ns + std::chrono::duration_cast<std::chrono::nanoseconds>(submitted - start).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(completed - submitted).count());
    }
}

void Scheduler::PopPendingOperations() {
    if (num_pending_ops.load(std::memory_order_acquire) == 0) {
        return; // every draw comes here
    }
    std::unique_lock lk(pending_ops_mutex);
    // resogun: this runs on every draw and dispatch. Querying the timeline semaphore is an
    // ioctl, so it is skipped when nothing waits and done once per 32 calls (~0.3 ms; reading
    // the clock per draw instead was itself a hot spot).
    if (pending_ops.empty()) {
        return;
    }
    if (!work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        if ((++pending_polls & 31) != 0 && !ResogunToggle::Disabled(ResogunToggle::PendingPollLimit)) {
            return;
        }
        work_semaphore.Refresh();
    }
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        num_pending_ops.fetch_sub(1, std::memory_order_release);
    }
}

void Scheduler::AllocateWorkerCommandBuffers(bool invalidate_state) {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));

    // Invalidate dynamic state so it gets applied to the new command buffer. (After an
    // asynchronous submit this runs on the recording thread; the submitting thread did it.)
    if (invalidate_state) {
        dynamic_state.Invalidate();
    }

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::SubmitExecution(SubmitInfo& info, std::source_location caller) {
    // resogun: a submission that waits for another scheduler's tick needs that tick's
    // submission on the queue first (its recorder may still hold it: asynchronous submits).
    WaitOtherSchedulers(info, caller);
    // Only plain submissions go through the recorder: semaphores and fences belong to
    // swapchain and present ordering, which expect the submission made here.
    const bool async = AsyncSubmitEnabled() && info.num_wait_semas == 0 &&
                       info.num_signal_semas == 0 && !info.fence;
    const u64 signal_value = work_semaphore.NextTick();

#if TRACY_GPU_ENABLED
    {
        SyncRecording(ResogunSync::Submit, caller);
        std::scoped_lock lk{submit_mutex};
        auto* profiler_ctx = instance.GetProfilerContext();
        if (profiler_ctx) {
            profiler_scope->~VkCtxScope();
            TracyVkCollect(profiler_ctx, current_cmdbuf);
        }
    }
#endif

    // resogun: not under submit_mutex: the recording thread takes it for asynchronous
    // submits, and on_submit may wait for that thread (data too large for a chunk). The
    // sparse binds it makes take the lock themselves.
    if (on_submit) {
        on_submit(info);
    }

    EndRendering();
    if (!large_pending.empty()) {
        // Freed on the recording thread after the commands that use them.
        Record([blocks = std::move(large_pending)](vk::CommandBuffer) {});
        large_pending.clear();
    }
    auto* profiler = GpuProfiler::Get();
    const bool profiled = profiler && profiler->Records(this);
    if (profiled) {
        // Until the next submission's first timestamp: mostly the GPU waiting for it.
        profiler->Mark(0x5B317ull, [] { return std::string{"(between submissions: GPU idle)"}; });
    }
    if (async && !profiled) {
        // resogun: the recording thread ends, submits and replaces the command buffer after
        // the commands before it, so this thread goes on translating while it catches up
        // (cold loads: the driver building pipelines). Commands recorded from here on belong
        // to the next submission: the submit ends its chunk (KickRecording), and the next chunk
        // starts on the command buffer the submit began.
        // Guest memory copies into staging read by this submission: the copy threads' issued
        // so far (not later ones), and the recording thread's, which come before the submit.
        auto copies_done = std::make_shared<std::atomic<bool>>(false);
        ResogunCopy::AfterCopies(
            [copies_done] { copies_done->store(true, std::memory_order_release); });
        direct_mode = false; // what was recorded directly is in the command buffer already
        Record([this, info, signal_value, copies_done,
                host_upto = host_copies_issued](vk::CommandBuffer cmdbuf) mutable {
            RunHostWork(host_upto);
            while (!copies_done->load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            QueueSubmit(cmdbuf, info, signal_value);
            AllocateWorkerCommandBuffers(false);
            queued_tick.store(signal_value, std::memory_order_release);
        });
        KickRecording(true);
        dynamic_state.Invalidate();
        // Bounded: the recorder may fall seconds behind while the driver builds pipelines.
        static const u64 depth = [] {
            const char* value = std::getenv("RESOGUN_ASYNC_SUBMIT_DEPTH");
            return value ? std::max<u64>(1, std::strtoull(value, nullptr, 10)) : u64{8};
        }();
        if (signal_value > depth) {
            WaitQueued(signal_value - depth, caller);
        }
        PopPendingOperations();
        return;
    }
    {
        ResogunStats::WaitTimer timer{ResogunStats::sync_submit_ns};
        SyncRecording(ResogunSync::Submit, caller);
    }
    // Guest memory copies into staging read by this submission (copy threads).
    WaitHostCopies(caller);
    QueueSubmit(current_cmdbuf, info, signal_value);
    queued_tick.store(signal_value, std::memory_order_release);
    AllocateWorkerCommandBuffers(true);

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::QueueSubmit(vk::CommandBuffer cmdbuf, SubmitInfo& info, u64 signal_value) {
    Check(cmdbuf.end());

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
    };

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    std::scoped_lock lk{submit_mutex};
    ImGui::Core::TextureManager::Submit();
    auto submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    work_semaphore.Refresh();
}

bool Scheduler::AsyncSubmitEnabled() const {
    // resogun: RESOGUN_LOAD_SYNCS=0: submissions drain the recorder and submit here.
    return ResogunSync::On(ResogunSync::AsyncSubmit) && !TRACY_GPU_ENABLED && recording_threaded &&
           !ResogunToggle::Disabled(ResogunToggle::ThreadedRecording);
}

void Scheduler::WaitQueued(u64 tick, std::source_location caller) {
    if (queued_tick.load(std::memory_order_acquire) >= tick) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    {
        ResogunStats::WaitTimer timer{ResogunStats::sync_recording_ns};
        std::unique_lock lk{recorder_mutex};
        recorder_idle_cv.wait(lk, [&] { return queued_tick.load(std::memory_order_acquire) >= tick; });
    }
    ResogunSync::Add(ResogunSync::Queued, SyncRole(), caller, ElapsedNs(start), true);
}

void Scheduler::WaitOtherSchedulers(const SubmitInfo& info, std::source_location caller) {
    if (info.num_wait_semas == 0) {
        return;
    }
    std::array<Scheduler*, 8> schedulers;
    {
        std::scoped_lock lk{registry_mutex};
        schedulers = registry;
    }
    for (u32 i = 0; i < info.num_wait_semas; ++i) {
        for (Scheduler* other : schedulers) {
            if (other && other != this && other->work_semaphore.Handle() == info.wait_semas[i]) {
                other->WaitQueued(info.wait_ticks[i], caller);
            }
        }
    }
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    // A null command buffer only updates the dirty flags, exactly as recording would.
    CommitWith(instance.IsDepthBoundsSupported(), instance.IsDynamicColorWriteMaskSupported(),
               instance.IsAttachmentFeedbackLoopLayoutSupported(), [&](auto&& command) {
                   if (cmdbuf) {
                       command(cmdbuf);
                   }
               });
}

} // namespace Vulkan
