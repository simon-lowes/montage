// Montage — loudness measurement (ITU-R BS.1770-4 / EBU R128), cinema loudness (Leq(M), ISO 21727), and the
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
// The same for interleaved audio of any channel count, with BS.1770 weights.
LoudnessResult measureLoudness(const float* data, int64_t frames, int channels, const double* weights, int sampleRate);

// The same measurement fed in pieces, for sound too long to hold at once.
class LoudnessMeter {
public:
    explicit LoudnessMeter(int sampleRate);
    ~LoudnessMeter();
    void add(const float* stereo, int64_t frames);
    // Interleaved audio of `channels` (up to 24), each weighted as BS.1770 says
    // (1 front, 1.41 surround, 0 for the LFE).
    void addChannels(const float* data, int64_t frames, int channels, const double* weights);
    LoudnessResult result() const;
    // Live readings (EBU R128): the last 400 ms and the last 3 s, in LUFS
    // (-70 or below for silence or before any sound), and their highest so far.
    double momentary() const;
    double shortTerm() const;
    double maxMomentary() const;
    double maxShortTerm() const;
    // Loudness range (EBU Tech 3342), in LU: the spread of the short-term
    // loudness, 10th to 95th percentile after gating; 0 until there is enough.
    double loudnessRange() const;
    // Seconds measured so far.
    double seconds() const;
    void reset();

    // Dialogue gating (render/DialogueLoudness.h): each 100 ms step measured is speech or not. Steps finished from
    // now on take `on` (false at first); markSpeech changes one after the fact.
    void setSpeech(bool on);
    void markSpeech(size_t step, bool on);
    size_t steps() const;  // the 100 ms steps finished so far
    // The loudness of the 400 ms ending with step `i` (LUFS, -200 for silence or before it), of the 3 s ending with
    // it (fewer at the start), and the true peak within step `i` (dBTP, -96 for silence).
    double momentaryAt(size_t i) const;
    double shortTermAt(size_t i) const;
    double truePeakAt(size_t i) const;
    // BS.1770 over only the 400 ms blocks that are speech throughout (all four of their steps); the true peak is the
    // whole programme's. Not valid without speech.
    LoudnessResult speechResult() const;
    // The share of the sound (steps above -70 LUFS) marked as speech, 0..1.
    double speechShare() const;
    // Loudness range (EBU Tech 3342) over only the short-term blocks that are mostly dialogue (at least two thirds of
    // their 3 s marked as speech), in LU; 0 without enough of them.
    double speechLoudnessRange() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Leq(M) (ISO 21727), the loudness cinema trailers and advertisements are held to: the programme through the M
// weighting (ITU-R 468's, 5.6 dB lower: 0 dB at 2 kHz, +6.6 dB at 6.3 kHz), its energy summed over the channels and
// averaged over its length, in dB on the cinema's scale (a full-scale sine's energy is 108.01, -20 dBFS pink noise
// in one channel about 85, as leqm-nrt reads it). Each channel is scaled by its gain first (amplitude;
// cinema calibration has the surrounds 3 dB down). The weighting is a linear-phase FIR built from the 468 network's
// response, applied by FFT, true to the curve within 0.01 dB from 31.5 Hz up.
class LeqMMeter {
public:
    // `channels` interleaved; `gains` one per channel (null: all 1).
    LeqMMeter(int sampleRate, int channels, const double* gains = nullptr);
    ~LeqMMeter();
    void add(const float* data, int64_t frames);
    double leqm() const;    // dB (0 for silence)
    double seconds() const;
    // The M weighting's gain at `hz` (dB), as the meter applies it.
    static double weightingDb(double hz);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// A look-ahead peak limiter: no sample leaves louder than the ceiling. The
// gain ramps down over the look-ahead before a peak and recovers smoothly
// after it. Output is delayed by latency() frames.
class PeakLimiter {
public:
    // `channels` interleaved (all limited by one gain, so the image holds).
    PeakLimiter(int sampleRate, double ceilingDb, double lookaheadMs = 5, double releaseMs = 80, int channels = 2);
    // Limits `frames` frames from `in` into `out` (which may be `in`).
    void process(const float* in, float* out, int frames);
    int latency() const { return lookahead_; }

private:
    float ceiling_;
    int channels_;
    int lookahead_;
    double release_;
    double gain_ = 1;
    std::vector<float> delay_;        // the last `lookahead_` frames
    size_t delayPos_ = 0;
    std::deque<std::pair<int64_t, float>> minQueue_;  // sliding minimum of the gains peaks need
    std::vector<float> box_;          // the last `lookahead_` minima, averaged
    size_t boxPos_ = 0;
    double boxSum_ = 0;
    int64_t n_ = 0;
};

}  // namespace montage
