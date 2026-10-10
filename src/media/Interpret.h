// Montage — Interpret Footage on a media item: its frame rate conformed, its pixel aspect, alpha and field order
// overridden (core/Interpretation.h says what each does and how it is carried).
#pragma once

#include "core/EditOps.h"
#include "core/Interpretation.h"
#include "core/Model.h"

namespace montage {

// The file's own frame rate, however `m` is read now (0 for a still).
Rational fileFrameRate(const MediaItem& m);
// A camera RAW still or a run of RAW frames (CinemaDNG), which take Camera RAW settings.
bool isRawMedia(const MediaItem& m);

namespace edit {

// Reads `media` (a video or a still; a subclip's media) with `i`, whose `fps` is the rate to play at (0, or the file's
// own rate, for the file's) and whose `fileFps` is filled in here. The media's rate, length, size and start timecode
// follow; its clips keep starting on the same frame and keep their length on the timeline, shortened where the media
// no longer reaches; its subclips, clip markers, transcript and visual and face indexes keep to the same frames; its
// proxy is dropped, to be made again as it is now read. An image sequence's rate is its own (setImageSequenceRate).
// Fails (with an empty message) when nothing changes.
Result interpretFootage(Project& p, Id media, Interpretation i);

}  // namespace edit

}  // namespace montage
