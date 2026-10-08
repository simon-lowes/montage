// Montage — distort and stylize effects, as Premiere, Resolve and the creator
// apps have them: Wave Warp, Twirl, Spherize, Ripple, Turbulent Displace and
// Motion Tile; Find Edges, Emboss, Halftone, Duotone, VHS, Tilt-Shift Blur and
// Camera Shake. Each works in place on a premultiplied frame; `t` is the
// clip-relative frame (what animates moves with it, so a frame always renders
// the same) and `pixelScale` the frame's pixels per source pixel, so sizes
// given in pixels look the same in previews at lower resolution.
#pragma once

#include "core/Model.h"
#include "media/Image.h"

namespace montage::sfx {

void waveWarp(const Effect& e, FrameTime t, Image& img, double pixelScale);
void twirl(const Effect& e, FrameTime t, Image& img, double pixelScale);
void spherize(const Effect& e, FrameTime t, Image& img, double pixelScale);
void ripple(const Effect& e, FrameTime t, Image& img, double pixelScale);
void turbulentDisplace(const Effect& e, FrameTime t, Image& img, double pixelScale);
void motionTile(const Effect& e, FrameTime t, Image& img, double pixelScale);
void findEdges(const Effect& e, FrameTime t, Image& img);
void emboss(const Effect& e, FrameTime t, Image& img, double pixelScale);
void halftone(const Effect& e, FrameTime t, Image& img, double pixelScale);
void duotone(const Effect& e, FrameTime t, Image& img);
void vhs(const Effect& e, FrameTime t, Image& img, double pixelScale);
void tiltShift(const Effect& e, FrameTime t, Image& img, double pixelScale);
void cameraShake(const Effect& e, FrameTime t, Image& img, double pixelScale);

// Smooth noise in 3D, about -1..1 (value noise, quintic-smoothed).
double valueNoise(double x, double y, double z, uint32_t seed = 0);

}  // namespace montage::sfx
