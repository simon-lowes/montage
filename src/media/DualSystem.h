// Montage — syncing a recorder's sound to camera clips: by timecode when both
// are stamped, else by matching the camera's own sound (the clap, the
// dialogue) against the recorder's, and Sync Dailies, which merges every
// picture with the sound file that belongs to it (core/MergeClips.h).
#pragma once

#include <string>
#include <vector>

#include "core/MergeClips.h"

namespace montage {

enum class SyncBy { Auto, Timecode, Waveform };

struct SoundSync {
    double offset = 0;      // seconds after the picture starts that the sound starts
    bool found = false;
    bool byTimecode = false;
    double confidence = 0;  // for a waveform match, 0..1
};

// Auto: timecode when both have it and they overlap, else the waveform.
SoundSync syncSound(const Project& p, Id video, Id sound, SyncBy by = SyncBy::Auto);

// Each picture among `media` with the sound file among them that belongs to
// it: timecode first, else the best waveform match. `report` gets a line for
// each picture left without one.
struct DailiesMatch {
    Id video = 0, sound = 0;
    SoundSync sync;
};
std::vector<DailiesMatch> matchDailies(const Project& p, const std::vector<Id>& media, std::vector<std::string>* report = nullptr);

// Merges the matches as new merged clips; returns their ids. `report` gets a
// line for each (what was paired, how, and the offset).
std::vector<Id> mergeDailies(Project& p, const std::vector<DailiesMatch>& matches, bool keepCameraAudio = true,
                             std::vector<std::string>* report = nullptr);

// Both at once.
std::vector<Id> syncDailies(Project& p, const std::vector<Id>& media, bool keepCameraAudio = true,
                            std::vector<std::string>* report = nullptr);

}  // namespace montage
