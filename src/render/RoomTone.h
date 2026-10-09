// Montage — room tone (iZotope RX's Ambience Match, the dialogue editor's fill): a clip's background, learned from
// its quietest moments (between words), and new sound with the same spectrum and width made to any length, to fill
// a gap in a dialogue track so the room does not drop out under a cut.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

struct RoomToneProfile {
    int sampleRate = 48000;
    int fftSize = 2048;
    std::vector<double> mid, side;  // magnitude spectra (fftSize / 2 + 1 bins)
    double rms = 0;                 // level of the quiet stretches (linear, mid and side together)
    int frames = 0;                 // analysis frames it was learned from
    bool valid() const { return !mid.empty() && rms > 0; }
};

// Learns the background of interleaved stereo audio from its quiet frames (those within 6 dB of the quietest tenth;
// digital silence left out). False with `error` if there is too little to go on.
bool learnRoomTone(const std::vector<float>& stereo, int sampleRate, RoomToneProfile& out, std::string* error = nullptr);
// `frames` stereo samples of noise with the profile's spectrum, width and level (random phases overlap-added; the
// same seed gives the same sound), faded in and out over 10 ms.
std::vector<float> synthesizeRoomTone(const RoomToneProfile& profile, int64_t frames, uint32_t seed = 1);
// The profile of a clip's sound over the part of its media it uses.
bool clipRoomTone(const Project& p, const Sequence& s, const Clip& c, RoomToneProfile& out, std::string* error = nullptr);
// The gap on a track around frame `at`: where it starts and ends (the end is the sequence's when nothing follows) and
// the clips before and after it. False if a clip covers `at`.
struct TrackGap {
    FrameTime start = 0, end = 0;
    Id before = 0, after = 0;
};
bool gapAt(const Sequence& s, TrackRef track, FrameTime at, TrackGap& out);
// A 24-bit stereo WAV.
bool writeStereoWav(const std::string& path, const std::vector<float>& stereo, int sampleRate, std::string* error = nullptr);

}  // namespace montage
