// Montage — colour-chart calibration (Resolve's Color Match from a chart, the camera-matching step of a grade): an
// X-Rite ColorChecker Classic in a shot is found (or its patches outlined by hand), its 24 patches measured, and a
// colour matrix fitted that brings them as close as it can to the chart's published values, so every shot of a
// scene starts from the same neutral, matched colour. The fit is made in linear light (a 2.4 gamma) as a 3 x 3
// matrix and an offset, applied by the Chart Match effect; how far each patch was off before and after is given as
// CIE76 delta E.
#pragma once

#include <array>

#include "core/Model.h"
#include "media/Image.h"
#include "media/Tracking.h"

namespace montage {

using ChartColor = std::array<double, 3>;
using ChartColors = std::array<ChartColor, 24>;

// The ColorChecker Classic's patches, row by row from dark skin (top left) to black, as sRGB values (0..1).
const ChartColors& colorChecker24();
const char* colorCheckerName(int patch);  // "Dark Skin" ... "Black"

// The 24 patches' colours in `img` (each the mean of its middle half), the patch area outlined by `quad`: its corners
// in fractions of the frame, the chart's top left first (the corner by Dark Skin), then clockwise.
ChartColors sampleChart(const Image& img, const TrackQuad& quad);
// Finds a chart in a picture: upright, upside down or on its side, square to the frame or nearly. Its patch area's
// outline, and how far the patches are from any colour matrix's best (the fit's residual, 0..1); false if none.
bool detectChart(const Image& img, TrackQuad& out, double* residual = nullptr);

struct ChartFit {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    double offset[3] = {0, 0, 0};  // linear light: out = m * in + offset
    double deltaEBefore = 0, deltaEAfter = 0, deltaEMax = 0;  // the patches' mean delta E before and after, the worst after
    bool ok = false;
};
// The matrix bringing `measured` to `reference` (least squares, held near no change when the patches allow less).
ChartFit fitChart(const ChartColors& measured, const ChartColors& reference = colorChecker24());
ChartColor applyChartFit(const ChartFit& f, const ChartColor& rgb);
double deltaE76(const ChartColor& a, const ChartColor& b);  // between two sRGB colours

// The Chart Match effect holding a fit (amount 100 %), and the effect's work on a frame.
Effect chartMatchEffect(const ChartFit& f, Id id);
void applyChartMatch(const Effect& e, FrameTime t, Image& img);

// The picture a clip's chart is read from: its media's frame at the clip's frame `at` (its middle when `at` is
// outside it), as decoded and before any of the clip's effects, at most 1280 wide; empty for a clip without a picture.
Image chartFrame(const Project& p, const Sequence& s, const Clip& c, FrameTime at);
// The fit put on clips as a Chart Match first in their effects (replacing one there); how many it went on.
int applyChartMatchToClips(Project& p, Sequence& s, const std::vector<Id>& clips, const ChartFit& fit);
// The clips of the sequence showing the same media as `clip` (it included).
std::vector<Id> clipsOfSameMedia(const Sequence& s, Id clip);

}  // namespace montage
