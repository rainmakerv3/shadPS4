// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/logging/log.h"
#include "common/perf_profiler.h"
#include "common/thread.h"

namespace Common::Perf {

namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<double, std::milli>;

constexpr size_t NumStalls = static_cast<size_t>(Stall::Count);

constexpr size_t NumCounters = static_cast<size_t>(Counter::Count);

constexpr std::string_view StallNames[] = {
    "shader translate", "pipeline create",  "pipeline wait", "gpu wait",
    "texture upload",   "texture evict",    "buffer upload", "buffer download",
    "readback wait",    "residency",        "sparse bind",   "page faults",
    "page protect",     "gpu page protect", "dma sync",      "gpu thread busy",
    "waiting on game",  "present",          "frame wait",    "command recording",
    "upload thread",    "upload wait",
};
static_assert(std::size(StallNames) == NumStalls);

/// Frames this much longer than usual, and at least MinSpikeMs, are logged one by one.
constexpr double SpikeFactor = 1.75;
constexpr double MinSpikeMs = 30.0;
/// Frames this long are always logged.
constexpr double AlwaysLogMs = 100.0;
constexpr double HitchMs = 25.0;
constexpr u32 MaxSpikeLogsPerSecond = 4;
constexpr auto SummaryInterval = std::chrono::seconds{10};

void AddThreadCpu(std::string_view name);

/// What one thread counted and the time it recorded, since it started. Only that thread writes
/// them, with plain stores, and the thread that flips sums them up. The GPU thread counts a dozen
/// things for every draw, and an atomic add for each was a locked instruction that waited for
/// the stores before it to finish.
struct alignas(64) ThreadCounters {
    std::array<std::atomic<u64>, NumCounters> events{};
    std::array<std::atomic<u64>, NumStalls> nanoseconds{};
    std::array<std::atomic<u64>, NumStalls> count{};
    std::array<std::atomic<u64>, NumStalls> bytes{};
};

/// Threads are added the first time they count anything and never removed, so entries stay put.
std::mutex thread_counters_mutex;
std::deque<ThreadCounters> thread_counters;

ThreadCounters& LocalCounters() {
    thread_local ThreadCounters* counters = nullptr;
    if (!counters) [[unlikely]] {
        std::scoped_lock lock{thread_counters_mutex};
        counters = &thread_counters.emplace_back();
        // Every thread that counts anything is also reported by how much CPU it used, so the
        // summary tells whose time the frame was made in.
        AddThreadCpu(Common::GetCurrentThreadName());
    }
    return *counters;
}

/// Adds to a value only the calling thread writes.
void AddLocal(std::atomic<u64>& value, u64 amount) {
    value.store(value.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
}

std::atomic<u64> frame_number{};

/// Processor time each registered thread has used. The sum of these over a window is the whole
/// process's CPU use, so the biggest entries are the threads the work is actually spread over:
/// telling them apart says whether the game or the emulator's own threads hold up the frame.
struct ThreadCpu {
    std::string name;
#ifdef _WIN32
    HANDLE handle{};
#endif
    u32 thread_id{};
    u64 last_ms{};
};

std::mutex thread_cpu_mutex;
std::deque<ThreadCpu> thread_cpu;

u64 ThreadCpuMilliseconds(void* handle) {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(static_cast<HANDLE>(handle), &creation, &exit, &kernel, &user)) {
        return 0;
    }
    const auto to_ms = [](const FILETIME& time) {
        ULARGE_INTEGER value;
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        return value.QuadPart / 10'000;
    };
    return to_ms(kernel) + to_ms(user);
#else
    (void)handle;
    return 0;
#endif
}

/// Lists the threads that used the most CPU over the window, so the shares add up to the process
/// total and the rest can be accounted to everything not listed.
std::string TakeThreadCpuShares(double window_ms) {
    struct Entry {
        std::string name;
        double share;
    };
    std::vector<Entry> entries;
    u64 total_ms{};
    {
        std::scoped_lock lock{thread_cpu_mutex};
        for (auto& thread : thread_cpu) {
            const u64 ms = ThreadCpuMilliseconds(thread.handle);
            const u64 used = ms > thread.last_ms ? ms - thread.last_ms : 0;
            thread.last_ms = ms;
            total_ms += used;
            if (used != 0) {
                entries.push_back({thread.name, static_cast<double>(used) * 100.0 / window_ms});
            }
        }
    }
    std::ranges::sort(entries, [](const Entry& a, const Entry& b) { return a.share > b.share; });
    std::string text;
    double listed_share{};
    size_t shown{};
    for (const auto& entry : entries) {
        if (shown == 8 || entry.share < 2.0) {
            break;
        }
        if (!text.empty()) {
            text += ", ";
        }
        text += fmt::format("{} {:.0f}%", entry.name, entry.share);
        listed_share += entry.share;
        ++shown;
    }
    // Whatever the listed threads don't cover was used by the ones left out, so the shares still
    // account for the process total.
    const double rest = static_cast<double>(total_ms) * 100.0 / window_ms - listed_share;
    if (entries.size() > shown && rest >= 2.0) {
        if (!text.empty()) {
            text += ", ";
        }
        text += fmt::format("{} more {:.0f}%", entries.size() - shown, rest);
    }
    return text.empty() ? std::string{"none"} : text;
}

/// Adds the calling thread to the ones whose CPU use is reported, by a name that stays with it.
/// A thread can get here more than once, from the first counter it counts and the first stall it
/// records, and each of its fibers counts separately: only the first call is kept.
void AddThreadCpu(std::string_view name) {
#ifdef _WIN32
    HANDLE handle{};
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle, 0,
                         FALSE, DUPLICATE_SAME_ACCESS)) {
        return;
    }
    const u32 thread_id = GetThreadId(handle);
    {
        std::scoped_lock lock{thread_cpu_mutex};
        for (const auto& thread : thread_cpu) {
            if (thread.thread_id == thread_id) {
                CloseHandle(handle);
                return;
            }
        }
    }
