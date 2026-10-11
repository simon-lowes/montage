// Montage — AAF export for audio post and conform (Pro Tools, Fairlight, Nuendo, Media
// Composer, Resolve, Premiere): the sequence's audio tracks as an AAF composition linked to
// mono 24-bit WAV files, one per channel of each source, written to a
// "<name> Media" folder beside it. Clip positions and source offsets carry
// over exactly, crossfades become AAF transitions (with their handles),
// fades the clips' fade properties, and clip gain and its keyframes
// "Audio Gain" operations. Clips that change speed or play backwards, and
// nested sequences, are rendered to their own files first. The video tracks
// come too, linked to the original picture files where they are (a master
// clip and file mob each, with a CDCI descriptor and the file's URL): source
// offsets exact, dissolves as transitions at their edit points (reaching into
// the handles the media has), constant speed (and a conformed frame rate) as
// Motion Control; titles, generated clips and nested sequences leave gaps.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "Exporter.h"
#include "core/Model.h"

namespace montage {

struct AafExportOptions {
    bool picture = true;  // the video tracks too, linked to the original files
};

struct AafExportResult {
    int audioTracks = 0;  // AAF audio tracks (mono: a stereo track is two)
    int clips = 0;        // sound source clips placed
    int transitions = 0;  // crossfades
    int videoTracks = 0;
    int videoClips = 0;
    int videoTransitions = 0;  // dissolves
    std::vector<std::string> mediaFiles;
    std::vector<std::string> warnings;
};

bool exportAaf(const Project& p, const Sequence& seq, const std::string& path, AafExportResult* result = nullptr,
               const ExportProgress& progress = {}, const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr,
               const AafExportOptions& options = {});

}  // namespace montage
