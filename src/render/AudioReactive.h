// Montage — pictures that follow the sound. An Audio Visualiser generator
// (Descript's audiograms, CapCut's audio visualisers) draws an audio track's
// sound at each frame: spectrum bars, mirrored bars, the waveform, or bars
// round a circle. Animate to Audio (Resolve's Fairlight animator, After
// Effects' Convert Audio to Keyframes) turns a track's loudness, or one band
// of it, into keyframes on any number setting of a clip: a logo pulsing with
// the beat, a light flickering with speech.
#pragma once

#include <string>
#include <vector>

#include "media/Image.h"
#include "core/EditOps.h"
#include "core/Model.h"

namespace montage {

// The sound of an audio track (1-based; 0 = every audio track summed) around
// timeline frame t: `samples` mono samples at `rate` centred on it, as clips
// play their media (not through effects or faders).
std::vector<float> trackSoundAt(const Project& p, const Sequence& seq, int track, double t, int samples, int rate = 48000);

// Magnitudes of a Hann-windowed FFT of `x` (its size a power of two): size/2 bins.
std::vector<float> spectrum(const std::vector<float>& x);

// An Audio Visualiser clip's picture at timeline frame t, w x h (the sequence's size times the render scale).
Image renderAudioVisualiser(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, int w, int h);

enum class AudioBand { All = 0, Low = 1, Mid = 2, High = 3 };  // all, under 250 Hz, 250 Hz-4 kHz, over 4 kHz

namespace edit {
// Keys on `param` of a clip's effect (`effect` 0 = its Transform) through the
// clip, one a frame thinned to the ones needed, from `low` (silence) to `high`
// (the loudest moment in the clip's span) by the track's level in `band`.
Result animateToAudio(Project& p, Sequence& s, Id clip, Id effect, const std::string& param, int track, AudioBand band, double low,
                      double high);
}  // namespace edit

}  // namespace montage
