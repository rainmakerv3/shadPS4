// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/sampling_profiler.h"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "common/logging/log.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "common/types.h"

namespace Common::Perf {

namespace {

constexpr auto SampleInterval = std::chrono::milliseconds{1};
constexpr auto ReportInterval = std::chrono::seconds{30};
/// Threads run with a 2 MB stack, so this holds any of them whole.
constexpr size_t MaxStackCopy = 4_MB;
/// Unwinding the last copied frame may read a little past the copy.
constexpr size_t StackCopyGuard = 4_MB;
constexpr u32 MaxFrames = 96;
constexpr size_t NumTopSelf = 25;
constexpr size_t NumTopInclusive = 45;

struct SampledThread {
    std::string name;
    DWORD thread_id{};
    HANDLE handle{};
    u64 stack_low{};
    u64 stack_high{};
    u64 samples{};
    /// Samples by function start: where the thread was, and every function on its stack.
    std::unordered_map<u64, u64> self;
    std::unordered_map<u64, u64> inclusive;
};

class Sampler {
public:
    static Sampler& Instance() {
        static Sampler sampler;
        return sampler;
    }

    void Add(std::string_view name) {
        HANDLE handle{};
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                             THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                 THREAD_QUERY_LIMITED_INFORMATION,
                             FALSE, 0)) {
            LOG_WARNING(Common, "Sampler: can't sample thread {}", name);
            return;
        }
        auto thread = std::make_unique<SampledThread>();
        thread->name = std::string{name};
        thread->thread_id = GetCurrentThreadId();
        thread->handle = handle;
        ULONG_PTR low{};
        ULONG_PTR high{};
        GetCurrentThreadStackLimits(&low, &high);
        thread->stack_low = low;
        thread->stack_high = high;

        std::scoped_lock lock{mutex};
        threads.push_back(std::move(thread));
        if (!worker.joinable()) {
            HMODULE exe = GetModuleHandleW(nullptr);
            LOG_INFO(Common, "Sampler: started, {} loaded at {:#x}", ModuleName(exe),
                     reinterpret_cast<u64>(exe));
            worker = std::jthread{[this](std::stop_token token) { Run(token); }};
        }
    }

    void Remove() {
        const DWORD thread_id = GetCurrentThreadId();
        std::scoped_lock lock{mutex};
        std::erase_if(threads, [&](const auto& thread) {
            if (thread->thread_id != thread_id) {
                return false;
            }
            CloseHandle(thread->handle);
            return true;
        });
    }

private:
    Sampler() {
        // Committed up front, so unwinding never touches memory that isn't there.
        stack_copy = static_cast<u8*>(VirtualAlloc(nullptr, MaxStackCopy + StackCopyGuard,
                                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }

    ~Sampler() {
        if (worker.joinable()) {
            worker.request_stop();
            worker.join();
        }
    }

    void Run(std::stop_token token) {
        Common::SetCurrentThreadName("shadPS4:Sampler");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
        HANDLE timer = CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        auto last_report = std::chrono::steady_clock::now();
        while (!token.stop_requested()) {
            if (timer) {
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(SampleInterval).count() /
                    100);
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(1);
            }

            std::scoped_lock lock{mutex};
            if (!stack_copy) {
                continue;
            }
            for (auto& thread : threads) {
                SampleThread(*thread);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report >= ReportInterval) {
                const double seconds = std::chrono::duration<double>(now - last_report).count();
                for (auto& thread : threads) {
                    Report(*thread, seconds);
                }
                last_report = now;
            }
        }
        if (timer) {
            CloseHandle(timer);
        }
    }

    void SampleThread(SampledThread& thread) {
        // While the thread is stopped only copy its registers and stack: it may hold any lock,
        // so anything that could take one has to wait until it runs again.
        alignas(16) CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (SuspendThread(thread.handle) == static_cast<DWORD>(-1)) {
            return;
        }
        if (!GetThreadContext(thread.handle, &context)) {
            ResumeThread(thread.handle);
            return;
        }
        const u64 rsp = context.Rsp;
        u64 copied{};
        if (rsp >= thread.stack_low && rsp < thread.stack_high) {
            copied = std::min<u64>(thread.stack_high - rsp, MaxStackCopy);
            std::memcpy(stack_copy, reinterpret_cast<const void*>(rsp), copied);
        }
        ResumeThread(thread.handle);

        std::array<u64, MaxFrames> functions{};
        const u32 num_frames = Unwind(context, rsp, copied, functions);
        if (num_frames == 0) {
            return;
        }
        ++thread.samples;
        ++thread.self[functions[0]];
        // Count each function once per sample, however deep it recursed.
        std::sort(functions.begin(), functions.begin() + num_frames);
        const auto end = std::unique(functions.begin(), functions.begin() + num_frames);
        for (auto it = functions.begin(); it != end; ++it) {
            ++thread.inclusive[*it];
        }
    }

    /// Walks the copied stack, filling in the start of each function on it, innermost first.
    u32 Unwind(CONTEXT& context, u64 rsp, u64 copied, std::array<u64, MaxFrames>& functions) {
        const u64 copy_begin = reinterpret_cast<u64>(stack_copy);
        const u64 copy_end = copy_begin + copied;
        // Pointers into the stack, saved on it or held in registers, now point into the copy.
        const auto relocate = [&](u64 value) {
            return value >= rsp && value < rsp + copied ? value - rsp + copy_begin : value;
        };
        auto* words = reinterpret_cast<u64*>(stack_copy);
        for (u64 i = 0; i < copied / sizeof(u64); ++i) {
            words[i] = relocate(words[i]);
        }
        for (DWORD64* reg : {&context.Rsp, &context.Rbp, &context.Rbx, &context.Rsi, &context.Rdi,
                             &context.R12, &context.R13, &context.R14, &context.R15}) {
            *reg = relocate(*reg);
        }

        u32 num_frames{};
        while (num_frames < MaxFrames) {
            const u64 pc = context.Rip;
            if (pc == 0) {
                break;
            }
            DWORD64 image_base{};
            const auto* function = RtlLookupFunctionEntry(pc, &image_base, nullptr);
            functions[num_frames++] = function ? image_base + function->BeginAddress : pc;
            if (copied == 0) {
                break;
            }
            if (function) {
                PVOID handler_data{};
                DWORD64 establisher_frame{};
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc,
                                 const_cast<PRUNTIME_FUNCTION>(function), &context, &handler_data,
                                 &establisher_frame, nullptr);
            } else {
                // Leaf function without unwind data: the return address is on top.
                if (context.Rsp < copy_begin || context.Rsp + sizeof(u64) > copy_end) {
                    break;
                }
                context.Rip = *reinterpret_cast<const u64*>(context.Rsp);
                context.Rsp += sizeof(u64);
            }
            if (context.Rsp < copy_begin || context.Rsp >= copy_end) {
                break;
            }
        }
        return num_frames;
    }

