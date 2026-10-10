// Montage — Match Voice (like Resolve's Dialogue Matcher or Audition's Match
// EQ): makes dialogue recorded on another microphone or in another room
// sound like a reference clip. Both voices' long-term spectra are measured
// (third-octave bands over the speech), and the Parametric EQ is fitted to
// the difference in shape (not level: Auto Mix and clip volume handle that).
#pragma once

#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

// Third-octave band centres, 100 Hz to 10 kHz.
const std::vector<double>& voiceBands();
// The long-term spectrum of the louder (spoken) moments, dB per band.
std::vector<double> speechSpectrum(const std::vector<float>& mono, int rate);

struct VoiceEq {
    double lowDb = 0, b1Db = 0, b2Db = 0, b3Db = 0, highDb = 0;  // the Parametric EQ's gains
    double beforeDb = 0;  // RMS difference in shape before matching
    double afterDb = 0;   // and what the EQ leaves
};
// The EQ setting that turns `target`'s spectrum towards `reference`'s: low
// shelf 120 Hz, bells at 300 Hz, 1.2 kHz and 4 kHz (Q 0.9), high shelf 8 kHz,
// each within +/-12 dB.
VoiceEq fitVoiceEq(const std::vector<double>& target, const std::vector<double>& reference, double rate = 48000);
// The Parametric EQ effect for it, tagged as Match Voice's (strings["match"] = "voice").
Effect voiceEqEffect(Project& p, const VoiceEq& eq);
// Applies an EQ to a spectrum (for checking a match).
std::vector<double> withVoiceEq(const std::vector<double>& spectrum, const VoiceEq& eq, double rate = 48000);

// The spectrum of what a clip plays (its source range, decoded).
bool clipSpeechSpectrum(const Project& p, const Sequence& s, const Clip& c, std::vector<double>& out, std::string* error = nullptr);
// Puts the EQ first in the clip's effects, replacing an earlier Match Voice EQ.
void applyVoiceEq(Project& p, Clip& c, const VoiceEq& eq);
// Matches each clip to the reference: a Match Voice EQ first in its effects
// (replacing an earlier one). Returns how many clips were matched.
int matchVoices(Project& p, Sequence& s, const std::vector<Id>& clips, const std::vector<double>& reference,
                std::string* error = nullptr);

}  // namespace montage
