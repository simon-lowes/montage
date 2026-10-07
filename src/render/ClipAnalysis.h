// Montage — tracking and stabilisation for clips: maps a clip's frames to
// the media it shows (trims, speed and reverse included), runs the analysis
// in media/Tracking.h, and turns the result into effect data.
#pragma once

#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "core/Model.h"
#include "media/Tracking.h"

namespace montage {

// The camera motion over the part of the media the clip shows (with a second
// either side, so smoothing has context), in the form the Stabilize effect
// keeps in strings["motion"].
bool analyzeClipStabilization(const Project& p, const Sequence& s, const Clip& c, std::string& motion,
                              const TrackProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                              std::string* error = nullptr);

// Follows the mask of effect `e` from clip-local frame `fromLocal` to the
// clip's end (forward) or start, as (clip-local frame, region) keys.
bool trackClipMask(const Project& p, const Sequence& s, const Clip& c, const Effect& e, FrameTime fromLocal, bool forward,
                   MotionModel model, std::vector<std::pair<FrameTime, TrackRegion>>& keys,
                   const TrackProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                   std::string* error = nullptr);
// Writes tracked keys into the mask's position, size and rotation,
// replacing the keyframes in the tracked range.
void applyMaskTrack(Effect& e, const std::vector<std::pair<FrameTime, TrackRegion>>& keys);

}  // namespace montage
