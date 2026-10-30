// Montage — sound that keeps its pitch when a clip is sped up, slowed down or
// ramped (Premiere's Maintain Audio Pitch, Final Cut's Preserve Pitch), by
// WSOLA (Verhelst and Roelands' waveform-similarity overlap-add): short
// overlapping grains taken from where the clip's time map says, each nudged to
// the offset where it best continues the grain before it, so the waveform
// stays smooth and nothing is resampled.
#pragma once

#include <atomic>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "media/Decoder.h"

namespace montage {

// The output hop (samples between grain centres) used at a sample rate.
int stretchHop(int sampleRate);

// Stretches `in` along a time map: output sample k * hop plays around source
// sample `positions[k]` (fractional; the last position is held past the end).
// `outFrames` frames are made at the input's rate and channels, pitch kept (an ambisonic field's four channels cut
// together, guided by W).
// Stops early (the rest silent) when `cancel` is set.
void wsolaStretch(const AudioBuffer& in, const std::vector<double>& positions, int hop, int64_t outFrames, AudioBuffer& out,
                  const std::atomic<bool>* cancel = nullptr);

// WSOLA in two steps, for sounds cut together at the same places (the microphones of one take warped as a group):
// where each grain is taken from, lined up on `guide` (mono; output sample k * hop plays around guide sample
// `positions[k]`; the grains numbered in `locked`, in order, are taken from exactly there, so a transient keeps its
// moment)...
std::vector<int64_t> wsolaGrainStarts(const std::vector<float>& guide, const std::vector<double>& positions, int hop,
                                      int64_t outFrames, const std::atomic<bool>* cancel = nullptr,
                                      const std::vector<int64_t>& locked = {});
// ...then a sound overlap-added from those grains: grain k, from sample starts[k] + shift of `in`, goes to output
// sample k * hop + shift less half a grain. `shift` is where the sound's time is against the guide's.
void wsolaOverlapAdd(const AudioBuffer& in, const std::vector<int64_t>& starts, int hop, int64_t shift, int64_t outFrames,
                     AudioBuffer& out);

// The same, cached under `key` (which must name the source and the time map;
// `positions` is only asked for when it is not cached): made now with
// `blocking`, else in the background, nullptr until it is ready.
AudioBufferPtr stretchedAudio(const std::string& key, const AudioBufferPtr& source, const std::function<std::vector<double>()>& positions,
                              int hop, int64_t outFrames, bool blocking);
// Stretches still being made in the background.
int stretchesRunning();

// Sound sped up or slowed down as it streams (a programme's speed change on export, render/RateConversion.h), read in
// order from `source`, which fills `frames` frames from its next sample on: varispeed (resampled with a windowed sinc,
// its pitch moving with the speed) or its pitch kept (WSOLA, as wsolaStretch, grain by grain). `speed` is source
// samples per output sample (25 / 24 for PAL's speed-up).
class SpeedStream {
public:
    using Source = std::function<void(int frames, float* out)>;
    SpeedStream(int channels, int sampleRate, double speed, bool keepPitch, Source source);
    // The next `frames` output frames, interleaved.
    void read(int frames, float* out);

private:
    void need(int64_t until);   // the source read up to (not including) sample `until`
    float sample(int64_t at, int c) const;
    void grain();               // one more WSOLA grain overlapped in
    int nch_;
    double speed_;
    bool keepPitch_;
    Source source_;
    std::vector<float> src_;    // source samples from srcBase_ on
    std::vector<float> mono_;   // the same as mono, for lining grains up
    int64_t srcBase_ = 0;
    int64_t outDone_ = 0;       // output frames handed out
    // Varispeed: its filter.
    static constexpr int kPhases = 512;
    int taps_ = 0;
    double cutoff_ = 1;
    std::vector<float> kernel_;
    // WSOLA: the hop, its window, the grains so far, and output overlapped but not yet handed out (from accBase_ on).
    int hop_ = 0;
    std::vector<float> window_;
    int64_t grains_ = 0;
    int64_t prev_ = 0;
    bool havePrev_ = false;
    std::vector<float> acc_, weight_;
    int64_t accBase_ = 0;
};

}  // namespace montage
