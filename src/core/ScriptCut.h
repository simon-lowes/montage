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

// A screenplay as it reads on the page, for the lined script: its blocks in
// order, and the lines to find (what parseScript would give for plain text),
// each dialogue block pointing at the lines it became.
struct ScriptBlock {
    enum class Kind { Heading, Action, Character, Parenthetical, Dialogue, Transition, Centered, Lyric, PageBreak };
    Kind kind = Kind::Action;
    std::string text;
    int firstLine = -1, lineCount = 0;  // Dialogue: Screenplay::lines[firstLine ...]
    bool operator==(const ScriptBlock&) const = default;
};
struct Screenplay {
    std::string title;  // a Fountain title page's Title ("" when it has none)
    std::vector<ScriptBlock> blocks;
    std::vector<ScriptLine> lines;
};
// Fountain (fountain.io, version 1.1): a title page, scene headings (INT.,
// EXT., or forced with "."), character cues in capitals (or forced with "@",
// "^" for dual dialogue), parentheticals, dialogue, transitions ("TO:" or
// forced with ">"), centred text (>text<), lyrics (~), forced action (!),
// page breaks (===); sections, synopses, [[notes]] and /* boneyard */ are
// left out, and *emphasis* and _underlining_ shown plain.
Screenplay parseFountain(const std::string& text);
// A Final Draft (.fdx) screenplay as Fountain: headings, action, cues,
// parentheticals, dialogue and transitions.
std::string fdxToFountain(const std::string& xml);
// Plain text read as parseScript reads it (screenplay.lines == parseScript),
// with what it skips (headings, transitions, directions) kept as blocks.
Screenplay plainScreenplay(const std::string& text);
// "fdx" for Final Draft XML, "fountain" for a .fountain or .spmd file or text
// with a Fountain title page or forced cues (@NAME), else "text".
std::string scriptFormat(const std::string& fileName, const std::string& text);
// The project's script read in its format.
Screenplay readScreenplay(const ProjectScript& s);
// The lines of a script in whichever format it is (scriptFormat).
std::vector<ScriptLine> scriptLines(const std::string& text, const std::string& fileName = {});

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

// Sets the project's script and finds each of its lines in the transcribed
// takes (matchScript; every reading that says at least o.minCoverage of the
// line). An empty text takes the script away. Returns how many lines have a
// reading.
int setProjectScript(Project& p, const std::string& name, const std::string& format, const std::string& text,
                     const ScriptCutOptions& o = {});
// Finds the lines again (after transcribing more takes). Returns how many
// lines have a reading.
int matchProjectScript(Project& p, const ScriptCutOptions& o = {});
// For each of the script's lines, which of its readings (an index into
// p.script.readings[line]) an enabled clip of the sequence plays at least half
// of; -1 for none.
std::vector<int> scriptLinesInCut(const Project& p, const Sequence& s);
// A take the script supervisor circled (the field recorder's "circled" note).
bool isCircledTake(const MediaItem& m);

}  // namespace montage
