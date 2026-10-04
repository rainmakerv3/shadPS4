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
    "shader translate", "pipeline create", "pipeline wait",   "gpu wait",         "texture upload",
    "texture evict",    "buffer upload",   "buffer download", "readback wait",    "residency",
    "sparse bind",      "page faults",     "page protect",    "gpu page protect", "dma sync",
    "gpu thread busy",  "waiting on game", "present",         "frame wait",
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

/// Each counter gets a cache line of its own: the GPU thread counts things for every draw, and
/// sharing lines with counters the game's threads update on every page fault made each count
/// wait for the line to come back.
struct alignas(64) StallCounter {
    std::atomic<u64> nanoseconds{};
    std::atomic<u64> count{};
    std::atomic<u64> bytes{};
};

struct alignas(64) EventCounter {
    std::atomic<u64> value{};
};

std::array<StallCounter, NumStalls> stall_counters{};
std::array<EventCounter, NumCounters> event_counters{};
std::atomic<u64> frame_number{};

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
    u64 frames{};
    u64 hitches{};
    double worst_frame_ms{};
    Totals window{};
};

FlipState flip_state{};

Totals TakeCounters() {
    Totals totals{};
    for (size_t i = 0; i < NumStalls; ++i) {
        totals.nanoseconds[i] =
            stall_counters[i].nanoseconds.exchange(0, std::memory_order_relaxed);
        totals.count[i] = stall_counters[i].count.exchange(0, std::memory_order_relaxed);
        totals.bytes[i] = stall_counters[i].bytes.exchange(0, std::memory_order_relaxed);
    }
    for (size_t i = 0; i < NumCounters; ++i) {
        totals.events[i] = event_counters[i].value.exchange(0, std::memory_order_relaxed);
    }
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
    auto& counter = stall_counters[static_cast<size_t>(stall)];
    counter.nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
    counter.count.fetch_add(1, std::memory_order_relaxed);
    if (bytes != 0) {
        counter.bytes.fetch_add(bytes, std::memory_order_relaxed);
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

void Count(Counter counter, u64 amount) {
    event_counters[static_cast<size_t>(counter)].value.fetch_add(amount, std::memory_order_relaxed);
}

void OnFlip() {
    auto& state = flip_state;
    const auto now = Clock::now();
    if (state.last_flip == Clock::time_point{}) {
        state.last_flip = now;
        state.window_start = now;
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
        LOG_INFO(Render,
                 "Perf: {:.1f} fps over {:.1f} s, worst frame {:.1f} ms, {} frames over {:.0f} ms, "
                 "gpu thread {:.0f}% busy, {:.0f}% waiting on game and {:.0f}% on presentation, "
                 "host GPU busy {:.1f} ms a frame, "
                 "{:.0f} draws, {:.0f} dispatches, {:.0f} submits, {:.0f} barriers, {:.0f} render "
                 "passes, {:.0f} buffer uploads and {:.0f} rewritten buffers copied per frame, "
                 "{:.0f}% of uploads recorded ahead, "
                 "{:.0f}% of shader lookups remembered, {:.0f}% of small buffers bound in place "
                 "| {}",
                 static_cast<double>(state.frames) * 1000.0 / window_ms, window_ms / 1000.0,
                 state.worst_frame_ms, state.hitches, HitchMs, busy, waiting, presenting,
                 per_frame(Counter::GpuBusyNs) / 1'000'000.0, per_frame(Counter::Draws),
                 per_frame(Counter::Dispatches), per_frame(Counter::Submits),
                 per_frame(Counter::Barriers), per_frame(Counter::RenderPasses),
                 per_frame(Counter::BufferUploads), per_frame(Counter::RewrittenBuffersCopied),
                 events_share(Counter::BufferUploadsAhead, Counter::BufferUploads),
                 events_share(Counter::ShaderLookupsRemembered, Counter::ShaderLookups),
                 events_share(Counter::SmallBuffersInPlace, Counter::SmallBuffers),
                 Describe(state.window));
        LOG_INFO(Render,
                 "Perf: game threads made {:.0f} write and {:.0f} read faults per frame, {:.0f}% "
                 "of the write faults on pages that faulted in the same or the previous frame and "
                 "{:.0f}% on the page after one that did, {:.1f} dma syncs per frame",
                 per_frame(Counter::WriteFaults), per_frame(Counter::ReadFaults),
                 events_share(Counter::WriteFaultsRepeated, Counter::WriteFaults),
                 events_share(Counter::WriteFaultsFollowing, Counter::WriteFaults),
                 per_frame(Counter::DmaSyncs));
        LOG_INFO(Render, "Perf: threads that spent the most time in page faults: {}",
                 TakeThreadStalls(window_ms));
        state.window_start = now;
        state.frames = 0;
        state.hitches = 0;
        state.worst_frame_ms = 0.0;
        state.window = {};
    }
}

} // namespace Common::Perf
