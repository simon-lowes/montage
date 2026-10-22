// Montage — Breath Control: the breaths between a speaker's lines found and turned down with volume keys, as a
// dialogue editor does by hand (iZotope RX's Breath Control, Audition's breath removal). Each breath is its own dip
// in the clip's volume line, so any one can be put back by dragging its keys.
#pragma once

#include <atomic>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/Model.h"
#include "core/Transcript.h"
#include "media/Decoder.h"

namespace montage {

using TimeSpans = std::vector<std::pair<double, double>>;  // [start, end) seconds, sorted

// Breaths in a recording (media seconds). A breath is a stretch of 0.12 to 0.9 s with no voicing (no pitch between
// 80 and 400 Hz), louder than the room by 6 dB or more but 10 dB or more under the speech, and not bright like an
// "s". With a transcript, only the gaps between its words are searched; without one, a breath must also be at least
// 0.15 s long. Only [from, to) seconds is looked at when given (the room and speech levels are taken there too).
TimeSpans findBreaths(const AudioBuffer& audio, const Transcript* transcript = nullptr, double from = 0, double to = -1,
                      const std::atomic<bool>* cancel = nullptr);

// The breaths in the stretch of media a clip plays (its audio, the channels it plays, and its transcript when it has
// one), in media seconds. `decoded` keeps each file's sound for the next clip of the same media.
TimeSpans breathsInClip(const Project& p, const Sequence& s, const Clip& c, std::string* error = nullptr,
                        const std::atomic<bool>* cancel = nullptr, std::map<std::string, AudioBufferPtr>* decoded = nullptr);

// Whether a clip's volume can take breath keys: not when an expression drives it or its keys repeat, where keys
// added would not leave the rest of the line as it was.
bool volumeTakesBreathKeys(const Clip& c);

// Turns each breath the clip plays down by `amountDb` (under 0) with keys on its volume (gain_db), a frame to fall
// and a frame to rise, around the frames it covers; the rest of the volume line is left as it was. A breath already
// down that far is left. Returns how many breaths were keyed.
int lowerBreaths(Clip& c, const Sequence& s, const TimeSpans& breaths, double amountDb);

// Where on the timeline (seconds) the clip plays a stretch of its media (media seconds), or {-1, -1} when it doesn't.
std::pair<double, double> timelineSpan(const Clip& c, const Sequence& s, double from, double to);

}  // namespace montage
