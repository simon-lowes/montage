// Montage — offline media and relinking (Premiere's Link Media and Replace Footage, Resolve's Relink Clips, Final
// Cut's Relink Files): finding the media whose files are gone, and pointing them at their files again.
#pragma once

#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

// Whether the item's file is missing (a nested sequence has none). A subclip is offline with its media.
bool isOffline(const MediaItem& m);
// The offline items, subclips left out (they follow their media).
std::vector<Id> offlineMedia(const Project& p);

enum class RelinkCheck {
    Strict,   // relinking the same footage: the same kind, picture size and duration (within a tenth of a second or 0.5 %)
    Replace,  // Replace Footage: any file that can stand in (pictures for pictures, sound for sound)
};
// Points the item and its subclips at `path`, re-reading the file's details (size, rate, codecs, duration). Logging,
// transcripts and indexes are kept when relinking; a replacement takes the new file's name and loses what described
// the old one (transcript, indexes, proxy). False, with the reason, if the file does not pass the check.
bool relinkMedia(Project& p, Id id, const std::string& path, RelinkCheck check = RelinkCheck::Strict, std::string* why = nullptr);

// Looks in `folder` and the folders below it (to `depth` levels) for these offline items' files (every offline item
// when `ids` is empty): by file name, ignoring case, else by name with another extension (a transcode, A001.mxf
// found as A001.mov). A candidate must pass the strict check. Relinks what it finds; returns the ids relinked.
std::vector<Id> relinkFromFolder(Project& p, const std::string& folder, std::vector<Id> ids = {}, int depth = 6);

}  // namespace montage