    void Report(SampledThread& thread, double seconds) {
        if (thread.samples == 0) {
            return;
        }
        LOG_INFO(Common, "Sampler: thread {}, {} samples over {:.0f} s", thread.name,
                 thread.samples, seconds);
        const auto log_top = [&](std::string_view kind, const std::unordered_map<u64, u64>& counts,
                                 size_t count) {
            std::vector<std::pair<u64, u64>> sorted(counts.begin(), counts.end());
            const size_t num = std::min(count, sorted.size());
            std::partial_sort(sorted.begin(), sorted.begin() + num, sorted.end(),
                              [](const auto& a, const auto& b) { return a.second > b.second; });
            for (size_t i = 0; i < num; ++i) {
                const auto [address, samples] = sorted[i];
                LOG_INFO(Common, "Sampler: {} {} {:5.1f}% {}", thread.name, kind,
                         static_cast<double>(samples) * 100.0 / static_cast<double>(thread.samples),
                         Describe(address));
            }
        };
        log_top("self", thread.self, NumTopSelf);
        log_top("total", thread.inclusive, NumTopInclusive);
        thread.samples = 0;
        thread.self.clear();
        thread.inclusive.clear();
    }

    /// Names an address as module+offset, e.g. "shadPS4.exe+0x1a2b30".
    std::string Describe(u64 address) {
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(address), &module)) {
            return fmt::format("{:#x}", address);
        }
        return fmt::format("{}+{:#x}", ModuleName(module), address - reinterpret_cast<u64>(module));
    }

    std::string ModuleName(HMODULE module) {
        const auto it = module_names.find(module);
        if (it != module_names.end()) {
            return it->second;
        }
        std::array<char, MAX_PATH> path{};
        const DWORD length = GetModuleFileNameA(module, path.data(), MAX_PATH);
        std::string name{path.data(), length};
        if (const auto slash = name.find_last_of("\\/"); slash != std::string::npos) {
            name = name.substr(slash + 1);
        }
        module_names.emplace(module, name);
        return name;
    }

    std::mutex mutex;
    std::vector<std::unique_ptr<SampledThread>> threads;
    std::unordered_map<HMODULE, std::string> module_names;
    u8* stack_copy{};
    std::jthread worker;
};

} // Anonymous namespace

void SampleCurrentThread(std::string_view name) {
    Sampler::Instance().Add(name);
}

void StopSamplingCurrentThread() {
    Sampler::Instance().Remove();
}

} // namespace Common::Perf

#else

namespace Common::Perf {

void SampleCurrentThread(std::string_view) {}

void StopSamplingCurrentThread() {}

} // namespace Common::Perf

#endif
