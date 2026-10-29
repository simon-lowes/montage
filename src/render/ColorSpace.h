// Montage — colour spaces and conversions between them: camera log formats,
// SDR and HDR (PQ, HLG) video, tone mapping between HDR and SDR. Pixels are
// converted when a clip's media enters a sequence whose colour space differs.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

struct Lut3D;  // (render/Processing.h)

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
    GoProLog,             // GoPro GP-Log (HERO12 and HERO13 Black)
    GoProLog2,            // GoPro GP-Log2 (MISSION 1)
    LLog,                 // Leica L-Log
};

enum class Primaries {
    Bt709, Bt2020, P3D65, SGamut3Cine, AlexaWideGamut3, AlexaWideGamut4, VGamut, CinemaGamut, Ap1, Ap0,
    SGamut3, DGamut, RedWideGamut, BmdWideGamut, DavinciWideGamut, AppleWideGamut
};

// How SDR and HDR meet. Montage's own: SDR white at HDR reference white (203 nits, BT.2408), and HDR into SDR through a
// roll-off of the brightest channel. Or the methods of Report ITU-R BT.2446-1: Method A (SDR's peak to 1000 nits and
// back, through a perceptual curve on luma with its colour correction) or Method C (a linear part and a log shoulder on
// luminance, between crosstalk matrices, the one exactly the other's inverse).
enum class ToneMapping { Montage, Bt2446A, Bt2446C };
// Its id: "" (Montage's), "bt2446a" or "bt2446c"; and back (false for anything else).
const char* toneMappingId(ToneMapping t);
bool parseToneMapping(const std::string& id, ToneMapping& out);
// The HDR light (cd/m²) a BT.2446 method brings down to SDR white (and SDR white up to): 1000 for Method A, about 333
// for C; 0 for Montage's, whose roll-off has no such point.
double toneMappingWhiteNits(ToneMapping t);

struct ColorSpace {
    std::string id;     // e.g. "rec709", "rec2100pq", "slog3-sgamut3cine"
    std::string label;  // shown in menus
    Primaries primaries = Primaries::Bt709;
    Transfer transfer = Transfer::Bt1886;
    bool sceneReferred = false;  // camera log / linear: needs a display rendering to view
    // An SDR display space's: how it is brought into HDR (a clip's picture) and how HDR is made into it (a delivery).
    ToneMapping toneMapping = ToneMapping::Montage;
    bool hdr() const { return transfer == Transfer::Pq || transfer == Transfer::Hlg; }
};

// Every colour space Montage knows, Rec.709 first.
const std::vector<ColorSpace>& colorSpaces();
// One of them by id, or an SDR display space with a tone mapping ("rec709+bt2446a", as withToneMapping names it), or a
// scene-referred one holding display light ("linear-rec709+display", displayLightOf).
const ColorSpace* findColorSpace(const std::string& id);
// An SDR display space with that tone mapping (its id the space's with "+bt2446a" or "+bt2446c"); any other space, or
// with Montage's, as it is (an SDR space with another tone mapping, without it).
const ColorSpace& withToneMapping(const ColorSpace& s, ToneMapping t);
// A scene-referred space's encoding holding display light ("linear-rec709+display"): what a display-referred sequence
// writes when asked for a scene-referred space (its display's light, no display rendering undone), so read back
// exactly, without a display rendering. A display space is its own.
const ColorSpace& displayLightOf(const ColorSpace& s);
// Whether two spaces hold the same code values (the same space, whatever tone mapping each has).
bool sameColorSpace(const ColorSpace& a, const ColorSpace& b);
// The space FFmpeg's tags describe (primaries and transfer names as av_color_*_name gives them).
std::string colorSpaceFromTags(const std::string& primaries, const std::string& transfer);

// Display spaces a sequence can work in and deliver to (no camera log).
std::vector<const ColorSpace*> displayColorSpaces();
// The space a media item is read as (its Interpret Colour override, else
// what its tags say, else Rec.709) and the space a sequence works in.
const ColorSpace& mediaColorSpace(const MediaItem& m);
const ColorSpace& sequenceColorSpace(const Sequence& s);
const ColorSpace& rec709Space();

// ---- Scene-referred grading: a sequence's working space apart from its delivery space ----
// Scene-referred spaces a sequence can grade and composite in.
std::vector<const ColorSpace*> workingColorSpaces();
// The space a sequence's clips are graded in: its scene-referred working space when it has one, else its delivery space
// (sequenceColorSpace).
const ColorSpace& workingColorSpace(const Sequence& s);
bool sceneReferredWorking(const Sequence& s);
// Linear light in `p`'s primaries (ACEScg for AP1, Linear Rec.709, ACES2065-1, else one made for them), and ACEScct's
// log curve on them (ACEScct itself for AP1): where effects that work in light, or in log, run.
const ColorSpace& linearColorSpace(Primaries p);
const ColorSpace& logColorSpace(Primaries p);

