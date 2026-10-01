// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <httplib.h>
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/gr2_online/online_host.h"
#include "core/libraries/gr2_online/online_http.h"
#include "core/libraries/gr2_online/online_patches.h"
#include "core/libraries/gr2_online/online_session.h"
#include "core/libraries/libs.h"
#include "core/libraries/network/http_error.h"

// libSceHttp for Gravity Rush 2. Every request goes to the one server configured for the game,
// whatever host its URL names, as plain HTTP and with the headers that tell the server who plays.
// What the game sets on a template, a connection or a request (headers, time-outs, TLS options)
// is accepted and not used. The nine functions that work on a URL or on header text and take no
// id are left to the emulator's own library.
namespace Libraries::Gr2Online::Http {

namespace {

// The method numbers of sceHttpCreateRequestWithURL.
enum class Method : s32 {
    Get = 0,
    Post = 1,
    Head = 2,
    Options = 3,
    Put = 4,
    Delete = 5,
    Trace = 6,
    Connect = 7,
    Count = 8,
};

// A request of the game. The round trip runs on a thread of its own that holds a reference, so
// a request the game deletes in the middle of one lives until that thread is done.
class Request : public std::enable_shared_from_this<Request> {
public:
    Request(s32 method, std::string path, u64 content_length)
        : method{static_cast<Method>(method)}, path{std::move(path)},
          content_length{content_length} {}

    // Takes one piece of the body. True when the request can go: the length declared at creation
    // has arrived, or none was declared.
    bool Append(const void* data, u64 size) {
        std::scoped_lock lk{mutex};
        if (data && size != 0) {
            body.append(static_cast<const char*>(data), size);
        }
        return content_length == 0 || body.size() >= content_length;
    }

    // Sends the request and returns when the answer is in or the attempt has failed.
    void Send() {
        {
            std::scoped_lock lk{mutex};
            finished = false;
        }
        std::thread([self = shared_from_this()] {
            self->RoundTrip();
            {
                std::scoped_lock lk{self->mutex};
                self->finished = true;
            }
            self->finished_cv.notify_all();
        }).detach();
        std::unique_lock lk{mutex};
        finished_cv.wait(lk, [this] { return finished; });
    }

    s32 GetStatus(s32* out) {
        std::scoped_lock lk{mutex};
        if (const s32 error = Unanswered(); error != ORBIS_OK) {
            return error;
        }
        *out = status;
        return ORBIS_OK;
    }

    s32 GetLength(s32* result, u64* length) {
        std::scoped_lock lk{mutex};
        if (const s32 error = Unanswered(); error != ORBIS_OK) {
            return error;
        }
        // 0 says that the length is known.
        *result = 0;
        *length = answer.size();
        return ORBIS_OK;
    }

    // Copies the next bytes of the answer. Returns how many, 0 at the end.
    s32 Read(char* dest, u32 size) {
        std::scoped_lock lk{mutex};
        if (const s32 error = Unanswered(); error != ORBIS_OK) {
            return error;
        }
        const u64 count = std::min<u64>(size, answer.size() - read_offset);
        std::memcpy(dest, answer.data() + read_offset, count);
        read_offset += count;
        return static_cast<s32>(count);
    }

private:
    // What the getters return while there is no answer to read, ORBIS_OK once there is one.
    s32 Unanswered() const {
        if (!sent) {
            return ORBIS_HTTP_ERROR_BEFORE_SEND;
        }
        // A request the server never answered stays here: this is the error the game gets when
        // the server cannot be reached.
        return status == -1 ? ORBIS_HTTP_ERROR_EAGAIN : ORBIS_OK;
    }

