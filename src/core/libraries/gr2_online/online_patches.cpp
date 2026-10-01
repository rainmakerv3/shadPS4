// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/types.h"
#include "core/libraries/gr2_online/online_host.h"
#include "core/libraries/gr2_online/online_patches.h"
#include "core/linker.h"
#include "core/tls.h"

// The only platform code of this folder. Gives the host pages that hold [addr, addr + size) read
// and write access, and execute access too when asked. False when the host refused.
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static bool HostProtect(u64 addr, u64 size, bool execute) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const u64 page = info.dwPageSize;
#else
    const u64 page = static_cast<u64>(sysconf(_SC_PAGESIZE));
#endif
    const u64 begin = addr & ~(page - 1);
    const u64 end = (addr + size + page - 1) & ~(page - 1);
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(reinterpret_cast<void*>(begin), end - begin,
                          execute ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old) != 0;
#else
    return mprotect(reinterpret_cast<void*>(begin), end - begin,
                    PROT_READ | PROT_WRITE | (execute ? PROT_EXEC : 0)) == 0;
#endif
}

namespace Libraries::Gr2Online::Patches {

namespace {

// The addresses in this file are those of version 01.11 of the executable, in a listing that
// starts at ListingBase. Before a site is written it is compared with the bytes that version
// holds there, so any other version of the game is left as it is.
constexpr u64 ListingBase = 0x107BF0;

constexpr u8 Nop = 0x90;

// Registers, by the number their encodings take.
constexpr u8 Rax = 0;
constexpr u8 Rcx = 1;
constexpr u8 Rsi = 6;

// A site that gets fixed bytes.
struct Site {
    std::string_view what;
    u64 va;
    std::span<const u8> expect;
    std::span<const u8> bytes;
};

// The start of the function, up to the load of the stack guard: push rbp; mov rbp, rsp;
// push r15; push r14; push rbx; sub rsp, 0x18; mov r15, [rip + 0x987a14]
constexpr u8 GateProlog[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48,
                             0x83, 0xec, 0x18, 0x4c, 0x8b, 0x3d, 0x14, 0x7a, 0x98, 0x00};
// mov eax, 1; ret
constexpr u8 GateOpen[] = {0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};
// The two calls of the import that frees, each with its own distance to it.
constexpr u8 GhostNameFree[] = {0xe8, 0x84, 0xaf, 0x55, 0x00};
constexpr u8 NoticeFree[] = {0xe8, 0xf6, 0x16, 0x51, 0x00};
constexpr u8 Nop5[] = {Nop, Nop, Nop, Nop, Nop};
constexpr u8 MovR15Rax[] = {0x49, 0x89, 0xc7};
constexpr u8 Nop3[] = {Nop, Nop, Nop};

constexpr Site Sites[] = {
    // The game draws a category of served content only when this function says that the
    // category is switched on, and the switches come from a table that stays empty here: mines
    // and every category of the Announcements screen would stay hidden. It answers 1.
    {"render gate", 0xe827a0, GateProlog, GateOpen},
    // A deferred teardown of the challenge screen frees the name of the challenger's ghost,
    // which was freed before and whose chunk was handed out again in between, and the C library
    // of the game aborts on that. The call goes; the instruction after it still clears the
    // pointer, and the short string is leaked.
    {"challenger ghost name free", 0xfa2317, GhostNameFree, Nop5},
    // The Announcements view keeps a pointer to a served answer it does not own. The network
    // layer frees the answer first, and the teardown of the view frees it again, which aborts
    // when Announcements is opened. The second free goes; the instruction after it still clears
    // the pointer.
    {"notice envelope free", 0xfebba5, NoticeFree, Nop5},
    // A served challenge makes the game put a figure that replays the ghost at the marker of
    // the challenge in the overworld. The script spawns it when the lookup of the ghost to show
    // returns a handle. The instruction that takes the handle (mov r15, rax) goes, so r15 keeps
    // its zero and there is no figure. The Challenges list and the ghost inside the race do not
    // use this lookup.
    {"overworld challenge puppet", 0xe99759, MovR15Rax, Nop3},
};

// The player's name as the two screens below read it. Apply keeps both current.
char race_name[17]{};
char treasure_name[20]{};

// Code the rerouted sites jump to: one stub of at most 28 bytes each, 32 bytes apart.
alignas(16) u8 race_stub[32];
alignas(16) u8 treasure_stub[64];

// A site that is rerouted through a stub, so that a register holds the address of the player's
// name when the game goes on. The site becomes "movabs via, stub; jmp via", filled up with nop.
// The stub repeats the instructions that made room for the jump, loads the name register and
// jumps to the target through rax, which is free at all three sites.
struct NameSite {
    std::string_view what;
    u64 va;
    std::span<const u8> expect;
    u64 size;                   // bytes replaced at the site
    u8 via;                     // register that carries the address of the stub
    std::span<const u8> replay; // instructions the stub repeats
    u8 name_reg;                // register the game takes the name from
    const char* name;           // one of the two buffers above
    u64 target;                 // where the stub goes on
    u8* stub;                   // 32 bytes of a stub buffer
};

constexpr u8 RaceTail[] = {0x5b, 0x41, 0x5e, 0x5d,       // pop rbx; pop r14; pop rbp
                           0xe9, 0x75, 0xa6, 0x62, 0x00, // jmp strncpy
                           0x0f, 0x1f, 0x44};            // 3 of the 5 bytes of a nop
constexpr u8 RacePops[] = {0x5b, 0x41, 0x5e, 0x5d};
constexpr u8 TreasureLoad0[] = {0x48, 0x8d, 0x0d, 0x55, 0x1e, 0xb6, 0x00, // lea rcx, [name]
                                0x48, 0x0f, 0x45, 0xd8,                   // cmovne rbx, rax
                                0x31, 0xd2};                              // xor edx, edx
constexpr u8 TreasureReplay0[] = {0x48, 0x0f, 0x45, 0xd8, 0x31, 0xd2};
constexpr u8 TreasureLoad1[] = {0x48, 0x8d, 0x0d, 0x5a, 0x11, 0xb6, 0x00, // lea rcx, [name]
                                0x31, 0xd2,                               // xor edx, edx
                                0x48, 0x89, 0xdf};                        // mov rdi, rbx
constexpr u8 TreasureReplay1[] = {0x31, 0xd2, 0x48, 0x89, 0xdf};

constexpr NameSite NameSites[] = {
    // The row of the player in the rank list of a challenge race takes its name from a buffer
    // that the game fills only while it handles a login event. That is over long before the
    // race makes the row, so the row shows whatever the buffer held. The function ends, for
    // this row only, in a tail call of strncpy(row text, name, 16); the stub gives it the
    // player's name in rsi. The rows of the rivals do not pass here. The jump takes the 9 bytes
    // of the tail and 3 of the 5 padding bytes that follow it.
    {"own name in the race list", 0xed2ca2, RaceTail, 12, Rax, RacePops, Rsi, race_name, 0x14fd320,
     race_stub},
    // The dialog that confirms sending a treasure hint builds its name label from a global
    // Online ID that holds the poster of the last hint received, not the player. These are the
    // two places that build the label for this dialog and for no other screen; both load the
    // address of that global into rcx, and the stub loads the player's name instead. The cmovne
    // of the first site is repeated before anything changes the flags it reads.
    {"own name on the treasure send dialog, site 0", 0x109468c, TreasureLoad0, 13, Rcx,
     TreasureReplay0, Rcx, treasure_name, 0x1094699, treasure_stub},
    {"own name on the treasure send dialog, site 1", 0x1095387, TreasureLoad1, 12, Rcx,
     TreasureReplay1, Rcx, treasure_name, 0x1095393, treasure_stub + 32},
};

// The game frees through one import of its C library, sceLibcMspaceFree(heap, pointer). Served
// content makes deferred teardowns free chunks that were freed before and handed out again in
// between, at more places than can be patched one by one, and the C library aborts on such a
// free. It fills a freed chunk, its header included, with 0xaf bytes. The guard takes the place
// of the import: a chunk that holds eight of those bytes at its header or at its start is free
// already and is skipped, which leaks it; every other free is passed on. If the header is not
// where this assumes, the guard skips nothing.
constexpr u64 FreeSlot = 0x180c8a0;
constexpr u64 FreedFill = 0xafafafafafafafafULL;

using MspaceFree = PS4_SYSV_ABI void (*)(void* mspace, void* ptr);
std::atomic<MspaceFree> libc_free{};
std::atomic<u64> frees_skipped{};

void PS4_SYSV_ABI FreeGuard(void* mspace, void* ptr) {
    const MspaceFree next = libc_free.load();
    if (!next) {
        return;
    }
    if (ptr) {
        const u64 header = *reinterpret_cast<volatile u64*>(static_cast<u8*>(ptr) - 0x10);
        const u64 first = *reinterpret_cast<volatile u64*>(ptr);
        if (header == FreedFill || first == FreedFill) {
            if (const u64 n = ++frees_skipped; n == 1 || (n & 0xff) == 0) {
                LOG_WARNING(Lib_Http,
                            "Gravity Rush 2: skipped the free of a freed chunk, {} so far", n);
            }
            return;
        }
    }
    next(mspace, ptr);
}

std::mutex mutex;
bool sites_done{};
bool guard_done{};

bool Holds(u64 addr, std::span<const u8> bytes) {
    const auto* site = reinterpret_cast<const volatile u8*>(addr);
    for (u64 i = 0; i < bytes.size(); ++i) {
        if (site[i] != bytes[i]) {
            return false;
        }
    }
    return true;
}

// Replaces the bytes of a site that holds what is expected there, reads them back and logs
// what became of it.
void Write(std::string_view what, u64 addr, std::span<const u8> expect, std::span<const u8> bytes) {
    if (!Holds(addr, expect)) {
        LOG_ERROR(Lib_Http, "Gravity Rush 2: {} at {:#x}: MISMATCH, left alone", what, addr);
        return;
    }
    // The emulator maps the pages of a module writable, so a refusal here decides nothing: the
    // bytes are written in any case and the read-back tells.
    const bool allowed = HostProtect(addr, bytes.size(), true);
    auto* site = reinterpret_cast<volatile u8*>(addr);
    for (u64 i = 0; i < bytes.size(); ++i) {
        site[i] = bytes[i];
    }
    const char* note = allowed ? "" : " (the host refused the protection change)";
    if (Holds(addr, bytes)) {
        LOG_INFO(Lib_Http, "Gravity Rush 2: {} at {:#x}: PATCHED{}", what, addr, note);
    } else {
        LOG_ERROR(Lib_Http, "Gravity Rush 2: {} at {:#x}: FAILED, the bytes did not stay{}", what,
                  addr, note);
    }
}

// movabs reg, value
u8* MovAbs(u8* out, u8 reg, u64 value) {
    *out++ = 0x48;
    *out++ = static_cast<u8>(0xb8 + reg);
    std::memcpy(out, &value, sizeof(value));
    return out + sizeof(value);
}

// jmp reg
u8* Jmp(u8* out, u8 reg) {
    *out++ = 0xff;
    *out++ = static_cast<u8>(0xe0 + reg);
    return out;
}

void Reroute(const NameSite& site, u64 base) {
    const u64 addr = base + (site.va - ListingBase);
    if (Holds(addr, site.expect)) {
        u8* out = std::copy(site.replay.begin(), site.replay.end(), site.stub);
        out = MovAbs(out, site.name_reg, reinterpret_cast<u64>(site.name));
        out = MovAbs(out, Rax, base + (site.target - ListingBase));
        out = Jmp(out, Rax);
        // The stub is emulator data. A jump into a page that must not be executed faults, so
        // without the permission the site stays as it is.
        if (!HostProtect(reinterpret_cast<u64>(site.stub), static_cast<u64>(out - site.stub),
                         true)) {
            LOG_ERROR(Lib_Http, "Gravity Rush 2: {} at {:#x}: FAILED, its stub may not be executed",
                      site.what, addr);
            return;
        }
    }
    std::array<u8, 13> jump;
    jump.fill(Nop);
    Jmp(MovAbs(jump.data(), site.via, reinterpret_cast<u64>(site.stub)), site.via);
    Write(site.what, addr, site.expect, std::span<const u8>{jump.data(), site.size});
}

// True when there is nothing left to do. The slot is read once. The linker does not write a
// resolved slot again when later modules load, so the guard stays in place.
bool InstallFreeGuard(u64 base) {
    const u64 slot = base + (FreeSlot - ListingBase);
    const u64 guard = reinterpret_cast<u64>(HOST_CALL(FreeGuard));
    const u64 current = *reinterpret_cast<volatile u64*>(slot);
    if (current == guard) {
        // Never taken over as the function to pass on to: every free would then call itself.
        return true;
    }
    if (current < 0x10000 || current >= (1ULL << 47)) {
        // Not a pointer, so there is nothing to pass a free on to. The next call looks again.
        return false;
    }
    // The slot is data of the game that may be mapped read-only.
    if (!HostProtect(slot, sizeof(u64), false)) {
        LOG_ERROR(Lib_Http, "Gravity Rush 2: free guard at {:#x}: FAILED, the slot is read-only",
                  slot);
        return true;
    }
    libc_free = reinterpret_cast<MspaceFree>(current);
    *reinterpret_cast<volatile u64*>(slot) = guard;
    if (*reinterpret_cast<volatile u64*>(slot) == guard) {
        LOG_INFO(Lib_Http, "Gravity Rush 2: free guard at {:#x}: PATCHED", slot);
    } else {
        LOG_ERROR(Lib_Http, "Gravity Rush 2: free guard at {:#x}: FAILED, the slot did not stay",
                  slot);
    }
    return true;
}

// The last byte of a buffer is always 0, so a reader finds a terminator at any moment.
void SetName(std::span<char> buffer, std::string_view name) {
    name = name.substr(0, buffer.size() - 1);
    if (std::string_view{buffer.data()} == name) {
        return;
    }
    std::memcpy(buffer.data(), name.data(), name.size());
    std::memset(buffer.data() + name.size(), 0, buffer.size() - name.size());
}

} // Anonymous namespace

void Apply() {
    // The game initialises its HTTP library on two threads within moments of each other.
    std::scoped_lock lk{mutex};
    const std::string name = Host::PlayerName();
    SetName(race_name, name);
    SetName(treasure_name, name);
    if (sites_done && guard_done) {
        return;
    }
    const auto* module = Common::Singleton<Core::Linker>::Instance()->GetModule(0);
    if (!module) {
        return;
    }
    const u64 base = module->GetBaseAddress();
    if (!sites_done) {
        // The two longest sites hold addresses of this one build. Any other build is left as
        // it is, the import slot included.
        if (!Holds(base + (NameSites[1].va - ListingBase), NameSites[1].expect) ||
            !Holds(base + (NameSites[2].va - ListingBase), NameSites[2].expect)) {
            LOG_ERROR(Lib_Http, "Gravity Rush 2: not version 01.11, the executable is left alone");
            sites_done = guard_done = true;
            return;
        }
        for (const Site& site : Sites) {
            Write(site.what, base + (site.va - ListingBase), site.expect, site.bytes);
        }
        for (const NameSite& site : NameSites) {
            Reroute(site, base);
        }
        sites_done = true;
    }
    if (!guard_done) {
        guard_done = InstallFreeGuard(base);
    }
}

} // namespace Libraries::Gr2Online::Patches
