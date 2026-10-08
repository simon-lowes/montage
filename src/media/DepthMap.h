// Montage — depth maps (like Resolve's Depth Map): how near each part of a
// picture is, from the picture alone, with Depth Anything V2 Small (Lihe
// Yang et al., Apache-2.0) on ONNX Runtime. Effects use it to work by
// distance: a depth range as a qualifier for any effect, lens blur that
// keeps one distance sharp, fog that thickens with distance.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Image.h"
#include "ModelFiles.h"

namespace montage {

// The model (99 MB), downloaded on first use; $MONTAGE_DEPTH_MODEL names
// another folder, $MONTAGE_DEPTH_MODEL_URL a mirror.
const ModelPack& depthModel();
bool depthAvailable();  // built with ONNX Runtime

// Relative depth: 0 for the farthest parts of the picture, 1 for the nearest
// (the model's disparity stretched between its 1st and 99th percentiles).
struct DepthMap {
    int width = 0, height = 0;
    std::vector<float> values;  // width * height, row by row
    bool empty() const { return width <= 0 || height <= 0; }
    // Bilinear, at fractions of the picture (0..1 across and down).
    float at(double u, double v) const;
    // Resampled to w x h (pixel centres).
    std::vector<float> resized(int w, int h) const;
};

// The depth of a picture (premultiplied RGBA, display-referred 0..1), the
// model run with the short side at `size` pixels (a multiple of 14; 518 is
// what it was trained at). False if the model is missing or fails.
bool estimateDepth(const Image& img, DepthMap& out, int size = 518, std::string* error = nullptr);

// The last few depth maps, by what went in (sampled) and the size: a paused
// or repainted frame is not run through the model again. Null on failure.
std::shared_ptr<const DepthMap> cachedDepth(const Image& img, int size = 518);

}  // namespace montage
