// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

#include "common/types.h"

/// Lightweight accounting of where time goes when frames run long. Hot paths record how long they
/// held up their thread, and every flip checks the frame time: frames that take much longer than
/// usual are logged with what was recorded during them, and a summary is logged every few seconds.
namespace Common::Perf {

enum class Stall : u32 {
    ShaderTranslate, ///< Translating guest shaders to SPIR-V on the GPU thread.
    PipelineCreate,  ///< Driver pipeline compilation outside the compiler threads.
    PipelineWait,    ///< Waiting for a compiler thread to finish a pipeline.
    GpuWait,         ///< Waiting for the host GPU to finish submitted work.
    TextureUpload,   ///< Uploading and detiling guest textures.
    TextureEvict,    ///< Textures dropped from the texture cache to save memory.
    BufferUpload,    ///< Copying guest memory into GPU buffers.
    BufferDownload,  ///< Reading GPU written buffers back to guest memory.
    Residency,       ///< Making buffer memory resident.
    SparseBind,      ///< Binding memory to sparse buffers in the driver.
    PageFault,       ///< Handling faults on GPU tracked guest memory.
    GpuThread,       ///< GPU command processor busy processing guest commands.
    Present,         ///< Presenting a frame to the swapchain.
    Count,
};

/// Adds time spent in a category, plus an optional amount of data it handled.
void Record(Stall stall, u64 nanoseconds, u64 bytes = 0);

/// Called once per presented guest frame.
void OnFlip();

class ScopedStall {
public:
    explicit ScopedStall(Stall stall_, u64 bytes_ = 0) noexcept
        : stall{stall_}, bytes{bytes_}, start{std::chrono::steady_clock::now()} {}

    ~ScopedStall() {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        Record(stall, std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(), bytes);
    }

    void AddBytes(u64 amount) noexcept {
        bytes += amount;
    }

    /// Records the time so far once it adds up to at least min_elapsed, so a scope that lasts
    /// several frames is accounted to the frames it spans.
    void Checkpoint(std::chrono::nanoseconds min_elapsed) noexcept {
        const auto now = std::chrono::steady_clock::now();
        if (now - start < min_elapsed) {
            return;
        }
        Record(stall, std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count(),
               bytes);
        bytes = 0;
        start = now;
    }

    ScopedStall(const ScopedStall&) = delete;
    ScopedStall& operator=(const ScopedStall&) = delete;

private:
    Stall stall;
    u64 bytes;
    std::chrono::steady_clock::time_point start;
};

} // namespace Common::Perf
