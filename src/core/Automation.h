// Montage — track fader automation, written live from the mixer as in Premiere's Audio Track Mixer, Pro Tools and
// Fairlight. Each audio track has a volume (dB) and a pan lane (and, in surround, x, y and z lanes for its position),
// keyed in timeline frames, and a mode:
//  - Off: the fader as set; the lanes are kept but not heard.
//  - Read: the lanes play (when they have keys) and the fader follows them.
//  - Write: from the moment playback starts until it stops, the fader is recorded.
//  - Latch: like Read until the fader is first moved, then recorded until playback stops.
//  - Touch: recorded while the fader is held; on letting go it glides back to what was there.
//  - Trim Write, Trim Latch and Trim Touch (Pro Tools' Trim modes): the faders (a track's, its sends', a bus's) write a
//    trim lane instead, an offset in dB played on top of the volume lane, which plays on as in Read. What cannot be
//    trimmed (pan, mute, the surround position, inserts) is written as in Write, Latch or Touch. coalesceTrim() folds
//    the trim into the volume lane.
// Beyond the live passes: values written over a range (Write To, Punch Preview), glides, snapshots of the mix captured
// and pasted elsewhere, and automation copied and pasted over a range.
#pragma once

#include <map>
#include <optional>
#include <string>

#include "Model.h"

