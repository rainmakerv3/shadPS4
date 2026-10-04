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
    ReadbackWait,    ///< Game threads waiting for the GPU to copy back memory they read.
    Residency,       ///< Making buffer memory resident.
    SparseBind,      ///< Binding memory to sparse buffers in the driver.
    PageFault,       ///< Handling faults on GPU tracked guest memory.
    PageProtect,     ///< Changing the protection of GPU tracked guest memory, game threads.
    PageProtectGpu,  ///< The same on the GPU thread, which limits the frame rate.
    DmaSync,         ///< Uploading all memory the CPU wrote, for shaders reading memory freely.
    GpuThread,       ///< GPU command processor busy processing guest commands.
    GuestWait,       ///< GPU command processor waiting on the game for a graphics command.
    Present,         ///< Presenting a frame to the swapchain.
    FrameWait,       ///< Waiting for a presentation frame to be free to draw the next one into.
    Count,
};

/// Events counted per frame.
enum class Counter : u32 {
    Draws,
    Dispatches,
    ShaderLookups,           ///< Lookups of a known program's permutation.
    ShaderLookupsRemembered, ///< Those found from a recent lookup with the same inputs.
    SmallBuffers,            ///< Small read-only storage buffers bound.
    SmallBuffersInPlace,     ///< Those bound from the GPU copy of the memory, without a copy.
    Submits,                 ///< Command buffers submitted to the host GPU.
    Barriers,                ///< Pipeline barriers between guest commands.
    RenderPasses,            ///< Render passes begun.
    BufferUploads,           ///< Copies of memory the CPU wrote into GPU buffers.
    BufferUploadsAhead,      ///< Those recorded ahead of the command buffer they were made in.
    RewrittenBuffersCopied,  ///< Buffers the game keeps writing, copied for a draw.
    DmaSyncs,                ///< Uploads of all memory the CPU wrote, for shaders reading freely.
    WriteFaults,             ///< Game thread writes to memory the GPU has a copy of.
    WriteFaultsRepeated,     ///< Those on pages that faulted in the same or the previous frame.
    WriteFaultsFollowing,    ///< Those on the page after one that faulted in either frame.
    ReadFaults,              ///< Game thread reads of memory the GPU wrote.
    Count,
};

/// Adds time spent in a category, plus an optional amount of data it handled.
void Record(Stall stall, u64 nanoseconds, u64 bytes = 0);

/// Counts events of a kind.
void Count(Counter counter, u64 amount = 1);

/// Called once per presented guest frame.
void OnFlip();

/// Number of the guest frame being made, counted from the first flip.
u64 FrameNumber();

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
