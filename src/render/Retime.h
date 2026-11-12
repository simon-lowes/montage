// Montage — in-between frames for slow motion and speed ramps: frame
// blending, and optical-flow interpolation that moves pixels along their
// motion instead of cross-fading two pictures.
#pragma once

#include <memory>
#include <string>

#include "media/Image.h"

namespace montage {

struct FlowField;

// a * (1 - t) + b * t (both premultiplied, same size).
Image blendFrames(const Image& a, const Image& b, double t);
// The picture at fraction t between a and b, warped along the optical flow
// from a to b. `cacheKey` names the pair (consecutive in-betweens of the same
// two frames reuse one flow field); empty disables the cache.
Image interpolateFrames(const Image& a, const Image& b, double t, const std::string& cacheKey = {});
// The flow interpolateFrames warps frames `width` wide along (measured on copies at most 480 wide), and `scale`, the
// frames' pixels to the flow's: measured from a to b, or with a or b null the one kept under `cacheKey` (null if none
// is). For the GPU's Smooth Cut, which draws the same warp (render/Compositor).
std::shared_ptr<const FlowField> frameFlow(const Image* a, const Image* b, const std::string& cacheKey, int width, double& scale);

}  // namespace montage
