// Montage — two colour tools that work on linear light in the working space.
//
// Crosstalk: a 3 x 3 matrix, each row summing to 1 so neutrals stay neutral, edited as handles: each primary's hue
// pulled towards one of its neighbours (red towards green or blue, and so on) and its strength changed (towards grey,
// or past itself). It can start from a matrix fitted to a colour chart (the camera's crosstalk under narrow-band light
// such as LED walls, taken out), the handles working on top of it, and it can be inverted.
//
// Colour Correlation: saturation changed by moving the channels apart or together about their luminance-weighted mean,
// bounded by a gamut (Rec.709, P3, Rec.2020 or primaries of one's own): how far a colour is towards the boundary is
// scaled, and from a starting point on compressed, easing towards the boundary without reaching it (so colours near it
// keep apart rather than clip, and colours outside are brought in, each to its own place). The change can be limited to
// a range of exposure.
#pragma once

#include <array>
#include <string>

#include "core/Model.h"
#include "media/Image.h"
#include "render/ColorChart.h"
#include "render/ColorSpace.h"
#include "render/Gamut.h"

namespace montage {

// ---- Crosstalk ----

// The matrix (row major, on linear light in `space`, the working space) a Crosstalk effect applies at `t`: its handles
// on its fitted matrix, inverted when asked. False when it changes nothing (`m` then the identity).
bool crosstalkMatrix(const Effect& e, FrameTime t, const ColorSpace& space, double m[9]);
// The handles' matrix alone: `hue` in degrees (-60..60, positive towards the next primary: red to green, green to blue,
// blue to red; the primary's own hue on the hexagonal circle) and `strength` (1 as it is, 0 grey) for red, green and
// blue.
void crosstalkHandles(const double hue[3], const double strength[3], double m[9]);
void crosstalk(const Effect& e, FrameTime t, const ColorSpace& space, Image& img);
// The working space's primaries and where the effect takes them, as xy chromaticities (red, green, blue), for the
// chromaticity scope.
std::array<std::array<Xy, 2>, 3> crosstalkPrimaries(const Effect& e, FrameTime t, const ColorSpace& space);
// The 9 numbers of a fitted matrix as the effect keeps them, and back (false for anything else).
std::string crosstalkMatrixText(const double m[9]);
bool parseCrosstalkMatrix(const std::string& text, double m[9]);

// A crosstalk matrix fitted to a chart's patches (as sampleChart gives them): least squares in linear Rec.709 (where it
// is kept, so it holds whatever space the sequence works in), each row summing to 1, no offset, held near no change
// where the patches allow. The chart's delta E before and after the fit, as ChartFit has them (the offset 0).
struct CrosstalkFit {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double deltaEBefore = 0, deltaEAfter = 0, deltaEMax = 0;
    bool ok = false;
};
CrosstalkFit fitCrosstalk(const ChartColors& measured, const ChartColors& reference = colorChecker24());
// A Crosstalk effect holding the fit; put first on the clips (replacing a Crosstalk there), how many it went on.
Effect crosstalkEffect(const CrosstalkFit& f, Id id);
int applyCrosstalkToClips(Project& p, Sequence& s, const std::vector<Id>& clips, const CrosstalkFit& fit);

// ---- Colour Correlation ----

struct CorrelationSettings {
    double saturation = 1;
    double toBoundary[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // linear working space to the boundary's linear RGB
    double fromBoundary[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double luma[3] = {0.2126, 0.7152, 0.0722};  // the boundary's luminance weights
    double start = 0.8;                         // of the way to the boundary where compression starts
    double low = -12, high = 12, soft = 1;      // the exposures changed, in stops from mid grey, and the fall-off
};
CorrelationSettings correlationSettings(const Effect& e, FrameTime t, const ColorSpace& space);
// One pixel of linear light in the working space.
void correlatePixel(const CorrelationSettings& s, double rgb[3]);
void colourCorrelation(const Effect& e, FrameTime t, const ColorSpace& space, Image& img);

}  // namespace montage
