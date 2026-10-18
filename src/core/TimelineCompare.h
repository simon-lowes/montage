// Montage — comparing two versions of a cut (Resolve 21.1's timeline comparison): the clips of the new version
// matched to the old one's by what they play, and what changed between them. A clip that only slid along with the
// edits around it (a ripple) is not a change; one that changed places with its neighbours is.
#pragma once

#include <string>
#include <vector>

#include "Model.h"

namespace montage {

enum class ChangeKind { Added, Removed, Trimmed, Moved, Changed };
const char* changeKindName(ChangeKind k);  // "Added", "Removed", "Trimmed", "Moved", "Changed"

struct TimelineChange {
    ChangeKind kind = ChangeKind::Changed;
    Id before = 0;          // the clip in the old version (0 for Added)
    Id after = 0;           // the clip in the new version (0 for Removed)
    TrackRef track;         // in the new version (the old one's for Removed)
    FrameTime at = 0;       // where, in the new version (Removed: where it was in the old one)
    FrameTime length = 0;
    std::string name;
    std::string details;    // e.g. "in +12, out -30" (source frames), "V1 → V2", "speed 100 % → 50 %, effects"
};

// What changed from `before` to `after` (two sequences of the same project, or of two projects with the same media),
// in the order of the new version (removals where they were).
std::vector<TimelineChange> compareSequences(const Project& p, const Sequence& before, const Sequence& after);
std::vector<TimelineChange> compareSequences(const Project& pBefore, const Sequence& before, const Project& pAfter,
                                             const Sequence& after);

// For each of `count` frames of `after` from `from`, the frame of `before` that shows the same picture, or -1: where every
// clip seen is one of `before`'s (matched as compareSequences matches them) on the same track, changed in nothing but
// its place or trims (and trimmed only when it has no in, out or loop animation), showing the same source frame over
// the same clips there. Frames in transitions, gaps where `before` has pictures, and frames of sequences that differ in
// size, rate or colour are -1. Supplemental IMF packages use it to send only the pictures that changed.
std::vector<FrameTime> samePictureFrames(const Project& p, const Sequence& before, const Sequence& after, FrameTime from,
                                         FrameTime count);

}  // namespace montage
