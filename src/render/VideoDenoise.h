// Montage — video noise reduction: a motion-compensated temporal filter
// (neighbouring frames warped onto this one along the optical flow, then
// averaged where they match it) followed by an edge-preserving spatial pass
// on luma and chroma, scaled to the noise left where the temporal pass could
// not help. The noise level is measured from the frame unless given.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "media/Image.h"
#include "media/Tracking.h"

namespace montage {

struct DenoiseSettings {
    bool motion = true;         // warp the neighbours along the motion (else only still areas are averaged)
    double temporal = 1;        // 0 off; higher averages more (and risks ghosting on fast motion)
    double spatialLuma = 0.25;  // 0..1
    double spatialChroma = 0.6; // 0..1
    double noise = 0;           // the noise's standard deviation (0..1 signal); 0 measures it
    double blend = 0;           // 0..1 of the original mixed back
};

// The standard deviation of white noise in the image (0..1 signal), from the
// median of the finest diagonal Haar details (Donoho's estimator), averaged
// over R, G and B. Texture raises it a little; flat noisy footage reads true.
double estimateNoise(const Image& img);

// Where the pixel at (x, y) of the current frame is in another frame, as an
// offset in pixels.
using MotionFn = std::function<Point2(double x, double y)>;

// Denoises `current` with its neighbouring frames (any order, the same size;
// none for spatial only). `motion[k]`, when given, is the motion to
// neighbour k; otherwise it is measured. `sigma` receives the noise level used.
Image denoiseFrame(const Image& current, const std::vector<const Image*>& neighbours, const DenoiseSettings& s,
                   const std::vector<MotionFn>& motion = {}, double* sigma = nullptr);

// The motion from frame `base` of a clip to frames base + offsets[i], chained
// from the flow between each pair of consecutive frames. Each of those is
// measured once and kept under `key` (the media and frame size), so playing
// on needs one new flow per frame rather than one per neighbour. `frame(i)`
// gives frame i when a flow has to be measured (nullptr if it cannot). An
// entry is empty where the chain cannot be made.
std::vector<MotionFn> chainedMotion(const std::string& key, int64_t base, const std::vector<int>& offsets,
                                    const std::function<const Image*(int64_t)>& frame);

// The spatial pass alone: a bilateral filter on luma (5x5) and on chroma
// (7x7, guided by luma so colour does not bleed over edges). `noise` holds
// each pixel's remaining noise level, or is empty for `sigma` everywhere.
void spatialDenoise(Image& img, double sigma, double luma, double chroma, const std::vector<float>& noise = {});

}  // namespace montage
