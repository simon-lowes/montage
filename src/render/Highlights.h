// Montage — Auto Highlights (like the highlight edits Resolve 21.1's assistant
// builds from long footage): the liveliest moments of the chosen videos, laid
// out as a short edit. Each half second is scored by how much louder it is
// than the clip usually is (cheering, laughter, a goal, the chorus), how much
// moves in the picture, and, when a description is given and the footage has
// a visual index, how much it looks like that description. The best peaks are
// taken a few seconds round each, never overlapping, until there is enough;
// they play in the order they happened, with cuts moved off words.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/EditOps.h"
#include "core/Model.h"

namespace montage {

struct HighlightOptions {
    double seconds = 30;                // the edit's length
    double minLength = 2, maxLength = 6;  // of each moment
    std::vector<float> lookFor;         // a CLIP text embedding (media/VisualSearch.h), or empty
    double soundWeight = 1, motionWeight = 1, lookWeight = 1.5;
};

struct HighlightMoment {
    Id media = 0;
    double in = 0, out = 0;  // media seconds
    double score = 0;
};

// Each half second of a video scored as findHighlights scores it (sound above its usual level, movement, the look
// asked for), smoothed over a second either side. Empty if it is not a video.
constexpr double kHighlightStep = 0.5;
std::vector<double> highlightCurve(const Project& p, const MediaItem& m, const HighlightOptions& o = {},
                                   const std::atomic<bool>* cancel = nullptr);

// The moments, in the order they happened (media in the order given). Empty
// (with `error`) if nothing could be read.
std::vector<HighlightMoment> findHighlights(const Project& p, const std::vector<Id>& media, const HighlightOptions& o,
                                            const std::function<void(double)>& progress = {},
                                            const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

// Lays the moments back to back (picture and sound) from `at` on the given tracks.
edit::Result layoutHighlights(Project& p, Sequence& s, const std::vector<HighlightMoment>& moments, FrameTime at = 0,
                              int videoTrack = 0, int audioTrack = 0);

// A new sequence named `name`, sized like the active one, holding the
// moments; it is added to the project (and its media), not made active.
// Returns its id, or 0 if nothing was placed.
Id makeHighlightSequence(Project& p, const std::vector<HighlightMoment>& moments, const std::string& name = "Highlights");

}  // namespace montage
