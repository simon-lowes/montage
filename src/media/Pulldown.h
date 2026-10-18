// Montage — 2:3 pulldown removed (Resolve's Remove 3:2 Pulldown, Premiere's Remove Pulldown, Media Composer's film
// projects): video made from 23.976 or 24 frame pictures by spreading each four over five frames (fields A A, B B B,
// C C, D D D, so the frames are A A, B B, B C, C D, D D) woven back into those four, field for field, so they play as
// they were shot with no combed or repeated frames.
// The cadence is read from the fields themselves (detectPulldown), stretch by stretch: each frame's own two fields and
// its first field with the frame before's second are compared for combing, and fields repeated from the frame before
// are found, so an edit made in the video after the pulldown, which breaks the cadence, starts a new stretch where it
// falls. Frames at such a break that lost one of their fields are made from the other (its lines doubled), as an
// online editor's pulldown removal makes them.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Interpretation.h"

struct AVFrame;

namespace montage {

// One frame with the pulldown removed: the file frames its first and second fields come from (-1: missing, made from
// the other), and which field is first.
struct PulldownFrame {
    int64_t first = -1, second = -1;
    bool topFirst = true;
    bool operator==(const PulldownFrame&) const = default;
};

// The frames a file of `fileFrames` frames gives with its cadence `stretches` removed, in order.
std::vector<PulldownFrame> pulldownFrames(const std::vector<PulldownStretch>& stretches, int64_t fileFrames);

// A cadence of one stretch, the file starting at `position` (0 A A, 1 B B, 2 B C, 3 C D), as an Interpretation's
// pulldown value: Interpret Footage's "starting on A" to "D".
std::string pulldownStartingAt(int position, bool topFirst);

// The rate a file plays at with its pulldown removed: four fifths of its own (29.97 to 23.976, 30 to 24).
Rational pulldownRate(Rational fileFps);
// Whether a file at this rate can have 2:3 pulldown removed (29.97 or 30 frames a second).
bool pulldownRateValid(Rational fileFps);

// The cadence of the video at `path` (a file path, without an interpretation), found from its fields: its stretches,
// a break wherever an edit made after the pulldown changed it. Stretches of video with no pulldown in it are given as
// such (position -1). False (with `error`) when it cannot be read, is cancelled, or has no pulldown in it at all.
bool detectPulldown(const std::string& path, std::vector<PulldownStretch>& out, const std::function<void(double)>& progress = {},
                    const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

// A progressive frame woven from `first`'s first field and `second`'s second (both of the same size and format, a
// planar or packed YUV or RGB format FFmpeg decodes to); either may be null, when the frame is made from the other
// field alone, each missing line the average of the lines above and below it. Null when it cannot be made.
AVFrame* weaveFields(const AVFrame* first, const AVFrame* second, bool topFirst);

}  // namespace montage