    void RoundTrip() {
        std::string out;
        {
            std::scoped_lock lk{mutex};
            out = body;
            sent = true;
        }
        const auto& server = Host::GetServer();
        httplib::Client cli(server.host, server.port);
        cli.set_connection_timeout(5, 0);
        cli.set_read_timeout(10, 0);
        cli.set_write_timeout(10, 0);

        httplib::Headers headers;
        for (auto& [name, value] : Host::IdentityHeaders()) {
            headers.emplace(std::move(name), std::move(value));
        }
        // The game labels a multipart body as multipart/form-data in a header of its own, which
        // is not sent. The server takes the boundary from the first line of the body.
        static constexpr const char* ContentType = "application/json";

        httplib::Result response;
        switch (method) {
        case Method::Get:
            response = cli.Get(path, headers);
            break;
        case Method::Post:
            response = cli.Post(path, headers, out.data(), out.size(), ContentType);
            break;
        case Method::Head:
            response = cli.Head(path, headers);
            break;
        case Method::Options:
            response = cli.Options(path, headers);
            break;
        case Method::Put:
            response = cli.Put(path, headers, out.data(), out.size(), ContentType);
            break;
        case Method::Delete:
            response = cli.Delete(path, headers);
            break;
        default:
            LOG_ERROR(Lib_Http, "Method {} is not supported, {} is not sent",
                      static_cast<s32>(method), path);
            return;
        }
        if (!response) {
            LOG_ERROR(Lib_Http, "No answer from {}:{} for {} ({})", server.host, server.port, path,
                      httplib::to_string(response.error()));
            return;
        }
        LOG_DEBUG(Lib_Http, "{} with {} bytes: status {}, {} bytes", path, out.size(),
                  response->status, response->body.size());

        // The game takes a status as the sign that the answer can be read, so the body is in
        // place before the status is.
        std::scoped_lock lk{mutex};
        if (!response->body.empty()) {
            answer = std::move(response->body);
            read_offset = 0;
        }
        status = response->status;
    }

    const Method method;
    const std::string path;
    const u64 content_length;

