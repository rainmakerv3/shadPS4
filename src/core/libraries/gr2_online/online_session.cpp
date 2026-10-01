// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <SDL3/SDL_messagebox.h>
#include <httplib.h>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/libraries/gr2_online/online_host.h"
#include "core/libraries/gr2_online/online_session.h"

namespace Libraries::Gr2Online::Session {

namespace {

constexpr const char* BoxTitle = "Gravity Rush 2 Online";

// The thread that registers the libraries. A sign-in on it is the login the emulator makes at
// boot, before the game runs.
std::thread::id boot_thread;
std::atomic<bool> gate_asked{};
std::atomic<bool> notice_shown{};
std::atomic<bool> work_started{};

// Every request of this file is plain HTTP to the server of the game and carries the headers
// that identify the player. Reading them may wait for the token of a fresh sign-in, so it is done
// on the thread that sends.
httplib::Headers Headers() {
    httplib::Headers headers;
    for (auto& [name, value] : Host::IdentityHeaders()) {
        headers.emplace(std::move(name), std::move(value));
    }
    return headers;
}

void SetTimeouts(httplib::Client& client) {
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);
}

enum class Verdict {
    Allowed,
    Outdated, // 426: this build is older than the server accepts
    Banned,   // 403 with X-GR2-Banned
    Locked,   // 403 with X-GR2-Gate: debug
};

struct Gate {
    Verdict verdict{Verdict::Allowed};
    int ban_days{}; // 0 when the ban has no end
};

// Asks the server whether this build and this account may play. No answer counts as allowed: a
// server that cannot be reached keeps nobody from playing offline.
Gate AskGate() {
    Gate gate;
    const auto& server = Host::GetServer();
    httplib::Client client{server.host, server.port};
    SetTimeouts(client);
    const auto res = client.Get("/clientgate?v=" + std::to_string(Host::OnlineVersion), Headers());
    if (!res) {
        return gate;
    }
    if (res->status == 426) {
        gate.verdict = Verdict::Outdated;
    } else if (res->status == 403) {
        if (const std::string banned = res->get_header_value("X-GR2-Banned"); !banned.empty()) {
            gate.verdict = Verdict::Banned;
            // The days that are left, or "perm". Anything that is not a number is a ban with no
            // end.
            int days = 0;
            for (const char c : banned) {
                if (c < '0' || c > '9') {
                    days = 0;
                    break;
                }
                days = days * 10 + (c - '0');
                if (days > 99999) {
                    break;
                }
            }
            gate.ban_days = days;
        } else if (res->get_header_value("X-GR2-Gate") == "debug") {
            gate.verdict = Verdict::Locked;
        }
    }
    return gate;
}

std::string Describe(const Gate& gate) {
    switch (gate.verdict) {
    case Verdict::Outdated:
        return "Your version of the emulator is outdated, please update to the latest version to "
               "access network services.";
    case Verdict::Banned: {
        std::string text = "You have been banned from the online server.";
        if (gate.ban_days > 0) {
            text += " Time remaining: " + std::to_string(gate.ban_days) +
                    (gate.ban_days == 1 ? " day." : " days.");
        }
        return text + " The game will continue without online features.";
    }
    case Verdict::Locked:
        return "The online server is temporarily locked for maintenance testing. Online features "
               "are unavailable right now; the game will continue offline.";
    default:
        return {};
    }
}

// At boot the answer is a message box, and an outdated build ends there. Once the game runs it is
// a notice and nothing quits: the server turns an outdated build away on every request anyway.
void Show(const Gate& gate, bool at_boot) {
    if (gate.verdict == Verdict::Allowed) {
        return;
    }
    const std::string text = Describe(gate);
    LOG_WARNING(Lib_Http, "Gravity Rush 2: {}", text);
    if (!at_boot) {
        Host::Notify(text);
        return;
    }
    switch (gate.verdict) {
    case Verdict::Outdated:
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, BoxTitle, text.c_str(), nullptr);
        std::quick_exit(0);
    case Verdict::Banned:
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, BoxTitle, text.c_str(), nullptr);
        break;
    default:
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, BoxTitle, text.c_str(), nullptr);
        break;
    }
}

void OnSignIn(s32 user_id, bool signed_in) {
    if (!signed_in || user_id != Host::UserId() || gate_asked.exchange(true)) {
        return;
    }
    LOG_INFO(Lib_Http,
             "Gravity Rush 2: user {} is signed in to shadNet; the trophies of this game are "
             "uploaded on that session",
             user_id);
    if (!Host::Connected()) {
        return;
    }
    if (std::this_thread::get_id() == boot_thread) {
        Show(AskGate(), true);
    } else {
        // A boot login that failed is tried again when the game reads the login event, on a
        // thread of the game. That thread is not kept waiting.
        std::thread([] { Show(AskGate(), false); }).detach();
    }
}

