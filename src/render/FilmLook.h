// Montage — Film Look (like Resolve's Film Look Creator): the marks film
// leaves on a picture, in one effect. Halation (the red-orange glow film's
// base reflects round bright lights), bloom, grain that is strongest in the
// mid-tones, gate weave (the slow wander of film in the gate), softness,
// chromatic aberration, a vignette, flicker and lifted blacks. The gauge
// (65mm, 35mm, 16mm, Super 8) sets how coarse the grain is and how far the
// frame wanders; the amounts set how much of each. Sizes are fractions of
// the frame, so previews and exports look alike, and the random parts are
// seeded by the frame, so a frame always renders the same.
#pragma once

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

struct FilmLookSettings {
    int gauge = 1;  // 0 65mm, 1 35mm, 2 16mm, 3 Super 8
    // 0..1 each.
    double halation = 0.3, bloom = 0.2, grain = 0.3, weave = 0.2, vignette = 0.25, softness = 0.1, flicker = 0, aberration = 0,
           fade = 0.1;
};

FilmLookSettings filmLookSettings(const Effect& e, FrameTime t);

// What the look does to a picture `W` by `H` at clip frame `t`, worked out once for the frame (as filmLook and the GPU
// draw it): the gate weave and aberration in pixels, the softness's blur and how much of it, the halation's and
// bloom's amounts and blurs, the flicker's gain, the lift, the vignette and the grain's grid of noise (3 a cell).
struct FilmLookPlan {
    double wx = 0, wy = 0, aberration = 0;
    bool moved = false;
    double softRadius = 0;  // (0: no softness)
    float softMix = 0;
    double halation = 0, bloom = 0, haloRadius = 0, bloomRadius = 0;
    float flicker = 1, lift = 0;
    double vignette = 0;
    float grain = 0;  // (0: no grain)
    double grainSize = 1;
    int gridW = 0, gridH = 0;
    std::vector<float> noise;
};
FilmLookPlan filmLookPlan(int W, int H, const FilmLookSettings& s, FrameTime t);
// The look on `img` (premultiplied) at clip frame `t`.
void filmLook(Image& img, const FilmLookSettings& s, FrameTime t);

}  // namespace montage
