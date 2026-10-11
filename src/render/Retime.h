// Montage — in-between frames for slow motion and speed ramps: frame
// blending, and optical-flow interpolation that moves pixels along their
// motion instead of cross-fading two pictures.
#pragma once

#include <string>

#include "media/Image.h"

namespace montage {

// a * (1 - t) + b * t (both premultiplied, same size).
Image blendFrames(const Image& a, const Image& b, double t);
// The picture at fraction t between a and b, warped along the optical flow
// from a to b. `cacheKey` names the pair (consecutive in-betweens of the same
// two frames reuse one flow field); empty disables the cache.
Image interpolateFrames(const Image& a, const Image& b, double t, const std::string& cacheKey = {});

}  // namespace montage
