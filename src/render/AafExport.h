// Montage — AAF export for audio post (Pro Tools, Fairlight, Nuendo, Media
// Composer): the sequence's audio tracks as an AAF composition linked to
// mono 24-bit WAV files, one per channel of each source, written to a
// "<name> Media" folder beside it. Clip positions and source offsets carry
// over exactly, crossfades become AAF transitions (with their handles),
// fades the clips' fade properties, and clip gain and its keyframes
// "Audio Gain" operations. Clips that change speed or play backwards, and
// nested sequences, are rendered to their own files first.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "Exporter.h"
#include "core/Model.h"

namespace montage {

struct AafExportResult {
    int audioTracks = 0;  // AAF audio tracks (mono: a stereo track is two)
    int clips = 0;        // source clips placed
    int transitions = 0;  // crossfades
    std::vector<std::string> mediaFiles;
    std::vector<std::string> warnings;
};

bool exportAaf(const Project& p, const Sequence& seq, const std::string& path, AafExportResult* result = nullptr,
               const ExportProgress& progress = {}, const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

}  // namespace montage
