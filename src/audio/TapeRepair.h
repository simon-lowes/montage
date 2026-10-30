// Montage — tape and archive sound repair, on a clip's whole source audio (as audio/AudioRepair.h).
//
// Wow and flutter: the speed the tape passed the head at is read from the steady partials in the sound (a pilot or
// hum tone, sustained notes): each one's frequency followed hop by hop from the phase of its own DFT, against its usual
// frequency. The speed's slow drift (wow, under 4 Hz) and fast wobble (flutter, above) each take their own amount, and
// the sound is resampled along the inverse of that time map with a windowed sinc, so pitch and timing come back
// together (unlike a time stretch).
//
// Azimuth: a tape head out of square delays one channel against the other by a fraction of a sample to a few
// samples. The delay is found from the cross-spectrum (GCC-PHAT for the whole samples, then the slope of its phase for
// the fraction), for the clip or adaptively over time, and the right channel is read that much earlier with a
// fractional-delay filter; a channel wired the wrong way round is turned back over.
//
// Strip Silence: the stretches of a clip quieter than a threshold for long enough are found, padded before and after,
// so the clip can be cut to its sound (core/EditOps.h edit::stripSilence).
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

#include "core/Model.h"
#include "media/Decoder.h"

namespace montage {

// The transfer's speed over time: speed[i] at start + i * hop seconds (1 = as recorded; the mean is 1).
struct TransferSpeed {
    double start = 0, hop = 0;
    std::vector<double> speed;
    double at(double seconds) const;
};
// `pilotHz` names a tone the tape carries (within 3 %); 0 finds the steady partials. Empty when none is found.
TransferSpeed measureTransferSpeed(const AudioBuffer& in, double pilotHz = 0, const std::atomic<bool>* cancel = nullptr);

// Wow and flutter taken out by `wow` and `flutter` (0..1 each). `deviation`, if given, is set to the largest speed
// error found (a fraction: 0.003 is 0.3 %). With nothing to correct (no steady partials, or nine readings in ten within
// 0.003 %), `out` is `in` exactly. False when cancelled.
bool removeWowFlutter(const AudioBuffer& in, AudioBuffer& out, double wow, double flutter, double pilotHz = 0,
                      const std::atomic<bool>* cancel = nullptr, double* deviation = nullptr);

// How far the right channel (the second) lags the left, in samples (negative: it leads), over [from, to) (to < 0:
// the end), and whether it is inverted. `coherence` 0..1: how much of the two channels is the same sound.
struct Azimuth {
    double delay = 0;
    bool inverted = false;
    double coherence = 0;
};
Azimuth measureAzimuth(const AudioBuffer& in, int64_t from = 0, int64_t to = -1);

// The right channel brought into line with the left: by one delay for the clip, or (`adaptive`) one followed over time;
// turned over too when inverted and `polarity`. `delay`, if given, is set to the clip's delay. Anything but two channels,
// or two with little in common (coherence under 0.3), is left as it is.
bool correctAzimuth(const AudioBuffer& in, AudioBuffer& out, bool adaptive, bool polarity, const std::atomic<bool>* cancel = nullptr,
                    double* delay = nullptr);

// The stretches with sound, in samples [from, to): where any channel passes `thresholdDb` (dBFS), joined across
// quieter gaps shorter than `minSilence` seconds, then `padBefore` and `padAfter` seconds wider (joined where they meet).
std::vector<std::pair<int64_t, int64_t>> soundRegions(const AudioBuffer& in, double thresholdDb, double minSilence, double padBefore,
                                                      double padAfter);
// Those regions (of the clip's source sound at `rate`, sample 0 at media time 0) as the sequence frames of clip `c` they
// play at: in-points rounded down and out-points up, within the clip. Empty for a clip at another speed than 1.
std::vector<std::pair<FrameTime, FrameTime>> clipSoundFrames(const Sequence& s, const Clip& c, const std::vector<std::pair<int64_t, int64_t>>& regions,
                                                             int rate);

}  // namespace montage
