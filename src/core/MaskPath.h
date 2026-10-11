// Montage — drawn masks (Premiere's free-draw Bézier, Resolve's curve
// window, Final Cut's Draw Mask): mask.shape 5 is a closed Bézier path.
//
// The path lives in the mask's box (mask.x / mask.y centre, mask.w / mask.h
// size, mask.rotation), in box units: (-0.5, -0.5) is the box's top left
// corner and (0.5, 0.5) its bottom right. So moving, scaling, rotating or
// tracking the box carries the path with it. Each point is stored as params
// "mask.p<i>.x" / ".y" (the anchor) and ".ix" / ".iy" / ".ox" / ".oy" (its
// incoming and outgoing Bézier handles, relative to the anchor; both zero
// makes a corner). The path is animated as a whole: when it is, every
// coordinate has a key at the same frames, and the keyframe panel shows them
// as one "Mask Path" row. "mask.open" is 1 while the path is still being drawn.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "Model.h"

namespace montage {

struct PathPoint {
    double x = 0, y = 0;    // anchor, box units
    double ix = 0, iy = 0;  // incoming handle, relative to the anchor
    double ox = 0, oy = 0;  // outgoing handle, relative to the anchor
    bool smooth() const { return ix != 0 || iy != 0 || ox != 0 || oy != 0; }
    bool operator==(const PathPoint&) const = default;
};

// The mask's box: centre and size as fractions of the clip's frame, rotation in degrees.
struct MaskBox {
    double x = 0.5, y = 0.5, w = 0.4, h = 0.4, rotation = 0;
};
MaskBox maskBox(const Effect& e, FrameTime t);

// The parameter the keyframe panel shows for the whole path, and whether a
// parameter name is one of the path's coordinates.
inline const std::string kMaskPathParam = "mask.p0.x";
bool isMaskPathParam(const std::string& name);
std::vector<std::string> maskPathParams(const Effect& e);

int maskPathCount(const Effect& e);
std::vector<PathPoint> maskPath(const Effect& e, FrameTime t);
bool maskPathAnimated(const Effect& e);
std::vector<FrameTime> maskPathKeyTimes(const Effect& e);
// The path at t becomes `pts`: with a key on every coordinate when the path
// is animated, else as its fixed shape. Points beyond `pts` are removed.
void setMaskPath(Effect& e, FrameTime t, const std::vector<PathPoint>& pts);
// Animating the path keys every coordinate at t; stopping keeps the path as it is at t.
void setMaskPathAnimated(Effect& e, FrameTime t, bool on);
// Changes the path the same way at each of its keys (or once, when not animated).
void editMaskPath(Effect& e, FrameTime t, const std::function<void(std::vector<PathPoint>&)>& fn);
// Adds a point on segment `seg` (from point seg to the next) at s in (0, 1),
// keeping the curve's shape, at every key. Returns the new point's index.
int insertMaskPoint(Effect& e, FrameTime t, int seg, double s);
bool removeMaskPoint(Effect& e, FrameTime t, int index);
// Corner <-> smooth (automatic handles from the neighbours), at every key.
void toggleMaskPointSmooth(Effect& e, FrameTime t, int index);

// The point at s along a segment, and the segment split there (de Casteljau).
std::pair<double, double> segmentPoint(const PathPoint& a, const PathPoint& b, double s);
// The handles a smooth point gets from its neighbours (Catmull-Rom).
PathPoint smoothed(const std::vector<PathPoint>& pts, int i, bool closed = true);
// The path as a polyline in box units (closed: back to the first point).
std::vector<std::pair<double, double>> flattenMaskPath(const std::vector<PathPoint>& pts, bool closed, int steps = 16);

// Box units <-> fractions of a frame of frameW x frameH pixels.
void boxToFrame(const MaskBox& b, double frameW, double frameH, double bx, double by, double& u, double& v);
void frameToBox(const MaskBox& b, double frameW, double frameH, double u, double v, double& bx, double& by);
// Fits the box to the path's bounds (same rotation), the points following so
// the path stays where it is. False (nothing changed) for a degenerate path.
bool fitMaskBox(MaskBox& box, std::vector<PathPoint>& pts, double frameW, double frameH);

// Finishes drawing (mask.open): needs three points. The box is fitted around
// the path when neither is animated.
bool closeMaskPath(Effect& e, FrameTime t, double frameW, double frameH);

// Makes the effect's mask the closed path through `pts`, given in fractions of
// the clip's frame (anchors, and handles as offsets): shape 5, the box fitted
// around it, unanimated. With `smooth`, points without handles get automatic ones.
void setMaskPathFromFrame(Effect& e, std::vector<PathPoint> pts, double frameW, double frameH, bool smooth);

}  // namespace montage
