// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "common/types.h"

// The photo album of Gravity Rush 2 as a host folder: one <id>.jpg per photo, in
// Capture Gallery/Gravity Rush 2 of the user directory. The guest sees the folder read-only at
// GuestDir; photos are written and deleted from the host side. An id is "YYYYMMDD_HHMMSS_NNN", so
// name order is the order photos were taken in.
namespace Libraries::Gr2Photo::Gallery {

constexpr std::string_view GuestDir = "/screenshot";

/// The game stops taking photos at 1000.
constexpr u32 MaxPhotos = 999;

struct Photo {
    u64 handle;     ///< What the game stores for a photo and hands back to delete it
    std::string id; ///< File name without the extension
};

/// Creates the host folder, mounts it and lists the photos in it. Runs once, at boot.
void Setup();

/// Writes a JPEG as a new photo and keeps its id for the export that follows.
bool Save(std::span<const u8> jpeg);

/// The encode that just ran left no photo to export.
void DropPending();

/// Takes the id Save() kept. Empty when there is none.
std::string TakePending();

/// Adds a saved photo to the album.
void Add(const std::string& id);

/// Number of photos in the album, at most MaxPhotos.
u32 Count();

/// Photos [start, start + limit) of the album.
std::vector<Photo> List(u32 start, u32 limit);

/// Removes a photo from the album and deletes its file.
bool Delete(u64 handle);

/// The photo a handle names. Its id is empty when the album has no such photo.
Photo Find(u64 handle);

/// Keeps the comment a photo was exported with. The game writes where the photo was taken into
/// it and reads it back when the photo is posted for review.
void SetComment(const std::string& id, std::string comment);

/// The comment of the photo a handle names. A photo that has none gets the comment of the last
/// export: a review is posted right after the photo is saved.
std::string Comment(u64 handle);

} // namespace Libraries::Gr2Photo::Gallery