namespace montage {

enum class AutomationMode { Off = 0, Read = 1, Write = 2, Latch = 3, Touch = 4, TrimWrite = 5, TrimLatch = 6, TrimTouch = 7 };
constexpr int kAutomationModes = 8;

const char* automationModeName(AutomationMode m);
bool automationTrims(AutomationMode m);
// Write, Latch or Touch for a trim mode (how what cannot be trimmed is written); the mode itself otherwise.
AutomationMode untrimmed(AutomationMode m);
// The trim variant of Write, Latch or Touch (Read and Off: Trim Touch); a trim mode stays.
AutomationMode trimmed(AutomationMode m);
// Whether the lanes play (Read, Latch, Touch and the trim modes) and whether a pass writes (all but Off and Read).
bool automationReads(AutomationMode m);
bool automationWrites(AutomationMode m);
AutomationMode trackAutomation(const Track& t);

// The fader values heard at timeline frame t (fractional frames interpolate between whole ones): the volume with its
// trim added where the track reads its lanes.
double trackVolumeAt(const Track& t, double frame);
// Whether the heard volume moves or is trimmed (the lane or a trim, where the track reads its lanes).
bool trackVolumeAnimated(const Track& t);
// A trim lane's offset at a frame (its static value without keys).
double trimAt(const Param& trim, double frame);
double trackPanAt(const Track& t, double frame);
// Mute lanes are switches: the value of the last key at or before the frame (the first key's before it), muted at 0.5
// or more; without keys, the static mute.
bool laneMuted(const Param& lane, bool still, double frame);
bool trackMutedAt(const Track& t, double frame);  // the static mute, or the mute lane where the track reads its lanes
// A send's level heard at frame t (its lane where the track reads its lanes).
double sendLevelAt(const Track& t, const Send& send, double frame);
bool sendLevelAnimated(const Track& t, const Send& send);
// Buses: their own mode and lanes, as tracks'.
AutomationMode busAutomation(const Bus& b);
double busVolumeAt(const Bus& b, double frame);
bool busVolumeAnimated(const Bus& b);
double busPanAt(const Bus& b, double frame);
bool busMutedAt(const Bus& b, double frame);

// Whether a track's surround position moves (any of its x, y, z lanes has keys), and where it is at frame t when the
// lanes play: SurroundPan with x, y and z from the lanes that have keys (within -1..1, z 0..1).
bool surroundAnimated(const Track& t);
SurroundPan trackSurroundAt(const Track& t, double frame);

// Reduces a lane to the keys needed to stay within `tolerance` of it (Ramer–Douglas–Peucker on linear segments).
void thinKeys(std::vector<Keyframe>& keys, double tolerance);

// ---- Trim ----
// Folds a trim into the lane it trims: the lane (or its still level, without keys) then plays what the two played
// together, and the trim is left at 0 dB with no keys. Exact where both lanes are straight between whole-frame points;
// otherwise sampled every frame and thinned to within 0.0001 dB. Left alone (false) where either lane has an
// expression or repeats after its last point.
bool coalesceTrim(Param& lane, double& still, Param& trim);
// Every trim on a track (its fader's and its sends') or a bus folded in, where it plays its lanes (a trim is not heard
// in Off or Write); whether anything changed.
bool coalesceTrim(Track& t);
bool coalesceTrim(Bus& b);
// Whether a track or bus has a trim to fold in.
bool hasTrim(const Track& t);
bool hasTrim(const Bus& b);

// ---- Values over a range ----
// Where Write To writes the value a pass ended on (Pro Tools' Write To Start, End, Punch, All and Next).
enum class WriteTo { Start, End, InOut, All, Next };
// The frames a Write To covers, `to` -1 for "to the end": Start from 0 to the pass's end, End from its start on, InOut
// the In to Out range ({-1, -1} without one), All the whole lane, Next from the pass's start to the lane's next point after its end (that
// point included; with none, to the end).
std::pair<FrameTime, FrameTime> writeToRange(WriteTo w, const Param& lane, FrameTime passFrom, FrameTime passTo, FrameTime in,
                                             FrameTime out);
// `value` written over frames from..to (to -1: to the end), the lane before `from` as it was. After `to` it goes back to
// what was there, a frame later; where `to` falls on one of the lane's points, that point is replaced and the lane
// runs on from it to the next. A switch lane's (hold) points hold.
void writeLaneValue(Param& lane, double still, FrameTime from, FrameTime to, double value, bool hold = false);
// A glide, a straight line in the lane's units (dB on a fader), from the lane's value at `from` to `value` at `to`;
// after it the lane goes back to what it was (hold: stays at `value` to the end).
void glideLane(Param& lane, double still, FrameTime from, FrameTime to, double value, bool hold = false);

// ---- Lanes by name ----
// A track's automation lanes by name: "volume", "trim", "pan", "mute", "x", "y", "z" (its surround position),
// "send:<bus id>", "send-trim:<bus id>" and "insert:<chain index>:<effect type>:<parameter>"; a bus's "volume",
// "trim", "pan", "mute" and "insert:...". With the value it holds without keys, and whether it is a switch.
struct LaneRef {
    Param* lane = nullptr;
    double* still = nullptr;  // its level without keys (a mute's is 0, not muted)
    bool hold = false;        // a switch: its points hold to the next
    double lo = -1e9, hi = 1e9;
    double at(FrameTime t) const { return lane->animated() ? lane->at(t) : *still; }
};
LaneRef trackLane(Track& t, const std::string& name);
LaneRef busLane(Bus& b, const std::string& name);
// The lanes a track or bus has (inserts' numeric parameters included).
std::vector<std::string> trackLaneNames(const Track& t);
std::vector<std::string> busLaneNames(const Bus& b);

// ---- Capture ----
// A snapshot of every control on some tracks and buses at one frame (Pro Tools' Capture): each lane's value there.
struct MixSnapshot {
    struct Strip {
        Id id = 0;
        bool bus = false;
        std::map<std::string, double> values;  // by lane name
        bool operator==(const Strip&) const = default;
    };
    FrameTime at = 0;
    std::vector<Strip> strips;
    bool operator==(const MixSnapshot&) const = default;
};
// `owners`: the ids of audio tracks and buses (empty: all of them). A static mute is captured as muted.
MixSnapshot captureMix(const Sequence& s, const std::vector<Id>& owners, FrameTime at);
// A snapshot written on its tracks and buses (where they still are) over from..to, or, with `to` < `from`, from
// `from` to each lane's next point (Write To Next). Lanes already at its values and not moving there are left alone.
// The number of lanes written.
int pasteMix(Sequence& s, const MixSnapshot& snap, FrameTime from, FrameTime to);

// ---- Copy and paste ----
// One lane's automation over a range: its points relative to the range's start, the first at 0 and the last at
// `length` (the lane's values at the range's ends).
struct LaneClip {
    FrameTime length = 0;
    std::vector<Keyframe> keys;
    bool hold = false;
};
LaneClip copyLane(const Param& lane, double still, FrameTime from, FrameTime to);
// A lane clip pasted at `at`, over what was there from `at` to `at` + its length (the lane either side kept).
// Relative: its movement (each value less its first) is added to what was there instead; a switch's is always pasted
// as it was.
void pasteLane(Param& lane, double still, const LaneClip& clip, FrameTime at, bool relative);
// Automation copied from tracks and buses over a range: for each, in the order given, its lanes that move (or `lanes`,
// when named) by name.
struct AutomationCopy {
    FrameTime length = 0;
    std::vector<std::map<std::string, LaneClip>> strips;
};
AutomationCopy copyAutomation(const Sequence& s, const std::vector<Id>& owners, FrameTime from, FrameTime to,
                              const std::vector<std::string>& lanes = {});
// Pasted at `at` on tracks and buses in order (the first copied onto the first given...; a lane a destination lacks, a
// send to a bus it does not feed, is left out). The number of lanes written.
int pasteAutomation(Sequence& s, const AutomationCopy& c, const std::vector<Id>& owners, FrameTime at, bool relative);

// Records one lane while playback runs forward from `start`. tick() is called for each frame played, in order, with
// the fader's value and whether it is held; it returns what the fader should show. finish() gives the new lane:
// what was written replaces the old keys over the passes written, with the level outside them unchanged.
class AutomationRecorder {
public:
    // `lane` and `still` are the lane and the fader's static value before recording.
    AutomationRecorder(AutomationMode mode, Param lane, double still, FrameTime start, FrameTime glide = 30);
    double tick(FrameTime t, double fader, bool held);
    // Whether the fader is being recorded now (what plays should then be the fader).
    bool writing() const { return writing_; }
    bool wrote() const { return !passes_.empty() || writing_; }
    // Where the first pass written began, and the last value written (for Write To).
    std::optional<FrameTime> writtenFrom() const {
        if (!passes_.empty()) return passes_.front().from;
        if (writing_) return passStart_;
        return std::nullopt;
    }
    double lastWritten() const { return lastValue_; }
    // Lays what is written over `lane` and `still` rather than those it began with (they were changed meanwhile): what
    // it shows when not writing, and the level either side of what it writes at finish().
    void rebase(Param lane, double still) {
        lane_ = std::move(lane);
        still_ = still;
    }
    Param finish(FrameTime stop, double tolerance);
    // Latch armed while stopped (a control moved before playback): it writes from the first frame played.
    void prime() {
        if (mode_ == AutomationMode::Latch) writing_ = true;
    }

private:
    double original(FrameTime t) const { return lane_.animated() ? lane_.at(t) : still_; }
    void endPass(FrameTime t);
    AutomationMode mode_;
    Param lane_;
    double still_;
    FrameTime glide_;
    bool writing_ = false;
    FrameTime passStart_ = 0, last_ = 0;
    double lastValue_ = 0;
    struct Pass {
        FrameTime from = 0, to = 0;  // written frames, inclusive
        std::vector<Keyframe> keys;
        bool glide = false;  // Touch: glide back after `to`
    };
    std::vector<Pass> passes_;
    std::vector<Keyframe> current_;
};

}  // namespace montage
