// Montage — a cut built from a script (like Resolve's IntelliScript, Premiere's
// paper edit or Avid's ScriptSync): each line of the script is found in the
// transcribed takes, the best reading of each goes on the timeline in script
// order, and the other readings sit on the tracks above as alternates.
#pragma once

#include <string>
#include <vector>

#include "Model.h"

namespace montage {

struct ScriptLine {
    std::string speaker;  // "" when the script does not say
    std::string text;
    bool operator==(const ScriptLine&) const = default;
};

// Parses a script into the lines to find. Paragraphs (separated by blank
// lines) are lines; a long one is split at sentence ends. "NAME: words" and a
// name in capitals on its own line above the words both give the speaker.
// Scene headings (INT./EXT.), transitions (CUT TO:), parentheticals,
// [directions] and # headings are skipped.
std::vector<ScriptLine> parseScript(const std::string& text);
// A Final Draft (.fdx) screenplay as text parseScript reads: each character
// cue and its dialogue (parentheticals, action and headings left out).
std::string fdxToScript(const std::string& xml);

// One reading of a line.
struct ScriptTake {
    Id mediaId = 0;
    double start = 0, end = 0;  // media seconds, first to last matching word
    double coverage = 0;        // share of the line's words that were said (0..1)
    int extraWords = 0;         // words said in that range that are not in the line (flubs, restarts)
    double score = 0;           // higher is better
    std::string speaker;        // who says it, when the transcript names them
};

struct ScriptMatch {
    ScriptLine line;
    std::vector<ScriptTake> takes;  // best first; empty when the line was not found
};

struct ScriptCutOptions {
    double minCoverage = 0.6;  // a reading must say at least this share of the line
    int maxAlternates = 3;     // readings placed above the chosen one
    double handle = 0.15;      // seconds kept before and after each reading
    bool markers = true;       // a marker on each line (and where a line is missing)
    std::vector<Id> media;     // the takes to search; empty = every transcribed item
};

// Finds each line's readings in the transcripts: a local alignment over
// normalised words (Smith-Waterman), so dropped, added and misheard words
// still match. The best reading says most of the line with the fewest extra
// words, by the speaker the script names when the transcript knows names;
// among equals the later take wins.
std::vector<ScriptMatch> matchScript(const Project& p, const std::vector<ScriptLine>& lines, const ScriptCutOptions& o = {});

// Builds a new sequence from the matches (sized like the active sequence):
// the chosen readings back to back on V1/A1 in script order, alternates
// disabled on the tracks above, a marker per line. It gets an item in the
// media bin. Returns its id, or 0 when no line was found.
struct ScriptCutResult {
    Id sequence = 0;
    int placed = 0;     // lines on the timeline
    int missing = 0;    // lines not found
    int alternates = 0;
};
ScriptCutResult buildScriptCut(Project& p, const std::vector<ScriptMatch>& matches, const std::string& name,
                               const ScriptCutOptions& o = {});

}  // namespace montage
