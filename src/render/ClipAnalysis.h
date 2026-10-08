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
#include "core/ObjectMask.h"
#include <memory>

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

// ---- Corner pins (planar tracking) -------------------------------------------
// The footage a Corner Pin on `c` follows at clip-local frame `local`: the
// topmost video clip under it on a lower track (the screen or sign being
// replaced), or `c` itself when nothing with a picture is underneath. Null
// if neither has video.
const Clip* cornerTrackSource(const Project& p, const Sequence& s, const Clip& c, FrameTime local);
// Follows the quad the corners of `e` make at clip-local `fromLocal` across
// that footage, to the end (forward) or start of whichever of the two clips
// ends first, as (clip-local frame, corners) keys. Corners stay in `c`'s own
// frame, mapped through both clips' transforms.
bool trackClipCorners(const Project& p, const Sequence& s, const Clip& c, const Effect& e, FrameTime fromLocal,
                      bool forward, std::vector<std::pair<FrameTime, TrackQuad>>& keys,
                      const TrackProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                      std::string* error = nullptr);
// The effect's corners at clip-local `t`, and writing tracked keys into its
// eight corner parameters (replacing the keyframes in the tracked range).
TrackQuad cornerPinQuad(const Effect& e, FrameTime t);
void applyCornerTrack(Effect& e, const std::vector<std::pair<FrameTime, TrackQuad>>& keys);

// ---- Following (titles and graphics that move with the footage) ---------------
// The topmost video clip beneath `c` on a lower track at clip-local `local`; null if none.
const Clip* footageBeneath(const Project& p, const Sequence& s, const Clip& c, FrameTime local);
struct FollowKey {
    FrameTime t = 0;  // clip-local
    double x = 0, y = 0, scale = 100, rotation = 0;  // the clip's pos_x, pos_y, scale and rotation
};
// Moves clip `c` with what is under its position (where its anchor point lands)
// in the footage beneath it: a square `size` (a fraction of the frame height)
// wide around that point is tracked from clip-local `fromLocal` to the end
// (forward) or start of whichever clip stops first. Keys carry position, and
// scale and rotation as the model allows (else as they were).
bool trackClipFollow(const Project& p, const Sequence& s, const Clip& c, FrameTime fromLocal, bool forward,
                     MotionModel model, double size, std::vector<FollowKey>& keys, const TrackProgress& progress = {},
                     const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);
// Writes the keys into the clip's position (and scale and rotation for those
// models), replacing the keyframes in the tracked range.
void applyFollow(Clip& c, const std::vector<FollowKey>& keys, MotionModel model);

// ---- Object masks (media/Segmenter.h) -----------------------------------------
// Frames are the media's own (ObjectMask::fps), so trims, speed changes and
// splits keep the segmentation lined up with the picture.

// The media frame on screen at clip-local frame `local`.
int64_t clipObjectFrame(const Project& p, const Sequence& s, const Clip& c, FrameTime local);
// The effect's object data with `point` added to the clicks at clip-local
// frame `local` (a new ObjectMask when the effect has none).
std::shared_ptr<ObjectMask> withObjectPoint(const Project& p, const Sequence& s, const Clip& c, const Effect& e,
                                            FrameTime local, const ObjectPoint& point);
// The same with the clicks at `local` replaced by `points` (none removes them
// and that frame's segmentation).
std::shared_ptr<ObjectMask> withObjectPrompts(const Project& p, const Sequence& s, const Clip& c, const Effect& e,
                                              FrameTime local, const std::vector<ObjectPoint>& points);
// Segments the clicked frame at clip-local `local` on its own (what the user
// sees straight after clicking), into a copy of `object`.
std::shared_ptr<const ObjectMask> segmentClipObjectFrame(const Project& p, const Sequence& s, const Clip& c,
                                                         const ObjectMask& object, FrameTime local,
                                                         std::string* error = nullptr);
// Follows the object from the nearest clicked frame at or before clip-local
// `fromLocal` (after it, going backwards) to the clip's end (start). Clicked
// frames on the way correct the track. The result replaces those frames.
std::shared_ptr<const ObjectMask> trackClipObject(const Project& p, const Sequence& s, const Clip& c, const ObjectMask& object,
                                                  FrameTime fromLocal, bool forward, const TrackProgress& progress = {},
                                                  const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

}  // namespace montage
