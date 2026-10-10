// Montage — checkerboarding dialogue: a clip where several people speak is
// split where the speaker changes (in the silence between them) and each
// person's parts are moved to a track of their own, so each voice can be
// levelled, EQ'd and cleaned up on its own track, as dialogue editors do
// before the mix. Uses the speaker labels of the media's transcript.
#pragma once

#include "EditOps.h"

namespace montage {

// One undo step's worth: `clip` (an audio clip at normal speed whose media has
// a transcript with speakers) split at each change of speaker; the first
// speaker's parts stay, the others' move to the first audio track below that
// is free where they go, or to a new track named after the speaker.
// `speakers`, if given, receives how many people were found.
edit::Result checkerboardBySpeaker(Project& p, Sequence& s, Id clip, int* speakers = nullptr);

}  // namespace montage
