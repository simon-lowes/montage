// Montage — image processing kernels for video effects, blending and transitions.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

// Applies one video filter effect in place. `t` is the clip-relative frame
// (for keyframes); `pixelScale` converts pixel-sized parameters (blur
// radius...) from sequence pixels to this image's pixels.
// `sourceSeconds` is the media time of the frame (for effects that follow
// the footage, such as Stabilize); -1 when there is none.
void applyVideoEffect(const Effect& e, FrameTime t, Image& img, double pixelScale, double sourceSeconds = -1);

// The effect's mask (Effects.h maskInfo) over `img` as one value in 0..1 per
// pixel; the qualifier reads the colours of `img`, an object mask the frame
// of the media at `sourceSeconds`. Empty when there is no mask.
std::vector<float> effectMatte(const Effect& e, FrameTime t, const Image& img, double pixelScale, double sourceSeconds = -1);

// Euclidean distance (px) from each pixel to the nearest pixel where seed is
// non-zero (exact; Felzenszwalb–Huttenlocher). Very large where there is no seed.
std::vector<float> distanceTransform(const std::vector<uint8_t>& seed, int w, int h);

// An object mask's matte at w x h from its logits (ObjectMask.h grid): a
// sub-pixel edge, moved out by `expand` px and softened over `feather` px.
std::vector<float> objectMatte(const std::vector<float>& logits, int w, int h, double feather, double expand);

// 3D LUT loaded from a .cube file.
struct Lut3D {
    int size = 0;
    float domainMin[3] = {0, 0, 0};
    float domainMax[3] = {1, 1, 1};
    std::vector<float> data;  // size^3 * 3, red varies fastest
    bool is1D = false;
    void apply(float& r, float& g, float& b) const;
};
std::shared_ptr<const Lut3D> loadCubeLut(const std::string& path, std::string* error = nullptr);

// Monotone cubic curve through "x,y x,y ..." points, sampled into `n` entries.
std::vector<float> buildCurve(const std::string& points, int n = 1024);

// Composites `src` over `dst` (same size) with a blend mode and opacity.
void blendOnto(Image& dst, const Image& src, const std::string& mode, float opacity);

// Mixes outgoing `a` and incoming `b` (either may be empty = transparent) for
// transition progress u in [0,1]; result has the given size.
Image transitionMix(const std::string& type, const Effect& params, const Image& a, const Image& b, double u, int w,
                    int h);

// Box-blur approximation of a Gaussian, separable; radius in pixels.
void gaussianBlur(Image& img, double radius, bool horizontal = true, bool vertical = true);

// Builds a primary colour correction that neutralises the image's colour
// cast (grey world) and stretches its levels to 0.5 % / 99.5 % percentiles.
Effect autoColorCorrection(const Image& img, Id effectId);

// Flattens premultiplied RGBA over an opaque colour.
void flattenOver(Image& img, float r, float g, float b);

}  // namespace montage
