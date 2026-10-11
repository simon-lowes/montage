// Montage — shape layers: the "shape" generator's rectangle, ellipse,
// polygon, star, line or arrow, with a solid or gradient fill, a stroke that
// can be dashed, and trim paths (Trim Start / End / Offset, keyframed to draw
// the outline on). Like After Effects' shape layers, everything animates.
#pragma once

#include <QPainterPath>

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

// The shape's outline in output pixels (w x h frame, `scale` output pixels per
// sequence pixel), before trimming. Open for a line.
QPainterPath shapeOutline(const Effect& g, FrameTime t, int w, int h, double scale);

// The part of `path` from `start` to `end` (fractions of its length), moved
// round by `offset` (a fraction, wrapping on closed paths).
QPainterPath trimPath(const QPainterPath& path, double start, double end, double offset);

Image renderShape(const Effect& g, FrameTime t, int w, int h, double scale);

}  // namespace montage
