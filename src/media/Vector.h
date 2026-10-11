// Montage — vector media: Lottie animations (.json, as exported by
// Bodymovin and LottieFiles) and SVG graphics, rendered with ThorVG at the
// size they are shown, so they stay sharp at any scale. They play through
// VideoDecoder like any other footage, with transparency.
#pragma once

#include <memory>
#include <string>

#include "Image.h"

namespace montage {

struct VectorInfo {
    int width = 0, height = 0;
    double fps = 0;        // 0 for a still
    double duration = 0;   // seconds; 0 for a still
    bool animated = false;
};

class VectorDocument;

// True for a .svg file, or a .json file holding a Lottie animation (when
// ThorVG is built in).
bool isVectorPath(const std::string& path);
bool vectorSupport();

std::shared_ptr<VectorDocument> openVector(const std::string& path, VectorInfo& info, std::string* error = nullptr);
// The picture at media time `t` (clamped to the animation), w x h, straight
// alpha. Safe to call from several threads (each document renders one at a time).
Frame16Ptr renderVector(VectorDocument& doc, double t, int w, int h);

}  // namespace montage
