// Montage — choosing between takes (core/TakeLanes.h suggestTake): the takes under a track over a range, each with
// what it says there (its media's transcript) and how loud it is and whether it clips (its sound decoded).
#pragma once

#include <string>
#include <vector>

#include "core/Model.h"
#include "core/TakeLanes.h"

namespace montage {

// The takes under audio track `track` over timeline [from, to): its lanes that cover it, and the takes of its
// audition clips there (the one playing too).
std::vector<TakeCandidate> takeCandidates(const Project& p, const Sequence& s, TrackRef track, FrameTime from, FrameTime to);

// The takes there and the one to suggest for `line` (an ADR cue's, say): its index in `takes`, -1 for none.
struct TakeChoice {
    std::vector<TakeCandidate> takes;
    int best = -1;
    std::string why;
};
TakeChoice chooseTake(const Project& p, const Sequence& s, TrackRef track, FrameTime from, FrameTime to, const std::string& line);
// Best Take over a track group (core/TrackGroups.h, comp switch): the track's takes, each pass judged by what every
// grouped track recorded of it (the words of the one closest to the line, clipping where any clips, the loudest
// level). The choice is a lane on `track`; compGroup gives its pass to every grouped track.
TakeChoice chooseGroupTake(const Project& p, const Sequence& s, TrackRef track, FrameTime from, FrameTime to, const std::string& line);

// A loop recording in a file (Record Voiceover's Loop takes one in passes; any file will do): its passes from In to
// Out, `length` seconds each after `preroll` seconds, at `sampleRate`, written as 32-bit float WAVs `stem` 1, 2... in
// `folder` (a last pass cut short kept when a second or more). Their paths, `complete` saying which are whole passes;
// empty with `error` when there is not a whole pass. With `channel` (from 0, counted across the file's streams), that
// channel alone, as mono files (a grouped recording's track).
std::vector<std::string> splitLoopRecording(const std::string& path, double preroll, double length, int sampleRate,
                                            const std::string& folder, const std::string& stem, std::vector<bool>* complete,
                                            std::string* error = nullptr, int channel = -1);

}  // namespace montage
