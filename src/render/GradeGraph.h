// Montage — a clip's grade node graph rendered (core/GradeGraph.h): each node after those feeding it, a corrector's
// effects limited by its window, qualifier and key input, the mixers joining their branches, the output's picture
// back in `img`. Everything runs in the sequence's working space, as the stack does.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

// Runs `c`'s node graph (nothing when it has none) on `img`, at clip time `lt`. `skip`: effects run elsewhere (those
// the compositor runs before or after the grade). `colourOnly`: only what depends on a pixel's colour alone (for a
// LUT): nodes limited by a window, a depth or an object are passed over and named in `skipped`.
void applyGradeGraph(const Sequence& s, const Clip& c, FrameTime lt, Image& img, double pixelScale, double sourceSeconds,
                     const std::function<bool(const Effect&)>& skip = {}, bool colourOnly = false,
                     std::vector<std::string>* skipped = nullptr);

}  // namespace montage
