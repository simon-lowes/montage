// Montage — Auto B-Roll (like Descript's and CapCut's AI B-roll): cutaways
// chosen by what is being said. The cut's dialogue is split into sentences
// (from the transcripts); each sentence is compared with what the footage
// shows (CLIP: its text embedding against the visual index), and over the
// sentences that match best a few seconds of the best-matching shot go on a
// track above, picture only, so the voice carries on underneath.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/EditOps.h"
#include "core/Model.h"

namespace montage {

struct BrollOptions {
    double coverage = 0.5;                  // the share of sentences that get a cutaway (the best-matching ones)
    double minSeconds = 1.5, maxSeconds = 5;  // a sentence shorter than min gets none; a cutaway lasts at most max
    float minScore = 0.2f;                  // CLIP similarity below this is no match
};

struct BrollPick {
    std::string sentence;
    FrameTime at = 0, length = 0;  // on the timeline
    Id media = 0;
    double in = 0;                 // media seconds
    float score = 0;
};

// The cutaways for sequence `s`, from the indexed videos in `candidates`.
// `embed` turns a sentence into a CLIP text embedding. Empty (with `error`)
// when nothing is transcribed or nothing matches.
std::vector<BrollPick> planBroll(const Project& p, const Sequence& s, const std::vector<Id>& candidates,
                                 const std::function<std::vector<float>(const std::string&)>& embed, const BrollOptions& o = {},
                                 std::string* error = nullptr);

// Places them on video track `videoTrack` (made if needed), picture only.
edit::Result placeBroll(Project& p, Sequence& s, const std::vector<BrollPick>& picks, int videoTrack = 1);

}  // namespace montage
