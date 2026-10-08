// Montage — logging a take from its spoken slate: the scene, shot and take
// called out at the head of a take ("Scene twelve apple, take three",
// "Slate 42, take 1", "12B take 2"), read from the clip's transcript. Like
// Resolve 21's Slate ID, which reads the clapperboard; this reads what is said
// over it. Numbers may be digits or words; shot letters may be letters or
// phonetic words (alpha, apple, bravo, baker...).
#pragma once

#include <optional>
#include <string>

#include "Transcript.h"
#include "Model.h"
#include <vector>

namespace montage {

struct SlateInfo {
    std::string scene, shot, take;  // "12", "A", "3"; empty if not said
    double at = -1;                 // seconds into the media where the slate was called
};

// The slate called in the first `withinSeconds` of a transcript, if any.
std::optional<SlateInfo> slateFromTranscript(const Transcript& t, double withinSeconds = 20);

// Sets the scene, shot and take fields of these media (all, if none are given)
// from the slates called in their transcripts; returns how many were logged.
int logFromSlates(Project& p, const std::vector<Id>& media = {});

}  // namespace montage
