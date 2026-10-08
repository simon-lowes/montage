// Montage — loudness measurement (ITU-R BS.1770-4 / EBU R128), and the
// limiter loudness-normalised exports use to stay under a peak ceiling.
#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "Decoder.h"

namespace montage {

struct LoudnessResult {
    double integrated = -70.0;  // LUFS (gated)
    double truePeakDb = -96.0;  // true peak (4× oversampled), dBTP
    bool valid = false;         // false if everything was below the absolute gate
};

// Measures interleaved-stereo samples [first, first + count) of `buf`
// (count < 0 = to the end).
LoudnessResult measureLoudness(const AudioBuffer& buf, int64_t first = 0, int64_t count = -1);

// The same measurement fed in pieces, for sound too long to hold at once.
class LoudnessMeter {
public:
    explicit LoudnessMeter(int sampleRate);
    ~LoudnessMeter();
    void add(const float* stereo, int64_t frames);
    LoudnessResult result() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// A look-ahead peak limiter: no sample leaves louder than the ceiling. The
// gain ramps down over the look-ahead before a peak and recovers smoothly
// after it. Output is delayed by latency() frames.
class PeakLimiter {
public:
    PeakLimiter(int sampleRate, double ceilingDb, double lookaheadMs = 5, double releaseMs = 80);
    // Limits `frames` stereo frames from `in` into `out` (which may be `in`).
    void process(const float* in, float* out, int frames);
    int latency() const { return lookahead_; }

private:
    float ceiling_;
    int lookahead_;
    double release_;
    double gain_ = 1;
    std::vector<float> delay_;        // the last `lookahead_` stereo frames
    size_t delayPos_ = 0;
    std::deque<std::pair<int64_t, float>> minQueue_;  // sliding minimum of the gains peaks need
    std::vector<float> box_;          // the last `lookahead_` minima, averaged
    size_t boxPos_ = 0;
    double boxSum_ = 0;
    int64_t n_ = 0;
};

}  // namespace montage
