// Montage — tracking and stabilisation for clips: maps a clip's frames to
// the media it shows (trims, speed and reverse included), runs the analysis
// in media/Tracking.h, and turns the result into effect data.
#pragma once

#include <atomic>
#include <map>
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

// ---- Panning that follows the picture (Resolve's IntelliTrack panning) ------------------
// The picture audio clip `a` plays with at timeline frame t: a video clip linked to it on screen then (the shot it was
// recorded with), else the topmost video clip on screen. Null if none.
const Clip* panFollowSource(const Project& p, const Sequence& s, const Clip& a, FrameTime t);
// Where the subject of that picture is at timeline frame t (what stands out or moves against the camera), in
// fractions of the sequence frame: a point to start following from.
bool panFollowSubject(const Project& p, const Sequence& s, const Clip& a, FrameTime t, double& x, double& y,
                      std::string* error = nullptr);
struct PanFollowKey {
    FrameTime t = 0;          // timeline frame
    double x = 0.5, y = 0.5;  // the point followed, in fractions of the sequence frame
};
// Follows the point (x, y) of the sequence frame (fractions) at timeline frame `from` (within clip `a`): a square
// `size` (a fraction of the frame height) wide around it is tracked through that picture both ways, to wherever the
// audio clip or the shot ends first. One key per frame tracked, in timeline order.
bool trackPanFollow(const Project& p, const Sequence& s, const Clip& a, FrameTime from, double x, double y, double size,
                    std::vector<PanFollowKey>& keys, const TrackProgress& progress = {},
                    const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);
// Where something seen at `x` across the sequence frame (a fraction) sounds, as the x and y of SurroundPan: in a flat
// picture somewhere across the front, its edges `width` of the way to the front left and right speakers (±30°); in a
// 360° sequence, the direction round the listener the picture shows there (behind them, y goes negative). Stereo pan
// is the x of a flat picture with its edges hard left and right.
void panFollowPosition(const Sequence& s, double x, double width, bool stereo, double& panX, double& panY);
// Writes the path into audio track `t`'s automation over its span, which is replaced: its pan lane in stereo, else its
// surround x lane (and y in 360° sequences). Lightly smoothed and thinned; just outside the span each lane is left as
// it was. The track reads its lanes (Off becomes Read). False with fewer than two keys.
bool applyPanFollow(const Sequence& s, Track& t, const std::vector<PanFollowKey>& keys, double width);

// ---- Auto Reframe (media/Reframe.h) ------------------------------------------------
// The subject of a picture clip at clip-local frame t, in fractions of its frame.
struct ReframeKey {
    FrameTime t = 0;
    double x = 0.5, y = 0.5;
};
// Finds the subject over the clip (five times a second) and smooths its path:
// speed 0 follows gently, 1 as most shots want, 2 keeps up with fast action.
bool analyzeClipReframe(const Project& p, const Sequence& s, const Clip& c, int speed, std::vector<ReframeKey>& path,
                        const TrackProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                        std::string* error = nullptr);
// Makes clip `c` fill sequence `s`'s frame and keys its position so the
// subject stays as central as the picture allows (replacing position keys).
void applyReframe(const Project& p, const Sequence& s, Clip& c, const std::vector<ReframeKey>& path);
// Every video and still clip of `s`, analysed (by clip id). Fails only if cancelled.
bool analyzeSequenceReframe(const Project& p, const Sequence& s, int speed, std::map<Id, std::vector<ReframeKey>>& paths,
                            const TrackProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                            std::string* error = nullptr);
// A copy of sequence `seq` at width x height (named `name`, or "<name> W:H")
// with the analysed clips reframed; returns its id.
Id makeReframedSequence(Project& p, Id seq, int width, int height, const std::map<Id, std::vector<ReframeKey>>& paths,
                        const std::string& name = {});
// The frame size for an aspect ratio (w:h) keeping `s`'s shorter side.
void reframeSize(const Sequence& s, int aspectW, int aspectH, int& width, int& height);

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