    std::mutex mutex;
    std::condition_variable finished_cv;
    bool finished = true;
    bool sent = false;
    std::string body;
    s32 status = -1;
    std::string answer;
    u64 read_offset = 0;
};

// Ids count from 0. The request thread of the game skips the delete of an id that is not above
// 0, so request 0 can stay in the map for the run.
std::mutex requests_mutex;
std::map<s32, std::shared_ptr<Request>> requests;
s32 next_request = 0;
std::atomic<s32> next_template{0};
std::atomic<s32> next_context{1};

// The reference is taken out of the map and the map lock is dropped before anything is done with
// the request: a send blocks for the whole round trip, and the game creates and reads other
// requests on other threads meanwhile.
std::shared_ptr<Request> Find(s32 id) {
    std::scoped_lock lk{requests_mutex};
    const auto it = requests.find(id);
    return it != requests.end() ? it->second : nullptr;
}

// What the server gets of a URL of the game: the path and the query. The scheme, the host and
// the port are dropped, since the request goes to the configured server.
std::string PathOf(std::string_view url) {
    const auto scheme = url.find("://");
    const auto authority = scheme == std::string_view::npos ? 0 : scheme + 3;
    const auto start = url.find_first_of("/?#", authority);
    if (start == std::string_view::npos) {
        return "/";
    }
    std::string path{url.substr(start)};
    if (path.front() != '/') {
        path.insert(path.begin(), '/');
    }
    return path;
}

} // Anonymous namespace

s32 PS4_SYSV_ABI sceHttpInit(s32 net_mem_id, s32 ssl_ctx_id, u64 pool_size) {
    Patches::Apply();
    return next_context++;
}

s32 PS4_SYSV_ABI sceHttpCreateTemplate(s32 ctx_id, const char* user_agent, s32 http_version,
                                       s32 auto_proxy_conf) {
    if (!user_agent) {
        return ORBIS_HTTP_ERROR_INVALID_VALUE;
    }
    return next_template++;
}

// The return value is the connection id.
s32 PS4_SYSV_ABI sceHttpCreateConnectionWithURL(s32 tmpl_id, const char* url, bool keep_alive) {
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttpCreateRequestWithURL(s32 conn_id, s32 method, const char* url,
                                             u64 content_length) {
    if (method >= static_cast<s32>(Method::Count)) {
        return ORBIS_HTTP_ERROR_UNKNOWN_METHOD;
    }
    if (!url) {
        return ORBIS_HTTP_ERROR_INVALID_VALUE;
    }
    Patches::Apply();
    Session::OnRequest();
    auto request = std::make_shared<Request>(method, PathOf(url), content_length);
    std::scoped_lock lk{requests_mutex};
    const s32 id = next_request++;
    requests.emplace(id, std::move(request));
    return id;
}

// sceHttpCreateRequest, sceHttpCreateRequest2 and sceHttpCreateRequestWithURL2. The game creates
// every request with sceHttpCreateRequestWithURL. These must not return 0: that is the id of a
// request.
s32 PS4_SYSV_ABI Unsupported() {
    LOG_ERROR(Lib_Http, "A request was created in a way that is not supported");
    return ORBIS_HTTP_ERROR_INVALID_ID;
}

// The headers of the game are not sent. The game also calls this with no name and no value, so
// neither pointer is read.
s32 PS4_SYSV_ABI sceHttpAddRequestHeader(s32 id, const char* name, const char* value, s32 mode) {
    return ORBIS_OK;
}

// The game hands over a body in several calls and gives up on the first one that does not
// return 0. The request leaves with the call that completes the body, and that call returns
// when the answer is in.
s32 PS4_SYSV_ABI sceHttpSendRequest(s32 req_id, const void* data, u64 size) {
    const auto request = Find(req_id);
    if (!request) {
        return ORBIS_HTTP_ERROR_INVALID_ID;
    }
    if (!Host::Connected()) {
        LOG_INFO(Lib_Http, "Online play is off, request {} is not sent", req_id);
        return ORBIS_HTTP_ERROR_NETWORK;
    }
    if (request->Append(data, size)) {
        request->Send();
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceHttpGetStatusCode(s32 req_id, s32* status_code) {
    if (!status_code) {
        return ORBIS_HTTP_ERROR_INVALID_VALUE;
    }
    const auto request = Find(req_id);
    if (!request) {
        return ORBIS_HTTP_ERROR_INVALID_ID;
    }
    return request->GetStatus(status_code);
}

s32 PS4_SYSV_ABI sceHttpGetResponseContentLength(u32 req_id, s32* result, u64* content_length) {
    if (!result || !content_length) {
        return ORBIS_HTTP_ERROR_INVALID_VALUE;
    }
    const auto request = Find(static_cast<s32>(req_id));
    if (!request) {
        return ORBIS_HTTP_ERROR_INVALID_ID;
    }
    return request->GetLength(result, content_length);
}

s32 PS4_SYSV_ABI sceHttpReadData(u32 req_id, char* dest, u32 size) {
    if (!dest || size == 0) {
        return ORBIS_HTTP_ERROR_INVALID_VALUE;
    }
    const auto request = Find(static_cast<s32>(req_id));
    if (!request) {
        return ORBIS_HTTP_ERROR_INVALID_ID;
    }
    return request->Read(dest, size);
}

s32 PS4_SYSV_ABI sceHttpGetAllResponseHeaders(s32 req_id, char** header, u64* header_size) {
    return ORBIS_FAIL;
}

s32 PS4_SYSV_ABI sceHttpDeleteRequest(s32 req_id) {
    std::scoped_lock lk{requests_mutex};
    return requests.erase(req_id) != 0 ? ORBIS_OK : ORBIS_HTTP_ERROR_INVALID_ID;
}

// The game asks for this number when a send returns ORBIS_HTTP_ERROR_NETWORK and takes it as the
// result of the request, so a number is always written.
s32 PS4_SYSV_ABI sceHttpGetLastErrno(s32 req_id, s32* error) {
    if (error) {
        *error = ORBIS_HTTP_ERROR_NETWORK;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI Unimplemented() {
    return ORBIS_OK;
}

// Every other function of the library that takes an id: aborts, options, time-outs, cookies,
// authentication, TLS, epoll and statistics. None is left out: the ids are this file's own, and a
// function left to the emulator's library would be handed an id it does not know.
constexpr std::array UnimplementedNids = {
    "hvG6GfBMXg8", "JKl06ZIAl6A", "sWQiqKvYTVA", "mNan6QSnpeY", "JM58a21mtrQ", "lGAjftanhFs",
    "Y1DCjN-s2BA", "zzB0StvRab4", "wF0KcxK20BE", "A7n9nNg7NBg", "nOkViL17ZOo", "seCvUt91WHY",
    "pFnXDxo3aog", "Kiwv9r4IZCc", "6381dWF+xsQ", "Lffcxao-QMM", "6gyx-I0Oob4", "fzzBpJjm9Kw",
    "VmqSnjZ5mE4", "KJtUHtp6y0U", "oEuPssSYskA", "L2gM3qptqHs", "pxBsD-X9eH0", "P6A3ytpsiYc",
    "4I8vEpuEhZ8", "wYhXVfS2Et4", "1rpZqxdMRwQ", "9m8EcOGzcIQ", "mmLexUbtnfY", "L-DwVoHXLtU",
    "+G+UsJpeXPc", "iSZjWw1TGiA", "xkymWiGdMiI", "7j9VcwnrZo4", "IQOP6McWJcY", "16sMmVuOvgU",
    "Wq4RNB3snSQ", "hkcfqAl+82w", "u05NnI+P+KY", "zNGh-zoQTD0", "4fgkfVeVsGU", "mSQCxzWTwVI",
    "zJYi5br6ZiQ", "f42K37mm5RM", "I4+4hKttt1w", "HRX1iyDoKR8", "qFg2SuyTJJY", "jf4TB2nUO40",
    "T-mGo9f3Pu4", "PDxS48xGQLs", "0S9tTH0uqTU", "XNUoD2B9a6A", "pM--+kIeW-8", "Kp6juCJUJGQ",
    "7Y4364GBras", "Kh6bS2HQKbo", "GnVDzYfy-KI", "pHc3bxUzivU", "8kzIXsRy1bY", "22buO-UufJY",
    "-xm7kZQNpHI", "LG1YW1Uhkgo", "pk0AuomQM1o", "i9mhafzkEi8", "s2-NPIvz+iA", "gZ9TpeFQ7Gk",
    "2NeZnMEP3-0", "i+quCZCL+D8", "mMcB2XIDoV4", "yigr4V0-HTM", "h9wmFZX4i-4", "PTiFIUxCpJc",
    "vO4B-42ef-k", "K1d1LqZRQHQ", "Tc-hAYDKtQc", "a4VsZ4oqn68", "xegFfZKBVlw", "POJ0azHZX3w",
    "7WcNoAI9Zcw", "gcUjwU3fa0M", "JBN6N-EY+3M", "DK+GoXCNT04", "jUjp+yqMNdQ", "htyBOoWeS58",
    "U5ExQGyyx9s", "zXqcE0fizz0", "Ik-KpLTlf7Q", "V-noPEjSB8c", "fmOs6MzCRqk", "59tL1AQBb8U",
    "qISjDHrxONc",
};

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("A9cVMUtEp4Y", "libSceHttp", 1, "libSceHttp", sceHttpInit);
    LIB_FUNCTION("0gYjPTR-6cY", "libSceHttp", 1, "libSceHttp", sceHttpCreateTemplate);
    LIB_FUNCTION("qgxDBjorUxs", "libSceHttp", 1, "libSceHttp", sceHttpCreateConnectionWithURL);
    LIB_FUNCTION("Aeu5wVKkF9w", "libSceHttp", 1, "libSceHttp", sceHttpCreateRequestWithURL);
    LIB_FUNCTION("EY28T2bkN7k", "libSceHttp", 1, "libSceHttp", sceHttpAddRequestHeader);
    LIB_FUNCTION("1e2BNwI-XzE", "libSceHttp", 1, "libSceHttp", sceHttpSendRequest);
    LIB_FUNCTION("0a2TBNfE3BU", "libSceHttp", 1, "libSceHttp", sceHttpGetStatusCode);
    LIB_FUNCTION("yuO2H2Uvnos", "libSceHttp", 1, "libSceHttp", sceHttpGetResponseContentLength);
    LIB_FUNCTION("P5pdoykPYTk", "libSceHttp", 1, "libSceHttp", sceHttpReadData);
    LIB_FUNCTION("aCYPMSUIaP8", "libSceHttp", 1, "libSceHttp", sceHttpGetAllResponseHeaders);
    LIB_FUNCTION("qe7oZ+v4PWA", "libSceHttp", 1, "libSceHttp", sceHttpDeleteRequest);
    LIB_FUNCTION("0onIrKx9NIE", "libSceHttp", 1, "libSceHttp", sceHttpGetLastErrno);

    LIB_FUNCTION("tsGVru3hCe8", "libSceHttp", 1, "libSceHttp", Unsupported);
    LIB_FUNCTION("rGNm+FjIXKk", "libSceHttp", 1, "libSceHttp", Unsupported);
    LIB_FUNCTION("Cnp77podkCU", "libSceHttp", 1, "libSceHttp", Unsupported);

    for (const char* nid : UnimplementedNids) {
        LIB_FUNCTION(nid, "libSceHttp", 1, "libSceHttp", Unimplemented);
    }
}

} // namespace Libraries::Gr2Online::Http
