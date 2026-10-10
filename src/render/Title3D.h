// Montage — extruded 3D titles (Final Cut's 3D titles, Resolve's Fusion 3D text): the text's outline pulled out into
// depth, turned in space and lit, seen through a perspective camera, every setting keyframeable. Drawn by a small
// z-buffered rasteriser at twice the size and filtered down: the front and back faces fill the projected outline
// (nonzero winding, so letters keep their holes; each pixel's depth from the face's plane), and the sides are quads
// along every edge, smooth where the outline curves and sharp at its corners.
#pragma once

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

// The "title3d" generator at frame t of a clip `duration` frames long (for its animation out), w x h output pixels,
// `scale` output pixels per sequence pixel.
Image renderTitle3D(const Effect& g, FrameTime t, int w, int h, double scale, FrameTime duration, double fps);

}  // namespace montage
