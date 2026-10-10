// Montage — one value per pixel at a model's resolution (a depth map, a
// matte), sampled at any size.
#pragma once

#include <vector>

namespace montage {

struct ValueMap {
    int width = 0, height = 0;
    std::vector<float> values;  // width * height, row by row
    bool empty() const { return width <= 0 || height <= 0; }
    // Bilinear, at fractions of the picture (0..1 across and down).
    float at(double u, double v) const;
    // Resampled to w x h (pixel centres).
    std::vector<float> resized(int w, int h) const;
};

}  // namespace montage
