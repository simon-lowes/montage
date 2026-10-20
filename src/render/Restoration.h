// Montage — restoration: dirt and dust taken out with the frames either side (followed along the motion), thin
// vertical scratches found and filled across, and patches, clone and heal strokes blended in the gradient domain
// (Poisson), so they take on the light around them. Used by the Dirt Removal, Dust Spot Removal, Scratch Removal, Patch
// Fill and Paint effects (render/Processing.cpp, render/Compositor.cpp).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "media/Image.h"
#include "render/VideoDenoise.h"

namespace montage {

// ---- Dirt and dust ------------------------------------------------------------------------------------------------------

struct DirtSettings {
    double sensitivity = 50;  // 0..100: higher finds fainter specks
    double maxSize = 12;      // pixels: a speck is at most this wide or tall (larger differences are left alone)
    double noise = -1;        // the noise's standard deviation (0..1 signal); below 0 it is measured
};
// The dirt in `current`: specks lighter or darker than the same place in both neighbouring frames (followed along the
// motion, `toPrev` and `toNext`, each taken from round about the pixel; empty for none) while those two agree, small,
// and where the motion was followed well around them. A matte, 0..1 a pixel (empty when a neighbour is missing);
// `count` receives the number of specks.
std::vector<float> detectDirt(const Image& current, const Image& prev, const Image& next, const MotionFn& toPrev,
                              const MotionFn& toNext, const DirtSettings& s, int* count = nullptr);
// Dirt that lasts two frames (on `current` and one neighbour): specks standing out from the frames two away (`prev2`,
// `next2`, followed along `toPrev2`, `toNext2`), as detectDirt finds them, that one neighbour (`prev` or `next`) has too
// and the other has not.
std::vector<float> detectLastingDirt(const Image& current, const Image& prev, const Image& next, const Image& prev2, const Image& next2,
                                     const MotionFn& toPrev, const MotionFn& toNext, const MotionFn& toPrev2, const MotionFn& toNext2,
                                     const DirtSettings& s, int* count = nullptr);
// The matte's pixels from the neighbours: their mean along the motion (or the one that reaches there). The motion
// at each pixel is taken from four points `reach` pixels around it (their mean), outside the defect, whose own pull
// on the measured motion would otherwise move the fill.
void fillFromNeighbours(Image& current, const std::vector<float>& matte, const Image& prev, const Image& next,
                        const MotionFn& toPrev, const MotionFn& toNext, double reach = 8);

// A spot marked by hand (Dust Spot Removal): on one frame of the clip, centre and radius as fractions of the frame's width
// and height (radius of the width).
struct DustSpot {
    int64_t frame = 0;
    double x = 0.5, y = 0.5, r = 0.02;
};
std::vector<DustSpot> dustSpotsFromString(const std::string& s);
std::string dustSpotsToString(const std::vector<DustSpot>& spots);
// Whether a spot is marked on that frame (quicker than reading them all).
bool hasDustSpotAt(const std::string& s, int64_t frame);
// A matte over part of a picture: w x h values from (x0, y0).
struct LocalMatte {
    int x0 = 0, y0 = 0, w = 0, h = 0;
    std::vector<float> m;
    std::vector<float> full(int W, int H) const;  // as a matte of the whole W x H picture
};
// What to replace inside a spot: the pixels that differ from the neighbours (more than `sensitivity` allows), grown a
// little, the circle's edge soft. With no neighbours, the whole circle.
LocalMatte dustArea(const Image& current, const Image* prev, const Image* next, const MotionFn& toPrev, const MotionFn& toNext,
                    const DustSpot& spot, double sensitivity);  // (its box: the circle and a few pixels round it)
std::vector<float> dustMatte(const Image& current, const Image* prev, const Image* next, const MotionFn& toPrev,
                             const MotionFn& toNext, const DustSpot& spot, double sensitivity);
// fillFromNeighbours and fillFromSurround over a part of the picture.
void fillFromNeighbours(Image& current, const LocalMatte& matte, const Image& prev, const Image& next, const MotionFn& toPrev,
                        const MotionFn& toNext, double reach);
void fillFromSurround(Image& img, const LocalMatte& matte);

// ---- Scratches -----------------------------------------------------------------------------------------------------------

struct ScratchSettings {
    double sensitivity = 50;  // 0..100
    double maxWidth = 4;      // pixels
    int polarity = 0;         // 0 either, 1 dark, 2 light
    bool follow = false;      // slanting and wandering scratches too
};
// Thin vertical lines lighter or darker than both sides, the same way down most of a band of rows (each eighth of the
// frame on its own, so a scratch may come and go); with `follow`, also lines that slant or wander, followed row by row (a
// pixel either way a row at most) over a quarter of the frame or more. A matte; `count` receives the number of scratch
// pieces.
std::vector<float> detectScratches(const Image& img, const ScratchSettings& s, int* count = nullptr);
// Each row's runs of the matte filled across, from the pixels either side.
void fillAcross(Image& img, const std::vector<float>& matte);

// ---- Patches, clone and heal -----------------------------------------------------------------------------------------------

// img, moved by (dx, dy) pixels: the result's (x, y) is img's (x + dx, y + dy), the edges held.
Image shiftedImage(const Image& img, double dx, double dy);
// Copies `source` (lined up with img) into img where the matte is, by its gradients: the source's detail, levelled
// to what surrounds the area so no seam shows (Poisson blending, solved coarse to fine).
void seamlessPaste(Image& img, const Image& source, const std::vector<float>& matte);
// Fills the matte's area smoothly from what surrounds it (seamlessPaste of a flat source).
void fillFromSurround(Image& img, const std::vector<float>& matte);
// The matte's area replaced by img's own picture (dx, dy) pixels away (the edges held): blended as seamlessPaste does,
// or copied. Only the matte's box is worked on.
void pasteShifted(Image& img, const std::vector<float>& matte, double dx, double dy, bool seamless);

// A clone or heal stroke (Paint effect), as drawn on the Program monitor.
struct PaintStroke {
    bool heal = false;     // blended by its gradients (else copied)
    int64_t frame = 0;     // the clip frame it was drawn on
    bool hold = false;     // on every frame (else on that frame only)
    bool track = false;    // follows the effect's mask box (Track Mask) from the frame it was drawn on
    double dx = 0.05, dy = 0;     // to where it copies from, fractions of the frame's width and height
    double radius = 0.02;         // fraction of the frame's width
    double softness = 0.5;        // 0..1 of the radius
    std::vector<std::array<double, 2>> points;  // fractions of the frame's width and height
};
std::vector<PaintStroke> paintStrokesFromString(const std::string& s);
std::string paintStrokesToString(const std::vector<PaintStroke>& strokes);
// The stroke's coverage of a w x h image, its points and offset already in pixels (radius in pixels).
std::vector<float> strokeMatte(const std::vector<std::array<double, 2>>& points, double radius, double softness, int w, int h);
// A stroke (points and radius in pixels) painted on img from its own picture (dx, dy) pixels away: healed (seamless) or
// cloned. Only the stroke's box is worked on.
void paintStroke(Image& img, const std::vector<std::array<double, 2>>& points, double radius, double softness, double dx,
                 double dy, bool heal);

}  // namespace montage
