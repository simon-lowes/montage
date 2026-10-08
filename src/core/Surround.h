// Montage — surround layouts and panning: which speakers a sequence mixes
// to, where each sound goes among them, and how the whole folds down to
// stereo for listening on headphones or two speakers.
#pragma once

#include <string>
#include <vector>

#include "Model.h"

namespace montage {

struct Speaker {
    const char* name;      // "L", "R", "C", "LFE", "Ls", "Rs", "Lb", "Rb"
    double angle;          // degrees from straight ahead, positive to the right
    bool lfe;
    double loudnessWeight;  // ITU-R BS.1770: 1 at the front, 1.41 round the sides and back, 0 for the LFE
};

// The speakers of "stereo", "5.1" (L R C LFE Ls Rs) or "7.1" (L R C LFE Lb Rb Ls Rs), in channel order
// (FFmpeg's). Anything else is stereo.
const std::vector<Speaker>& layoutSpeakers(const std::string& layout);
int layoutChannels(const std::string& layout);
const std::vector<std::string>& audioLayouts();  // "stereo", "5.1", "7.1"

// Constant-power gains for a single sound at `angle` degrees, `distance` 0
// (the middle: spread evenly) to 1 (at the speakers: between the two either
// side of it). The LFE gets nothing.
std::vector<float> panGains(const std::string& layout, double angle, double distance);

// A stereo track's two channels through its panner: the gains of its left
// and right channels into each speaker, and its LFE send (linear, applied
// to (L + R) / sqrt 2). Narrowed, both channels are turned down by up to
// 3 dB, so a sound in both is as loud on one speaker as it was on two.
struct SurroundGains {
    std::vector<float> left, right;
    float lfe = 0;
};
SurroundGains surroundGains(const std::string& layout, const SurroundPan& p);

// ITU-R BS.775 fold-down to stereo: the centre and the surrounds at -3 dB
// into their side, the LFE left out. `in` has the layout's channels; `out`
// is stereo. In place is not allowed.
void downmixToStereo(const std::string& layout, const float* in, int frames, float* out);

}  // namespace montage
