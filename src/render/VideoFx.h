// Montage — more video effects: Levels, Glow, Film Grain, Directional Blur,
// Chromatic Aberration, Lens Distortion, Corner Pin, Letterbox and Posterize.
// Each works in place on a premultiplied frame; `t` is the clip-relative frame
// and `pixelScale` the frame's pixels per source pixel (sizes are in source
// pixels, so previews at lower resolution look the same).
#pragma once

#include "core/Model.h"
#include "media/Image.h"

namespace montage::vfx {

void levels(const Effect& e, FrameTime t, Image& img);
void glow(const Effect& e, FrameTime t, Image& img, double pixelScale);
void filmGrain(const Effect& e, FrameTime t, Image& img, double pixelScale);
void directionalBlur(const Effect& e, FrameTime t, Image& img, double pixelScale);
void chromaticAberration(const Effect& e, FrameTime t, Image& img, double pixelScale);
void lensDistortion(const Effect& e, FrameTime t, Image& img);
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
