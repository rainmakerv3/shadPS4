// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

/// Statistical profiler for emulator threads. A background thread interrupts the registered
/// threads about a thousand times a second, walks their stacks, and every half minute logs the
/// functions they were found in most often, as module offsets. Those are matched to function
/// names with the linker map of the same build. Only implemented on Windows.
namespace Common::Perf {

/// Starts sampling the calling thread.
void SampleCurrentThread(std::string_view name);

/// Stops sampling the calling thread.
void StopSamplingCurrentThread();

} // namespace Common::Perf
