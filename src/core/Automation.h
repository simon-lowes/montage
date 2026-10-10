// Montage — track fader automation, written live from the mixer as in Premiere's Audio Track Mixer, Pro Tools and
// Fairlight. Each audio track has a volume (dB) and a pan lane (and, in surround, x, y and z lanes for its position),
// keyed in timeline frames, and a mode:
//  - Off: the fader as set; the lanes are kept but not heard.
//  - Read: the lanes play (when they have keys) and the fader follows them.
//  - Write: from the moment playback starts until it stops, the fader is recorded.
//  - Latch: like Read until the fader is first moved, then recorded until playback stops.
//  - Touch: recorded while the fader is held; on letting go it glides back to what was there.
#pragma once

#include "Model.h"

namespace montage {

enum class AutomationMode { Off = 0, Read = 1, Write = 2, Latch = 3, Touch = 4 };

const char* automationModeName(AutomationMode m);
AutomationMode trackAutomation(const Track& t);

// The fader values heard at timeline frame t (fractional frames interpolate between whole ones).
double trackVolumeAt(const Track& t, double frame);
double trackPanAt(const Track& t, double frame);
// Whether a track's surround position moves (any of its x, y, z lanes has keys), and where it is at frame t when the
// lanes play: SurroundPan with x, y and z from the lanes that have keys (within -1..1, z 0..1).
bool surroundAnimated(const Track& t);
SurroundPan trackSurroundAt(const Track& t, double frame);

// Reduces a lane to the keys needed to stay within `tolerance` of it (Ramer–Douglas–Peucker on linear segments).
void thinKeys(std::vector<Keyframe>& keys, double tolerance);

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
    Param finish(FrameTime stop, double tolerance);

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
