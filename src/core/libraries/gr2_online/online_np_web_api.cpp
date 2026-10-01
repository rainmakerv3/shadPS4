// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <httplib.h>
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/gr2_online/online_host.h"
#include "core/libraries/gr2_online/online_np_web_api.h"
#include "core/libraries/libs.h"
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_types.h"

// The toolkit of the game looks up the profile of the player, of the players on a leaderboard and
// of a challenger, and the friend list, through libSceNpWebApi. Here each lookup goes to the
// server of the game as /npwebapi/<api group><path>, and the answer is kept until the toolkit has
// read it. The whole library is registered: the request ids are this file's own, and a function
// left to the emulator's implementation would be handed an id it does not know.
namespace Libraries::Gr2Online::NpWebApi {

namespace {

struct Request {
    std::string api_group;
    std::string path;
    std::string body; ///< The answer of the server; empty when there is none
    u64 offset{};     ///< How much of the answer sceNpWebApiReadData has handed out
};

std::mutex mutex;
std::map<s64, Request> requests;
s64 next_request_id{0x4e500001};
// The Online ID in the last lookup of a single player, ".../users/<id>/profile".
std::string last_profile;
std::atomic<s32> next_push_id{0x5001};

// Keeps the id of ".../users/<id>/profile". Any other path changes nothing. The caller holds the
// mutex.
void KeepProfileId(std::string_view path) {
    constexpr std::string_view Users = "/users/";
    const auto users = path.find(Users);
    if (users == std::string_view::npos) {
        return;
    }
    const auto start = users + Users.size();
    const auto end = path.find('/', start);
    if (end == std::string_view::npos || !path.substr(end).starts_with("/profile")) {
        return;
    }
    last_profile = path.substr(start, end - start);
}

bool IsPrintable(std::string_view text) {
    return std::ranges::all_of(text, [](u8 c) { return c >= 0x20 && c <= 0x7e; });
}

// Percent-encodes every byte that is not a visible ASCII character.
std::string Encode(std::string_view text) {
    constexpr std::string_view Digits = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (const u8 c : text) {
        if (c >= 0x21 && c <= 0x7e) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(Digits[c >> 4]);
            out.push_back(Digits[c & 0xf]);
        }
    }
    return out;
}

std::string DecodeBase64(std::string_view text) {
    const auto value = [](char c) -> s32 {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };
    std::string out;
    s32 buffer = 0;
    s32 bits = 0;
    for (const char c : text) {
        if (c == '=') {
            break;
        }
        const s32 v = value(c);
        if (v < 0) {
            continue;
        }
        buffer = (buffer << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xff));
        }
    }
    return out;
}

} // Anonymous namespace

s32 PS4_SYSV_ABI sceNpWebApiInitialize() {
    static std::atomic<s32> context_id{};
    return ++context_id;
}

// sceNpWebApiCreatePushEventFilter, sceNpWebApiCreateServicePushEventFilter,
// sceNpWebApiRegisterNotificationCallback, sceNpWebApiRegisterPushEventCallback and
// sceNpWebApiRegisterServicePushEventCallback. Each call gets an id of its own, which the toolkit
// passes on: the id of a filter comes back in the registration of its callback. Nothing is kept
// and no event is delivered, because the server of the game pushes none.
s32 PS4_SYSV_ABI NewPushId() {
    return next_push_id++;
}

