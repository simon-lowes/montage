// Montage — colour spaces and conversions between them: camera log formats,
// SDR and HDR (PQ, HLG) video, tone mapping between HDR and SDR. Pixels are
// converted when a clip's media enters a sequence whose colour space differs.
#pragma once

#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

enum class Transfer {
    Bt1886,   // SDR video (gamma 2.4 display)
    Srgb,     // sRGB piecewise curve
    Linear,   // scene or display linear
    Pq,       // SMPTE ST 2084 (absolute, 10 000 nits = 1.0)
    Hlg,      // ARIB STD-B67 / BT.2100 HLG
    SLog3,    // Sony
    LogC3,    // ARRI ALEXA (EI 800)
    LogC4,    // ARRI ALEXA 35
    VLog,     // Panasonic
    CLog3,    // Canon Log 3
    AcesCct,  // ACEScct
    AppleLog,             // Apple Log (iPhone)
    DLog,                 // DJI D-Log
    FLog2,                // Fujifilm F-Log2
    NLog,                 // Nikon N-Log
    Log3G10,              // RED Log3G10
    BmdFilmGen5,          // Blackmagic Film Generation 5
    DavinciIntermediate,  // DaVinci Intermediate
};

enum class Primaries {
    Bt709, Bt2020, P3D65, SGamut3Cine, AlexaWideGamut3, AlexaWideGamut4, VGamut, CinemaGamut, Ap1, Ap0,
    SGamut3, DGamut, RedWideGamut, BmdWideGamut, DavinciWideGamut
};

struct ColorSpace {
    std::string id;     // e.g. "rec709", "rec2100pq", "slog3-sgamut3cine"
    std::string label;  // shown in menus
    Primaries primaries = Primaries::Bt709;
    Transfer transfer = Transfer::Bt1886;
    bool sceneReferred = false;  // camera log / linear: needs a display rendering to view
    bool hdr() const { return transfer == Transfer::Pq || transfer == Transfer::Hlg; }
};

// Every colour space Montage knows, Rec.709 first.
const std::vector<ColorSpace>& colorSpaces();
const ColorSpace* findColorSpace(const std::string& id);
// The space FFmpeg's tags describe (primaries and transfer names as av_color_*_name gives them).
std::string colorSpaceFromTags(const std::string& primaries, const std::string& transfer);

// Display spaces a sequence can work in and deliver to (no camera log).
std::vector<const ColorSpace*> displayColorSpaces();
// The space a media item is read as (its Interpret Colour override, else
// what its tags say, else Rec.709) and the space a sequence works in.
const ColorSpace& mediaColorSpace(const MediaItem& m);
const ColorSpace& sequenceColorSpace(const Sequence& s);
const ColorSpace& rec709Space();

// Transfer functions: code value -> linear (display light for display
// transfers, relative to SDR white = 1.0, so PQ 203 nits = 1.0) and back.
double toLinear(Transfer t, double v);
double fromLinear(Transfer t, double l);

// 3x3 matrix taking linear RGB in `from` primaries to `to` primaries, through
// XYZ D65 (Bradford adaptation for the ACES whites). Camera gamuts whose makers
// publish a matrix to ACES use it, as the ACES IDTs and OpenColorIO do.
void primariesMatrix(Primaries from, Primaries to, double m[9]);
// Linear RGB in `p` to CIE XYZ relative to D65 (Bradford adapted when `p` has another white).
void primariesToXyz(Primaries p, double m[9]);

// Converts premultiplied RGBA pixels from one space to another in place.
// HDR shown in SDR is tone mapped (BT.2390-style roll-off from `hdrPeakNits`);
// SDR placed in HDR puts SDR white at 203 nits (BT.2408); scene-referred
// sources get a filmic display rendering.
void convertColor(Image& img, const ColorSpace& from, const ColorSpace& to, double hdrPeakNits = 1000);

// Light in cd/m² (nits) for display spaces: PQ as coded, HLG on a 1000-nit display (its OOTF), SDR with white at
// 100 nits. A neutral code value's light and back (scope graticules); `nits` below 0 for camera log and linear.
double codeToNits(const ColorSpace& space, double code);
double nitsToCode(const ColorSpace& space, double nits);
// A pixel's brightest channel in nits (CTA-861.3's maxRGB, which MaxCLL and MaxFALL are made of).
double pixelMaxNits(const ColorSpace& space, const float rgb[3]);

// For tests and scopes: one pixel through the same conversion.
void convertPixel(float rgb[3], const ColorSpace& from, const ColorSpace& to, double hdrPeakNits = 1000);

}  // namespace montage
