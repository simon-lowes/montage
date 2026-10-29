// Montage — elastic audio: the timing of a clip's sound changed inside the clip by warp markers, and its pitch edited
// stretch by stretch, without leaving the timeline.
//
// Warp markers are an "elastic_audio" source-audio effect (audio/SpeechCleanup.h): its "points" string is a warp
// (audio/DialogueAlign.h AlignPoint: the moment `w` of the file plays at `u`), in the seconds of the group the clip is
// warped with, so the sound between two markers is stretched or squeezed to fit and everything outside the first and
// last stays where it was. Three ways to play it ("mode"): Speech (WSOLA with short grains, audio/TimeStretch.h),
// General (longer grains, for music and effects) and Varispeed (resampled, the pitch moving with the speed, like tape).
// Clips warped together (the microphones of one take, linked clips) share the markers and are cut at the same places,
// chosen from all their sounds together, so they stay in phase with each other. The effect names its group's members
// ("members": media, channels and where each one's source time is against the group's) and its own place ("offset").
//
// Pitch edits are a "pitch_contour" effect after it: its "edits" string lists stretches of the played sound (source
// seconds) with a shift, a scaling of the inflection (0 flattens it), a correction to the notes of a scale and a
// formant shift. The pitch is tracked by pYIN (Mauch and Dixon's probabilistic YIN: candidate periods weighed over
// many thresholds, then the likeliest path through them by Viterbi) and the sound remade by TD-PSOLA (pitch-synchronous
// overlap-add: two-period grains cut at each glottal pulse, laid at the new period), each grain resampled for a formant
// shift of its own. Both are cached with the clip's other source effects, so edits are heard once rendered and undo
// like any other.
#pragma once

#include <atomic>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "DialogueAlign.h"
#include "core/EditOps.h"
#include "core/Model.h"
#include "media/Decoder.h"

