// Montage — a first mix in one step (like Resolve's AI Audio Assistant or
// Premiere's Essential Sound auto-match): each audio clip is recognised as
// dialogue, music or effects, brought to a loudness for its kind, dialogue
// levels are ridden gently with keyframes, and music dips under speech.
// Everything is ordinary clip volume, so the result can be adjusted by hand.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

enum class AudioRole { Dialogue, Music, Effects, Silence };
const char* audioRoleName(AudioRole r);  // "Dialogue", "Music", "Effects", "Silence"

// What a stretch of sound is, from mono audio at `rate`:
// - speech: strong syllable-rate (2.5-8 Hz) loudness modulation and many
//   short pauses;
// - music: a steady beat (onset autocorrelation) and few pauses;
// - effects: neither (ambience, hits, foley).
// `wordsPerSecond` from a transcript decides for speech when known (>= 0).
struct RoleGuess {
    AudioRole role = AudioRole::Silence;
    double speech = 0;    // 0..1
    double music = 0;     // 0..1
    double loudness = -70;  // integrated LUFS
    // The measurements behind them:
    double pauses = 0;    // share of 10 ms frames far below their second's average
    double syllabic = 0;  // share of loudness modulation at 2.5-8 Hz
    double beat = 0;      // onset autocorrelation at the best tempo
};
RoleGuess classifyAudio(const std::vector<float>& mono, int rate, double wordsPerSecond = -1);

struct MixOptions {
    double dialogueLufs = -18;  // dialogue level (a web mix then lands near -16 LUFS)
    double musicLufs = -22;     // music on its own
    double effectsLufs = -24;
    bool ride = true;           // even out dialogue within each clip
    double rideRangeDb = 6;     // furthest a ride moves from the clip's level
    bool duck = true;           // music dips under dialogue
    double duckDb = -12;
};

// One audio clip's part in the mix.
struct ClipMix {
    Id clip = 0;
    RoleGuess guess;
    AudioRole role = AudioRole::Silence;   // the guess unless overridden
    double gainDb = 0;                     // its new level
    std::vector<std::pair<FrameTime, double>> ride;  // clip-relative frame, dB (dialogue only)
    std::vector<std::pair<FrameTime, double>> shortTerm;  // measured: clip-relative frame, LUFS (3 s)
};

// Listens to every audio clip of the sequence (not muted tracks) and works out the mix.
std::vector<ClipMix> planMix(const Project& p, const Sequence& s, const MixOptions& o,
                             const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                             std::string* error = nullptr);

// Re-works the levels and ride for clips whose role was changed by hand (no listening needed).
void replanClip(ClipMix& m, const MixOptions& o);

// Writes the plan into the clips' volume (level, ride keyframes), then ducks
// the music under the dialogue clips. Returns how many clips changed.
int applyMix(Project& p, Sequence& s, const std::vector<ClipMix>& plan, const MixOptions& o);

}  // namespace montage
