// Montage — HDR light levels (CTA-861.3 MaxCLL and MaxFALL, which HDR10 deliveries must state and Netflix, Apple and
// YouTube check): the brightest pixel of a programme and its brightest frame on average, measured on what is
// actually rendered rather than guessed from the mastering peak. Exports to HDR measure as they go and write the
// result into the file; Analyse HDR Light Levels measures a sequence ahead of time, for encoders that state the
// levels before the first frame, and the scopes read out the same measures for the frame on screen.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"
#include "render/ColorSpace.h"

namespace montage {

struct LightLevels {
    double maxCll = 0;   // cd/m²: the brightest channel of the brightest pixel in any frame
    double maxFall = 0;  // cd/m²: the highest frame average of the pixels' brightest channels
    FrameTime maxCllFrame = -1, maxFallFrame = -1;  // where (sequence frames)
    int64_t frames = 0;  // frames measured
};

// Measures frames in one display space (PQ, HLG or SDR), through a table on 16-bit code values.
class LightMeter {
public:
    explicit LightMeter(const ColorSpace& space);
    bool valid() const { return valid_; }  // not for camera log or linear light
    // One frame's brightest pixel and average, in nits (premultiplied pixels over black, as programme frames are).
    void measure(const Image& img, double& peak, double& average) const;
    // Adds a frame at `f` to the running levels.
    void add(const Image& img, FrameTime f, LightLevels& into) const;

private:
    const ColorSpace* space_;
    bool valid_ = false;
    std::vector<float> nits_;  // per 16-bit code value (PQ and SDR); HLG's scene light per code value
};

// Renders `from` to `to` (exclusive; a sequence's whole length when `to` is not after `from`) at `scale` of its size
// and measures it in its own colour space. False with `error` for camera log or linear sequences, or when cancelled.
bool measureLightLevels(const Project& p, const Sequence& s, FrameTime from, FrameTime to, LightLevels& out,
                        std::string* error = nullptr, double scale = 1.0, const std::function<void(double)>& progress = {},
                        const std::atomic<bool>* cancel = nullptr);

// The levels as HDR10 states them: whole nits, MaxFALL no more than MaxCLL, at least 1.
void hdr10LightLevels(const LightLevels& l, unsigned& maxCll, unsigned& maxFall);

}  // namespace montage
