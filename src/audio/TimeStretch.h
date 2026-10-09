// Montage — sound that keeps its pitch when a clip is sped up, slowed down or
// ramped (Premiere's Maintain Audio Pitch, Final Cut's Preserve Pitch), by
// WSOLA (Verhelst and Roelands' waveform-similarity overlap-add): short
// overlapping grains taken from where the clip's time map says, each nudged to
// the offset where it best continues the grain before it, so the waveform
// stays smooth and nothing is resampled.
#pragma once

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
// `outFrames` frames are made at the input's rate, pitch kept.
void wsolaStretch(const AudioBuffer& in, const std::vector<double>& positions, int hop, int64_t outFrames, AudioBuffer& out);

// The same, cached under `key` (which must name the source and the time map;
// `positions` is only asked for when it is not cached): made now with
// `blocking`, else in the background, nullptr until it is ready.
AudioBufferPtr stretchedAudio(const std::string& key, const AudioBufferPtr& source, const std::function<std::vector<double>()>& positions,
                              int hop, int64_t outFrames, bool blocking);
// Stretches still being made in the background.
int stretchesRunning();

}  // namespace montage