s32 PS4_SYSV_ABI sceNpWebApiCreateRequest(s32 user_ctx_id, const char* api_group, const char* path,
                                          s32 method, const void* content_parameter,
                                          s64* request_id) {
    Request request{api_group ? api_group : "", path ? path : ""};
    std::scoped_lock lk{mutex};
    KeepProfileId(request.path);
    const s64 id = next_request_id++;
    requests[id] = std::move(request);
    if (request_id) {
        *request_id = id;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpWebApiDeleteRequest(s64 request_id) {
    std::scoped_lock lk{mutex};
    requests.erase(request_id);
    return ORBIS_OK;
}

// Always ORBIS_OK. A request that is not sent, or that the server does not answer with a 2xx
// status, has no answer to read, which the toolkit takes as "not found".
s32 PS4_SYSV_ABI sceNpWebApiSendRequest2(s64 request_id, const void* data, u64 data_size,
                                         void* response_information) {
    // Online play is off, or the user has not signed in to shadNet in this run.
    if (!Host::Connected() || !Host::HasIdentity()) {
        return ORBIS_OK;
    }
    std::string target;
    {
        std::scoped_lock lk{mutex};
        const auto it = requests.find(request_id);
        if (it == requests.end()) {
            LOG_ERROR(Lib_NpWebApi, "Request {:#x} does not exist", request_id);
            return ORBIS_OK;
        }
        // The game also asks for the profile of a row it never filled in, and the name in that
        // path is whatever the memory held. Such a lookup is not sent: the game would write the
        // profile that came back through the same row, into memory that is not a profile.
        if (!IsPrintable(it->second.path)) {
            LOG_WARNING(Lib_NpWebApi, "Request {:#x} names no player, not sent", request_id);
            return ORBIS_OK;
        }
        target = Encode("/npwebapi/" + it->second.api_group + it->second.path);
    }

    const auto& server = Host::GetServer();
    httplib::Client client{server.host, server.port};
    client.set_connection_timeout(5, 0);
    client.set_read_timeout(5, 0);
    client.set_write_timeout(5, 0);
    const httplib::Result result =
        data && data_size != 0
            ? client.Post(target, static_cast<const char*>(data), data_size, "application/json")
            : client.Get(target);
    std::string body;
    if (!result) {
        LOG_ERROR(Lib_NpWebApi, "No answer from {}:{} for {}", server.host, server.port, target);
    } else if (result->status / 100 != 2) {
        LOG_ERROR(Lib_NpWebApi, "Status {} for {}", result->status, target);
    } else {
        LOG_DEBUG(Lib_NpWebApi, "{} bytes for {}", result->body.size(), target);
        body = result->body;
    }

    std::scoped_lock lk{mutex};
    if (const auto it = requests.find(request_id); it != requests.end()) {
        it->second.body = std::move(body);
        it->second.offset = 0;
    }
    return ORBIS_OK;
}

// Returns the number of bytes copied, and 0 once the answer has been read to its end.
s32 PS4_SYSV_ABI sceNpWebApiReadData(s64 request_id, void* data, u64 size) {
    std::scoped_lock lk{mutex};
    const auto it = requests.find(request_id);
    if (it == requests.end() || !data) {
        return 0;
    }
    Request& request = it->second;
    const u64 count = std::min<u64>(size, request.body.size() - request.offset);
    std::memcpy(data, request.body.data() + request.offset, count);
    request.offset += count;
    return static_cast<s32>(count);
}

// The text is the base64 form of "<Online ID>@<opt>", where opt may hold '/' and '.' between its
// parts. For the profiles the server of the game sends, the toolkit passes an empty text. The
// name is then the Online ID of the single player that was looked up last; without it the
// player's own line of a leaderboard has no name.
s32 PS4_SYSV_ABI sceNpWebApiUtilityParseNpId(const char* json_np_id, Np::OrbisNpId* np_id) {
    if (np_id) {
        *np_id = {};
    }
    if (!json_np_id) {
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    }
    const std::string decoded = DecodeBase64(json_np_id);
    const auto at = decoded.find('@');
    std::string handle = decoded.substr(0, at);
    std::string opt;
    if (at != std::string::npos) {
        for (const char c : std::string_view{decoded}.substr(at + 1)) {
            if (c != '/' && c != '.') {
                opt.push_back(c);
            }
        }
    }
    if (handle.empty()) {
        std::scoped_lock lk{mutex};
        handle = last_profile;
        opt.clear();
    }
    if (handle.empty()) {
        // No name at all: the output stays zeroed, so the caller cannot take it for a player.
        return ORBIS_NP_ERROR_INVALID_ARGUMENT;
    }
    if (np_id) {
        std::memcpy(np_id->handle.data, handle.data(),
                    std::min<u64>(handle.size(), Np::ORBIS_NP_ONLINEID_MAX_LENGTH));
        std::memcpy(np_id->opt, opt.data(), std::min<u64>(opt.size(), sizeof(np_id->opt)));
        // Marks the handle as valid, as the firmware library does.
        np_id->reserved[0] = 1;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Unimplemented() {
    return ORBIS_OK;
}

constexpr std::array UnimplementedNids = {
    "WKcm4PeyJww", "JzhYTP2fG18", "joRjtRXTFoc", "19KgfJXgM+U", "gVNNyxf-1Sg", "KQIkDGf80PQ",
    "f-pgaNSd1zc", "x1Y7yiYSk7c", "zk6c65xoyO0", "M2BUB+DNEGE", "79M-JqvvGo0", "KBxgeNpoRIQ",
    "XUjdsSTTZ3U", "pfaJtb7SQ80", "5Mn7TYwpl30", "zE+R6Rcx3W0", "PfQ+f6ws764", "UJ8H+7kVQUE",
    "2qSZ0DgwTsc", "VwJ5L0Higg0", "743ZzEBzlV8", "k210oKgP80Y", "3OnubUs02UM", "FkuwsD64zoQ",
    "c1pKoztonB8", "N2Jbx4tIaQ4", "TZSep4xB4EY", "8Vjplhyyc44", "VjVukb2EWPc", "sfq23ZVHVEw",
    "vrM02A5Gy1M", "jhXKGQJ4egI", "KCItz6QkeGs", "DsPOTEvSe7M", "kVbL4hL3K7w", "6g6q-g1i4XU",
    "gRiilVCvfAI", "i0dr6grIZyc", "qWcbJkBj1Lg", "asz3TtIqGF8", "PqCY25FMzPs", "wjYEvo4xbcA",
    "qK4o2656W4w", "2edrkr0c-wg", "uRsskUhAfnM", "BkxO0e2+ueg", "B4OVXU6VY9o", "Gm138-2DI6g",
    "HgaTom-g+VQ", "JKm18ddwAM8", "JKqm9Q5MI2E", "JNiFPWtH-Hk", "J5s+nHxKncU", "KEYeKen41pc",
    "PCliRwT6ueA", "PwJ4BO0uwR4", "QGbJTngpl80", "R8hTVoFdvpA", "T86AZUN+O4c", "U2KAvj2rtSE",
    "V6DhvHJCGfM", "WBl0nAQLZjc", "YZjQyCXoYxk", "YfK56KsJN0M", "a8OI5hE-DUQ", "dQDwxPjcLRY",
    "daA4FMfpA58", "eJ1gJsUhQW4", "fe1j0GOZ7-8", "flWi3MA9OVo", "fmyPn7hpZ-Q", "fwS31KfUHoA",
    "jhZyUt+lyVc", "ldAEblBOOwk", "lyhL-aTxj98", "meMsH0c36rQ", "nP9mHqC8v4M", "nrDh9GesOyk",
    "ojGP5vur+qM", "ugei4b97OXE", "vQgD7uDMKaA", "vm9OVSS7E18", "wNSQ60gepNA", "wXXTksptCEo",
    "zQE2rxZdLy8", "0cCtt7Uv6rU", "4yR2XRjuTRI", "54n5gNkHtlM", "+aMuhoVidDY",
};

constexpr std::array UnimplementedCompatNids = {
    "x1Y7yiYSk7c", "zE+R6Rcx3W0", "PfQ+f6ws764", "vrM02A5Gy1M",
    "wjYEvo4xbcA", "qK4o2656W4w", "2edrkr0c-wg",
};

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("G3AnLNdRBjE", "libSceNpWebApi", 1, "libSceNpWebApi", sceNpWebApiInitialize);
    LIB_FUNCTION("rdgs5Z1MyFw", "libSceNpWebApi", 1, "libSceNpWebApi", sceNpWebApiCreateRequest);
    LIB_FUNCTION("noQgleu+KLE", "libSceNpWebApi", 1, "libSceNpWebApi", sceNpWebApiDeleteRequest);
    LIB_FUNCTION("KjNeZ-29ysQ", "libSceNpWebApi", 1, "libSceNpWebApi", sceNpWebApiSendRequest2);
    LIB_FUNCTION("CQtPRSF6Ds8", "libSceNpWebApi", 1, "libSceNpWebApi", sceNpWebApiReadData);
    LIB_FUNCTION("or0e885BlXo", "libSceNpWebApi", 1, "libSceNpWebApi", sceNpWebApiUtilityParseNpId);

    LIB_FUNCTION("y5Ta5JCzQHY", "libSceNpWebApi", 1, "libSceNpWebApi", NewPushId);
    LIB_FUNCTION("sIFx734+xys", "libSceNpWebApi", 1, "libSceNpWebApi", NewPushId);
    LIB_FUNCTION("HVgWmGIOKdk", "libSceNpWebApi", 1, "libSceNpWebApi", NewPushId);
    LIB_FUNCTION("PfSTDCgNMgc", "libSceNpWebApi", 1, "libSceNpWebApi", NewPushId);
    LIB_FUNCTION("kJQJE0uKm5w", "libSceNpWebApi", 1, "libSceNpWebApi", NewPushId);
    LIB_FUNCTION("y5Ta5JCzQHY", "libSceNpWebApiCompat", 1, "libSceNpWebApi", NewPushId);
    LIB_FUNCTION("sIFx734+xys", "libSceNpWebApiCompat", 1, "libSceNpWebApi", NewPushId);
    LIB_FUNCTION("HVgWmGIOKdk", "libSceNpWebApiCompat", 1, "libSceNpWebApi", NewPushId);
    LIB_FUNCTION("PfSTDCgNMgc", "libSceNpWebApiCompat", 1, "libSceNpWebApi", NewPushId);
    LIB_FUNCTION("kJQJE0uKm5w", "libSceNpWebApiCompat", 1, "libSceNpWebApi", NewPushId);

    for (const char* nid : UnimplementedNids) {
        LIB_FUNCTION(nid, "libSceNpWebApi", 1, "libSceNpWebApi", Unimplemented);
    }
    for (const char* nid : UnimplementedCompatNids) {
        LIB_FUNCTION(nid, "libSceNpWebApiCompat", 1, "libSceNpWebApi", Unimplemented);
    }
}

} // namespace Libraries::Gr2Online::NpWebApi
