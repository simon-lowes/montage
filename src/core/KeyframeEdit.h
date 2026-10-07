// Montage — editing a parameter's keyframes directly, as the timeline's
// lines over clips do (volume on audio clips, opacity on video clips):
// dragging the line between two keys, moving a key in time and value, and
// the scale volume lines are drawn on.
#pragma once

#include "Model.h"

namespace montage {

// Volume lines run from silence at the bottom to +6 dB at the top, linear in
// the square root of amplitude (0 dB sits at 71 %, -6 dB at half height).
constexpr double kGainLineMinDb = -60.0;
constexpr double kGainLineMaxDb = 6.0;
double gainToLevel(double db);     // 0 (bottom) to 1 (top)
double levelToGain(double level);  // clamped to [kGainLineMinDb, kGainLineMaxDb]

// Adds `delta` to the line at clip-local time t: to the static value, or to
// the two keys around t (to the first or last key outside them). Values stay
// within [lo, hi].
void offsetLine(Param& p, FrameTime t, double delta, double lo, double hi);
// Moves the key at `from` to time `to`, kept between its neighbours and
// within [0, last], with value v. Returns its new time, or -1 if no key is at `from`.
FrameTime moveKey(Param& p, FrameTime from, FrameTime to, double v, FrameTime last);

}  // namespace montage