// How scene light is shown on a display: Montage's filmic transform (the ACES 2.0 tone scale on each channel, in the
// display's primaries), or ACES 2.0 itself through OpenColorIO.
enum class OutputTransformKind { Filmic, Aces2 };
OutputTransformKind outputTransformKind(const Sequence& s);
// Whether ACES 2.0 can render for `to` (OpenColorIO 2.5 and later, for the displays its ACES 2.0 config has).
bool aces2Renders(const ColorSpace& to);
// A display's peak in nits: SDR 100, HLG 1000, PQ `peakNits`.
double displayPeakNits(const ColorSpace& to, double peakNits);
// The filmic tone scale: scene light (0.18 grey) to the nits a display of `peakNits` shows, and back.
double filmicNits(double scene, double peakNits);
double filmicScene(double nits, double peakNits);
// Premultiplied scene-referred pixels in `from` through the output transform into display space `to` (`peakNits` an
// HDR one's peak), through a table unless `exact`; into a scene-referred `to` they are converted exactly instead (a VFX
// pull keeps the working space's light).
void outputTransform(Image& img, const ColorSpace& from, const ColorSpace& to, double peakNits, OutputTransformKind kind,
                     bool exact = false);
// A display-referred picture in `from` (graphics, a display-referred clip) back through the inverse of display `from`'s
// output transform into scene-referred `to`, so it comes out of the output transform as it went in.
void inverseOutputTransform(Image& img, const ColorSpace& from, const ColorSpace& to, double peakNits, OutputTransformKind kind);
// The same on one colour (not premultiplied), exactly.
void outputPixel(float rgb[3], const ColorSpace& from, const ColorSpace& to, double peakNits, OutputTransformKind kind);
void inverseOutputPixel(float rgb[3], const ColorSpace& from, const ColorSpace& to, double peakNits, OutputTransformKind kind);
// A sequence graded scene-referred, as the compositor draws it (set for each such sequence's frame): where its clips
// are graded, the linear light of its primaries (where layers are composited), and the display the frame is made for
// (graphics come in through its inverse output transform; effects made for display values run through it and back).
struct SceneGrading {
    const ColorSpace* working = nullptr;
    const ColorSpace* linear = nullptr;
    const ColorSpace* display = nullptr;
    const ColorSpace* delivery = nullptr;  // the sequence's own space (where grain and the like are measured)
    double peakNits = 1000;
    OutputTransformKind kind = OutputTransformKind::Filmic;
    bool exact = false;  // (deliveries: the output transform worked out for each pixel)
};
const SceneGrading* currentSceneGrading();
class SceneGradingScope {
public:
    explicit SceneGradingScope(const SceneGrading* g);
    ~SceneGradingScope();
    SceneGradingScope(const SceneGradingScope&) = delete;
    SceneGradingScope& operator=(const SceneGradingScope&) = delete;

private:
    const SceneGrading* previous_;
};
// Premultiplied pixels from one space's code values to another's exactly (linear light through the primaries, no
// display rendering, no table): between scene-referred spaces.
void convertExactly(Image& img, const ColorSpace& from, const ColorSpace& to);

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
// That conversion as the 65^3 LUT it runs through (on colour clamped to 0..1), kept per pair; null for the same space.
std::shared_ptr<const Lut3D> conversionLut(const ColorSpace& from, const ColorSpace& to, double hdrPeakNits = 1000);

// Light in cd/m² (nits) for display spaces: PQ as coded, HLG on a 1000-nit display (its OOTF), SDR with white at
// 100 nits. A neutral code value's light and back (scope graticules); `nits` below 0 for camera log and linear.
double codeToNits(const ColorSpace& space, double code);
// BT.2390's EETF on PQ code values normalised to the source's peak, `maxLum` the target's peak so normalised.
double bt2390Eetf(double e, double maxLum);
double nitsToCode(const ColorSpace& space, double nits);
// A pixel's brightest channel in nits (CTA-861.3's maxRGB, which MaxCLL and MaxFALL are made of).
double pixelMaxNits(const ColorSpace& space, const float rgb[3]);

// For tests and scopes: one pixel through the same conversion.
void convertPixel(float rgb[3], const ColorSpace& from, const ColorSpace& to, double hdrPeakNits = 1000);

// The steps either side of a display-referred conversion: a pixel's code values to the light it shows (relative to SDR
// white, which is 203 nits for PQ and HLG; HLG through its OOTF on a 1000-nit display), and such light back to code
// values (clamped to 0..1).
void displayLight(const ColorSpace& space, const float rgb[3], double light[3]);
void encodeDisplayLight(const ColorSpace& space, const double light[3], float rgb[3]);
// Linear RGB with these CIE 1931 xy chromaticities (red, green, blue, white) to XYZ relative to D65 (Bradford adapted
// from another white).
void chromaticitiesToXyz(const double xy[8], double m[9]);
// CIE L*a*b* (D65) of linear light in primaries `p`, white (1, 1, 1) at L* 100; and the CIEDE2000 colour difference.
void linearToLab(Primaries p, const double rgb[3], double lab[3]);
double deltaE2000(const double lab1[3], const double lab2[3]);
// 3x3 matrix helpers.
void invertMatrix3(const double m[9], double out[9]);
void multiplyMatrix3(const double a[9], const double b[9], double out[9]);

}  // namespace montage
