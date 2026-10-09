// Montage — image processing kernels for video effects, blending and transitions.
#pragma once

#include <memory>
#include <string>
#include <utility>
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

// The depth of the frame being worked on (media/DepthMap.h), for depth
// effects and depth qualifiers: set by the compositor for the effects of one
// clip on this thread, null when there is none.
struct DepthMap;
struct ValueMap;
const DepthMap* currentDepth();
class DepthScope {
public:
    explicit DepthScope(std::shared_ptr<const DepthMap> depth);
    ~DepthScope();
    DepthScope(const DepthScope&) = delete;
    DepthScope& operator=(const DepthScope&) = delete;

private:
    std::shared_ptr<const DepthMap> previous_;
};
// The same for the people in the frame (media/Matting.h): Remove Background and People masks.
const ValueMap* currentPersonMatte();
class PersonScope {
public:
    explicit PersonScope(std::shared_ptr<const ValueMap> matte);
    ~PersonScope();
    PersonScope(const PersonScope&) = delete;
    PersonScope& operator=(const PersonScope&) = delete;

private:
    std::shared_ptr<const ValueMap> previous_;
};
// And the faces in the frame (media/Faces.h), for Face Refinement.
struct FaceBox;
const std::vector<FaceBox>* currentFaces();
class FaceScope {
public:
    explicit FaceScope(std::shared_ptr<const std::vector<FaceBox>> faces);
    ~FaceScope();
    FaceScope(const FaceScope&) = delete;
    FaceScope& operator=(const FaceScope&) = delete;

private:
    std::shared_ptr<const std::vector<FaceBox>> previous_;
};
// A soft matte's edge moved out by `expand` px (re-edged by distance, softened over
// `feather`), or else blurred by `blur` px; unchanged when both are zero.
void refineMatte(std::vector<float>& matte, int w, int h, double expand, double feather, double blur);

// The effect's mask (Effects.h maskInfo) over `img` as one value in 0..1 per
// pixel; the qualifier reads the colours of `img`, the depth qualifier
// currentDepth() (selecting nothing without one), an object mask the frame
// of the media at `sourceSeconds`. Empty when there is no mask.
std::vector<float> effectMatte(const Effect& e, FrameTime t, const Image& img, double pixelScale, double sourceSeconds = -1);

// Euclidean distance (px) from each pixel to the nearest pixel where seed is
// non-zero (exact; Felzenszwalb–Huttenlocher). Very large where there is no seed.
std::vector<float> distanceTransform(const std::vector<uint8_t>& seed, int w, int h);

// An object mask's matte at w x h from its logits (ObjectMask.h grid): a
// sub-pixel edge, moved out by `expand` px and softened over `feather` px.
// A matte from a field positive inside and crossing zero at the edge (feather and expansion in pixels).
std::vector<float> fieldMatte(const std::vector<float>& field, int w, int h, double feather, double expand);
// How much of each pixel a polygon (pixel coordinates, non-zero winding) covers, 0 to 1.
std::vector<float> polygonCoverage(const std::vector<std::pair<double, double>>& poly, int w, int h);
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
// Drops a cached .cube so the next load reads the file again (after it was rewritten).
void forgetCubeLut(const std::string& path);

// Monotone cubic curve through "x,y x,y ..." points, sampled into `n` entries.
std::vector<float> buildCurve(const std::string& points, int n = 1024);
// A Hue Curves curve: points "x,y" with x in 0..1 (hue, or luma/saturation) and
// y in 0..1, 0.5 meaning no change; smooth through the points without overshoot,
// wrapping round when `cyclic` (hue), flat past the ends otherwise. No points:
// 0.5 throughout. n + 1 samples over 0..1.
std::vector<float> buildFlatCurve(const std::string& points, int n = 360, bool cyclic = true);

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
// Builds a primary colour correction that makes `img` look like `reference`
// (shot matching): per channel, lift and gain put its shadows and highlights
// (5th and 95th percentiles) on the reference's and gamma its mid-tones
// (medians). The pictures need not show the same thing, only similar light.
Effect colorMatchCorrection(const Image& img, const Image& reference, Id effectId);
// The colour of a green or blue screen in a picture: the cleaner half of the pixels where green (or blue, whichever
// covers more) clearly leads the other two, as a median. False if neither screen colour covers 2 % of the picture.
bool estimateScreenColor(const Image& img, double rgb[3]);

// Flattens premultiplied RGBA over an opaque colour.
void flattenOver(Image& img, float r, float g, float b);

}  // namespace montage
