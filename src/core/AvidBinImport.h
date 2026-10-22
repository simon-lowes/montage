// Montage — Media Composer bins (.avb, read by core/AvidBin.h) brought into a project: each master clip becomes media
// in a bin of the same name, linked to the files Media Composer made for it (its MXF media, found where the bin's
// locators say or by their package IDs under a drive's "Avid MediaFiles", as Media Composer finds them; else the
// file it was linked to or transcoded from; else offline with its name and length); subclips become subclips of it;
// the bin's custom columns become media metadata (Scene, Take and the like as Montage's own fields); and each sequence
// becomes a sequence as an AAF of it would (core/AafImport.h): source clips followed through master clips to their
// files, fillers as gaps, transitions as dissolves or crossfades at their cut points, a constant motion effect as
// clip speed, a group clip as its selected angle, locators as markers. Effects it cannot carry keep their input and
// are listed by name.
#pragma once

#include <string>
#include <vector>

#include "Interchange.h"

namespace montage {

struct AvidBinImport {
    bool ok = false;
    std::string error;
    std::string bin;                    // the project bin the media went into (the .avb file's name)
    std::vector<Id> media;              // media items added, master clips and subclips (the sequences' own too)
    std::vector<Id> sequences;          // the bin's sequences, in its order (the first made active)
    int clips = 0;                      // clips placed in the sequences
    std::vector<std::string> offline;   // media that could not be found
    std::vector<std::string> warnings;  // what could not be carried over
};

AvidBinImport importAvidBin(Project& p, const std::string& path, const MediaProber& probe = {});

}  // namespace montage
