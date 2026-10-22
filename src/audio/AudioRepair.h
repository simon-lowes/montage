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

// De-clip: samples that were clipped (at or past the level the recording was cut off at, found where the loudest
// samples pile up, hard or soft, or `clipLevelDb` when it is under 0 dBFS) rebuilt by AR interpolation from the sound
// around them, held beyond the clip level (each side of zero judged by itself). Everything else is left as it was,
// unless the rebuilt peaks pass full scale: then the whole is lowered just enough to bring them under it. `found` is
// set to the number of clipped runs repaired. False when cancelled.
bool declip(const AudioBuffer& in, AudioBuffer& out, double clipLevelDb, int* found = nullptr, const std::atomic<bool>* cancel = nullptr);

// De-plosive: a burst under 150 Hz that rises fast and outweighs the rest of the sound (a popped P or B) has its low
// band turned down for its length (up to 80 ms), to the level that band usually has around it, by `reductionDb` at
// most; the voice's own low end and everything above are left alone. `sensitivity` 0..100. `found` is set to the
// number of plosives.
void deplosive(const AudioBuffer& in, AudioBuffer& out, double sensitivity, double reductionDb, int* found = nullptr,
               const std::atomic<bool>* cancel = nullptr);

// De-wind: with two microphones, each channel's sound under about 600 Hz is turned down by how little it agrees with
// the other's (wind on one mic is not on the other; a voice is on both); with one (or one silent), the lowest bins (under 100 Hz) are
// held to their recent level, so gusts are cut. `amount` 0..100 scales the reduction, `maxReductionDb` limits it.
void dewind(const AudioBuffer& in, AudioBuffer& out, double amount, double maxReductionDb, const std::atomic<bool>* cancel = nullptr);

// Up or down by `semitones` (fractions allowed, within -24..24), same length.
void pitchShift(const AudioBuffer& in, AudioBuffer& out, double semitones, const std::atomic<bool>* cancel = nullptr);

}  // namespace montage
