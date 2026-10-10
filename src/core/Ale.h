// Montage — Avid Log Exchange (.ale): the tab-separated logs that dailies arrive with (from Media Composer, Resolve,
// Silverstack, LiveGrade or the lab), one row per clip with its name, timecode, tape, scene, take and the on-set ASC
// CDL. Importing one fills in the matching media's log fields and CDLs; exporting writes the bin's media as one.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "Model.h"

namespace montage {

struct AleTable {
    std::vector<std::pair<std::string, std::string>> heading;  // FIELD_DELIM, VIDEO_FORMAT, AUDIO_FORMAT, FPS...
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    // A row's value in a column (named case-insensitively); "" if there is none.
    std::string value(size_t row, const std::string& column) const;
    std::string headingValue(const std::string& key) const;
    Rational fps() const;  // the heading's FPS (23.976, 29.97... exactly), else 24
};

// Reads an ALE (Heading, Column and Data sections; any line ending). False, with the reason, if it is not one.
bool parseAle(const std::string& text, AleTable& out, std::string* error = nullptr);
std::string writeAle(const AleTable& table);

struct AleImport {
    int rows = 0;
    std::vector<Id> matched;                // media items given a row's log
    std::vector<std::string> unmatched;     // rows matching no media (their names)
    int cdls = 0;                           // rows that carried a CDL
    int clips = 0;                          // clips given their media's CDL
};
// Applies an ALE to the project: each row finds its media (by source file, by name with or without extension, or by
// tape and start timecode) and sets its scene, shot, take, tape, camera, description and comment, and its CDL
// (ASC_SOP, ASC_SAT). With `cdlToClips`, every clip of a media item that got a CDL (in every sequence) is graded with it.
AleImport applyAle(Project& p, const AleTable& table, bool cdlToClips);

// An ALE of these media items (stills and sequences left out), timecodes at `fps`: name, tracks, start, end (the
// frame after the last), duration, tape, source file, the log fields and the CDL (the media's own, else the first
// graded clip's).
AleTable aleFromMedia(const Project& p, const std::vector<Id>& media, Rational fps);

}  // namespace montage
