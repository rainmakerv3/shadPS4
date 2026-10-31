// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

/// The command buffer of a session a command goes to.
enum class CommandTarget : u32 {
    Primary,
    Upload,
};

/// The command buffers of the session a thread is recording.
struct RecordingContext {
    vk::CommandBuffer primary{};
    vk::CommandBuffer upload{};

    [[nodiscard]] vk::CommandBuffer Get(CommandTarget target) const noexcept {
        return target == CommandTarget::Upload ? upload : primary;
    }
};

/// Commands one thread records for another to replay into Vulkan command buffers, so the time the
/// driver takes to record them is spent on that thread. The GPU thread limits the frame rate in
/// heavy scenes, and a fifth of its time went to the driver.
///
/// Each command is a closure called with the replaying thread's recording context. Data a command
/// points to is copied right before it, into the same block of memory, and blocks go back to be
/// reused once the replaying thread is past them. The replaying thread catches up on a batch of
/// commands at a time, or right away on a flush, rather than following each one: reading what the
/// recording thread has just written takes the memory back and forth between their cores.
class CommandStream {
public:
    CommandStream();
    ~CommandStream();

    CommandStream(const CommandStream&) = delete;
    CommandStream& operator=(const CommandStream&) = delete;

    static constexpr size_t Alignment = 16;
    /// Room a command takes besides the data copied for it, at most.
    static constexpr size_t MaxCommandSize = 256;

    /// Room data of this many bytes takes in the stream.
    [[nodiscard]] static constexpr size_t DataSize(size_t bytes) noexcept {
        return bytes == 0 ? 0 : PacketSize(bytes);
    }

    /// Makes sure the next this many bytes of the stream are in one block, so data copied for a
    /// command and the command go together.
    void Reserve(size_t bytes) {
        if (static_cast<size_t>(write_end - write_ptr) < bytes) [[unlikely]] {
            NextBlock(bytes);
        }
    }

    /// Takes room for data the next command points to, which stays where it is until the command
    /// is replayed. Room for both is reserved beforehand.
    [[nodiscard]] u8* AllocateData(size_t bytes) {
        return Allocate(PacketSize(bytes), &Skip);
    }

    /// Copies data the next command points to, as AllocateData does.
    template <typename T>
    [[nodiscard]] T* Copy(const T* data, size_t count) {
        static_assert(std::is_trivially_copyable_v<T> && alignof(T) <= Alignment);
        if (count == 0) {
            return nullptr;
        }
        const size_t bytes = count * sizeof(T);
        u8* const payload = AllocateData(bytes);
        std::memcpy(payload, data, bytes);
        return reinterpret_cast<T*>(payload);
    }

    /// Records a function the replaying thread calls with its recording context.
    template <typename Func>
    void Emit(Func&& func) {
        using Closure = std::decay_t<Func>;
        static_assert(std::is_trivially_copyable_v<Closure> &&
                      std::is_trivially_destructible_v<Closure>);
        static_assert(alignof(Closure) <= Alignment);
        static_assert(PacketSize(sizeof(Closure)) <= MaxCommandSize);
        u8* const payload = Allocate(PacketSize(sizeof(Closure)), &Invoke<Closure>);
        new (payload) Closure(std::forward<Func>(func));
    }

    /// Lets the replaying thread see what was recorded so far. With `flush`, it replays all of it
    /// right away, waking up if it has to. Otherwise it gets to it with the next batch.
    void Publish(bool flush);

    /// Replays everything published, returning whether there was anything.
    bool Replay(RecordingContext& context);

    /// Whether the replaying thread should go on replaying rather than wait: a flush asks for
    /// what was published, or a batch of it piled up.
    [[nodiscard]] bool HasWork() const noexcept;

    /// Waits until a flush or a batch of commands, or a short while.
    void WaitForWork(std::stop_token token);

    /// Wakes the replaying thread.
    void Wake();

private:
    using ReplayFunc = void (*)(void* payload, RecordingContext& context);

    struct alignas(Alignment) Header {
        /// Null where a block ends, with the next block's address after the header.
        ReplayFunc func;
        size_t size;
    };
    static_assert(sizeof(Header) == Alignment);

    struct alignas(Alignment) BlockHeader {
        size_t capacity;
    };

    static constexpr size_t BlockSize = 1_MB;
    static constexpr size_t MarkerSize = sizeof(Header) + Alignment;
    static constexpr size_t MaxFreeBlocks = 32;
    /// Published bytes the replaying thread waits for before it catches up on them. Waking it
    /// for every command would cost more than recording them. A draw takes about half a kilobyte,
    /// so a batch is a couple hundred draws, a tenth of a millisecond of driver time, which a
    /// flush may have to wait for too.
    static constexpr u64 BatchBytes = 128_KB;
    static constexpr auto MaxWait = std::chrono::milliseconds{2};

    [[nodiscard]] static constexpr size_t PacketSize(size_t payload) noexcept {
        return sizeof(Header) + ((payload + Alignment - 1) & ~(Alignment - 1));
    }

    template <typename Closure>
    static void Invoke(void* payload, RecordingContext& context) {
        (*static_cast<Closure*>(payload))(context);
    }

    static void Skip(void*, RecordingContext&) {}

    u8* Allocate(size_t size, ReplayFunc func) {
        Reserve(size);
        auto* const header = reinterpret_cast<Header*>(write_ptr);
        header->func = func;
        header->size = size;
        u8* const payload = write_ptr + sizeof(Header);
        write_ptr += size;
        written_bytes += size;
        return payload;
    }

    /// Ends the block written and goes on in another with room for this many bytes.
    void NextBlock(size_t bytes);

    u8* AcquireBlock(size_t capacity);
    void ReleaseBlock(u8* block);

    /// Written by the recording thread only.
    u8* write_ptr{};
    u8* write_end{};
    u64 written_bytes{};
    /// The wait of the replaying thread it was last woken from.
    u64 woken_wait{~0ULL};

    /// Read by the replaying thread only.
    u8* read_ptr{};
    u8* read_block{};

    /// Written by the recording thread.
    alignas(64) std::atomic<u8*> published{};
    std::atomic<u64> published_bytes{};
    /// Bytes recorded when the last flush was published.
    std::atomic<u64> flush_bytes{};

    /// Written by the replaying thread.
    alignas(64) std::atomic<u64> replayed_bytes{};
    std::atomic<bool> waiting{};
    /// Counts the times it started waiting.
    std::atomic<u64> waits{};

    alignas(64) std::mutex wake_mutex;
    std::condition_variable wake_cv;

    std::mutex free_mutex;
    std::vector<u8*> free_blocks;
};

} // namespace Vulkan