#else
    void* handle{};
    const u32 thread_id = 0;
#endif
    ThreadCpu entry;
    entry.name = name;
    // Without the fiber the thread happens to run first, as the entry is for the thread.
    if (const auto fiber = entry.name.find("@@"); fiber != std::string::npos) {
        entry.name.resize(fiber);
    }
#ifdef _WIN32
    entry.handle = handle;
#endif
    entry.thread_id = thread_id;
    entry.last_ms = ThreadCpuMilliseconds(handle);
    std::scoped_lock lock{thread_cpu_mutex};
    thread_cpu.push_back(std::move(entry));
}

/// Time a thread spent in page faults and waiting for copies back. Which threads these hold up
/// tells whether the frame rate waits on them.
struct ThreadStalls {
    std::string name;
    std::atomic<u64> faults_ns{};
    std::atomic<u64> readbacks_ns{};
};

/// Threads are added the first time they stall and never removed, so entries stay put.
std::mutex thread_stalls_mutex;
std::deque<ThreadStalls> thread_stalls;

ThreadStalls& CurrentThreadStalls() {
    thread_local ThreadStalls* stalls = nullptr;
    if (!stalls) {
        std::string name = Common::GetCurrentThreadName();
        // Without the fiber the thread happens to run first, as the entry is for the thread.
        if (const auto fiber = name.find("@@"); fiber != std::string::npos) {
            name.resize(fiber);
        }
        std::scoped_lock lock{thread_stalls_mutex};
        stalls = &thread_stalls.emplace_back();
        AddThreadCpu(name);
        stalls->name = std::move(name);
    }
    return *stalls;
}

/// Lists the threads that spent the most of the window in page faults, or waiting for copies
/// back, which they mostly do from within a fault, e.g. "Main 31% (24% waiting for copies back)".
std::string TakeThreadStalls(double window_ms) {
    struct Entry {
        const std::string* name;
        u64 faults_ns;
        u64 readbacks_ns;
    };
    std::vector<Entry> entries;
    {
        std::scoped_lock lock{thread_stalls_mutex};
        for (auto& stalls : thread_stalls) {
            const u64 faults_ns = stalls.faults_ns.exchange(0, std::memory_order_relaxed);
            const u64 readbacks_ns = stalls.readbacks_ns.exchange(0, std::memory_order_relaxed);
            if (faults_ns != 0 || readbacks_ns != 0) {
                entries.push_back({&stalls.name, faults_ns, readbacks_ns});
            }
        }
    }
    std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
        return std::max(a.faults_ns, a.readbacks_ns) > std::max(b.faults_ns, b.readbacks_ns);
    });
    std::string text;
    const auto share = [window_ms](u64 ns) {
        return static_cast<double>(ns) / 1'000'000.0 * 100.0 / window_ms;
    };
    for (size_t i = 0; i < std::min<size_t>(entries.size(), 4); ++i) {
        if (!text.empty()) {
            text += ", ";
        }
        text += fmt::format("{} {:.0f}% ({:.0f}% waiting for copies back)", *entries[i].name,
                            share(entries[i].faults_ns), share(entries[i].readbacks_ns));
    }
    return text.empty() ? std::string{"none"} : text;
}

