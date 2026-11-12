// Montage — distort and stylize effects, as Premiere, Resolve and the creator
// apps have them: Wave Warp, Twirl, Spherize, Ripple, Turbulent Displace and
// Motion Tile; Find Edges, Emboss, Halftone, Duotone, VHS, Tilt-Shift Blur and
// Camera Shake. Each works in place on a premultiplied frame; `t` is the
// clip-relative frame (what animates moves with it, so a frame always renders
// the same) and `pixelScale` the frame's pixels per source pixel, so sizes
// given in pixels look the same in previews at lower resolution.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

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
// What VHS and Camera Shake work out for the whole frame, w x h, before they touch a pixel (for render/Processing's
// GPU passes too). VHS: each row's shift, tearing and scanline brightness, and the frame's seed (false when it is off).
struct VhsSetup {
    std::vector<double> shift;
    std::vector<float> torn, line;
    uint32_t seed = 0;
};
bool vhsSetup(const Effect& e, FrameTime t, int w, int h, double pixelScale, VhsSetup& out);
// Camera Shake's move: its offset (pixels), turn (radians) and zoom (false when it leaves the frame as it is).
struct ShakeMove {
    double ox = 0, oy = 0, angle = 0, zoom = 1;
};
bool cameraShakeMove(const Effect& e, FrameTime t, int w, int h, double pixelScale, ShakeMove& out);
// Lens Flare: light from a point (on or off the frame) as a lens would scatter it: a hot core and halo, a star of
// rays (or an anamorphic streak), a ring, and ghosts strung along the line through the frame's centre. Added to the
// picture.
void lensFlare(const Effect& e, FrameTime t, Image& img);
// Its ghosts for a lens (0 the zoom, 1 and 2 the primes, 3 anamorphic): each one's place along the line from the light
// through the centre (1 the centre), radius (a share of half the diagonal), strength and colour.
struct FlareGhosts {
    static constexpr int kMost = 6;
    int count = 0;
    double at[kMost], radius[kMost], strength[kMost];
    float rgb[kMost][3];
};
bool lensFlareGhosts(int lens, FlareGhosts& out);

// Radial Blur: each pixel smeared towards the centre (Zoom, the share of the way) and round it (Spin, degrees through
// the pixel), or both, with Zoom Blur's and Spin's taps (one every 1.5 px the smear reaches, up to 64), bilinear,
// transparent outside. The setup is what the GPU's pass takes too (render/Processing's gpuColorOps).
struct RadialBlur {
    float cx = 0, cy = 0;  // the centre, in the picture's pixels
    float zoom = 0;        // 0..1 of the way to the centre
    float spin = 0;        // radians, half each way
};
bool radialBlurSetup(const Effect& e, FrameTime t, int w, int h, RadialBlur& out);  // false: nothing to do
void radialBlur(const Effect& e, FrameTime t, Image& img, double pixelScale);

// Light Rays: light above a threshold in the picture (scene light, the linear domain), tinted, streaming out from a
// centre (Light X and Y, keyed, or the effect's mask as tracked when `follow` is on): blurred along the lines through
// the centre in three passes of growing length, fading with `decay`; with Shadows Block Rays, the picture's darker
// parts cut the rays crossing them. Added to the picture.
void lightRays(const Effect& e, FrameTime t, Image& img, double pixelScale);
// Shadows Block Rays works on a small copy of the picture (at most 320 pixels across): its size, and the picture's
// columns [x0, x1) and rows [y0, y1) each of its pixels covers.
struct RayShadowGrid {
    int tw = 1, th = 1;
    double shrink = 1;
    std::vector<int> x0, x1, y0, y1;
};
RayShadowGrid rayShadowGrid(int W, int H);

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
// What the GPU draws Aperture Glare from (render/Processing's gpuColorOps), as apertureGlare works it out: the light
// above `threshold`, shrunk f times each way to GW x GH, convolved in tiles T square by FFTs M square (overlap-add)
// with the kernel's spectra (M rows of red and green, then M of blue, complex, as RGBA), the FFTs' twiddles
// (exp(-2 pi i k / M), k < M / 2) and the glare drawn back up by `intensity`. False: nothing to do.
struct GlareSetup {
    double threshold = 1, intensity = 1;
    int f = 1, radius = 2, GW = 0, GH = 0, M = 0, T = 0;
    std::shared_ptr<const std::vector<float>> spectra, twiddles;
};
bool apertureGlareSetup(const Effect& e, FrameTime t, int W, int H, double pixelScale, GlareSetup& out);

// Smooth noise in 3D, about -1..1 (value noise, quintic-smoothed).
double valueNoise(double x, double y, double z, uint32_t seed = 0);

}  // namespace montage::sfx
