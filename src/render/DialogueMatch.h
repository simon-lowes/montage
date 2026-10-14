// Montage — Dialogue Match: makes ADR or a pickup sound like the production take it sits with (the reference clip).
// The reference's room is measured blind from its speech (the decay time in each octave, audio/RoomResponse.h) and
// added to the clip as a Convolution Reverb at the level that leaves as much late reverberation in its pauses as the
// reference has; then Match Voice's EQ (render/VoiceMatch.h) is fitted to what that sounds like, once or every half
// second so it follows the reference over time; and the clip's volume brings its speech to the reference's loudness.
// Everything is ordinary effects and clip volume, tagged so matching again replaces them.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "VoiceMatch.h"
#include "audio/RoomResponse.h"
#include "core/Model.h"

namespace montage {

struct DialogueMatchOptions {
    bool room = true;
    bool tone = true;
    bool toneOverTime = false;  // the EQ refitted every half second, against the same moment of the reference
    bool level = true;
};

struct DialogueMatchPlan {
    // The room: what was measured on the reference and added (wetDb is the Convolution Reverb's Room Level).
    RoomEstimate measured;
    RoomShape room;
    bool roomAdded = false;
    double wetDb = -60;
    double referenceLateDb = -60, targetLateDb = -60, matchedLateDb = -60;  // lateReverbDb at the reference's decay
    // The tone, and when it follows the reference, its settings over time (seconds into the clip's sound).
    VoiceEq eq;
    std::vector<std::pair<double, VoiceEq>> eqOverTime;
    // The level: the clip volume (dB) and the loudness (LUFS) of the reference and of the clip before and after.
    double gainDb = 0;
    bool levelSet = false;  // (not when either's speech could not be measured)
    double referenceLufs = -70, targetLufs = -70, matchedLufs = -70;
};

// Works the match out from mono audio at `rate`: `target` made to sound like `reference`.
DialogueMatchPlan planDialogueMatch(const std::vector<float>& reference, const std::vector<float>& target, int rate,
                                    const DialogueMatchOptions& o = {});
// `target` as the plan makes it sound (room, tone and level; the tone as fitted once).
std::vector<float> withDialogueMatch(const std::vector<float>& target, int rate, const DialogueMatchPlan& plan);

// Puts the plan on a clip: its EQ first in the effects (keyed over time when it follows the reference) and the room
// right after it, replacing an earlier Dialogue Match or Match Voice EQ and Dialogue Match room; the volume set.
void applyDialogueMatch(Project& p, const Sequence& s, Clip& c, const DialogueMatchPlan& plan, const DialogueMatchOptions& o = {});
// Matches each audio clip to the reference clip (as its source sounds, with its clip volume). Returns how many were
// matched; `plans` gets each one's plan, in order.
int matchDialogue(Project& p, Sequence& s, Id reference, const std::vector<Id>& clips, const DialogueMatchOptions& o = {},
                  std::vector<std::pair<Id, DialogueMatchPlan>>* plans = nullptr, std::string* error = nullptr,
                  const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

}  // namespace montage
