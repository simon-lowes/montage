// Montage — clip animations (CapCut's In, Out and Combo animations, Premiere
// 26's drag-on clip animations): a preset entrance, exit or repeating motion
// on any picture clip, drawn on top of the clip's own transform and
// keyframes, so it keeps working when the clip is moved, trimmed or resized.
// In plays over the clip's first seconds, Out over its last, Combo all through.
#pragma once

#include <string>
#include <vector>

#include "EditOps.h"
#include "Model.h"

namespace montage {

enum class AnimationSlot { In = 0, Out = 1, Combo = 2 };

struct AnimationPreset {
    const char* id;
    const char* name;
};
// The kinds for a slot. In and Out share theirs (an exit mirrors its entrance:
// Slide Left comes in from the right and leaves to the left).
const std::vector<AnimationPreset>& animationPresets(AnimationSlot slot);
const AnimationPreset* findAnimationPreset(AnimationSlot slot, const std::string& id);

// How the animations move a clip at clip-local frame `local`: an offset as a
// fraction of the frame, a scale, a turn in degrees and an opacity factor.
struct AnimationPose {
    double dx = 0, dy = 0;
    double scale = 1;
    double rotation = 0;
    double opacity = 1;
};
AnimationPose clipAnimationPose(const Clip& c, double local, double fps);
bool hasClipAnimation(const Clip& c);

namespace edit {
// Sets (or with "" or "none" removes) a picture clip's animation in a slot,
// lasting `seconds` (In and Out; Combo's repeat period). Refused for sound
// clips, unknown kinds and lengths outside 0.1-10 s.
Result setClipAnimation(Sequence& s, Id clipId, AnimationSlot slot, const std::string& type, double seconds = 0.5);
}  // namespace edit

}  // namespace montage
