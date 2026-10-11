// Montage — audio repair and pitch, worked on a clip's whole source audio
// (like Noise Reduction, see SpeechCleanup.h): with the whole recording to
// hand, a click can be rebuilt from the sound on both sides of it, and a
// pitch shift needs no latency.
//
// De-click (like iZotope RX's De-click or Audition's DeClicker): clicks and
// crackle are found where the sound departs from what a linear predictor
// expects, looking both forwards and backwards in time, so a click is pinned
// to the samples it hit. Those samples are then rebuilt by least-squares
// autoregressive interpolation (Vaseghi and Rayner) from the sound either
// side; everything else is left exactly as it was.
//
// Pitch shift: a phase vocoder with identity phase locking (Laroche and
// Dolson) stretches the sound by the pitch ratio, and a windowed-sinc
// resampler brings it back to its length, so timing holds and the pitch moves.
#pragma once

#include <atomic>

#include "media/Decoder.h"

namespace montage {

// `sensitivity` 0..100 (higher finds quieter clicks); clicks longer than
// `maxClickMs` are taken to be part of the sound and left. `found`, if given,
// is set to the number of clicks repaired.
void declick(const AudioBuffer& in, AudioBuffer& out, double sensitivity, double maxClickMs, int* found = nullptr,
             const std::atomic<bool>* cancel = nullptr);

// Up or down by `semitones` (fractions allowed, within -24..24), same length.
void pitchShift(const AudioBuffer& in, AudioBuffer& out, double semitones, const std::atomic<bool>* cancel = nullptr);

}  // namespace montage