// The server keeps a picture for each player and cannot read the files of the client, so the
// picture of the user, <home>/<user id>/gr2_avatar.png, is sent to it as it is. The server files
// it under X-GR2-Player and scales it itself.
void UploadAvatar(s32 user_id) {
    std::ifstream file{Host::UserHome(user_id) / "gr2_avatar.png", std::ios::binary};
    if (!file) {
        return;
    }
    const std::string body{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (body.empty()) {
        return;
    }
    const httplib::Headers headers = Headers();
    if (!headers.contains("X-GR2-Player")) {
        return;
    }
    const auto& server = Host::GetServer();
    httplib::Client client{server.host, server.port};
    SetTimeouts(client);
    const auto res =
        client.Post("/uploadavatar", headers, body.data(), body.size(), "application/octet-stream");
    LOG_INFO(Lib_Http, "Gravity Rush 2: avatar upload of {} bytes, status {}", body.size(),
             res ? res->status : -1);
}

// Dusty Tokens are handed out by the server alone, so its count is the true one, and a save that
// holds more was edited. The game itself only ever raises the count, so a count that is too high
// has to be corrected in the file, before the game loads it. A count below the server's is left
// alone: the token mail of the server raises it, and the rewards on the way up are given only
// while the save is behind. Writing the higher count here would skip them.
//
// A save slot is data0000.bin or data0001.bin: "ggdL", the number of entries as u32 at 12, then
// 16-byte entries with the value as a float at +8 and the hash of the key at +12.
constexpr u32 DustyTokenKey = 0x8492dbd1;

void CheckDustyTokens(s32 user_id) {
    const httplib::Headers headers = Headers();
    if (!headers.contains("X-GR2-Player")) {
        return;
    }
    const auto& server = Host::GetServer();
    httplib::Client client{server.host, server.port};
    SetTimeouts(client);
    const auto res = client.Get("/gr2token", headers);
    if (!res || res->status != 200) {
        // Without an answer the saves stay as they are.
        return;
    }
    long long total = -1;
    try {
        total = std::stoll(res->body);
    } catch (...) {
        return;
    }
    if (total < 0) {
        return;
    }
    const float want = static_cast<float>(total);

    // Only the saves of the running title: no file of another game is opened.
    const auto saves = Host::UserHome(user_id) / "savedata" / Host::Serial();
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it{saves, ec}, end; !ec && it != end;
         it.increment(ec)) {
        const auto& path = it->path();
        if (!it->is_regular_file(ec) ||
            (path.filename() != "data0000.bin" && path.filename() != "data0001.bin")) {
            continue;
        }
        std::vector<char> data;
        {
            std::ifstream in{path, std::ios::binary};
            if (!in) {
                continue;
            }
            data.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
        }
        if (data.size() < 16 || std::memcmp(data.data(), "ggdL", 4) != 0) {
            continue;
        }
        u32 count = 0;
        std::memcpy(&count, data.data() + 12, 4);
        for (u32 i = 0; i < count; ++i) {
            const u64 off = 16 + static_cast<u64>(i) * 16;
            if (off + 16 > data.size()) {
                break;
            }
            u32 key = 0;
            std::memcpy(&key, data.data() + off + 12, 4);
            if (key != DustyTokenKey) {
                continue;
            }
            float have = 0.0f;
            std::memcpy(&have, data.data() + off + 8, 4);
            const std::string name = Common::FS::PathToUTF8String(path.filename());
            if (have > want) {
                // Only the four bytes of the value are written; the file keeps its size.
                std::fstream out{path, std::ios::in | std::ios::out | std::ios::binary};
                out.seekp(static_cast<std::streamoff>(off + 8));
                out.write(reinterpret_cast<const char*>(&want), sizeof(want));
                out.flush();
                if (out) {
                    LOG_INFO(Lib_Http, "Gravity Rush 2: Dusty Tokens in {} lowered from {} to {}",
                             name, have, want);
                } else {
                    LOG_ERROR(Lib_Http, "Gravity Rush 2: could not write {}", name);
                }
            } else {
                LOG_INFO(Lib_Http, "Gravity Rush 2: Dusty Tokens in {}: {}, the server has {}",
                         name, have, want);
            }
            break;
        }
    }
}

} // Anonymous namespace

void Arm() {
    boot_thread = std::this_thread::get_id();
    Host::OnSignInChange(OnSignIn);
}

void OnRequest() {
    if (!Host::Connected()) {
        return;
    }
    if (!Host::HasIdentity()) {
        if (!notice_shown.exchange(true)) {
            LOG_WARNING(Lib_Http, "Gravity Rush 2: online play needs a shadNet sign-in");
            Host::Notify("Gravity Rush 2 online play needs a shadNet sign-in.");
        }
        return;
    }
    if (work_started.exchange(true)) {
        return;
    }
    const s32 user_id = Host::UserId();
    std::thread(UploadAvatar, user_id).detach();
    std::thread(CheckDustyTokens, user_id).detach();
}

} // namespace Libraries::Gr2Online::Session
