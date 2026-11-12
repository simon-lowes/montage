// Montage — pictures for HDR and EDR displays. The viewer shows HDR sequences on an extended-range surface (macOS
// EDR, Windows HDR) as the light they ask for instead of tone mapped to SDR: linear light in Rec. 709 primaries
// relative to SDR white (1.0 = SDR white, which BT.2408 puts at 203 nits for PQ and HLG), negative where a colour is
// outside Rec. 709 (an extended-range surface shows those too), and above 1.0 for highlights, up to the display's
// headroom (its peak over its SDR white). Brighter than the display can go, highlights roll off softly, keeping hue.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "ColorSpace.h"
#include "media/Image.h"

namespace montage {

// A frame for the HDR viewer: light relative to SDR white in Rec. 709 primaries (hdrViewImage with no roll-off), as
// RGBA half floats, and the brightest it can be; the viewer rolls it off to its display's headroom as it draws.
struct HdrPicture {
    int width = 0, height = 0;
    std::vector<uint16_t> rgba;
    double contentPeak = 1;
    // What it shows, for a surface whose light is described to the compositor (Linux, hdrSurfaceLight): the
    // sequence's transfer (PQ or HLG) and its measured light levels in nits (0 = not measured).
    Transfer transfer = Transfer::Pq;
    double maxCll = 0, maxFall = 0;
};
using HdrPicturePtr = std::shared_ptr<const HdrPicture>;

// The brightest a picture in `space` can ask for, relative to SDR white: the mastering peak for PQ (`hdrPeakNits`),
// 1000 nits for HLG (its reference display), SDR white for SDR spaces.
double hdrContentPeak(const ColorSpace& space, double hdrPeakNits);

// One pixel's code values in `space` to the light shown on a display with this much headroom (1 = an SDR display:
// HDR then rolls off from 75 % of white, as the SDR preview does). Scene-referred spaces give their SDR view.
void hdrViewPixel(float rgb[3], const ColorSpace& space, double hdrPeakNits, double headroom);

// A frame (premultiplied, shown over black) as RGBA half floats for an extended-range texture, alpha 1. `scale`
// multiplies the light: 1 where 1.0 is SDR white (macOS EDR), SDR white's nits / 80 for Windows' scRGB.
void hdrViewImage(const Image& in, const ColorSpace& space, double hdrPeakNits, double headroom, std::vector<uint16_t>& out,
                  float scale = 1);

// The viewer on a surface described to the compositor as BT.2100 (Linux, Wayland's colour management): light relative
// to SDR white in Rec. 709 primaries, as drawn, to the PQ or HLG signal in BT.2020 primaries, SDR white at BT.2408's
// 203 nits (HLG on its 1000-nit reference display), so a sequence's own code values come out as they went in. Light
// outside BT.2020 is clipped to it. The viewer's shader does the same on the GPU.
void hdrSurfaceSignal(float rgb[3], Transfer transfer);

// The light such a surface carries, for its image description: the mastering display (P3-D65 primaries, its peak the
// picture's, no darker than PQ's 0.005 nits so the volume stays inside the signal's) and the measured MaxCLL and
// MaxFALL, none brighter than the display's peak (`headroom` times 203 nits) where the viewer rolls highlights off
// to it, so the compositor does not roll them off a second time.
struct HdrSurfaceLight {
    double masteringMinNits = 0.005, masteringMaxNits = 1000;
    double maxCll = 0, maxFall = 0;  // 0 = not stated
    bool operator==(const HdrSurfaceLight&) const = default;
};
HdrSurfaceLight hdrSurfaceLight(const HdrPicture& picture, double headroom);

// That light as wp_image_description_creator_params_v1 takes it: whole nits, the mastering minimum in 1/10000 nit,
// and 0 for a MaxCLL or MaxFALL that is not sent. Whatever it is given, the result is inside what the protocol
// allows, because a value outside it is a protocol error, which ends the whole connection: the mastering range
// inside the signal's own (0.005 nits to PQ's 10 000 or HLG's 1000) with its maximum above its minimum, MaxCLL and
// MaxFALL above that minimum and no more than that maximum, and MaxFALL no more than MaxCLL. For a compositor that
// takes no mastering range (`masteringRange` false) the signal's own range is the bound, as the protocol has it.
struct HdrSurfaceLightArgs {
    uint32_t masteringMin10k = 50, masteringMax = 10000, maxCll = 0, maxFall = 0;
    bool operator==(const HdrSurfaceLightArgs&) const = default;
};
HdrSurfaceLightArgs hdrSurfaceLightArgs(const HdrSurfaceLight& light, Transfer transfer, bool masteringRange);

// A display's headroom (its peak over SDR white) from the image description its compositor prefers for a surface:
// the target volume's peak (else the primary volume's) over the reference white, at least 1.
double hdrHeadroomFromDescription(double referenceNits, double maxNits, double targetMaxNits);

}  // namespace montage
