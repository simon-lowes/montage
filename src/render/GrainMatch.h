// Montage — grain matching: a plate's grain measured (how strong it is in each channel as the picture's light rises,
// its size and softness, how its colours go together and how it carries over from frame to frame) and grain made to
// that measure on other pictures, so CG, titles, repairs and cleaned shots sit in the shot.
//
// Measured from consecutive frames: each frame's neighbours two and three frames away, moved onto it along the optical
// flow (to whole pixels, so their grain is not softened) and averaged, are the picture without its grain; what is left
// is the grain (and a little of theirs, allowed for). Only flat areas count: tiles of 64 pixels where the picture
// changes least. Made: Gaussian noise shaped by the measured spectrum in the Fourier domain, mixed across the channels
// by the Cholesky factor of their correlation, carried over between frames as measured, scaled by the picture's own
// light in linear light, from a seed and the frame alone (so any frame renders the same, in any order).
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"
#include "render/VideoDenoise.h"

namespace montage {

struct ColorSpace;

struct GrainProfile {
    static constexpr int kBands = 8;   // of light, a stop each
    static constexpr int kBins = 32;   // of spatial frequency, 0..0.5 cycles a pixel
    // The grain's standard deviation in linear light, per channel, in each band of the picture's linear light (its
    // luma): band b holds lights from 2^(b-8) to 2^(b-7), band 0 everything darker and band 7 everything brighter
    // (in SDR, the top stop below white and above).
    double amplitude[3][kBands] = {};
    // Its power at frequency (i + 0.5) / 64 cycles a pixel of the frame it was measured on, the largest 1.
    double spectrum[kBins] = {};
    double correlation[3] = {0.8, 0.8, 0.8};  // red with green, red with blue, green with blue
    double temporal = 0;                      // from one frame to the next (0..0.45)
    int width = 1920;                         // of the frames it was measured on: the grain scales with the picture
    bool valid() const;
};

std::string grainProfileText(const GrainProfile& g);
bool parseGrainProfile(const std::string& text, GrainProfile& out);
// A profile measured on pictures in `from` as the grain is in `to` (each band's amplitude carried through the conversion
// between them), to take it out of pictures in `to`.
GrainProfile grainProfileIn(const GrainProfile& g, const ColorSpace& from, const ColorSpace& to);
// Film's grain in Film Look's gauges (0 65mm, 1 35mm, 2 16mm, 3 Super 8) as a profile.
GrainProfile stockGrain(int gauge);
// About how large one grain is, in pixels of the frame measured (from where the spectrum falls to half).
double grainSize(const GrainProfile& g);
// The amplitude at linear light `light` in channel c, between the bands' middles.
double grainAmplitude(const GrainProfile& g, int c, double light);

struct GrainRegion {
    int x = 0, y = 0, w = 0, h = 0;  // in pixels of the frames; w or h 0: the whole frame
};

// Measures the grain of consecutive frames (at least 7; premultiplied, in `space`). False with `error` when there
// are too few, nowhere flat enough, or it was cancelled.
bool measureGrain(const std::vector<Image>& frames, const ColorSpace& space, const GrainRegion& region, GrainProfile& out,
                  std::string* error = nullptr, const std::function<void(double)>& progress = {},
                  const std::atomic<bool>* cancel = nullptr);
// The grain of a clip's media: a few runs of seven frames spread over the part the clip uses, in the sequence's
// working space.
bool measureClipGrain(const Project& p, const Sequence& s, const Clip& c, const GrainRegion& region, GrainProfile& out,
                      std::string* error = nullptr, const std::function<void(double)>& progress = {},
                      const std::atomic<bool>* cancel = nullptr);

struct GrainSettings {
    double amount = 1;      // of the measured strength
    double size = 1;        // of the measured size
    double saturation = 1;  // of its colour: 0 the same in every channel
    uint32_t seed = 0;
};
// Adds grain to `img` (premultiplied, in `space`), as frame `t` of a run of it.
void addGrain(Image& img, const ColorSpace& space, const GrainProfile& g, const GrainSettings& s, int64_t t);

// The grain of `current` (premultiplied, in `space`) taken out with its neighbouring frames (Video Noise Reduction's
// temporal and spatial passes; `motion` as denoiseFrame takes it), the grain being as `g` says: the picture is first
// put where its grain is the same strength at every light (each channel's code values through the integral of one
// over the grain's amplitude), so dark and bright parts are cleaned alike.
Image removeGrain(const Image& current, const std::vector<const Image*>& neighbours, const std::vector<MotionFn>& motion,
                  const ColorSpace& space, const GrainProfile& g);

// Replace Grain takes the plate's grain out with this many frames each side. What they leave (in still areas, a 2n + 1
// frame average leaves 1 / (2n + 1) of the grain's power) is allowed for in the grain put back.
constexpr int kReplaceGrainFrames = 3;

// The Grain Match effect's profile: its own measured one, else its Film Look gauge's.
GrainProfile effectGrainProfile(const Effect& e);
void grainMatch(const Effect& e, FrameTime t, Image& img);

}  // namespace montage
