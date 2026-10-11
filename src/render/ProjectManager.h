// Montage — Project Manager (Premiere's Project Manager, Resolve's Media Management): a copy of the project in a new
// folder with the media it uses, for archiving or handing on. Collect copies each used file whole; Consolidate
// transcodes only the part of each video the sequences use, plus handles, and points the clips at the new files.
// Media nothing uses is left out, and sequences can be limited to some.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

struct ConsolidateOptions {
    std::string folder;          // the project goes here, its media in folder/Media
    std::string name = "Project";  // the project file's name, without .montage
    std::vector<Id> sequences;   // the sequences to keep (and what they nest); empty = all
    bool trim = false;           // Consolidate: only the used parts of videos, transcoded
    double handles = 1.0;        // seconds kept either side of what is used, when trimming
    std::string codec = "prores_ks";  // trimmed copies: prores_ks (ProRes 422 HQ, .mov) or libx264 (.mp4)
    bool keepUnused = false;     // keep media items nothing uses
};

struct ConsolidateResult {
    std::string projectPath;
    int copied = 0;    // files copied whole
    int trimmed = 0;   // videos transcoded to their used part
    int64_t bytes = 0;  // written to the folder's Media
    std::vector<std::string> missing;  // used files that could not be found (left pointing where they were)
};

// The media items the sequences use, directly or through nested sequences and multicam clips.
std::vector<Id> usedMedia(const Project& p, const std::vector<Id>& sequences);

bool consolidateProject(const Project& p, const ConsolidateOptions& o, ConsolidateResult* result = nullptr,
                        const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                        std::string* error = nullptr);

}  // namespace montage
