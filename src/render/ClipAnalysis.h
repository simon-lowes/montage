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
#include "media/CameraSolve.h"
#include "media/Tracking.h"
#include "core/ObjectMask.h"
#include <memory>

namespace montage {

// The camera motion over the part of the media the clip shows (with a second
// either side, so smoothing has context), in the form the Stabilize effect
// keeps in strings["motion"]. With `gyro`, the camera's own motion data too
// when the media has some (media/MotionData.h), lined up with that motion, as
// strings["gyro"] keeps it; empty, with why in `gyroNote`, when it has none or
// it does not match.
bool analyzeClipStabilization(const Project& p, const Sequence& s, const Clip& c, std::string& motion,
                              const TrackProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                              std::string* error = nullptr, std::string* gyro = nullptr, std::string* gyroNote = nullptr);

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

// ---- 3D camera tracking (media/CameraSolve.h) -------------------------------------
// The camera of video clip `c`, solved over the part of its media it shows and stood on its ground (orientScene) when
// the scene has one.
bool solveClipCamera(const Project& p, const Sequence& s, const Clip& c, const CameraSolveOptions& options, CameraSolve& out,
                     const SolveProgress& progress = {}, const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);
// The solve a clip's 3D Camera Tracker effect holds (its strings["solve"]); false when it has none. `effect` gets the
// effect's id.
bool clipCameraSolve(const Clip& c, CameraSolve& out, Id* effect = nullptr);
// The solve's frame showing clip-local frame `local`; -1 when the solve does not reach it.
int cameraSolveFrame(const Sequence& s, const Clip& c, const CameraSolve& solve, FrameTime local);
// A flat rectangle in the scene for a title: centred where the point (x, y) (fractions of the clip's frame) at
// clip-local `local` meets the ground, `size` (a share of its distance from the camera) wide and `aspect` (height /
// width) of that high, lying on the ground or (`upright`) standing on it facing the camera, turned `turn` degrees.
bool sceneRectangleAt(const Sequence& s, const Clip& c, const CameraSolve& solve, FrameTime local, double x, double y, double size,
                      double aspect, bool upright, double turn, std::array<Vec3, 4>& out, std::string* error = nullptr);
// Corner Pin keys that put the whole frame of clip `over` (on a track above `footage`) on the scene rectangle
// `corners` in each frame the two share where the camera is solved, as (`over`-local frame, corners) keys.
bool sceneCornerKeys(const Project& p, const Sequence& s, const Clip& footage, const CameraSolve& solve,
                     const std::array<Vec3, 4>& corners, const Clip& over, std::vector<std::pair<FrameTime, TrackQuad>>& keys,
                     std::string* error = nullptr);
// A title saying `text` over `footage` for its length (on the first track above it with room, a new one if none),
// pinned with a Corner Pin to the scene rectangle `corners` of the footage's solve. Its id, 0 with `error` when it
// cannot be.
Id placeSceneTitle(Project& p, Sequence& s, Id footage, const std::string& text, const std::array<Vec3, 4>& corners,
                   std::string* error = nullptr);

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
// The picture audio clip `a` plays with at timeline frame t. With a point (x, y in fractions of the sequence frame):
// what is seen there, the topmost clip with a file whose frame covers it (titles and other generators are passed
// over), null if that is not video that moves (a still, a nested sequence) or nothing is. Without one: a video clip
// linked to it on screen (the shot it was recorded with), else the topmost video clip on screen.
const Clip* panFollowSource(const Project& p, const Sequence& s, const Clip& a, FrameTime t, double x = -1, double y = -1);
// Where the subject of that picture is at timeline frame t (what stands out or moves against the camera), in
// fractions of the sequence frame: a point to start following from.
bool panFollowSubject(const Project& p, const Sequence& s, const Clip& a, FrameTime t, double& x, double& y,
                      std::string* error = nullptr);
struct PanFollowKey {
    FrameTime t = 0;          // timeline frame
    double x = 0.5, y = 0.5;  // the point followed, in fractions of the sequence frame
};
// Follows the point (x, y) of the sequence frame (fractions) at timeline frame `from` (within clip `a`): a square
// `size` (a fraction of the frame height) wide around it is tracked through the picture seen there both ways, to
// wherever the audio clip or the shot ends first. One key per frame tracked, in timeline order. `source`, when
// given, is set to the clip tracked.
bool trackPanFollow(const Project& p, const Sequence& s, const Clip& a, FrameTime from, double x, double y, double size,
                    std::vector<PanFollowKey>& keys, const TrackProgress& progress = {},
                    const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr, Id* source = nullptr);
// Whether audio track `t` is panned in stereo (its pan) rather than placed with its surround panner: in a stereo
// sequence, and in surround when it goes to a bus (buses are stereo inside).
bool panFollowStereo(const Sequence& s, const Track& t);
// Where something seen at `x` across the sequence frame (a fraction) sounds. Stereo: the pan, the picture's edges
// `width` of the way to hard left and right (`panY` 1). Surround: the direction as SurroundPan's x and y at distance
// 1, a flat picture across the front (its edges `width` of the way to the front left and right speakers, ±30°, in
// perspective), a 360° sequence's picture all the way round the listener (behind them, y goes negative).
void panFollowPosition(const Sequence& s, double x, double width, bool stereo, double& panX, double& panY);
// Writes the path into audio track `t`'s automation over its span, which is replaced: its pan lane in stereo, else its
// surround x and y lanes (the direction followed at the distance the track was heard at, so a sound kept nearer the
// middle stays there). Lightly smoothed and thinned. Outside the span what was heard stays: a reading track's lanes as
// they were (keys just outside hold them), and a track not reading its lanes (Off, Write) has them replaced, its
// fader's value kept outside the span, and is set to Read. False with fewer than two keys.
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
// A mask holds one object or several (ObjectMask::more); `which` picks the one the clicks and tracking are for (0 the
// first; the count adds a new one).
//
// The effect's object data with `point` added to the clicks at clip-local
// frame `local` (a new ObjectMask when the effect has none).
std::shared_ptr<ObjectMask> withObjectPoint(const Project& p, const Sequence& s, const Clip& c, const Effect& e,
                                            FrameTime local, const ObjectPoint& point, int which = 0);
// The same with the clicks at `local` replaced by `points` (none removes them
// and that frame's segmentation).
std::shared_ptr<ObjectMask> withObjectPrompts(const Project& p, const Sequence& s, const Clip& c, const Effect& e,
                                              FrameTime local, const std::vector<ObjectPoint>& points, int which = 0);
// Another object, not clicked yet, added to the effect's mask (or cut out of it when `subtract`); its index in `which`.
// A mask with no object yet gets its first, which always adds: null for one to cut out (there is nothing to cut from).
std::shared_ptr<ObjectMask> withNewObject(const Project& p, const Sequence& s, const Clip& c, const Effect& e, bool subtract,
                                          int* which = nullptr);
// The mask without object `which`; null when none is left. Without the first, the next object that adds takes its place
// (the others keep their order); when only objects cut out are left, an empty first object stays to cut them from
// (removing that empty first removes the mask).
std::shared_ptr<ObjectMask> withoutObject(const Effect& e, int which);
// Object `which` (not the first) cut out of the mask or added to it again.
std::shared_ptr<ObjectMask> withObjectSubtracted(const Effect& e, int which, bool subtract);
// Object `chosen` of the effect's mask, clamped to the objects it has (which object is being picked is the editor's
// choice, not part of the project: EditorState::maskObject).
int currentObject(const Effect& e, int chosen);
// Segments the clicked frame at clip-local `local` on its own (what the user
// sees straight after clicking), into a copy of `object`.
std::shared_ptr<const ObjectMask> segmentClipObjectFrame(const Project& p, const Sequence& s, const Clip& c,
                                                         const ObjectMask& object, FrameTime local,
                                                         std::string* error = nullptr, int which = 0);
// Follows the object from the nearest clicked frame at or before clip-local
// `fromLocal` (after it, going backwards) to the clip's end (start). Clicked
// frames on the way correct the track. The result replaces those frames.
std::shared_ptr<const ObjectMask> trackClipObject(const Project& p, const Sequence& s, const Clip& c, const ObjectMask& object,
                                                  FrameTime fromLocal, bool forward, const TrackProgress& progress = {},
                                                  const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr,
                                                  int which = 0);

}  // namespace montage
