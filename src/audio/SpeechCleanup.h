// Montage — dialogue cleanup: noise reduction and voice isolation.
//
// Both work on a clip's whole source audio rather than block by block: with
// the entire recording available they need no latency compensation, the
// noise print can come from the quietest moments of the file, and the
// result is cached, so seeking and scrubbing cost nothing. Playback uses
// the original audio until the cleaned copy is ready; exports wait for it.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Decoder.h"

namespace montage {

// Effect types processed this way ("denoise", "voice_isolate").
bool isSourceAudioEffect(const std::string& type);
// True if this build includes the RNNoise voice model.
bool hasVoiceIsolation();

// Spectral noise reduction. The noise print is the average spectrum of the
// quietest tenth of the recording; each frequency is then reduced where it
// is not clearly above that print. `reductionDb` limits how far noise is
// turned down, `sensitivity` (0..100) how readily a sound counts as noise.
void reduceNoise(const AudioBuffer& in, AudioBuffer& out, double reductionDb, double sensitivity,
                 const std::atomic<bool>* cancel = nullptr);

// Keeps speech and removes everything else with RNNoise (Xiph, BSD), mixed
// with the original by `amount` (0..100). False if unavailable.
bool isolateVoice(const AudioBuffer& in, AudioBuffer& out, double amount, const std::atomic<bool>* cancel = nullptr);

// The source audio with the clip's source effects applied, in order. With
// `blocking` the work is done now; otherwise it starts in the background and
// nullptr is returned until it is ready.
AudioBufferPtr cleanedAudio(const std::string& path, const AudioBufferPtr& source, const std::vector<const Effect*>& effects,
                            bool blocking);
// Number of cleanups still running in the background.
int cleanupsRunning();

}  // namespace montage