struct Totals {
    std::array<u64, NumStalls> nanoseconds{};
    std::array<u64, NumStalls> count{};
    std::array<u64, NumStalls> bytes{};
    std::array<u64, NumCounters> events{};

    void Add(const Totals& other) {
        for (size_t i = 0; i < NumStalls; ++i) {
            nanoseconds[i] += other.nanoseconds[i];
            count[i] += other.count[i];
            bytes[i] += other.bytes[i];
        }
        for (size_t i = 0; i < NumCounters; ++i) {
            events[i] += other.events[i];
        }
    }

    [[nodiscard]] u64 Events(Counter counter) const {
        return events[static_cast<size_t>(counter)];
    }
};

/// Only touched by the thread that flips.
struct FlipState {
    Clock::time_point last_flip{};
    Clock::time_point window_start{};
    Clock::time_point spike_second{};
    u32 spike_logs{};
    double usual_frame_ms{};
    double window_cpu_ms{};
    u64 frames{};
    u64 hitches{};
    double worst_frame_ms{};
    Totals window{};
};

FlipState flip_state{};

/// Processor time the whole process has used so far. The system keeps it, so reading it costs a
/// call and no clock of our own, and comparing two readings tells how much CPU the emulator used
/// over the time between them, whatever the number of threads it was spread across.
double ProcessCpuMilliseconds() {
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        return 0.0;
    }
    const auto to_ms = [](const FILETIME& time) {
        ULARGE_INTEGER value;
        value.LowPart = time.dwLowDateTime;
        value.HighPart = time.dwHighDateTime;
        return static_cast<double>(value.QuadPart) / 10'000.0;
    };
    return to_ms(kernel) + to_ms(user);
#else
    return 0.0;
#endif
}

/// Counters only go up, and each frame takes what they went up by since the last.
Totals taken{};

Totals TakeCounters() {
    Totals sums{};
    {
        std::scoped_lock lock{thread_counters_mutex};
        for (const auto& counters : thread_counters) {
            for (size_t i = 0; i < NumStalls; ++i) {
                sums.nanoseconds[i] += counters.nanoseconds[i].load(std::memory_order_relaxed);
                sums.count[i] += counters.count[i].load(std::memory_order_relaxed);
                sums.bytes[i] += counters.bytes[i].load(std::memory_order_relaxed);
            }
            for (size_t i = 0; i < NumCounters; ++i) {
                sums.events[i] += counters.events[i].load(std::memory_order_relaxed);
            }
        }
    }
    Totals totals{};
    for (size_t i = 0; i < NumStalls; ++i) {
        totals.nanoseconds[i] = sums.nanoseconds[i] - taken.nanoseconds[i];
        totals.count[i] = sums.count[i] - taken.count[i];
        totals.bytes[i] = sums.bytes[i] - taken.bytes[i];
    }
    for (size_t i = 0; i < NumCounters; ++i) {
        totals.events[i] = sums.events[i] - taken.events[i];
    }
    taken = sums;
    return totals;
}

/// Lists the categories that took time, largest first, e.g.
/// "texture upload 12.3 ms x40 (31.2 MB), shader translate 4.1 ms x6".
std::string Describe(const Totals& totals) {
    std::array<size_t, NumStalls> order{};
    for (size_t i = 0; i < NumStalls; ++i) {
        order[i] = i;
    }
    std::ranges::sort(
        order, [&](size_t a, size_t b) { return totals.nanoseconds[a] > totals.nanoseconds[b]; });

    std::string text;
    for (const size_t i : order) {
        const double ms = static_cast<double>(totals.nanoseconds[i]) / 1'000'000.0;
        if (ms < 0.1 && totals.bytes[i] < 1_MB) {
            continue;
        }
        if (!text.empty()) {
            text += ", ";
        }
        text += fmt::format("{} {:.1f} ms x{}", StallNames[i], ms, totals.count[i]);
        if (totals.bytes[i] != 0) {
            text += fmt::format(" ({:.1f} MB)", static_cast<double>(totals.bytes[i]) / 1048576.0);
        }
    }
    return text.empty() ? std::string{"nothing recorded"} : text;
}

} // namespace

