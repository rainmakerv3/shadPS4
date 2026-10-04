// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "common/perf_profiler.h"

namespace Common::Perf {

namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::duration<double, std::milli>;

constexpr size_t NumStalls = static_cast<size_t>(Stall::Count);

constexpr size_t NumCounters = static_cast<size_t>(Counter::Count);

constexpr std::string_view StallNames[] = {
    "shader translate", "pipeline create", "pipeline wait",   "gpu wait",         "texture upload",
    "texture evict",    "buffer upload",   "buffer download", "readback wait",    "residency",
    "sparse bind",      "page faults",     "page protect",    "gpu page protect", "gpu thread busy",
    "waiting on game",  "present",         "frame wait",
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
                 "{:.0f} draws, {:.0f} dispatches, {:.0f} submits, {:.0f} barriers, {:.0f} render "
                 "passes and {:.0f} buffer uploads per frame, {:.0f}% of uploads recorded ahead, "
                 "{:.0f}% of shader lookups remembered, {:.0f}% of small buffers bound in place "
                 "| {}",
                 static_cast<double>(state.frames) * 1000.0 / window_ms, window_ms / 1000.0,
                 state.worst_frame_ms, state.hitches, HitchMs, busy, waiting, presenting,
                 per_frame(Counter::Draws), per_frame(Counter::Dispatches),
                 per_frame(Counter::Submits), per_frame(Counter::Barriers),
                 per_frame(Counter::RenderPasses), per_frame(Counter::BufferUploads),
                 events_share(Counter::BufferUploadsAhead, Counter::BufferUploads),
                 events_share(Counter::ShaderLookupsRemembered, Counter::ShaderLookups),
                 events_share(Counter::SmallBuffersInPlace, Counter::SmallBuffers),
                 Describe(state.window));
        state.window_start = now;
        state.frames = 0;
        state.hitches = 0;
        state.worst_frame_ms = 0.0;
        state.window = {};
    }
}

} // namespace Common::Perf
