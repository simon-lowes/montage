// Montage — film scans: a negative inverted from its film base to the picture it was exposed for, faded dyes brought back
// towards neutral, and frames held still on their perforations. Used by the Film Negative, Faded Colour Restore and
// Steady on Perforations effects (render/Processing.cpp, render/Compositor.cpp).
#pragma once

#include <string>
#include <vector>

#include "media/Image.h"
#include "media/Tracking.h"
#include "render/ColorSpace.h"

namespace montage {

// ---- Negatives -----------------------------------------------------------------------------------------------------------

// How a scan's code values stand for the light the film let through: by the working space's curve (a scan read as any
// other picture), as that light itself, or as printing density (Cineon: 0.002 density a code value of 1023).
enum class ScanEncoding { Curve, Linear, PrintingDensity };

struct NegativeSettings {
    ScanEncoding encoding = ScanEncoding::Curve;
    double base[3] = {1, 1, 1};  // the film base as code values in the scan's encoding
    double gamma = 0.6;          // the negative's contrast (density over log exposure)
    double exposure = 0;         // stops, on the print
};

// The film base (the unexposed film's own colour: on colour negative its orange mask) as code values: the clearest film
// in the frame (each channel's median over its clearest 2 %), its rebate when it is in the frame, leaving out the
// perforations (clear in every channel) and their edges. False when there is none to find.
bool filmBase(const Image& img, ScanEncoding encoding, Transfer transfer, double base[3]);
// Each channel's density above the base taken as Cineon printing density and turned into light as Cineon's
// conversion does (reference white at 685, black at 95, the negative's gamma), in the working space's curve `transfer`.
void invertNegative(Image& img, const NegativeSettings& s, Transfer transfer);
// The reverse: a picture (in `transfer`'s curve) as a scan of its negative over the base.
void makeNegative(Image& img, const NegativeSettings& s, Transfer transfer);

// ---- Fading --------------------------------------------------------------------------------------------------------------

// Each channel's levels: its darkest (0.5 %), middle and lightest (99.5 %) values.
struct FadeLevels {
    float lo[3] = {0, 0, 0}, mid[3] = {0.5f, 0.5f, 0.5f}, hi[3] = {1, 1, 1};
};
FadeLevels fadeLevels(const Image& img);
// The mean of several frames' levels (a shot's, so the restoration holds steady).
FadeLevels meanFadeLevels(const std::vector<FadeLevels>& levels);
// Faded dyes brought back: each channel's levels and gamma (`levels`, the frame's or the shot's) matched to those of the
// channel that kept the widest range, so the picture's black, white and middle come back neutral; by `strength` (0..1).
void restoreFade(Image& img, const FadeLevels& levels, double strength);

// ---- Perforations --------------------------------------------------------------------------------------------------------

struct PerfSettings {
    int side = 0;          // 0 left, 1 right
    double rebate = 0.12;  // the width searched, a fraction of the frame's
};
// The perforations in the rebate (bright rounded rectangles, the film's holes), their centres in pixels, top to bottom:
// whole ones only, each to a fraction of a pixel. `found` (when given) receives a matte of the holes.
std::vector<Point2> findPerforations(const Image& img, const PerfSettings& s, std::vector<float>* found = nullptr);
// How far the perforations `now` are from where they were (`reference`): the mean offset of each from the nearest of the
// reference's, those further than half their spacing left out. False when none match.
bool perforationOffset(const std::vector<Point2>& now, const std::vector<Point2>& reference, double& dx, double& dy);
// Perforations as text (a line each: x and y as fractions of the frame's width and height) and back, in pixels of a W x H
// frame.
std::string perforationsToString(const std::vector<Point2>& perfs, int W, int H);
std::vector<Point2> perforationsFromString(const std::string& s, int W, int H);

}  // namespace montage