void Record(Stall stall, u64 nanoseconds, u64 bytes) {
    auto& counters = LocalCounters();
    const auto index = static_cast<size_t>(stall);
    AddLocal(counters.nanoseconds[index], nanoseconds);
    AddLocal(counters.count[index], 1);
    if (bytes != 0) {
        AddLocal(counters.bytes[index], bytes);
    }
    if (stall == Stall::PageFault) {
        CurrentThreadStalls().faults_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
    } else if (stall == Stall::ReadbackWait) {
        CurrentThreadStalls().readbacks_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
    }
}

u64 FrameNumber() {
    return frame_number.load(std::memory_order_relaxed);
}

std::atomic<u64>* Detail::LocalEvents() {
    return LocalCounters().events.data();
}

u64 Total(Counter counter) {
    return LocalCounters().events[static_cast<size_t>(counter)].load(std::memory_order_relaxed);
}

void RegisterCurrentThread(std::string_view name) {
    AddThreadCpu(name);
}

void OnFlip() {
    auto& state = flip_state;
    const auto now = Clock::now();
    if (state.last_flip == Clock::time_point{}) {
        state.last_flip = now;
        state.window_start = now;
        state.window_cpu_ms = ProcessCpuMilliseconds();
        TakeCounters();
        return;
    }

    frame_number.fetch_add(1, std::memory_order_relaxed);
    const double frame_ms = Milliseconds(now - state.last_flip).count();
    state.last_flip = now;
    const Totals frame = TakeCounters();

    const bool is_spike =
        frame_ms >= AlwaysLogMs || (state.usual_frame_ms > 0.0 && frame_ms >= MinSpikeMs &&
                                    frame_ms >= state.usual_frame_ms * SpikeFactor);
    if (is_spike) {
        if (now - state.spike_second >= std::chrono::seconds{1}) {
            state.spike_second = now;
            state.spike_logs = 0;
        }
        if (state.spike_logs++ < MaxSpikeLogsPerSecond) {
            LOG_WARNING(Render,
                        "Perf: slow frame {:.1f} ms (usual {:.1f} ms), {} draws, {} "
                        "dispatches: {}",
                        frame_ms, state.usual_frame_ms, frame.Events(Counter::Draws),
                        frame.Events(Counter::Dispatches), Describe(frame));
        }
    }
    // Follow the usual frame time without letting single spikes drag it up.
    const double sample = state.usual_frame_ms > 0.0
                              ? std::min(frame_ms, state.usual_frame_ms * SpikeFactor)
                              : frame_ms;
    state.usual_frame_ms =
        state.usual_frame_ms > 0.0 ? state.usual_frame_ms * 0.95 + sample * 0.05 : sample;

    ++state.frames;
    state.hitches += frame_ms >= HitchMs ? 1 : 0;
    state.worst_frame_ms = std::max(state.worst_frame_ms, frame_ms);
    state.window.Add(frame);

    const auto window = now - state.window_start;
    if (window >= SummaryInterval) {
        const double window_ms = Milliseconds(window).count();
        const auto window_share = [&](Stall stall) {
            const double ms =
                static_cast<double>(state.window.nanoseconds[static_cast<size_t>(stall)]) /
                1'000'000.0;
            return ms * 100.0 / window_ms;
        };
        // The GPU thread spins while it waits on the game, and waits for presentation to free a
        // buffer for the next frame, so neither part is real work.
        const double waiting = window_share(Stall::GuestWait);
        const double presenting = window_share(Stall::FrameWait);
        const double busy = std::max(window_share(Stall::GpuThread) - waiting - presenting, 0.0);
        // All the threads together, so a process that fits in one core shows 100% even while
        // the GPU thread waits: whatever is left is what the guest threads were doing.
        const double process_share =
            (ProcessCpuMilliseconds() - state.window_cpu_ms) * 100.0 / window_ms;
        const double frames = static_cast<double>(std::max<u64>(state.frames, 1));
        const auto events_share = [&](Counter part, Counter whole) {
            const u64 total = state.window.Events(whole);
            return total == 0 ? 0.0
                              : static_cast<double>(state.window.Events(part)) * 100.0 /
                                    static_cast<double>(total);
        };
        const auto per_frame = [&](Counter counter) {
            return static_cast<double>(state.window.Events(counter)) / frames;
        };
        // Microseconds of a time counted in nanoseconds for each of something counted.
        const auto per_op = [&](Counter ns, Counter ops) {
            const u64 total = state.window.Events(ops);
            return total == 0 ? 0.0
                              : static_cast<double>(state.window.Events(ns)) / 1000.0 /
                                    static_cast<double>(total);
        };
        LOG_INFO(Render,
                 "Perf: {:.1f} fps over {:.1f} s, worst frame {:.1f} ms, {} frames over {:.0f} ms, "
                 "gpu thread {:.0f}% busy, {:.0f}% waiting on game and {:.0f}% on presentation, "
                 "{:.0f}% of a core used by every thread, "
                 "host GPU busy {:.1f} ms a frame ({:.1f} ms in runs of draws, {:.2f} us a draw, "
                 "{:.1f} ms in runs of dispatches, {:.2f} us a dispatch), "
                 "{:.0f} draws, {:.0f} dispatches, {:.0f} switches between them with {:.0f} "
                 "barriers right before, {:.0f} dispatches with shared memory in a buffer "
                 "({:.1f} MB cleared), {:.0f} submits, {:.0f} barriers ({:.0f} memory, {:.0f} image "
                 "over {:.0f} subresources), {:.0f} render passes, "
                 "{:.0f} buffer uploads and {:.0f} rewritten buffers copied per frame, "
                 "{:.0f}% of uploads recorded ahead, "
                 "{:.0f}% of shader lookups remembered, {:.0f}% of small buffers bound in place, "
                 "{:.0f} copies into the stream of {:.1f} KB each per frame "
                 "| {}",
                 static_cast<double>(state.frames) * 1000.0 / window_ms, window_ms / 1000.0,
                 state.worst_frame_ms, state.hitches, HitchMs, busy, waiting, presenting,
                 process_share,
                 per_frame(Counter::GpuBusyNs) / 1'000'000.0,
                 per_frame(Counter::GpuDrawRunNs) / 1'000'000.0,
                 per_op(Counter::GpuDrawRunNs, Counter::Draws),
                 per_frame(Counter::GpuDispatchRunNs) / 1'000'000.0,
                 per_op(Counter::GpuDispatchRunNs, Counter::Dispatches), per_frame(Counter::Draws),
                 per_frame(Counter::Dispatches), per_frame(Counter::WorkSwitches),
                 per_frame(Counter::SwitchBarriers), per_frame(Counter::SharedMemoryDispatches),
                 per_frame(Counter::SharedMemoryBytes) / 1'000'000.0, per_frame(Counter::Submits),
                 per_frame(Counter::Barriers), per_frame(Counter::MemoryBarriers),
                 per_frame(Counter::ImageBarriers), per_frame(Counter::ImageBarrierSubresources),
                 per_frame(Counter::RenderPasses),
                 per_frame(Counter::BufferUploads), per_frame(Counter::RewrittenBuffersCopied),
                 events_share(Counter::BufferUploadsAhead, Counter::BufferUploads),
                 events_share(Counter::ShaderLookupsRemembered, Counter::ShaderLookups),
                 events_share(Counter::SmallBuffersInPlace, Counter::SmallBuffers),
                 per_frame(Counter::StreamCopies),
                 per_frame(Counter::StreamCopies) > 0.0
                     ? per_frame(Counter::StreamCopyBytes) / per_frame(Counter::StreamCopies) / 1024.0
                     : 0.0,
                 Describe(state.window));
        LOG_INFO(Render, "Perf: threads {}", TakeThreadCpuShares(window_ms));
        LOG_INFO(Render,
                 "Perf: cpu on the gpu thread {:.1f} ms a frame recording draws and dispatches "
                 "({:.2f} us a draw, {:.2f} us a dispatch), {:.0f} of {:.0f} copies into the "
                 "stream reused a copy made before the next barrier or wait",
                 (per_frame(Counter::DrawCpuNs) + per_frame(Counter::DispatchCpuNs)) / 1'000'000.0,
                 per_op(Counter::DrawCpuNs, Counter::Draws),
                 per_op(Counter::DispatchCpuNs, Counter::Dispatches),
                 per_frame(Counter::StreamCopiesReused), per_frame(Counter::StreamCopies));
        if (per_frame(Counter::StreamCopySourceChanged) > 0.0) {
            LOG_INFO(Render, "Perf: {} of {} stream copies found their source rewritten meanwhile",
                     per_frame(Counter::StreamCopySourceChanged), per_frame(Counter::StreamCopies));
        }
        if (per_frame(Counter::StreamCopyProbed) > 0.0) {
            LOG_INFO(Render, "Perf: {} stream copies compared their source with the one they had",
                     per_frame(Counter::StreamCopyProbed));
        }
        LOG_INFO(Render,
                 "Perf: guest semaphores {:.0f} waits and {:.0f} signals a frame, "
                 "{:.1f} ms a frame waiting in them, {:.2f} us a wait",
                 per_frame(Counter::SemaWaits), per_frame(Counter::SemaReleases),
                 per_frame(Counter::SemaAcquireNs) / 1'000'000.0,
                 per_op(Counter::SemaAcquireNs, Counter::SemaWaits));
        LOG_INFO(Render,
                 "Perf: the game slept {:.0f} times a frame for {:.2f} ms in total ({:.1f} us a "
                 "sleep)",
                 per_frame(Counter::GuestSleeps),
                 per_frame(Counter::GuestSleepNs) / 1'000'000.0,
                 per_op(Counter::GuestSleepNs, Counter::GuestSleeps));
        LOG_INFO(Render,
                 "Perf: game threads made {:.0f} write and {:.0f} read faults per frame, {:.0f}% "
                 "of the write faults on pages that faulted in the same or the previous frame and "
                 "{:.0f}% on the page after one that did, {:.1f} dma syncs and {:.0f} command "
                 "processor writes ({:.0f} through the GPU copy) per frame",
                 per_frame(Counter::WriteFaults), per_frame(Counter::ReadFaults),
                 events_share(Counter::WriteFaultsRepeated, Counter::WriteFaults),
                 events_share(Counter::WriteFaultsFollowing, Counter::WriteFaults),
                 per_frame(Counter::DmaSyncs), per_frame(Counter::CpWrites),
                 per_frame(Counter::CpWritesGpu));
        LOG_INFO(Render,
                 "Perf: gpu thread made {:.0f} read and {:.0f} write faults per frame (wait reg "
                 "mem {:.0f}, cond exec {:.0f}, semaphore {:.0f}, rewind {:.0f}, event write "
                 "{:.0f}, eos store {:.0f}, dump const {:.0f}, other {:.0f}), {:.0f} linear image "
                 "downloads ({:.1f} MB) in {:.1f} batches",
                 per_frame(Counter::GpuThreadReadFaults),
                 per_frame(Counter::GpuThreadWriteFaults), per_frame(Counter::CpFaultWaitRegMem),
                 per_frame(Counter::CpFaultCondExec), per_frame(Counter::CpFaultMemSemaphore),
                 per_frame(Counter::CpFaultRewind), per_frame(Counter::CpFaultEventWrite),
                 per_frame(Counter::CpFaultEventWriteEos), per_frame(Counter::CpFaultDumpConstRam),
                 per_frame(Counter::CpFaultOther), per_frame(Counter::LinearImageDownloads),
                 per_frame(Counter::LinearImageBytes) / 1'000'000.0,
                 per_frame(Counter::LinearImageBatches));
        LOG_INFO(Render, "Perf: threads that spent the most time in page faults: {}",
                 TakeThreadStalls(window_ms));
        const u64 async_work = state.window.Events(Counter::AsyncDispatches) +
                               state.window.Events(Counter::AsyncUploads);
        if (async_work != 0) {
            LOG_INFO(Render,
                     "Perf: second queue ran {:.0f} of {:.0f} compute ring dispatches, {:.0f} "
                     "buffer uploads and {:.0f} copies back per frame, the graphics queue "
                     "submitted {:.1f} times a frame for its work",
                     per_frame(Counter::AsyncDispatches), per_frame(Counter::ComputeRingDispatches),
                     per_frame(Counter::AsyncUploads), per_frame(Counter::AsyncReadbacks),
                     per_frame(Counter::AsyncGraphicsSubmits));
        }
        state.window_start = now;
        state.window_cpu_ms = ProcessCpuMilliseconds();
        state.frames = 0;
        state.hitches = 0;
        state.worst_frame_ms = 0.0;
        state.window = {};
    }
}

} // namespace Common::Perf
