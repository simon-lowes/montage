// Montage — dual-system sound: picture from a camera and sound from a separate
// recorder joined into one clip (Premiere's Merge Clips, Final Cut's
// Synchronize Clips, Resolve's Auto Sync Audio, Avid's AutoSync). A merged
// clip is a nested sequence like a compound clip: the camera's picture on V1,
// each recorder file on its own audio track where its sound lines up, and the
// camera's own (scratch) sound below them, muted, when kept. The recorder's
// sound is trimmed to the picture.
#pragma once

#include <string>
#include <vector>

#include "Model.h"

namespace montage {

struct MergeOptions {
    std::string name;             // "" = "<camera clip> (merged)"
    bool keepCameraAudio = true;  // on a muted track under the recorder's sound
};

// `offsets[i]`: seconds after the picture starts that sound i starts (negative
// when it started first). Returns the merged media item, or 0 and `error`.
Id mergeClips(Project& p, Id video, const std::vector<Id>& sounds, const std::vector<double>& offsets, const MergeOptions& options = {},
              std::string* error = nullptr);

// Where a sound starts against a picture by their start timecodes, in
// seconds, when both have one and their spans overlap; false otherwise.
bool timecodeOffset(const Project& p, Id video, Id sound, double& offset);

// Each picture with the sound file whose timecode span overlaps it most (one
// sound may serve several pictures, as when the recorder rolls through takes).
struct SoundMatch {
    Id video = 0, sound = 0;
    double offset = 0;  // seconds, as for mergeClips
};
std::vector<SoundMatch> matchByTimecode(const Project& p, const std::vector<Id>& videos, const std::vector<Id>& sounds);

// Whether a media item is a merged clip.
bool isMergedClip(const Project& p, Id media);

}  // namespace montage
