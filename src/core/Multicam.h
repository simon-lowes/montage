// Montage — multicam. A multicam clip is a nested sequence flagged `multicam`
// whose video tracks are the camera angles (one per camera, synced) and whose
// audio tracks carry the sound of every source. A timeline clip of it shows
// one angle (Clip::angle) and plays either one audio track or the whole mix
// (Clip::audioAngle). Switching angles cuts the clip; flattening replaces it
// with the chosen angles' own clips.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "EditOps.h"

namespace montage {

// Builds a multicam sequence (and the media item that places it) from media
// items: each one with video becomes an angle, in order, and each one with
// sound gets an audio track. offsets[i] is where item i starts on the multicam
// timeline, in seconds (any origin: the earliest start becomes 0). Returns the
// new media item's id, or 0 and `error`.
Id makeMulticam(Project& p, const std::vector<Id>& media, const std::vector<double>& offsets, const std::string& name,
                std::string* error = nullptr);

// The multicam sequence a clip shows, or nullptr if it is not a multicam clip.
const Sequence* multicamSequence(const Project& p, const Clip& c);
// Angle names (the video tracks' names) of a multicam sequence.
std::vector<std::string> angleNames(const Sequence& mc);
// The audio track carrying angle `angle`'s own sound (the same media), or -1.
int angleAudioTrack(const Sequence& mc, int angle);
// The angle whose source is on audio track `track`, or -1.
int audioTrackAngle(const Sequence& mc, int track);

// Offsets from each media item's start timecode (MediaItem::timecode). False
// if any item has none.
bool timecodeOffsets(const Project& p, const std::vector<Id>& media, std::vector<double>& offsets);

namespace edit {

// Shows `angle` on multicam clip `clipId` (a video clip). With `cut`, the clip
// is first cut at `at` (when that falls inside it) and only the part from `at`
// on switches; without, the whole clip switches. With `audioFollows`, its
// linked multicam audio clips are cut alike and switch to the angle's sound.
Result switchAngle(Project& p, Sequence& s, Id clipId, int angle, FrameTime at, bool cut, bool audioFollows);
// Audio angle of a multicam audio clip: an audio track of the multicam
// sequence, or -1 for all of them mixed.
Result setAudioAngle(Project& p, Sequence& s, Id clipId, int audioAngle);
// Applies angle changes given in multicam-sequence frames (as an automatic
// switch produces them): the clip is cut at every change inside it and each
// part shows the angle in effect there.
Result applyAngleChanges(Project& p, Sequence& s, Id clipId, const std::vector<std::pair<FrameTime, int>>& changes,
                         bool audioFollows);
// Replaces multicam clips (and their linked audio) by the chosen angles' own
// clips, so the cut no longer depends on the multicam sequence. Clips at other
// speeds or reversed are left as they are.
Result flattenMulticam(Project& p, Sequence& s, const std::vector<Id>& clips);

}  // namespace edit

}  // namespace montage
