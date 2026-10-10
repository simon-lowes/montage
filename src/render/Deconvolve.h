// Montage — Focus Repair (like Resolve 21's UltraSharpen, for a shot slightly
// out of focus or softened by a lens): Richardson–Lucy deconvolution of the
// picture's brightness with a Gaussian blur of the given size, so detail the
// blur spread out is gathered back rather than edges merely exaggerated as
// Sharpen does. Colour follows the brightness (each pixel scaled), so no
// coloured fringes appear.
#pragma once

#include <vector>

#include "media/Image.h"

namespace montage {

struct FocusRepair {
    double blur = 1.5;    // the blur to undo: its Gaussian sigma, in pixels
    int iterations = 20;  // more recovers more, and amplifies more noise
    double strength = 1;  // 0..1: blended with the original
    double noise = 0.01;  // differences this small (relative) are left alone, so grain is not sharpened
};
void focusRepair(Image& img, const FocusRepair& o);

// A true Gaussian blur of one plane (w x h, row-major), edges clamped.
void gaussianPlane(std::vector<float>& plane, int w, int h, double sigma);

}  // namespace montage
