// Montage — ADR (automated dialogue replacement, "looping"): the lines an actor re-records to picture. A cue list
// held by the sequence (cue number, character, the line, why, its status), made from captions, range markers or the
// In/Out marks, or read from a cue sheet; each recording pass of a cue (a cycle) plays the picture from a pre-roll with
// three beeps a second apart and a streamer that crosses the picture, the line starting where a fourth beep would
// be, and every take recorded lands as a take of one audition clip over the line, the newest the pick (Pro Tools'
// and Fairlight's ADR tools work this way; Premiere, Final Cut and Media Composer have no cue list or cycles).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "EditOps.h"
#include "Model.h"

namespace montage {

enum AdrStatus : int { kAdrToRecord = 0, kAdrRecorded = 1, kAdrApproved = 2, kAdrOmitted = 3 };
std::string adrStatusName(int status);              // "To Record", "Recorded", "Approved", "Omitted"
int adrStatusFromName(const std::string& name);     // any case; unknown names are To Record

AdrCue* findAdrCue(Sequence& s, Id id);
const AdrCue* findAdrCue(const Sequence& s, Id id);

// The next free cue number for a character: their initials and a number from 101 ("JD101"), "ADR101" without one.
std::string nextAdrCueName(const Sequence& s, const std::string& character);

// Cues from the captions of caption track `track` (-1 = every track) that overlap `from`..`to` (frames, `to` < 0 =
// to the end): one a caption, its text the line. A speaker label before the text ("ANNA: ...", "[Anna] ...",
// "(ANNA) ...") is taken off as the character.
std::vector<AdrCue> adrCuesFromCaptions(const Sequence& s, int track = -1, FrameTime from = 0, FrameTime to = -1);
// Cues from the range markers (those with a duration) that overlap `from`..`to`, of label `color` (-1 = any): the
// marker's name is the note and its comment the line.
std::vector<AdrCue> adrCuesFromMarkers(const Sequence& s, FrameTime from = 0, FrameTime to = -1, int color = -1);

// Adds cues to the sequence in time order: a cue named like one already there updates it (times, character, line,
// note and, when given, status); the others get an id and, when blank, the next cue number. Cues with no length are
// left out. Returns the ids of the cues added or updated.
std::vector<Id> addAdrCues(Project& p, Sequence& s, std::vector<AdrCue> cues);
bool removeAdrCue(Sequence& s, Id id);
// Takes frames [a, b) out of the cue list, as a ripple delete or closing a gap does to the timeline: cues after it move
// back, cues across it keep what is outside it, cues inside it go.
void rippleAdrCues(Sequence& s, FrameTime a, FrameTime b);

// How many takes a cue has (0 before the first; its clip gone counts as none).
int adrTakeCount(const Sequence& s, const AdrCue& cue);

// The cue sheet, for the stage and the actor: "Cue,Character,Start,End,Duration,Line,Note,Status,Takes" with SMPTE
// timecodes.
std::string adrCueSheetCsv(const Sequence& s);
// Cues from a cue sheet with a header row (Cue / Cue Number, Character / Role, Start / In / TC In, End / Out /
// TC Out, Duration, Line / Dialogue, Note / Reason, Status), comma or tab separated. When every time is an hour or
// more in and the sequence is shorter, the sheet is taken to start at 01:00:00:00. A row with no end or duration
// lasts two seconds. False (with `error`) when no cue could be read.
bool parseAdrCueSheet(const std::string& text, const Sequence& s, std::vector<AdrCue>& out, std::string* error = nullptr);

// One recording pass of a cue.
struct AdrSettings {
    double preRoll = 4;    // seconds of picture before the line; never less than the beeps need
    double postRoll = 1;   // seconds after it
    int beeps = 3;         // 0-3, a second apart, the line where the next would be
    double streamer = 2;   // seconds the streamer takes to cross the picture, reaching the edge on the line; 0 = none
};
struct AdrCycle {
    FrameTime playFrom = 0, playTo = 0;  // what plays and records (playTo exclusive)
    FrameTime lineFrom = 0, lineTo = 0;  // the line
    std::vector<FrameTime> beeps;        // where each beep starts
    FrameTime streamerFrom = -1, streamerTo = -1;
    bool valid() const { return playTo > playFrom; }
};
AdrCycle adrCycle(const Sequence& s, const AdrCue& cue, const AdrSettings& settings = {});

// Where the streamer is at frame `t`: 0 at the picture's left edge to 1 at its right (on the line's first frame),
// or < 0 when it is not shown.
double adrStreamerPosition(const AdrCycle& c, FrameTime t);
// The punch: a flash on the line's first two frames.
bool adrPunch(const AdrCycle& c, FrameTime t);
// Adds the beeps (1 kHz, a frame long and never under 40 ms, at `gain`) to `frames` frames of interleaved sound that
// start at timeline sample `firstSample`.
void addAdrBeeps(const AdrCycle& c, double fps, int sampleRate, int64_t firstSample, float* interleaved, int frames, int channels,
                 float gain = 0.25f);

namespace edit {
// The audio track named "ADR", added at the bottom when there is none; its index.
int adrTrack(Project& p, Sequence& s);
// A recorded take of a cue: `media`, whose first sample belongs at timeline frame `recordedFrom`. The first take
// becomes a clip over the line (as much of it as the take covers), in sync, on audio track `track` when it has room
// there, else on the first ADR track free over the line ("ADR", "ADR 2"... added as needed); a take's picture, if
// any, is left out. Later takes join that clip as takes of its audition, the newest the pick, the clip growing to
// what a take covers when an earlier one was cut short. A take that ends before the line is refused. A cue still
// To Record becomes Recorded.
Result addAdrTake(Project& p, Sequence& s, Id cueId, Id mediaId, FrameTime recordedFrom, int track);
}  // namespace edit

}  // namespace montage
