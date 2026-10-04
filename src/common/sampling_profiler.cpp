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

constexpr auto SampleInterval = std::chrono::milliseconds{4};
constexpr auto ReportInterval = std::chrono::seconds{30};
/// Sampling stops after this long: by then it has seen the game in play.
constexpr auto SamplingTime = std::chrono::minutes{8};
/// Threads run with a 2 MB stack, so this holds any of them whole.
constexpr size_t MaxStackCopy = 4_MB;
/// Unwinding reads a little around the frames it walks: below the stack pointer when it is set
/// from a frame pointer, and past the copy for the outermost frame. Room is left on both sides.
constexpr size_t StackCopyHeadroom = 64_KB;
constexpr size_t StackCopyGuard = 4_MB;
constexpr u32 MaxFrames = 96;

/// Nonvolatile registers, as numbered in unwind data, and where CONTEXT holds them.
DWORD64& Register(CONTEXT& context, u32 index) {
    return (&context.Rax)[index];
}

/// Returns false if unwinding the function could read memory through a register that doesn't
/// point into the copied stack. Unwinding reads relative to the frame register once a function
/// set one up, and a frame register that was never relocated can point anywhere.
bool FrameRegistersInRange(const RUNTIME_FUNCTION* function, u64 image_base, CONTEXT& context,
                           u64 copy_begin, u64 copy_end) {
    constexpr u8 UnwindFlagChainInfo = 0x4;
    for (u32 depth = 0; function && depth < 8; ++depth) {
        u32 unwind_data = function->UnwindData;
        if (unwind_data & 1) {
            // Points at the function entry whose unwind data this fragment shares.
            function = reinterpret_cast<const RUNTIME_FUNCTION*>(image_base + (unwind_data & ~1u));
            unwind_data = function->UnwindData;
        }
        const auto* info = reinterpret_cast<const u8*>(image_base + unwind_data);
        const u8 flags = info[0] >> 3;
        const u8 count_of_codes = info[2];
        const u8 frame_register = info[3] & 0xF;
        if (frame_register != 0) {
            const u64 value = Register(context, frame_register);
            if (value < copy_begin || value >= copy_end) {
                return false;
            }
        }
        if (!(flags & UnwindFlagChainInfo)) {
            break;
        }
        // The chained entry follows the unwind codes, which are padded to an even count.
        const u32 codes_size = ((count_of_codes + 1u) & ~1u) * sizeof(u16);
        function = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + codes_size);
    }
    return true;
}
constexpr size_t NumTopSelf = 15;
constexpr size_t NumTopOwn = 40;
constexpr size_t NumTopCalls = 30;
constexpr size_t NumTopInclusive = 30;
constexpr size_t NumTopModules = 8;

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
    /// Samples by the innermost function of the emulator itself on the stack, so time spent in
    /// the system libraries and drivers it calls, such as memcpy or locking, is counted for it.
    std::unordered_map<u64, u64> own;
    /// For samples in a system library or driver, where in it, by the emulator function that
    /// called in: tells a copy from a lock or a driver call.
    std::unordered_map<u64, std::unordered_map<u64, u64>> own_calls;
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
            const auto* dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
            const auto* nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                reinterpret_cast<const u8*>(exe) + dos_header->e_lfanew);
            exe_begin = reinterpret_cast<u64>(exe);
            exe_end = exe_begin + nt_headers->OptionalHeader.SizeOfImage;
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
        stack_buffer = static_cast<u8*>(
            VirtualAlloc(nullptr, StackCopyHeadroom + MaxStackCopy + StackCopyGuard,
                         MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        stack_copy = stack_buffer ? stack_buffer + StackCopyHeadroom : nullptr;
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
        const auto start = std::chrono::steady_clock::now();
        auto last_report = start;
        while (!token.stop_requested() && last_report - start < SamplingTime) {
            if (timer) {
                LARGE_INTEGER due{};
                due.QuadPart = -static_cast<LONGLONG>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(SampleInterval).count() /
                    100);
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(4);
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
        for (u32 i = 0; i < num_frames; ++i) {
            if (functions[i] >= exe_begin && functions[i] < exe_end) {
                ++thread.own[functions[i]];
                if (i != 0) {
                    ++thread.own_calls[functions[i]][functions[0]];
                }
                break;
            }
        }
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
        for (u32 reg = 0; reg < 16; ++reg) {
            Register(context, reg) = relocate(Register(context, reg));
        }

        u32 num_frames{};
        while (num_frames < MaxFrames) {
            const u64 pc = context.Rip;
            if (pc == 0) {
                break;
            }
            DWORD64 image_base{};
            const auto* function = RtlLookupFunctionEntry(pc, &image_base, nullptr);
            if (!function && num_frames > 0) {
                // Only the innermost frame may lack unwind data. Further out it means the walk
                // went wrong, and its addresses can't be trusted.
                break;
            }
            functions[num_frames++] = function ? image_base + function->BeginAddress : pc;
            if (copied == 0) {
                break;
            }
            if (function) {
                if (!FrameRegistersInRange(function, image_base, context, copy_begin, copy_end)) {
                    break;
                }
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
        // Where the time went by module: the emulator itself, the driver, the system's locks and
        // page protection, the runtime's copies. The function lists only show the top of each.
        std::unordered_map<std::string, u64> modules;
        for (const auto& [address, samples] : thread.self) {
            modules[ModuleOf(address)] += samples;
        }
        std::vector<std::pair<std::string, u64>> sorted_modules(modules.begin(), modules.end());
        std::ranges::sort(sorted_modules,
                          [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string module_text;
        for (size_t i = 0; i < std::min(NumTopModules, sorted_modules.size()); ++i) {
            module_text += fmt::format("{}{} {:.1f}%", i == 0 ? "" : ", ", sorted_modules[i].first,
                                       static_cast<double>(sorted_modules[i].second) * 100.0 /
                                           static_cast<double>(thread.samples));
        }
        LOG_INFO(Common, "Sampler: {} modules {}", thread.name, module_text);

        log_top("self", thread.self, NumTopSelf);
        log_top("own", thread.own, NumTopOwn);
        log_top("total", thread.inclusive, NumTopInclusive);

        struct Call {
            u64 caller;
            u64 callee;
            u64 samples;
        };
        std::vector<Call> calls;
        for (const auto& [caller, callees] : thread.own_calls) {
            for (const auto& [callee, samples] : callees) {
                calls.push_back({caller, callee, samples});
            }
        }
        const size_t num_calls = std::min(NumTopCalls, calls.size());
        std::partial_sort(calls.begin(), calls.begin() + num_calls, calls.end(),
                          [](const Call& a, const Call& b) { return a.samples > b.samples; });
        for (size_t i = 0; i < num_calls; ++i) {
            LOG_INFO(Common, "Sampler: {} calls {:5.1f}% {} -> {}", thread.name,
                     static_cast<double>(calls[i].samples) * 100.0 /
                         static_cast<double>(thread.samples),
                     Describe(calls[i].caller), Describe(calls[i].callee));
        }

        thread.samples = 0;
        thread.self.clear();
        thread.own.clear();
        thread.own_calls.clear();
        thread.inclusive.clear();
    }

    /// Names the module an address is in, e.g. "nvoglv64.dll".
    std::string ModuleOf(u64 address) {
        HMODULE module{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(address), &module)) {
            return "unknown";
        }
        return ModuleName(module);
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
    u64 exe_begin{};
    u64 exe_end{};
    u8* stack_buffer{};
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
