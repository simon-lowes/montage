// Montage — Follow Focus for Lens Blur (Depth): the focus keyed through a clip to stay on what it follows (a point
// tracked from where it is picked, a picked object's AI mask, or a face), as a focus puller keeps it: each frame the
// median depth inside the target, from the clip's steady video depth (media/VideoDepth.h), eased as a puller eases it.
#pragma once

#include <functional>
#include <string>

#include "core/Model.h"

namespace montage {

enum class FocusTarget { Point, Object, Face };

struct FollowFocusSettings {
    FocusTarget target = FocusTarget::Point;
    // The point to follow (or, for a face, to pick the face nearest), fractions of the clip's frame, on clip-local
    // frame `at`.
    double x = 0.5, y = 0.5;
    FrameTime at = 0;
    double ease = 0.3;  // seconds a change of distance is eased over (0: none)
    int size = 518;     // the depth analysis's short side, when the clip has none yet
};

// What the clip's Lens Blur (Depth) `e` (its Focus Depth, 0..100) should be keyed to: a key a frame across the clip,
// fewer where the distance changes evenly. The clip's media is analysed for depth first where its analysis does not
// cover the clip. The object is `e`'s (its Object mask); a point is tracked forwards and backwards from `at`; a face is
// the one there nearest the point, followed through the clip. Where the target is lost the focus holds. `progress`
// (0..1) may return false to stop. False on failure (nothing to follow, no depth, cancelled).
bool followFocus(const Project& p, const Sequence& seq, const Clip& c, const Effect& e, const FollowFocusSettings& s, Param& focus,
                 const std::function<bool(double)>& progress = {}, std::string* error = nullptr);

// The media file and seconds a video clip shows (its first and last frames' media times, whichever way it plays).
bool clipMediaRange(const Project& p, const Sequence& seq, const Clip& c, std::string& path, double& from, double& to);
// Analyses the depth of what the clip shows (media/VideoDepth.h analyzeVideoDepth), so its depth effects are steady.
bool analyzeClipDepth(const Project& p, const Sequence& seq, const Clip& c, int size = 518, const std::function<bool(double)>& progress = {},
                      std::string* error = nullptr);
// Whether that analysis covers the clip.
bool clipDepthAnalysed(const Project& p, const Sequence& seq, const Clip& c);

// The focus a puller gives a target at depths `target` (one a frame at `fps`, negative where it is lost: held): a
// change of distance eased into and out of over `ease` seconds rather than snapped to, and centred on the change
// (the shot is known, as a puller knows it from rehearsal), so a subject moving in depth is not left trailing soft.
std::vector<double> pullFocus(const std::vector<double>& target, double fps, double ease);

}  // namespace montage
