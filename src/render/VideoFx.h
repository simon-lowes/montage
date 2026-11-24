// Montage — more video effects: Levels, Glow, Film Grain, Directional Blur,
// Chromatic Aberration, Lens Distortion, Corner Pin, Letterbox and Posterize.
// Each works in place on a premultiplied frame; `t` is the clip-relative frame
// and `pixelScale` the frame's pixels per source pixel (sizes are in source
// pixels, so previews at lower resolution look the same).
#pragma once

#include "core/LensProfile.h"
#include "core/Model.h"
#include "media/Image.h"

namespace montage::vfx {

void levels(const Effect& e, FrameTime t, Image& img);
void glow(const Effect& e, FrameTime t, Image& img, double pixelScale);
void filmGrain(const Effect& e, FrameTime t, Image& img, double pixelScale);
// The seed of Film Grain's noise grid at `t` (render/shaders/gpufilter.frag makes the same grid from it).
uint32_t grainSeed(FrameTime t);
void directionalBlur(const Effect& e, FrameTime t, Image& img, double pixelScale);
void chromaticAberration(const Effect& e, FrameTime t, Image& img, double pixelScale);
void lensDistortion(const Effect& e, FrameTime t, Image& img);
// Lens Correction: a lens profile's correction (core/LensProfile) of the frame, its vignetting undone in linear light
// (the working space's), then its distortion, colour fringes and fisheye projection, scaled to fill the frame.
void lensCorrection(const Effect& e, FrameTime t, Image& img);
lens::Settings lensSettings(const Effect& e, FrameTime t);
// Lens Correction for a frame of a size as the GPU draws it (render/shaders/gpufilter.frag's mode 77). The correction is
// radial: where each of red, green and blue comes from is the pixel's offset from the lens's centre times a factor of
// its distance from it, and vignetting's gain a function of that distance too, so `table` holds red's, green's and
// blue's factors and the gain at kSteps + 1 distances evenly over 0..reach (calibration units). False where it changes
// nothing (no profile, or nothing to correct).
struct LensPlan {
    static constexpr int kSteps = 4096;
    bool vignetting = false, geometric = false, fringes = false;  // (fringes: red and blue not from green's place)
    double cx = 0, cy = 0, across = 1, down = 1, reach = 1;       // (calibration units per pixel across and down)
    std::vector<float> table;
};
bool lensCorrectionPlan(const Effect& e, FrameTime t, int width, int height, LensPlan& plan);
void cornerPin(const Effect& e, FrameTime t, Image& img);
void letterbox(const Effect& e, FrameTime t, Image& img);
void posterize(const Effect& e, FrameTime t, Image& img);
// Magnify (Premiere 26.3's magnifying lens): a circle or square of the picture shown enlarged in place, with a soft
// edge, a border and an opacity.
void magnify(const Effect& e, FrameTime t, Image& img, double pixelScale);
// Channel Blur: red, green, blue and alpha blurred by their own amounts, in both or one direction.
void channelBlur(const Effect& e, FrameTime t, Image& img, double pixelScale);
// Noise: random brightness (or colour) noise, different every frame, optionally kept within black and white.
void noise(const Effect& e, FrameTime t, Image& img);

// The projective map taking the unit square's corners (0,0) (1,0) (1,1) (0,1)
// to the four points given; row-major 3×3. False if the points are degenerate.
bool squareToQuad(const double quad[4][2], double h[9]);

}  // namespace montage::vfx
