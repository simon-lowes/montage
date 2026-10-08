// Montage — timeline editing operations.
//
// Every function here mutates a Sequence in place and keeps the invariants
// (clips sorted, never overlapping on a track, transitions referencing
// adjacent clips). Callers snapshot the Project beforehand for undo
// (see History.h), so operations either succeed or leave the model in a
// consistent state; they never need to be reversible themselves.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Model.h"

namespace montage::edit {

struct Result {
    bool ok = true;
    std::string error;
    FrameTime applied = 0;     // delta actually applied (interactive ops clamp)
    std::vector<Id> created;   // ids of clips created by the operation
    static Result fail(std::string e) { return Result{false, std::move(e), 0, {}}; }
};

enum class Edge { In, Out };
enum class TrimMode { Normal, Ripple };

// ---- Lookup ---------------------------------------------------------------
struct ClipLoc {
    TrackRef track;
    size_t index = 0;
};
std::optional<ClipLoc> locate(const Sequence& s, Id clipId);
Clip* clipById(Sequence& s, Id clipId);
const Clip* clipById(const Sequence& s, Id clipId);
const Clip* clipAt(const Sequence& s, TrackRef t, FrameTime frame);
// The clip ids sharing a link group with `clipId`, including itself.
std::vector<Id> linkedClips(const Sequence& s, Id clipId);
std::vector<Id> expandLinks(const Sequence& s, const std::vector<Id>& ids);
// Number of source frames available for a clip (kInfiniteFrames for stills/generators).
FrameTime sourceLimit(const Project& p, const Sequence& s, const Clip& c);
bool trackEmpty(const Track& t, FrameTime a, FrameTime b, const std::vector<Id>& ignore = {});

// ---- Primitive helpers -------------------------------------------------------
// Returns the part of `c` covering timeline [from, to) (must lie inside the clip).
Clip subClip(const Clip& c, FrameTime from, FrameTime to);
// Shifts keyframes of every parameter in the clip by `delta` frames.
void shiftKeyframes(Clip& c, FrameTime delta);
void normalize(Track& t);
void clearRange(Project& p, Track& t, FrameTime a, FrameTime b);
// Splits clip at index into [start,t) and [t,end); returns the new right-hand id or 0.
Id splitClip(Project& p, Track& t, size_t index, FrameTime frame);
// Opens a gap of `len` frames at `at` on the given tracks (splitting clips that span it).
void rippleOpen(Project& p, Sequence& s, FrameTime at, FrameTime len, const std::vector<TrackRef>& tracks);
// Tracks affected by a ripple on `primary`: the primary tracks plus every
// other unlocked, sync-locked track.
std::vector<TrackRef> rippleTracks(const Sequence& s, const std::vector<TrackRef>& primary);

// ---- Edits -------------------------------------------------------------------
Result overwrite(Project& p, Sequence& s, TrackRef t, Clip clip);
Result insert(Project& p, Sequence& s, TrackRef t, Clip clip);
// Places a media item on the timeline (video and/or linked audio).
// srcIn / srcOut are in sequence frames; srcOut < 0 means "to the end".
Result placeMedia(Project& p, Sequence& s, Id mediaId, FrameTime at, double srcIn, double srcOut,
                  TrackRef videoTrack, TrackRef audioTrack, bool insertMode);
Result razor(Project& p, Sequence& s, TrackRef t, FrameTime frame);
Result razorAll(Project& p, Sequence& s, FrameTime frame);
Result removeClips(Project& p, Sequence& s, const std::vector<Id>& ids, bool ripple);
Result moveClips(Project& p, Sequence& s, const std::vector<Id>& ids, FrameTime delta, int videoTrackDelta,
                 int audioTrackDelta, bool insertMode = false);
Result trim(Project& p, Sequence& s, Id clipId, Edge edge, FrameTime delta, TrimMode mode, bool includeLinked = true);
Result roll(Project& p, Sequence& s, Id leftClip, Id rightClip, FrameTime delta);
Result slip(Project& p, Sequence& s, Id clipId, FrameTime delta);
Result slide(Project& p, Sequence& s, Id clipId, FrameTime delta);
Result setSpeed(Project& p, Sequence& s, Id clipId, double speed, bool ripple, bool reverse = false,
                bool includeLinked = true);
Result closeGap(Project& p, Sequence& s, TrackRef t, FrameTime frame);
Result liftRange(Project& p, Sequence& s, FrameTime a, FrameTime b, const std::vector<TrackRef>& tracks);
Result extractRange(Project& p, Sequence& s, FrameTime a, FrameTime b, const std::vector<TrackRef>& tracks);
Result linkClips(Project& p, Sequence& s, const std::vector<Id>& ids);
Result unlinkClips(Sequence& s, const std::vector<Id>& ids);
Result duplicateClips(Project& p, Sequence& s, const std::vector<Id>& ids, FrameTime at);
// Copies clips; returns copies positioned relative to the earliest start (start 0).
struct ClipboardItem {
    TrackRef track;
    Clip clip;
};
std::vector<ClipboardItem> copyClips(const Sequence& s, const std::vector<Id>& ids);
Result pasteClips(Project& p, Sequence& s, const std::vector<ClipboardItem>& items, FrameTime at, bool insertMode);

// ---- Transitions -----------------------------------------------------------------
Result addTransition(Project& p, Sequence& s, Id clipId, Edge edge, const std::string& type, FrameTime duration);
Result removeTransition(Sequence& s, Id transitionId);
Transition* transitionById(Sequence& s, Id id, TrackRef* where = nullptr);
// Timeline range covered by a transition on a track.
bool transitionRange(const Track& t, const Transition& tr, FrameTime& from, FrameTime& to);

// ---- Editing staples -----------------------------------------------------------------
// The nearest clip edge (a clip's start or end on any track) before / after
// `frame`; -1 when there is none.
FrameTime previousClipEdge(const Sequence& s, FrameTime frame);
FrameTime nextClipEdge(const Sequence& s, FrameTime frame);
// Premiere's Q and W: ripple-deletes from the previous edit to `frame`
// (previous) or from `frame` to the next edit, on every unlocked track.
Result rippleTrimToPlayhead(Project& p, Sequence& s, FrameTime frame, bool previous);

// Clip attributes for Paste Attributes and Remove Attributes.
enum Attribute : unsigned {
    AttrMotion = 1,      // position, scale, rotation, anchor, crop, flip (the transform but opacity)
    AttrOpacity = 2,     // opacity and blend mode
    AttrTimeRemap = 4,   // the Time Remapping curve and frame sampling
    AttrVolume = 8,      // clip gain and pan
    AttrEffects = 16,    // the effect stack (pasted ones are added after the clip's own)
    AttrAll = 31
};
// Copies the chosen attributes of `from` (a clip on a `fromKind` track,
// keyframes included) onto the clips: picture ones onto video clips, volume
// onto audio clips, effects onto clips of the same kind.
Result pasteAttributes(Project& p, Sequence& s, const Clip& from, TrackKind fromKind, const std::vector<Id>& to,
                       unsigned what);
// Puts the chosen attributes back to their defaults (effects: removed).
Result removeAttributes(Project& p, Sequence& s, const std::vector<Id>& ids, unsigned what);

// Freezes video clip `clipId` from `frame` on: it is split there and the rest
// holds that frame (0 % Time Remapping). Returns the held part's id.
Result addFrameHold(Project& p, Sequence& s, Id clipId, FrameTime frame);

// Replace Edit: clip `clipId` shows media `mediaId` instead, in the same place
// and length with its effects and transform, the media's frame `srcAlign`
// (sequence frames) landing on timeline frame `at`. Linked clips follow.
Result replaceClip(Project& p, Sequence& s, Id clipId, Id mediaId, double srcAlign, FrameTime at);
// Fit to Fill: overwrites timeline frames [tlIn, tlOut] with source frames
// [srcIn, srcOut] (both inclusive, sequence frames), the speed changed to fit.
Result fitToFill(Project& p, Sequence& s, Id mediaId, double srcIn, double srcOut, FrameTime tlIn, FrameTime tlOut,
                 TrackRef videoTrack, TrackRef audioTrack);

// Swap with Previous / Next Clip (Resolve's swap, Final Cut's reorder): the clip
// and its neighbour on the track change places within the span they share
// (any gap between them stays between them); clips linked to each move with
// it. A transition between the two moves to their new edit; others on their
// edges are removed.
Result swapClip(Project& p, Sequence& s, Id clipId, bool withNext);

// Clips that start at or after `frame` (Track Select Forward), on every track
// or only `track`.
std::vector<Id> clipsFrom(const Sequence& s, FrameTime frame, std::optional<TrackRef> track = {});

// ---- Sequences -------------------------------------------------------------------------
// A copy of sequence `id` with fresh ids throughout (clips, tracks, effects,
// transitions, link groups, buses, caption tracks), named `name` or
// "<name> Copy", added to the project with its own media bin item. Returns
// its id (0 if `id` is unknown).
// `clipIds`, if given, receives old clip id -> new clip id.
Id duplicateSequence(Project& p, Id id, const std::string& name = {}, std::map<Id, Id>* clipIds = nullptr);

// ---- Tracks & markers ---------------------------------------------------------------
TrackRef addTrack(Project& p, Sequence& s, TrackKind kind);
Result removeTrack(Sequence& s, TrackRef t);
void addMarker(Sequence& s, Marker m);
bool removeMarkerAt(Sequence& s, FrameTime frame);

// ---- Snapping & navigation -------------------------------------------------------------
std::vector<FrameTime> snapPoints(const Sequence& s, const std::vector<Id>& exclude, bool includePlayhead = true);
FrameTime snap(const std::vector<FrameTime>& points, FrameTime frame, FrameTime tolerance, bool* snapped = nullptr);
FrameTime nextEdit(const Sequence& s, FrameTime frame);
FrameTime prevEdit(const Sequence& s, FrameTime frame);

// Adopts the media's frame size and rate if the sequence is still empty
// (like "match sequence settings to clip"). Returns true if anything changed.
bool matchSequenceToMedia(Sequence& s, const MediaItem& m);

// ---- Nesting --------------------------------------------------------------------------
// Moves the clips into a new sequence and replaces them with a compound clip.
// The effect chain owned by `owner`: a clip's effects, an audio track's
// inserts, a bus's effects, or (the sequence's own id) the master effects.
// `origin` receives the timeline frame its keyframes count from.
std::vector<Effect>* effectChain(Sequence& s, Id owner, FrameTime* origin = nullptr);
Effect* ownedEffect(Sequence& s, Id owner, Id effect, FrameTime* origin = nullptr);

// Render and Replace: points an audio clip at `media` (its sound with its
// effects baked in, from the clip's first frame), dropping the effects and
// remembering the clip as it was. Restore puts the original back, keeping
// trims made since.
Result replaceWithRender(Sequence& s, Id clip, Id media);
Result restoreUnrendered(Sequence& s, Id clip);

Result makeCompound(Project& p, Sequence& s, const std::vector<Id>& ids, const std::string& name);

}  // namespace montage::edit
