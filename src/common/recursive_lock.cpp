// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include "common/assert.h"
#include "common/recursive_lock.h"

namespace Common::Detail {

namespace {

struct RecursiveLockState {
    void* mutex;
    RecursiveLockType type;
    int count;
};

/// A thread only holds a few of these locks at once, and one is taken on every fault on GPU
/// tracked memory, so they are kept in a small array instead of a map that allocated and freed
/// a node each time, from inside the fault handler.
constexpr size_t MaxHeldLocks = 16;
thread_local std::array<RecursiveLockState, MaxHeldLocks> g_recursive_locks{};
thread_local size_t g_num_recursive_locks{};

RecursiveLockState* FindRecursiveLock(void* mutex) {
    for (size_t i = 0; i < g_num_recursive_locks; ++i) {
        if (g_recursive_locks[i].mutex == mutex) {
            return &g_recursive_locks[i];
        }
    }
    return nullptr;
}

} // Anonymous namespace

bool IncrementRecursiveLock(void* mutex, RecursiveLockType type) {
    auto* state = FindRecursiveLock(mutex);
    if (!state) {
        ASSERT_MSG(g_num_recursive_locks < MaxHeldLocks, "Too many recursive locks held");
        state = &g_recursive_locks[g_num_recursive_locks++];
        *state = {mutex, type, 0};
    }
    ASSERT(state->type == type);
    return state->count++ == 0;
}

bool DecrementRecursiveLock(void* mutex, RecursiveLockType type) {
    auto* state = FindRecursiveLock(mutex);
    ASSERT(state && state->type == type && state->count > 0);
    if (--state->count == 0) {
        *state = g_recursive_locks[--g_num_recursive_locks];
        return true;
    }
    return false;
}

} // namespace Common::Detail