namespace montage {

enum class ElasticMode { Speech, General, Varispeed };
const char* elasticModeName(ElasticMode m);  // "speech", "general", "varispeed"
bool parseElasticMode(const std::string& name, ElasticMode& out);
ElasticMode elasticModeOf(const Effect& e);

// One clip of a group warped together: its media and channels, and its source seconds less the group's.
struct ElasticMember {
    Id media = 0;
    std::vector<int> channels;
    double offset = 0;
    bool operator==(const ElasticMember&) const = default;
};
std::string elasticMembersToString(const std::vector<ElasticMember>& members);  // "media|ch,ch|offset;..."
std::vector<ElasticMember> elasticMembersFromString(const std::string& text);

// The sound re-timed along `warp` (group seconds; this sound's time less `offset` is the group's), the same length.
// With `group` (every member's own sound and offset, this one's included) the grains are cut where they suit all of
// them, so members rendered apart line up; empty, where they suit this sound.
void applyElasticWarp(const AudioBuffer& in, const DialogueWarp& warp, ElasticMode mode, double offset,
                      const std::vector<std::pair<const AudioBuffer*, double>>& group, AudioBuffer& out,
                      const std::atomic<bool>* cancel = nullptr);

// Where sounds start (spectral flux of the log spectrum against its running median, each peak then placed to the
// sample by the sharpest rise in level). `sensitivity` 0..100; at least `minGap` seconds apart. Seconds from the start.
std::vector<double> detectOnsets(const AudioBuffer& in, double sensitivity = 50, double minGap = 0.05,
                                 const std::atomic<bool>* cancel = nullptr);

// The pitch every `hop` seconds (frame i at start + i * hop), 0 where there is no pitch.
struct PitchTrack {
    double start = 0;  // seconds of frame 0
    double hop = 0.005;
    std::vector<double> f0;      // Hz
    std::vector<float> voicing;  // how likely each frame is to have a pitch, 0..1
    double at(double t) const;   // f0 at t, between frames (0 when either side has none)
};
PitchTrack trackPitch(const AudioBuffer& in, double fmin = 50, double fmax = 800, const std::atomic<bool>* cancel = nullptr);
// Over [from, to] seconds only.
PitchTrack trackPitch(const AudioBuffer& in, double from, double to, double fmin, double fmax, const std::atomic<bool>* cancel = nullptr);

// A pitch edit over [start, end) seconds of the played sound. In order: the inflection scaled about the stretch's
// median pitch (0 flat, 1 as it was), drawn `correct` of the way (0..1) to the nearest note of the scale (the pitch
// averaged over 150 ms, so vibrato is kept), shifted by `shift` semitones; the formants moved by `formant` semitones.
struct PitchEdit {
    double start = 0, end = 0;
    double shift = 0;
    double inflection = 1;
    double correct = 0;
    int key = 0;                    // the scale's root: 0 C, 1 C#... 11 B
    std::string scale = "chromatic";  // "chromatic", "major" or "minor"
    double formant = 0;
    bool operator==(const PitchEdit&) const = default;
};
std::string pitchEditsToString(const std::vector<PitchEdit>& edits);  // JSON
std::vector<PitchEdit> pitchEditsFromString(const std::string& text);
bool validPitchScale(const std::string& scale);
// The pitch the edits ask for, frame by frame of `track` (0 where it has none). Each edit eases in and out over 20 ms.
std::vector<double> editedPitch(const PitchTrack& track, const std::vector<PitchEdit>& edits);
// The sound with its pitch remade (TD-PSOLA) where the edits change it; sample for sample the same elsewhere.
void applyPitchEdits(const AudioBuffer& in, const std::vector<PitchEdit>& edits, AudioBuffer& out,
                     const std::atomic<bool>* cancel = nullptr, const PitchTrack* track = nullptr);

// Before a clip's source effects are played: each "elastic_audio" effect with members gets "member_keys" (each
// member's sound as media/MediaPool.h names it, and its offset), copied into `keep`.
void resolveElasticMembers(const Project& p, std::vector<const Effect*>& effects, std::deque<Effect>& keep);
// The elastic step of a source-effect chain (audio/SpeechCleanup.cpp): the members' sounds read at the buffer's rate.
void playElasticEffect(const Effect& e, const AudioBuffer& in, AudioBuffer& out, const std::atomic<bool>* cancel);

// The clip's sound as it plays before its pitch edits (through its source effects up to them), at `rate`; null when
// it has none. Waits for it.
AudioBufferPtr soundBeforePitchEdits(const Project& p, const Clip& c, int rate);

// The clip's effects, or null.
const Effect* elasticEffectOf(const Clip& c);
const Effect* pitchContourOf(const Clip& c);
// The clip's markers in group seconds and its offset (0 without elastic audio).
double elasticOffsetOf(const Clip& c);

namespace edit {
// The clips warped with `clip` (itself included), in the sequence.
std::vector<Id> elasticGroup(const Sequence& s, Id clip);
// Elastic audio on for the audio clips (each at its own speed, forwards), warped together when there are several (and
// with the clips linked to them), in `mode`. Clips already warped keep their markers; their group gains the others.
Result enableElasticAudio(Project& p, Sequence& s, const std::vector<Id>& clips, ElasticMode mode = ElasticMode::Speech);
// Elastic audio and pitch edits off (the clip's group too).
Result removeElasticAudio(Project& p, Sequence& s, Id clip);
Result setElasticMode(Project& p, Sequence& s, Id clip, ElasticMode mode);
// The group's markers replaced (group seconds, in order). Markers at the clip's ends are added so the sound outside
// them stays put.
Result setWarpPoints(Project& p, Sequence& s, Id clip, DialogueWarp points);
// The marker that plays `from` (group seconds) moved to play at `to`, kept between its neighbours; no marker within
// `grab` seconds of `from`: one is made there first.
Result moveWarpMarker(Project& p, Sequence& s, Id clip, double from, double to, double grab = 0.01);
Result removeWarpMarker(Project& p, Sequence& s, Id clip, double at, double grab = 0.01);
// Markers at the onsets heard in the group's sound over the clip (none moved yet). `added` says how many.
Result addWarpMarkersAtOnsets(Project& p, Sequence& s, Id clip, double sensitivity = 50, int* added = nullptr);
// The clip's pitch edits replaced (none: taken off).
Result setPitchEdits(Project& p, Sequence& s, Id clip, const std::vector<PitchEdit>& edits);
}  // namespace edit

}  // namespace montage
