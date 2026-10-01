// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/libraries/gr2_online/online_stat.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/libs.h"
#include "core/linker.h"

namespace Libraries::Gr2Online::Stat {

using Kernel::OrbisKernelStat;

namespace {

using StatFunc = PS4_SYSV_ABI s32 (*)(const char* path, OrbisKernelStat* sb);
using FstatFunc = PS4_SYSV_ABI s32 (*)(s32 fd, OrbisKernelStat* sb);

// The emulator's stat and fstat. Its header does not declare them, so they are taken from the
// table the imports of the game are resolved with. The kernel library fills that table after
// this file is registered, and the table grows while the game loads modules, so it is searched
// at the first call and never again.
std::once_flag looked_up;
StatFunc stat_func{};
FstatFunc fstat_func{};

u64 Find(const char* nid) {
    Core::Loader::SymbolResolver symbol{};
    symbol.name = nid;
    symbol.library = "libkernel";
    symbol.module = "libkernel";
    auto* linker = Common::Singleton<Core::Linker>::Instance();
    const auto* record = linker->GetHLESymbols().FindSymbol(symbol);
    return record ? record->virtual_address : 0;
}

void LookUp() {
    std::call_once(looked_up, [] {
        stat_func = reinterpret_cast<StatFunc>(Find("E6ao34wPw+U"));
        fstat_func = reinterpret_cast<FstatFunc>(Find("mqQMh1zPPT8"));
        if (!stat_func || !fstat_func) {
            LOG_ERROR(Kernel_Fs, "Gravity Rush 2: the emulator's stat or fstat was not found");
        }
    });
}

} // Anonymous namespace

// The guest file system has no symbolic links, so lstat is stat.
s32 PS4_SYSV_ABI posix_lstat(const char* path, OrbisKernelStat* sb) {
    LookUp();
    return stat_func ? stat_func(path, sb) : -1;
}

// The game names files by absolute paths, so the directory and the flags have no say.
s32 PS4_SYSV_ABI posix_fstatat(s32 dirfd, const char* path, OrbisKernelStat* sb, s32 flags) {
    LookUp();
    return stat_func ? stat_func(path, sb) : -1;
}

s32 PS4_SYSV_ABI posix_fstat(s32 fd, OrbisKernelStat* sb) {
    LookUp();
    return fstat_func ? fstat_func(fd, sb) : -1;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("DRGXpDDh8Ng", "libScePosix", 1, "libkernel", posix_lstat);
    LIB_FUNCTION("DRGXpDDh8Ng", "libkernel", 1, "libkernel", posix_lstat);
    LIB_FUNCTION("t6haf4s-eE0", "libScePosix", 1, "libkernel", posix_fstatat);
    LIB_FUNCTION("t6haf4s-eE0", "libkernel", 1, "libkernel", posix_fstatat);
    LIB_FUNCTION("A0O5kF5x4LQ", "libkernel", 1, "libkernel", posix_fstat);
}

} // namespace Libraries::Gr2Online::Stat
