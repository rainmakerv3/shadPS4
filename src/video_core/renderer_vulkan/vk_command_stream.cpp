// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "video_core/renderer_vulkan/vk_command_stream.h"

namespace Vulkan {

CommandStream::CommandStream() {
    u8* const block = AcquireBlock(BlockSize);
    write_ptr = block + sizeof(BlockHeader);
    write_end = block + BlockSize - MarkerSize;
    read_block = block;
    read_ptr = write_ptr;
    published.store(write_ptr, std::memory_order_relaxed);
}

CommandStream::~CommandStream() {
    // The replaying thread is gone by now. What it didn't replay is in the blocks from the one it
    // read last to the one written last.
    const auto free_memory = [](u8* block) {
        ::operator delete(block, std::align_val_t{Alignment});
    };
    u8* block = read_block;
    u8* ptr = read_ptr;
    while (ptr != write_ptr) {
        const auto* const header = reinterpret_cast<const Header*>(ptr);
        if (header->func) {
            ptr += header->size;
            continue;
        }
        u8* next;
        std::memcpy(&next, ptr + sizeof(Header), sizeof(next));
        free_memory(block);
        block = next;
        ptr = next + sizeof(BlockHeader);
    }
    free_memory(block);
    for (u8* const free_block : free_blocks) {
        free_memory(free_block);
    }
}

void CommandStream::Publish(bool flush) {
    published_bytes.store(written_bytes, std::memory_order_relaxed);
    if (flush) {
        // Sequentially consistent on both sides of the check whether the replaying thread waits,
        // so that it either sees the flush before it waits or this sees it waiting.
        flush_bytes.store(written_bytes, std::memory_order_seq_cst);
        published.store(write_ptr, std::memory_order_seq_cst);
        if (waiting.load(std::memory_order_seq_cst)) {
            woken_wait = waits.load(std::memory_order_relaxed);
            Wake();
        }
        return;
    }
    published.store(write_ptr, std::memory_order_release);
    // A batch that piled up wakes the replaying thread once for its wait. Missing that it just
    // started waiting only puts the batch off to the next publish, or the end of the wait.
    if (!waiting.load(std::memory_order_relaxed)) {
        return;
    }
    const u64 wait = waits.load(std::memory_order_relaxed);
    if (wait != woken_wait &&
        written_bytes - replayed_bytes.load(std::memory_order_relaxed) >= BatchBytes) {
        woken_wait = wait;
        Wake();
    }
}

bool CommandStream::Replay(RecordingContext& context) {
    u8* const end = published.load(std::memory_order_acquire);
    if (read_ptr == end) {
        return false;
    }
    u64 replayed = replayed_bytes.load(std::memory_order_relaxed);
    while (read_ptr != end) {
        const auto* const header = reinterpret_cast<const Header*>(read_ptr);
        if (!header->func) [[unlikely]] {
            u8* next;
            std::memcpy(&next, read_ptr + sizeof(Header), sizeof(next));
            ReleaseBlock(read_block);
            read_block = next;
            read_ptr = next + sizeof(BlockHeader);
            continue;
        }
        header->func(read_ptr + sizeof(Header), context);
        read_ptr += header->size;
        replayed += header->size;
    }
    replayed_bytes.store(replayed, std::memory_order_relaxed);
    return true;
}

bool CommandStream::HasWork() const noexcept {
    const u64 replayed = replayed_bytes.load(std::memory_order_relaxed);
    return flush_bytes.load(std::memory_order_seq_cst) > replayed ||
           published_bytes.load(std::memory_order_seq_cst) - replayed >= BatchBytes;
}

void CommandStream::WaitForWork(std::stop_token token) {
    std::unique_lock lock{wake_mutex};
    waits.store(waits.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    waiting.store(true, std::memory_order_seq_cst);
    if (!HasWork() && !token.stop_requested()) {
        wake_cv.wait_for(lock, MaxWait);
    }
    waiting.store(false, std::memory_order_relaxed);
}

void CommandStream::Wake() {
    std::scoped_lock lock{wake_mutex};
    wake_cv.notify_one();
}

void CommandStream::NextBlock(size_t bytes) {
    const size_t capacity = std::max(BlockSize, sizeof(BlockHeader) + bytes + MarkerSize);
    u8* const block = AcquireBlock(capacity);
    // The end of a block always has room for the marker that leads to the next.
    auto* const marker = reinterpret_cast<Header*>(write_ptr);
    marker->func = nullptr;
    marker->size = 0;
    std::memcpy(write_ptr + sizeof(Header), &block, sizeof(block));
    write_ptr = block + sizeof(BlockHeader);
    write_end = block + capacity - MarkerSize;
}

u8* CommandStream::AcquireBlock(size_t capacity) {
    if (capacity == BlockSize) {
        std::scoped_lock lock{free_mutex};
        if (!free_blocks.empty()) {
            u8* const block = free_blocks.back();
            free_blocks.pop_back();
            return block;
        }
    }
    auto* const block = static_cast<u8*>(::operator new(capacity, std::align_val_t{Alignment}));
    new (block) BlockHeader{capacity};
    return block;
}

void CommandStream::ReleaseBlock(u8* block) {
    if (reinterpret_cast<const BlockHeader*>(block)->capacity == BlockSize) {
        std::scoped_lock lock{free_mutex};
        if (free_blocks.size() < MaxFreeBlocks) {
            free_blocks.push_back(block);
            return;
        }
    }
    ::operator delete(block, std::align_val_t{Alignment});
}

} // namespace Vulkan
