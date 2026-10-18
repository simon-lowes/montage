// Montage — a programme delivered at another frame rate (Resolve's pulldown on render and its frame rate conversion,
// Media Composer's film projects, a standards converter):
//  - Pulldown: a 23.976 (or 24) programme as 29.97 (or 30) interlaced video, each four frames spread over five as
//    fields, 2:3 (A A, B B, B C, C D, D D) or 2:2:2:4 (A A, B B, C C, D D, D D: no frame of mixed fields); or as 59.94
//    (or 60) progressive frames, each repeated in the same pattern.
//  - Speed change: each frame shown once at the new rate, the programme faster or slower (PAL's 25 from 24 is 4 %
//    shorter), its sound sped with it, its pitch kept (WSOLA) or not (varispeed), its captions and markers with it.
//  - Standards conversion (25 to 29.97, 50 to 59.94 and back, or any rates): the programme keeps its length, each new
//    frame made from the two nearest by optical flow (or the RIFE model, as Frame Sampling makes in-betweens), and the
//    nearest frame instead wherever a cut falls between them, so no frame is a blend of two shots.
// The exporter renders and mixes the programme (render/Exporter.cpp) and writes what deliveredSequence gives.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

struct ExportSettings;

struct RateConversion {
    enum class Mode { None, Pulldown, Speed, Convert };
    Mode mode = Mode::None;
    Rational from{0, 1}, to{0, 1};
    bool fields = false;       // pulldown into fields (29.97 or 30, interlaced), not repeated frames
    bool cadence2224 = false;  // 2:2:2:4 rather than 2:3
    bool keepPitch = true;     // a speed change's sound keeps its pitch
    bool active() const { return mode != Mode::None; }
    // Programme seconds per delivered second: more than 1 for a speed-up (25 / 24); 1 for the others.
    double speed() const;
    // The same as a fraction in lowest terms.
    void speedRatio(int64_t& num, int64_t& den) const;
};

// The conversion `s` asks for (ExportSettings::rateChange and rate) of `seq`'s frame rate; false (with `error`) when
// it cannot be done.
bool planRateConversion(const Sequence& seq, const ExportSettings& s, RateConversion& out, std::string* error);

// How many frames are delivered for `frames` of the programme.
int64_t deliveredFrames(const RateConversion& c, int64_t frames);
// The delivered frame that shows programme frame `f` first (both counted from the start of the range).
int64_t deliveredFrameOf(const RateConversion& c, int64_t f);

// What delivered frame `o` is made of: programme frames counted from the range's start, under `frames`.
struct DeliveredPicture {
    int64_t a = 0, b = 0;  // fields: a's first field with b's second; an in-between: from a to b
    double t = 0;          // an in-between: how far from a towards b (0 = a itself)
    bool woven = false;    // a and b are woven as fields
};
DeliveredPicture deliveredPicture(const RateConversion& c, int64_t o, int64_t frames);

// The sequence as delivered: at the new rate, its caption tracks and markers on the delivered frames, counted from the
// range's start (programme frame `in` is delivered frame 0), anything outside [in, out) left out. Its clips are placed
// on the delivered frames too, for where its cuts fall (it is rendered and mixed as the programme, not as this).
Sequence deliveredSequence(const Sequence& seq, const RateConversion& c, FrameTime in, FrameTime out);

// Whether programme frames f - 1 and f are either side of an edit: a clip starting or ending there on a video track
// that is shown, with no transition across it.
bool editBetween(const Sequence& seq, FrameTime f);

// A picture's look for telling shots apart (64 by 36 luma and colour histograms, as scene detection compares frames).
struct ShotLook {
    std::vector<float> hist, luma;
};
ShotLook shotLook(const Image& img);
// 0 (alike) to 1 (nothing in common).
double shotDifference(const ShotLook& a, const ShotLook& b);
// Whether `a` and `b` are different shots: far apart, and far more so than the frames either side of them are from them
// (`before` a, `after` b; null when there is none), so fast motion is not taken for a cut.
bool looksLikeCut(const ShotLook& a, const ShotLook& b, const ShotLook* before, const ShotLook* after);

}  // namespace montage
