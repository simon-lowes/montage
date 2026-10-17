// Montage — dialogue alignment: an ADR take, a pickup or a converted voice fitted to the guide track's timing with its
// pitch kept, or a second microphone of the same take lined up with the boom by shifting alone. Nuendo's Audio
// Alignment and the paid VocAlign and Revoice Pro plug-ins do the first; this one is Montage's own.
//
// Both sounds become frames 5 ms apart: the broad shape of the spectrum (the first six cepstra of 40 mel bands, each
// over its mean and spread across the speech, so the microphone, the room and the voice count little) and the level's
// rise and fall (the rhythm a different voice keeps), with their deltas, gated to speech by level. Dynamic time warping finds the cheapest path between them in a band either side of the diagonal,
// its steps limited to slopes between 1:2 and 2:1, with the start and end free within the take's handles; silence
// against silence costs little, so pauses take up the difference. The path is smoothed and thinned to anchors (the
// guide's syllable onsets, and a grid as close as Alignment Precision asks within the speech), kept monotone, and
// played by WSOLA (audio/TimeStretch.h), which keeps the pitch.
//
// The result is a source-audio effect, "dialogue_align" (audio/SpeechCleanup.h): its "warp" string maps times of the
// sound the clip plays to times of the take's own file, in seconds, so it is cached like Noise Reduction, undone like
// any edit and taken off by removing it. Its "media" string names the take it was made for: on an audition only that
// take is warped. Shifting is the same effect with one pair (a fixed delay, played at sub-sample precision) and
// "invert" when the polarity was flipped.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "core/EditOps.h"
#include "core/Model.h"
#include "media/Decoder.h"

namespace montage {

// Plays the original's time `w` at time `u` of the result (source seconds). Before the first point and after the last
// the offset (w - u) stays as it is there.
struct AlignPoint {
    double u = 0, w = 0;
    bool operator==(const AlignPoint&) const = default;
};
using DialogueWarp = std::vector<AlignPoint>;
std::string dialogueWarpToString(const DialogueWarp& warp);  // "u:w,u:w,..." (seconds, to the microsecond)
DialogueWarp dialogueWarpFromString(const std::string& text);  // points that are not in order are left out
double dialogueWarpAt(const DialogueWarp& warp, double u);   // w at u
double dialogueWarpInverse(const DialogueWarp& warp, double w);  // u at w (the warp only ever moves forward)

// The warp played: the result is `in` re-timed (WSOLA), or, when the warp is a fixed delay, shifted with a windowed-sinc
// interpolator so a fraction of a sample counts; `invert` flips its polarity. Its length is the original's less the
// last offset.
void applyDialogueWarp(const AudioBuffer& in, const DialogueWarp& warp, bool invert, AudioBuffer& out,
                       const std::atomic<bool>* cancel = nullptr);

enum class AlignPrecision { Loose, Normal, Tight };
const char* alignPrecisionName(AlignPrecision p);  // "loose", "normal", "tight"
bool parseAlignPrecision(const std::string& name, AlignPrecision& out);

// Where each moment of the guide is in the take: anchors (guide seconds, take seconds), in order, both from the
// start of the buffers given. The take may start anywhere in its first `startSlack` seconds and end anywhere in its
// last `endSlack` (its handles). Empty (with `error`) when either sound has no speech or it was cancelled.
struct SpeechAlignment {
    std::vector<std::pair<double, double>> anchors;
    double minRate = 1, maxRate = 1;  // the steepest and shallowest stretch between anchors (take seconds per guide second)
    double distance = 0;              // mean feature distance along the path over the guide's speech
};
SpeechAlignment alignSpeech(const AudioBuffer& guide, const AudioBuffer& take, double startSlack, double endSlack,
                            AlignPrecision precision = AlignPrecision::Normal, const std::atomic<bool>* cancel = nullptr,
                            std::string* error = nullptr);

// The delay and polarity between two microphones of one take: `other`'s sound comes `delay` seconds after `ref`'s
// (negative: before), searched within +-maxLag, to a fraction of a sample (parabolic peak of the cross-correlation).
struct DialogueShift {
    bool found = false;
    double delay = 0;
    bool inverted = false;
    double confidence = 0;  // normalised correlation at the peak, 0..1
};
DialogueShift findDialogueShift(const AudioBuffer& ref, const AudioBuffer& other, double maxLag = 0.25);

struct DialogueAlignOptions {
    AlignPrecision precision = AlignPrecision::Normal;
    bool shift = false;              // Prefer Shifting: a fixed delay (and polarity), no stretch
    FrameTime from = -1, to = -1;    // the guide's range on the timeline; -1: where the two clips overlap
    double handles = 0.5;            // seconds of the take either side of that range it may move into
};
struct DialogueAlignReport {
    int anchors = 0;
    double minRate = 1, maxRate = 1;   // stretches (warp)
    double delay = 0;                  // seconds (shift)
    bool inverted = false;
    double confidence = 0;
    double from = 0, to = 0;           // the guide's range, timeline seconds
};

namespace edit {
// Aligns the take clip's sound (an audio clip; on an audition, the take it plays) to the guide clip's over the range:
// a "dialogue_align" effect on the take clip, replacing an earlier one for the same take. The clip stays where it is;
// when it does not reach to the end of the range and the track has room, it is lengthened to. Both clips must play
// at their own speed, forwards.
Result alignDialogue(Project& p, Sequence& s, Id takeClip, Id guideClip, const DialogueAlignOptions& o,
                     DialogueAlignReport* report = nullptr, const std::atomic<bool>* cancel = nullptr);
// Takes the alignment off the clip's current take; false when it had none.
bool removeDialogueAlignment(Project& p, Sequence& s, Id clip);
}  // namespace edit

}  // namespace montage
