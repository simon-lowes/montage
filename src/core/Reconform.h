// Montage — re-conforming to a new cut (Avid's Change List tool, the Conformalizer and Matchbox that sound and VFX
// departments use, Resolve's conform from a change EDL). The picture of the new version of a cut is matched frame by
// frame to the old one's: the stretches of the old cut it still plays and where (in the same order, or moved), what it
// adds (new shots, or more of a shot) and what it drops (whole shots, or part of one). A sequence laid out against the
// old cut (the sound mix, the grade, the effects and titles over it) is then rebuilt to play against the new one, the
// new material filled in from the new cut, and the list exported as a change EDL (CMX 3600, the old cut as the source
// and the new as the record, which conform tools read) or as a CSV.
//
// What a frame shows is the topmost enabled clip with media on a visible track (titles and adjustment layers count only
// where nothing with media is under them), at its source frame; a still or a gap matches wherever it is.
#pragma once

#include <string>
#include <vector>

#include "Model.h"

namespace montage {

enum class CutEventKind { Same, Moved, Inserted, Extended, Deleted, Trimmed };
const char* cutEventName(CutEventKind k);  // "Same", "Moved", "Inserted", "Extended", "Deleted", "Trimmed"

struct CutEvent {
    CutEventKind kind = CutEventKind::Same;
    FrameTime oldIn = 0, oldOut = 0;  // the stretch of the old cut, [in, out); empty for Inserted and Extended
    FrameTime newIn = 0, newOut = 0;  // in the new cut; Deleted and Trimmed: where it would have been (in == out)
    std::string shot;                 // the first shot's clip name
    int shots = 1;                    // how many shots the stretch spans
    bool black = false;               // nothing but black (a gap)
    FrameTime length() const { return std::max(newOut - newIn, oldOut - oldIn); }
    FrameTime shift() const { return newIn - oldIn; }  // Same and Moved: how far it slid
};

struct CutChanges {
    std::vector<CutEvent> events;  // in the new cut's order (what was dropped where it was)
    Rational fps{30, 1};
    FrameTime oldLength = 0, newLength = 0;
    std::string oldName, newName;
    int changed() const;  // the events that are not Same
};

// The new cut against the old (two sequences of the same project, or of two projects with the same media). Both must
// have the same frame rate: otherwise there are no events and `error` says why.
CutChanges cutChanges(const Project& p, const Sequence& oldCut, const Sequence& newCut, std::string* error = nullptr);
CutChanges cutChanges(const Project& pOld, const Sequence& oldCut, const Project& pNew, const Sequence& newCut,
                      std::string* error = nullptr);

// The list as CSV (event, change, shot, old in/out, new in/out, length, shift; timecodes from 00:00:00:00).
std::string changeListCsv(const CutChanges& c);
// The new cut as a CMX 3600 EDL whose events come from reel `oldReel` (the old cut, at its own timecode) or, for new
// material, `newReel` (the new cut at the same timecode); comments name each shot and its change.
std::string changeEdl(const CutChanges& c, const std::string& oldReel = "OLDCUT", const std::string& newReel = "NEWCUT");

struct ReconformOptions {
    std::string name;             // empty: "<source> (Conformed)"
    bool fillFromNewCut = true;   // the new cut's clips where it has new material (labelled Forest)
    bool markers = true;          // a marker on each insert and where something was taken out
};

struct ReconformResult {
    Id sequence = 0;
    int pieces = 0;   // stretches of the source carried over
    int inserts = 0;  // stretches of new material
    int clips = 0;    // clips in the new sequence
};

// A new sequence playing `source` (cut to the old version: a mix, a grade, a sequence of effects and titles over it)
// against the new version: each stretch of the old cut the new one keeps is copied from `source` (every track, with
// its transitions, fades, automation, markers and captions) to where the new cut plays it; new material comes from
// `newCut` when asked. `source` must have the changes' frame rate.
ReconformResult reconformSequence(Project& p, Id source, const CutChanges& changes, Id newCut, const ReconformOptions& options = {},
                                  std::string* error = nullptr);

}  // namespace montage
