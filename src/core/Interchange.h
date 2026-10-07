// Montage — timeline interchange: CMX 3600 EDL and OpenTimelineIO export,
// for finishing in other tools (Resolve, Premiere, Avid, Nuke...).
#pragma once

#include <string>

#include "Model.h"

namespace montage {

// CMX 3600 EDL of one video track and the audio tracks (cuts and dissolves).
std::string exportEdl(const Project& p, const Sequence& s, int videoTrack = 0);

// OpenTimelineIO (.otio, JSON) of the whole sequence: every track, gaps,
// transitions, speed changes, markers, generators.
std::string exportOtio(const Project& p, const Sequence& s);

}  // namespace montage
