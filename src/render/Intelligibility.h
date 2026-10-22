// Montage — dialogue intelligibility: how much of what is said can be made out over the music and effects under it,
// as the Speech Intelligibility Index (ANSI S3.5-1997, its one-third-octave band procedure: 18 bands from 160 Hz to
// 8 kHz, each band's speech-to-masker ratio, upward spread of masking and the speech's own masking, the hearing
// threshold at the level it is played, weighted by how much each band matters to understanding speech). The dialogue
// is what the sequence's Dialogue clips play and the masker the rest of the mix (the mix less them); with no Dialogue
// clips, the mix is split with the dialogue, music and effects model (media/BanditSeparation.h) when it is installed.
// Measured every 400 ms where someone speaks. 0 understands nothing, 1 everything; under about 0.45 speech is hard to
// follow, over about 0.75 it is easy. Used by Sequence › Dialogue Intelligibility…, the timeline's intelligibility
// lane, Quality Check and MCP `montage_intelligibility`.
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/Model.h"

namespace montage {

constexpr int kSiiBands = 18;
const std::array<double, kSiiBands>& siiBandCentres();      // Hz, 160 .. 8000
const std::array<double, kSiiBands>& siiBandImportance();   // summing to 1

// The index from each band's speech and masker levels (dB SPL in the band). Band levels under the hearing threshold
// count as silence.
double speechIntelligibilityIndex(const std::array<double, kSiiBands>& speechDb, const std::array<double, kSiiBands>& maskerDb);

// How the figure is judged: under `poor` red, under `fair` amber, else green.
struct IntelligibilityLimits {
    double poor = 0.45;
    double fair = 0.6;
};
// Named presets ("Standard", "Accessible" (stricter, for viewers hard of hearing), "Lenient"); false for an unknown name.
bool intelligibilityPreset(const std::string& name, IntelligibilityLimits& out);
const std::vector<std::string>& intelligibilityPresetNames();

enum class DialogueSource { Auto, Roles, Separated };

struct IntelligibilityOptions {
    DialogueSource source = DialogueSource::Auto;  // Auto: Dialogue clips, else separated from the mix
    std::vector<bool> tracks;                       // the audio tracks heard (true = in); empty = all
    FrameTime in = 0, out = -1;                     // out -1 = the sequence's end
    double listeningLevel = 79;                     // dB SPL a -20 dBFS (RMS) sound plays at (a TV mixing room's)
    double block = 0.4;                             // seconds measured at a time
};

struct IntelligibilityBlock {
    double start = 0, end = 0;  // timeline seconds
    bool speech = false;        // someone speaks (otherwise there is nothing to understand and `index` is -1)
    double index = -1;          // 0..1
    std::array<float, kSiiBands> ratioDb{};  // speech over masker, each band
};

struct Intelligibility {
    bool valid = false;
    DialogueSource source = DialogueSource::Roles;  // where the dialogue came from
    std::vector<IntelligibilityBlock> blocks;
    double average = -1;  // over the speech
    // Timeline seconds where the index stays under `limit` for at least `minSeconds` (pauses in the speech of up to a
    // second bridged), in order.
    std::vector<std::pair<double, double>> under(double limit, double minSeconds) const;
    // The index at a time (-1 where nobody speaks or outside what was measured).
    double at(double seconds) const;
};

// Mixes the range's dialogue and the rest apart and measures them. Fails (invalid, with `error`) when there is no
// dialogue to measure, the separation model is needed but missing, or it is cancelled.
Intelligibility measureIntelligibility(const Project& p, const Sequence& s, const IntelligibilityOptions& o,
                                       const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                                       std::string* error = nullptr);

}  // namespace montage
