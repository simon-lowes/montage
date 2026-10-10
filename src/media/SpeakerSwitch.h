// Montage — automatic multicam switching by who is speaking. Who speaks is
// known either from microphones (each speaker angle listens to an audio
// track of the multicam sequence, usually that person's mic, and the clearly
// loudest one gets the shot) or from a transcript's speaker labels (one
// recording is enough). A wide angle covers silence and cross-talk, and
// shots are held for a minimum length so the cut does not chatter.
#pragma once

#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "core/Model.h"

namespace montage {

struct AutoSwitchOptions {
    // Per angle: the multicam sequence's audio track carrying its speaker, or
    // -1 for an angle that is not anyone's close-up. Empty = each angle's own sound.
    std::vector<int> listen;
    int wideAngle = -1;          // shown during silence and cross-talk; -1 = stay on the current angle
    double minShotSeconds = 2.0;
    double marginDb = 4.0;       // a speaker must be this much louder than everyone else
    double silenceDb = -45.0;    // quieter than this (dBFS) is nobody speaking
    double holdSeconds = 0.3;    // how long a new speaker must last before the cut
};

// Angle changes as (multicam sequence frame, angle), the first at frame 0.
// Empty (and `error`) if no angle has a speaker or the audio cannot be read.
std::vector<std::pair<FrameTime, int>> speakerAngleChanges(const Project& p, const Sequence& mc, const AutoSwitchOptions& o,
                                                           std::string* error = nullptr,
                                                           const std::atomic<bool>* cancel = nullptr);

// Speaker turns heard on audio track `track` of the multicam sequence, from
// its clips' labelled transcripts, in multicam sequence seconds. `speakers`
// receives how many speakers the transcripts know.
std::vector<SpeakerTurn> transcriptTurns(const Project& p, const Sequence& mc, int track, int* speakers = nullptr);
// Angle changes from speaker turns: angleOfSpeaker[speaker] is the angle
// that shows them (-1: no close-up). Shots follow `o` (listen is ignored).
std::vector<std::pair<FrameTime, int>> turnAngleChanges(const Sequence& mc, const std::vector<SpeakerTurn>& turns,
                                                        const std::vector<int>& angleOfSpeaker, const AutoSwitchOptions& o,
                                                        std::string* error = nullptr);

}  // namespace montage
