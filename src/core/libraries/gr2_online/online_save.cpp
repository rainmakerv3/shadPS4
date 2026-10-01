// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "core/libraries/gr2_online/online_save.h"
#include "core/libraries/libs.h"
#include "core/libraries/save_data/savedata.h"
#include "core/libraries/save_data/savedata_error.h"

namespace Libraries::Gr2Online::Save {

namespace {

// Gravity Rush 2 keeps every item the server put on its Announcements screen (challenges,
// treasure hints, results, photo reviews, notices) under News in system0020.bin and restores
// them at the next boot. A restored card never asks for the profile of its sender again, so it
// shows no avatar, and the kept set only grows: an item leaves it when the player completes it,
// not when the server withdraws it. The server lists what the player has on every poll, so the
// kept items are cleared when the save is mounted and the session fills the screen again.
//
// The file is a tree: a 16-byte header ("ggdL", a format number, the file size, the entry count),
// then one 16-byte entry per node {u32 name offset, u32 (value offset << 4) | type, u32 n,
// u32 name hash}, then the names, each followed by the bytes of its value. Entries are in
// depth-first order, so the descendants of an object are the entries right after it. Values are
// rewritten in place and never resized, so every offset and the size in the header stay valid.
constexpr u64 HeaderSize = 16;
constexpr u64 EntrySize = 16;
constexpr u32 TypeObject = 0x8; // n is the number of children; no bytes of its own
constexpr u32 TypeScalar = 0x9; // n is the value
constexpr u32 TypeString = 0xB; // n is the length of the bytes at the value offset

// A string that may fill its field with no terminator.
std::string_view Text(const char* text, u64 max) {
    return {text, strnlen(text, max)};
}

// What a slot the game never used holds in a field of this name. A cleared slot gets the same
// text, so the game cannot tell the two apart.
std::string_view UnusedSlotText(std::string_view field) {
    if (field == "onlineID" || field == "missionID") {
        return "None";
    }
    if (field == "last_update") {
        return "yyyymmddhhmmss";
    }
    if (field == "limit" || field == "scoreId" || field == "get_time" || field == "recv_time") {
        return "0";
    }
    return "";
}

void ClearServedNews(const std::filesystem::path& file) {
    std::error_code ec;
    const u64 size = std::filesystem::file_size(file, ec);
    if (ec || size < HeaderSize) {
        return;
    }
    std::vector<char> data(size);
    {
        std::ifstream in{file, std::ios::binary};
        if (!in.read(data.data(), static_cast<std::streamsize>(size))) {
            return;
        }
    }
    if (std::memcmp(data.data(), "ggdL", 4) != 0) {
        return;
    }
    u32 count = 0;
    std::memcpy(&count, data.data() + 12, 4);
    const u64 entries_end = HeaderSize + count * EntrySize;
    if (entries_end > data.size()) {
        return;
    }

    const auto entry_at = [&](u64 i, u32& name_off, u32& value_off, u32& type, u32& n) {
        const u64 off = HeaderSize + i * EntrySize;
        u32 packed = 0;
        std::memcpy(&name_off, data.data() + off, 4);
        std::memcpy(&packed, data.data() + off + 4, 4);
        std::memcpy(&n, data.data() + off + 8, 4);
        value_off = packed >> 4;
        type = packed & 0xF;
    };
    const auto name_of = [&](u32 name_off) -> std::string_view {
        if (name_off >= data.size()) {
            return {};
        }
        return Text(data.data() + name_off, data.size() - name_off);
    };

    u64 news = count;
    for (u64 i = 0; i < count; ++i) {
        u32 name_off = 0, value_off = 0, type = 0, n = 0;
        entry_at(i, name_off, value_off, type, n);
        if (type == TypeObject && name_of(name_off) == "News") {
            news = i;
            break;
        }
    }
    if (news == count) {
        return;
    }

    // Walks the entries below News: every object met adds its children to the number still to
    // come, so the walk stops at the end of the subtree.
    u32 cleared = 0;
    u64 outstanding = 1;
    for (u64 i = news; i < count && outstanding > 0; ++i) {
        u32 name_off = 0, value_off = 0, type = 0, n = 0;
        entry_at(i, name_off, value_off, type, n);
        --outstanding;
        if (type == TypeObject) {
            outstanding += n;
            continue;
        }
        if (type == TypeScalar) {
            if (n != 0) {
                const u32 zero = 0;
                std::memcpy(data.data() + HeaderSize + i * EntrySize + 8, &zero, 4);
                ++cleared;
            }
            continue;
        }
        // Both numbers come from the file, so the sum is made in 64 bits.
        if (type != TypeString || n == 0 || static_cast<u64>(value_off) + n > data.size()) {
            continue;
        }
        // The text of an unused slot goes into the field and the rest is zeroed, so the field
        // keeps its length. A field too short for that text is emptied.
        std::string_view want = UnusedSlotText(name_of(name_off));
        if (want.size() + 1 > n) {
            want = "";
        }
        char* dst = data.data() + value_off;
        if (Text(dst, n) == want) {
            continue;
        }
        std::memset(dst, 0, n);
        std::memcpy(dst, want.data(), want.size());
        ++cleared;
    }
    if (cleared == 0) {
        return;
    }
    std::fstream out{file, std::ios::in | std::ios::out | std::ios::binary};
    if (!out || !out.write(data.data(), static_cast<std::streamsize>(data.size())) ||
        !out.flush()) {
        LOG_ERROR(Lib_SaveData, "Gravity Rush 2: could not rewrite {}",
                  Common::FS::PathToUTF8String(file));
        return;
    }
    LOG_INFO(Lib_SaveData, "Gravity Rush 2: cleared {} served News field(s) in system0020.bin",
             cleared);
}

// What the wrapper reads of the two parameter blocks, as the game lays them out: the emulator's
// header only names their types.
constexpr u64 DirNameSize = 32;
constexpr u64 MountPointSize = 16;

struct MountParam {
    s32 user_id;
    s32 : 32;
    const char* dir_name; // char[DirNameSize]
};
static_assert(offsetof(MountParam, dir_name) == 8);

struct MountResult {
    char mount_point[MountPointSize];
};

} // Anonymous namespace

// The mount itself is the emulator's. The file is cleared before this returns, so the game cannot
// have read it yet.
SaveData::Error PS4_SYSV_ABI sceSaveDataMount2(const SaveData::OrbisSaveDataMount2* mount,
                                               SaveData::OrbisSaveDataMountResult* mount_result) {
    const SaveData::Error ret = SaveData::sceSaveDataMount2(mount, mount_result);
    if (ret != SaveData::Error::OK || !mount || !mount_result) {
        return ret;
    }
    const char* dir_name = reinterpret_cast<const MountParam*>(mount)->dir_name;
    if (!dir_name || Text(dir_name, DirNameSize) != "system0020") {
        return ret;
    }
    const auto* result = reinterpret_cast<const MountResult*>(mount_result);
    std::string guest_path{Text(result->mount_point, MountPointSize)};
    guest_path += "/system0020.bin";
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    if (const auto file = mnt->GetHostPath(guest_path); !file.empty()) {
        ClearServedNews(file);
    }
    return ret;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("0z45PIH+SNI", "libSceSaveData", 1, "libSceSaveData", sceSaveDataMount2);
}

} // namespace Libraries::Gr2Online::Save
