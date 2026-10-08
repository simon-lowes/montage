// Montage — music on the timeline: beat and bar markers for a clip, and
// fitting a music clip to a length (media/Beats.h does the listening).
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "core/EditOps.h"
#include "core/Model.h"
#include "media/Beats.h"

namespace montage {

// The beat grid of a clip's media file (kept for the session, by file).
bool mediaBeats(const Project& p, Id mediaId, BeatGrid& out, const std::atomic<bool>* cancel = nullptr,
                std::string* error = nullptr);

// Sequence markers where the clip plays its bars ("Bar 12"), or every beat
// ("12.3": bar 12, beat 3). Replaces the markers this put there before.
// Returns how many were added.
int addBeatMarkers(Sequence& s, const Clip& c, const BeatGrid& g, bool everyBeat);

// Works out how to re-edit the clip's source range so it lasts `target`
// frames (media/Beats.h fitMusic). Segments come back in media seconds.
bool analyzeMusicFit(const Project& p, const Sequence& s, const Clip& c, FrameTime target, MusicFit& fit,
                     const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

// Replaces the clip with one clip per segment, back to back from its start,
// joined by short equal-power crossfades. Each piece starts a fraction of a
// frame into its segment as needed, so the beat runs on across the joins.
// Only for audio clips at normal speed that are not linked to a picture.
edit::Result applyMusicFit(Project& p, Sequence& s, Id clipId, const MusicFit& fit, double crossfadeSeconds = 0.08);

}  // namespace montage
