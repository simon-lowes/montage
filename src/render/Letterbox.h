// Montage — removing letterbox and pillarbox bars (Descript's automatic crop; Premiere's and Resolve's editors do it by
// hand with crop and scale): footage that arrives with black bars baked in (a 2.39:1 film in a 16:9 file, 4:3 in 16:9,
// a phone recording framed in a landscape video) has them found over several frames, cropped off, and the picture
// left scaled to fill the frame.
#pragma once

#include <atomic>
#include <string>

#include "core/EditOps.h"
#include "core/Model.h"

namespace montage {

struct Bars {
    double left = 0, right = 0, top = 0, bottom = 0;  // fractions of the picture
    bool any() const { return left > 0 || right > 0 || top > 0 || bottom > 0; }
};

// The black bars around the picture of a video between `from` and `to` (seconds of the media): rows and columns that
// stay near black on every frame looked at (frames black all over, a fade, are passed over). Bars under 1 % of the
// picture are not counted. False (with `error`) if the video cannot be read.
bool detectBars(const std::string& path, double from, double to, Bars& out, std::string* error = nullptr, int samples = 9,
                const std::atomic<bool>* cancel = nullptr);

namespace edit {

// Crops the bars off clip `clip` and scales and centres what is left to fill the sequence's frame (static values;
// position, scale and crop keyframes are replaced).
Result removeLetterbox(Project& p, Sequence& s, Id clip, const Bars& bars);

}  // namespace edit

}  // namespace montage
