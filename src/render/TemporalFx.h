// Montage — effects that look at the frames either side: Motion Blur (like
// Resolve's, from optical flow: each pixel averaged along the path it moves
// on while the shutter is open) and Deflicker (like Resolve's, for timelapse
// and lights that flicker: the brightness of each part of the frame brought
// to its average over the neighbouring frames).
#pragma once

#include <vector>

#include "VideoDenoise.h"
#include "media/Image.h"

namespace montage {

// `back` and `fwd` give, for a pixel, where it is in the previous and next
// frame (either may be empty: no motion that way). `shutter` is the fraction
// of a frame the shutter is open (180° is 0.5), centred on the frame.
Image motionBlur(const Image& src, const MotionFn& back, const MotionFn& fwd, double shutter);

// Mean brightness of each of gw x gh areas of a frame.
struct TileStats {
    int gw = 0, gh = 0;
    std::vector<float> mean;
};
TileStats tileStats(const Image& img, int gw, int gh);

// `img` with each area's brightness moved towards the weighted average of the
// same area in `around` (each with its weight; the frame itself counts with
// `selfWeight`), by `strength` (0..1). The gains are blended smoothly across
// the frame, so no area edges show.
void deflicker(Image& img, const TileStats& self, double selfWeight, const std::vector<TileStats>& around,
               const std::vector<double>& weights, double strength);

}  // namespace montage
