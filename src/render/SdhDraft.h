// Montage — drafting SDH captions (core/SdhCaptions.h) for a sequence: the dialogue's captions from its transcripts,
// and what else is heard and seen found by listening and looking. The sound outside speech is heard in short windows
// by CLAP (What's Heard's model, media/SoundSearch.h) and scored against the project's sound descriptions; the best
// above a threshold becomes a cue. Dialogue that CLAP hears as singing rather than speech is marked as sung. Where the
// speaker changes, the frame's faces are counted (media/Faces.h), and the text in the picture where each caption
// would sit is read (media/TextReader.h). Each model is used when it is installed; without one, that part is left
// out (no sound cues, no songs, a speaker label at every change, no moving over text in the footage) and said so.
// The draft is placed for the style of the track shown (or the default one), which its own track should take.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/Model.h"
#include "core/SdhCaptions.h"

namespace montage {

struct SdhDraftOptions {
    SdhOptions sdh;            // the style and cueing rules (look and aspect are taken from the sequence)
    CaptionRules rules;        // how the dialogue is broken into captions
    double threshold = 0.3;    // how alike (cosine, CLAP) a sound and its description must be to be captioned
    double loudDb = -40;       // sound quieter than this (dBFS, 50 ms RMS) is not captioned
    double minSoundSeconds = 0.2;
    bool sounds = true;        // listen for sounds
    bool faces = true;         // count faces at speaker changes
    bool readText = true;      // read the text in the picture
};

struct SdhDraft {
    std::vector<Caption> captions;
    CaptionStyle look;              // the style the captions were placed for (the shown track's), for their track
    std::string language;           // the transcripts' (ISO 639-1), "" when unknown
    SdhSummary summary;
    std::vector<SdhSound> heard;    // what was heard outside speech (before cueing)
    std::vector<std::string> notes; // parts left out (a model not installed)
};

// Stretches of the sequence's sound (timeline seconds) at least `minSeconds` long and louder than `loudDb` (50 ms
// windows, gaps under 0.3 s bridged) that fall outside `speech`.
std::vector<std::pair<double, double>> loudStretches(const Project& p, const Sequence& s, const std::vector<std::pair<double, double>>& speech,
                                                     double loudDb = -40, double minSeconds = 0.2, const std::atomic<bool>* cancel = nullptr);

// Drafts the SDH captions. False (with `error`) when the sequence has no transcribed speech and nothing was heard, or
// when cancelled.
bool draftSdhCaptions(const Project& p, const Sequence& s, const SdhDraftOptions& o, SdhDraft& out,
                      const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                      std::string* error = nullptr);

}  // namespace montage
