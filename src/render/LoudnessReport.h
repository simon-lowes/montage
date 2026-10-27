// Montage — a loudness report for a delivery, as a PDF to read and a CSV to load elsewhere: what was asked (a target
// in LUFS or LKFS, a true peak ceiling, a Leq(M) limit), what was measured (integrated and dialogue-gated loudness,
// loudness range over everything and over the dialogue, true peak, Leq(M)), every stretch over a limit with its
// timecodes, and the readings every 100 ms (momentary and short-term loudness, true peak, whether it is dialogue).
#pragma once

#include <string>
#include <vector>

#include "core/Model.h"
#include "render/DialogueLoudness.h"

namespace montage {

struct LoudnessReportSpec {
    std::string title;           // the programme
    std::string targetName;      // "EBU R128", "Netflix"... ("" none)
    double target = 0;           // LUFS (LKFS with dialogueGated); 0 = none
    double tolerance = 1;        // LU either side
    bool dialogueGated = false;  // the target is the dialogue's (DialogueLoudness::value)
    double peakCeiling = 0;      // dBTP; 0 = none
    double leqmLimit = 0;        // Leq(M); 0 = none (85 trailers, 82 advertising)
};

// A stretch over a limit, in timeline seconds.
struct LoudnessOver {
    double from = 0, to = 0;
    std::string what;  // "True peak -0.3 dBTP over -1.0", "Short-term -18.2 LUFS over -22.0"
    bool truePeak = false;  // over the ceiling (else over the target)
};

// Where the readings go over the limits: true peak over the ceiling, and short-term loudness more than the tolerance
// above the target (a programme target, or the dialogue's over the dialogue alone); steps within 1 s are joined.
std::vector<LoudnessOver> loudnessOvers(const DialogueLoudness& d, const LoudnessReportSpec& spec);
// Whether the whole passes: the target (within the tolerance), the ceiling and the Leq(M) limit.
bool loudnessPasses(const DialogueLoudness& d, const LoudnessReportSpec& spec);

// The report as CSV: '#' lines with the figures and the overs, then a row every 100 ms (seconds, timecode, momentary,
// short-term, true peak, dialogue). Timecodes in the sequence's rate.
bool writeLoudnessCsv(const std::string& path, const DialogueLoudness& d, const LoudnessReportSpec& spec, const Sequence& s,
                      std::string* error = nullptr);
// The same as a PDF page or more: the figures, a graph of the readings against the target and ceiling, and the overs.
bool writeLoudnessPdf(const std::string& path, const DialogueLoudness& d, const LoudnessReportSpec& spec, const Sequence& s,
                      std::string* error = nullptr);

}  // namespace montage
