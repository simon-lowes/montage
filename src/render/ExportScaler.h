// Montage — the scaler an export converts its rendered pictures with (render/Exporter), shared with Compare Encode
// (render/EncodeCompare), so the sequence an encode is compared with has its chroma made by the same filter, at the
// same sites, as the export made the encode's: the reference then differs from the encode by the encoder's loss alone.
#pragma once

extern "C" {
#include <libavutil/pixfmt.h>
}

struct SwsContext;

namespace montage {

// From 16-bit RGBA (as toRgba16 makes it) at srcW x srcH to `dst` at dstW x dstH: bicubic, accurate rounding, the
// chroma made from every input pixel, sited where `loc` says (left, or top left: the scaler centres it otherwise), and
// coded by `cs`'s matrix (BT.2020's, else BT.709's) in `range`. `cached` is the context from the frame before (kept
// when it fits, freed otherwise); null when the scaler refuses.
SwsContext* exportPictureScaler(SwsContext* cached, int srcW, int srcH, int dstW, int dstH, AVPixelFormat dst,
                                AVChromaLocation loc, AVColorSpace cs, AVColorRange range);

}  // namespace montage
