// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "core/libraries/gr2_photo/photo_gallery.h"

namespace Libraries::Gr2Photo::Gallery {

namespace {

std::mutex mutex;
std::filesystem::path host_dir;
bool mounted = false;
std::vector<std::string> ids; // sorted
std::unordered_map<u64, std::string> handle_to_id;
std::unordered_map<std::string, u64> id_to_handle;
// The game keeps the handle of every listed photo, so a handle names one photo for the whole
// run. A list position would renumber on each delete.
u64 next_handle = 0x4752324400000001ULL; // "GR2D" in the high half, counting from 1
std::string pending;

std::filesystem::path FileOf(const std::string& id) {
    return host_dir / (id + ".jpg");
}

u64 HandleOf(const std::string& id) {
    if (const auto it = id_to_handle.find(id); it != id_to_handle.end()) {
        return it->second;
    }
    const u64 handle = next_handle++;
    id_to_handle.emplace(id, handle);
    handle_to_id.emplace(handle, id);
    return handle;
}

std::string NewId() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[20]{};
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &local);
    std::string id;
    std::error_code ec;
    // An encode that is never exported leaves a file the list does not know, so ask the disk too.
    for (u32 n = 1; n < 1000; ++n) {
        id = fmt::format("{}_{:03}", stamp, n);
        if (!std::ranges::binary_search(ids, id) && !std::filesystem::exists(FileOf(id), ec)) {
            break;
        }
    }
    return id;
}

} // Anonymous namespace

void Setup() {
    std::scoped_lock lk{mutex};
    // Not ScreenshotsDir: the Gravity Rush 2 fork this port comes from keeps its album in this
    // folder, so an album made there carries over.
    host_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "Capture Gallery" /
               "Gravity Rush 2";
    std::error_code ec;
    std::filesystem::create_directories(host_dir, ec);
    // Mount asserts on a folder that does not exist.
    if (!std::filesystem::is_directory(host_dir, ec)) {
        LOG_ERROR(Lib_ContentExport, "Cannot create the photo folder {}",
                  Common::FS::PathToUTF8String(host_dir));
        return;
    }
    Common::Singleton<Core::FileSys::MntPoints>::Instance()->Mount(host_dir, std::string{GuestDir},
                                                                   true);
    mounted = true;
    for (std::filesystem::directory_iterator it{host_dir, ec}, end; !ec && it != end;
         it.increment(ec)) {
        std::error_code file_ec;
        if (it->is_regular_file(file_ec) && it->path().extension() == ".jpg") {
            std::string id = Common::FS::PathToUTF8String(it->path().stem());
#ifdef _WIN32
            // The game opens a photo by a narrow path, which is not UTF-8 there.
            if (std::ranges::any_of(id, [](char c) { return static_cast<u8>(c) >= 0x80; })) {
                continue;
            }
#endif
            ids.push_back(std::move(id));
        }
    }
    std::ranges::sort(ids);
    LOG_INFO(Lib_ContentExport, "{} photos in {}", ids.size(),
             Common::FS::PathToUTF8String(host_dir));
}

bool Save(std::span<const u8> jpeg) {
    std::scoped_lock lk{mutex};
    pending.clear();
    if (!mounted) {
        return false;
    }
    const std::string id = NewId();
    const auto path = FileOf(id);
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file.write(reinterpret_cast<const char*>(jpeg.data()),
               static_cast<std::streamsize>(jpeg.size()));
    file.close();
    if (!file) {
        LOG_ERROR(Lib_ContentExport, "Cannot write {}", Common::FS::PathToUTF8String(path));
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return false;
    }
    LOG_INFO(Lib_ContentExport, "Saved {} ({} bytes)", Common::FS::PathToUTF8String(path),
             jpeg.size());
    pending = id;
    return true;
}

void DropPending() {
    std::scoped_lock lk{mutex};
    pending.clear();
}

std::string TakePending() {
    std::scoped_lock lk{mutex};
    return std::exchange(pending, {});
}

void Add(const std::string& id) {
    std::scoped_lock lk{mutex};
    const auto it = std::ranges::lower_bound(ids, id);
    if (it == ids.end() || *it != id) {
        ids.insert(it, id);
    }
}

u32 Count() {
    std::scoped_lock lk{mutex};
    return std::min<u32>(static_cast<u32>(ids.size()), MaxPhotos);
}

std::vector<Photo> List(u32 start, u32 limit) {
    std::scoped_lock lk{mutex};
    const u32 total = std::min<u32>(static_cast<u32>(ids.size()), MaxPhotos);
    std::vector<Photo> photos;
    for (u32 i = start; i < total && photos.size() < limit; ++i) {
        photos.push_back({HandleOf(ids[i]), ids[i]});
    }
    return photos;
}

bool Delete(u64 handle) {
    std::scoped_lock lk{mutex};
    const auto it = handle_to_id.find(handle);
    if (it == handle_to_id.end()) {
        return false;
    }
    const std::string id = it->second;
    // The handle goes with the photo, so a stale one can never name another photo.
    handle_to_id.erase(it);
    id_to_handle.erase(id);
    std::erase(ids, id);
    std::error_code ec;
    std::filesystem::remove(FileOf(id), ec);
    LOG_INFO(Lib_ContentExport, "Deleted photo {}", id);
    return true;
}

} // namespace Libraries::Gr2Photo::Gallery
