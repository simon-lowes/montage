// Montage — conform check against the offline reference: the editor's reference movie (the offline cut as it was
// signed off) laid against a sequence built from a list (an imported XML, AAF or EDL), and each shot of the sequence
// compared with it. A shot can match; show its source at another frame (slipped); show another take; show the right
// frames framed differently (a reposition the list lost, solved from the reference as scale, rotation and position,
// keyed where it moves); or match nothing near. Where the sequence has nothing and the reference a picture, a shot is
// missing. Grades are not counted (each picture is normalised), and burnt-in timecode and other overlays the reference
// carries are masked: what stays the same across its shots.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

enum class ConformVerdict { Matches, Reframed, Slipped, OtherTake, NoMatch, Missing };
// "Matches", "Reframed", "Slipped", "Other take", "No match", "Missing"
const char* conformVerdictName(ConformVerdict v);

// A reposition at one frame of a shot (clip-local frame `at`), in the Transform's own terms.
struct ConformKey {
    FrameTime at = 0;
    double posX = 0, posY = 0, scale = 100, rotation = 0;  // pixels, percent, degrees
};

struct ConformShot {
    Id clip = 0;                      // 0: Missing
    FrameTime start = 0, length = 0;  // the part of the timeline where it is the picture on top (Missing: the gap)
    ConformVerdict verdict = ConformVerdict::NoMatch;
    double score = 0;                 // 0..1: how alike the reference and the shot (as fixed) are
    FrameTime slip = 0;               // Slipped: the source frames to move it by (as edit::slip takes them)
    Id media = 0;                     // OtherTake: the take the reference shows
    double sourceIn = 0;              // OtherTake: where the clip starts in it (sequence frames, as Clip::sourceIn)
    std::vector<ConformKey> keys;     // Reframed: the Transform (one key: still; more: it moves)
    std::string details;              // e.g. "scale 112.0 %, rotation 1.5°, position (40, -20)", "3 frames late"
    bool fixable() const { return verdict == ConformVerdict::Reframed || verdict == ConformVerdict::Slipped || verdict == ConformVerdict::OtherTake; }
};

struct ConformOptions {
    FrameTime referenceStart = 0;  // the timeline frame the reference's first frame shows
    int analysisWidth = 640;       // pictures are compared at most this wide
    int slipRange = 0;             // frames searched either side for a slip (0: two seconds)
    int keyEvery = 12;             // frames between the samples a moving reposition is keyed from
    double matchScore = 0.8;       // the likeness a shot needs to count as the same frames
};

using ConformProgress = std::function<void(double fraction)>;

// Each shot of `seq` (each stretch where one clip from media is the picture on top) against `reference` (a video
// media item of `p`), and the gaps where the reference has a picture, in time order. Empty with `error` set when the
// reference cannot be read.
std::vector<ConformShot> conformCheck(const Project& p, const Sequence& seq, Id reference, const ConformOptions& options = {},
                                      const ConformProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                                      std::string* error = nullptr);

// Puts right what a shot's check found (its Transform keyed, its source slipped, or the other take put in, with its
// linked sound); false when there is nothing to fix or the clip is gone.
bool applyConformFix(Project& p, Sequence& seq, const ConformShot& shot);

}  // namespace montage
