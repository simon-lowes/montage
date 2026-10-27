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
// Lens Flare (Premiere's, Resolve's Lens Flare): light from a point (on or off the frame) as a lens would scatter
// it: a hot core and halo, a star of rays (or an anamorphic streak), a ring, and ghosts strung along the line
// through the frame's centre. Added to the picture.
void lensFlare(const Effect& e, FrameTime t, Image& img);

// Light Rays: light above a threshold in the picture (scene light, the linear domain), tinted, streaming out from a
// centre (Light X and Y, keyed, or the effect's mask as tracked when `follow` is on): blurred along the lines through
// the centre in three passes of growing length, fading with `decay`; with Shadows Block Rays, the picture's darker
// parts cut the rays crossing them. Added to the picture.
void lightRays(const Effect& e, FrameTime t, Image& img, double pixelScale);

// Aperture Glare: the diffraction a lens's aperture makes of the highlights (spikes and a soft glow round them).
struct ApertureShape {
    int blades = 6;        // 3 or more
    double rounding = 0;   // 0 straight blades .. 1 round
    double rotation = 0;   // degrees
    bool operator==(const ApertureShape&) const = default;
};
// The far-field diffraction pattern of the aperture at one wavelength (nm): the squared magnitude of its Fourier
// transform, on a grid `size` pixels square (odd, centred), `perLambdaOverD` pixels to each lambda / D at 550 nm (the
// pattern grows with the wavelength), summing to 1.
std::vector<double> apertureDiffraction(const ApertureShape& a, double nm, int size, double perLambdaOverD);
// The glare kernel: the pattern at eight wavelengths from 400 to 700 nm summed into linear Rec.709 RGB (size x size x
// 3, `size` = 2 * radius + 1, round), each channel summing to 1. lambda / D at 550 nm is radius / 20 pixels.
std::vector<float> apertureKernel(const ApertureShape& a, int radius);
void apertureGlare(const Effect& e, FrameTime t, Image& img, double pixelScale);

// Smooth noise in 3D, about -1..1 (value noise, quintic-smoothed).
double valueNoise(double x, double y, double z, uint32_t seed = 0);

}  // namespace montage::sfx
