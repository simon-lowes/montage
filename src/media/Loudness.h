// Montage — loudness measurement (ITU-R BS.1770-4 / EBU R128).
#pragma once

#include <cstdint>

#include "Decoder.h"

namespace montage {

struct LoudnessResult {
    double integrated = -70.0;  // LUFS (gated)
    double truePeakDb = -96.0;  // sample peak, dBFS
    bool valid = false;         // false if everything was below the absolute gate
};

// Measures interleaved-stereo samples [first, first + count) of `buf`
// (count < 0 = to the end).
LoudnessResult measureLoudness(const AudioBuffer& buf, int64_t first = 0, int64_t count = -1);

}  // namespace montage
