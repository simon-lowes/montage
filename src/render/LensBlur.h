// Montage — synthetic focus from a depth map (Lens Blur (Depth)): each pixel blurs by its own circle of confusion,
// from how far its depth is from the focus. The picture is cut into layers by that blur (far to near, soft between
// neighbouring layers), each layer blurred with the aperture's shape and laid over the ones behind it, the layers
// behind first filled in where nearer ones covered them, so a near edge blurs over the background without a dark or
// sharp halo and the background never bleeds over a sharp near subject. The blur is gathered in linear light, and
// highlights can be lifted before it so they open into bright discs (bokeh). Apertures: round or with 3 to 12 blades
// (turned), squeezed tall as anamorphic lenses' are, and cut toward the frame's edges (cat's eye).
#pragma once

#include <cstdint>
#include <vector>

#include "media/Image.h"

namespace montage {

struct TransferTables;

struct Aperture {
    int blades = 0;         // 0: round; 3..12: a polygon
    double rotation = 0;    // degrees
    double squeeze = 1;     // >= 1: the disc that much taller than wide
    double catEye = 0;      // 0..1: how much the shape is cut toward the frame's edges
};

struct LensBlurSettings {
    double focus = 0.8;     // the depth in focus (0 the farthest, 1 the nearest)
    double half = 0.05;     // half the depth range kept sharp around it
    double falloff = 0.3;   // the depth beyond that over which the blur grows to its full radius
    double radius = 12;     // the full blur's radius, pixels
    bool blurNear = true;   // nearer than the focus blurs too (else only farther)
    Aperture aperture;
    double highlights = 0;  // 0..1: how much highlights are lifted before the blur
    double threshold = 0.8; // where highlights start (working-space brightness)
};

// The circle of confusion's radius (pixels) at a depth; negative behind the focus.
double signedCoc(const LensBlurSettings& s, double depth);

// How lensBlur goes about it, worked out from the depth alone (and shared with the GPU's passes, render/Processing):
// each pixel's two layers (the farther, bin0, and the nearer one's share), the highlights' lift, and each layer far to
// near: laid down sharp, or first what is behind filled in over a box, then the layer worked on at 1/f of its size over
// its window and gathered through the aperture's taps.
struct LensBlurTap {
    int dx, dy;     // where the source pixel is, from the one gathered
    double kx, ky;  // the aperture's position (source to output)
    float w;
};
struct LensBlurLayer {
    int k = 0;
    double r = 0;            // its blur's radius (pixels)
    bool sharp = false;      // laid down as it is
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;  // its pixels' box
    bool fill = false;       // what is behind filled in first, over (fx0, fy0) to (fx1, fy1)
    int fx0 = 0, fy0 = 0, fx1 = 0, fy1 = 0;
    int f = 1;               // the scale it is worked at
    double rr = 0;           // its radius there
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0, lw = 0, lh = 0;  // its window, and its size at that scale
    std::vector<LensBlurTap> taps;  // none: nothing gathered (what is behind still filled)
};
struct LensBlurPlan {
    bool none = true;  // nothing to blur
    std::vector<int16_t> bin0;
    std::vector<float> share;
    double liftThreshold = 1, lift = 0;  // highlights: light above the threshold raised by lift u^2
    std::vector<LensBlurLayer> layers;
};
LensBlurPlan lensBlurPlan(const std::vector<float>& depth, int W, int H, const LensBlurSettings& s, const TransferTables* tf);

// Blurs img (premultiplied RGBA in the working space; `tf` decodes it to light, null when it is light already) by
// `depth` (W x H, 0 far .. 1 near).
void lensBlur(Image& img, const std::vector<float>& depth, const LensBlurSettings& s, const TransferTables* tf);

// The aperture's weight (0..1, a pixel's soft edge) at (dx, dy) from a disc's centre, its radius r pixels; (ox, oy)
// moves the second disc that cuts it into a cat's eye (none when catEye is 0).
float apertureWeight(const Aperture& a, double dx, double dy, double r, double ox = 0, double oy = 0);

// The depth at (u, v) (fractions of the frame): the median round about the point.
double depthAt(const std::vector<float>& depth, int w, int h, double u, double v);

// The picture scaled about its centre (a lens breathing as it focuses), the edges held.
void breathe(Image& img, double scale);

}  // namespace montage
