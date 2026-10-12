// Montage — dialogue-gated loudness, the "Dialog LKFS" Netflix asks for (-27 LKFS ±2, true peaks under -2 dBTP) and
// ATSC A/85 allows: BS.1770 measured over only the stretches where someone speaks, so a quiet film with loud action
// is levelled by its voices. Where speech is less than 15 % of the sound, the programme's own loudness counts instead.
// Dolby's Dialogue Intelligence, the gate Netflix's QC runs, is licensed only as object code inside professional
// meters, so Montage's gate is its own and its figure an estimate: the speech is found from the sequence's Dialogue
// clips (each heard where its sound is within 20 LU of the dialogue's loudness), else its transcripts' words, else by
// listening (render/AutoMix.h's speech detection, run on the mix a second at a time).
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/Model.h"

namespace montage {

enum class SpeechGate { Auto, Roles, Transcripts, Classifier };
const char* speechGateName(SpeechGate g);  // "auto", "roles", "transcripts", "classifier"
bool parseSpeechGate(const std::string& name, SpeechGate& out);

struct DialogueLoudnessOptions {
    SpeechGate gate = SpeechGate::Auto;  // Auto: roles, else transcripts, else listening
    std::vector<bool> tracks;            // the audio tracks heard (true = in); empty = all
    FrameTime in = 0, out = -1;          // the range measured; out -1 = the sequence's end
    bool layout = false;                 // the sequence's own channels (surround weighted as BS.1770 says), not stereo
    double minSpeechShare = 0.15;        // below this share of speech the programme's loudness counts
};

struct DialogueLoudness {
    bool valid = false;          // false: nothing above the absolute gate
    double dialogue = -70;       // LKFS over the speech (-70 when none was found)
    double programme = -70;      // LKFS over everything (BS.1770)
    double truePeak = -96;       // dBTP, the whole programme
    double speechShare = 0;      // of the sound, 0..1
    bool usesProgramme = true;   // too little speech (or none in whole 400 ms blocks): the programme figure counts
    double minSpeechShare = 0.15;  // the share asked for
    SpeechGate gate = SpeechGate::Auto;  // how the speech was found
    std::vector<std::pair<double, double>> speech;  // timeline seconds
    double value() const { return usesProgramme ? programme : dialogue; }
};

// Where the sequence itself says someone speaks, in timeline seconds (merged, in order), without listening: heard
// Dialogue-role clips (Roles) or transcribed words, a little padded (Transcripts), over [from, to) seconds (to < 0:
// the end). Auto picks Roles when a Dialogue clip is heard there, else Transcripts, and says which in `gate`;
// Classifier gives none. Whole clips: measureDialogueLoudness narrows them to where their sound is.
std::vector<std::pair<double, double>> timelineSpeech(const Project& p, const Sequence& s, SpeechGate& gate,
                                                      const std::vector<bool>& tracks = {}, double from = 0, double to = -1);

// Mixes the range and measures it. Fails (invalid, with `error`) only when cancelled or the range is empty.
DialogueLoudness measureDialogueLoudness(const Project& p, const Sequence& s, const DialogueLoudnessOptions& o,
                                         const std::function<void(double)>& progress = {},
                                         const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

// "Dialogue -26.8 LKFS (estimate: 64 % speech, by Dialogue clips)", or the programme's figure with why.
std::string describeDialogueLoudness(const DialogueLoudness& d);

}  // namespace montage
