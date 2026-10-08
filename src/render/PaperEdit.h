// Montage — Paper Edit (Premiere 26.5's, Avid's ScriptSync-style assembly):
// lines chosen from the transcripts of source clips, in the order wanted,
// laid out back to back as a new sequence with their picture and sound.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

struct PaperLine {
    Id media = 0;
    double in = 0, out = 0;  // media seconds (from a word's start to a word's end)
    std::string text;        // what is said
};

// Where `phrase` is said in the media's transcript (case-insensitive, ignoring
// punctuation; the first time it is said at or after `after` seconds).
std::optional<PaperLine> findLine(const Project& p, Id media, const std::string& phrase, double after = 0);

// A new sequence named `name` (sized like the active one) holding the lines in
// order, each with `handle` seconds of air either side; added to the project
// and its media, not made active. Returns its id, or 0 if nothing was placed.
Id makePaperEdit(Project& p, const std::vector<PaperLine>& lines, const std::string& name = "Paper Edit", double handle = 0.1);

}  // namespace montage
