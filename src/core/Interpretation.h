// Montage — Interpret Footage (Premiere's Interpret Footage, Resolve's Clip Attributes, Avid's Source Settings, Final
// Cut's Conform Speed and anamorphic override): how a video file is read, set once on the media item for every clip
// made from it.
//  - Frame rate: each of the file's frames is shown for 1/fps of a second, so high-frame-rate footage plays as smooth
//    slow motion and 25 fps material conforms to 23.976; its sound conforms with it (varispeed, or pitch kept).
//  - Pixel aspect: the file's own replaced, for anamorphic footage that is not flagged (or flagged wrongly).
//  - Alpha: read as straight (the file's way) or premultiplied, ignored, or inverted.
//  - Fields: the file's flags overridden, to deinterlace footage flagged progressive or leave alone footage that is not
//    interlaced at all.
//  - Pulldown removed: 2:3 pulldown (film or 23.976 footage carried as 29.97 or 30 frame video) undone, its fields woven
//    back into the original frames (media/Pulldown.h), so it plays at 23.976 or 24 with no combing and no repeats.
//    The cadence is given stretch by stretch (a video edit made after the pulldown breaks it and starts another).
//  - Camera RAW (stills and CinemaDNG runs): exposure in stops, white balance as the light's temperature and tint
//    (or as shot), highlight recovery and a half-size decode for speed, as Resolve's Camera RAW panel and Lightroom
//    develop them.
// Like an image sequence's rate, the interpretation travels with the media path, after a separator no file name holds,
// so everything that opens media by path (decoders, the frame and audio caches, thumbnails, waveforms, proxies and
// analysis) reads it the same way: "clip.mov\x1d" "fps=24,1;file=120,1;par=2;alpha=ignore;fields=upper;pitch=1".
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Model.h"

namespace montage {

struct Interpretation {
    Rational fps{0, 1};      // the rate its frames play at; 0 = the file's
    Rational fileFps{0, 1};  // the file's own rate (kept with `fps`)
    double par = 0;          // pixel aspect ratio; 0 = the file's
    std::string alpha;       // "" as the file says (straight), "premultiplied", "ignore", "invert"
    std::string fields;      // "" as the frames are flagged, "progressive", "upper" (top field first), "lower"
    // Pulldown removed (media/Pulldown.h): each stretch of the cadence as "frame:position:order" (the file frame it starts
    // at, that frame's place in the cadence, 0 A A, 1 B B, 2 B C, 3 C D, 4 D D or v for video with none, and t for the top
    // field first or b),
    // joined by commas, as "0:0:t,1873:2:t"; "" none.
    std::string pulldown;
    bool keepPitch = false;  // conformed sound keeps its pitch (time-stretched) rather than playing at the new speed
    double rawExposure = 0;     // stops
    double rawTemperature = 0;  // the light's colour temperature in kelvin; 0 = the camera's white balance (as shot)
    double rawTint = 0;         // + towards magenta, - towards green
    std::string rawHighlights;  // "" clipped, "blend", "rebuild"
    bool rawHalf = false;       // decoded at half size (faster)
    // Stereoscopic 3D: how the two eyes are packed in each frame ("" as the file says, "none" not stereo, "sbs" side
    // by side at full size, "sbs_half" squeezed side by side, "tb" top and bottom, "tb_half" squeezed top and bottom),
    // whether the eyes are the other way round, and which eye is read (0 left, 1 right; set by the renderer).
    std::string stereo;
    bool swapEyes = false;
    int eye = 0;
    // One layer (AOV) of a multi-layer OpenEXR, read with OpenEXR (media/Exr.h); "" the main picture. Set per clip by
    // the renderer, as the eye is.
    std::string layer;
    // A JPEG with a gain map (media/GainMap.h) read as its HDR rendition (Rec.2100 PQ) rather than its SDR picture: set
    // when it is imported, and dropped while its colour is interpreted as an SDR space or it is in an SDR sequence.
    bool gainMap = false;
    bool hasRaw() const { return rawExposure != 0 || rawTemperature > 0 || rawTint != 0 || !rawHighlights.empty() || rawHalf; }
    bool empty() const {
        return !conformed() && par <= 0 && (alpha.empty() || alpha == "straight") && fields.empty() && pulldown.empty() && !hasRaw() && stereo.empty() && !swapEyes &&
               eye == 0 && layer.empty() && !gainMap;
    }
    bool conformed() const { return fps.valid() && fileFps.valid() && !(fps == fileFps); }
    // Seconds of the file per second of media time (the new rate over the file's); 1 when not conformed.
    double timeScale() const { return conformed() ? fps.toDouble() / fileFps.toDouble() : 1.0; }
    bool operator==(const Interpretation&) const = default;
};

// The interpretation a media path carries. False (and `out` left empty) for a path without one.
bool parseInterpretation(const std::string& path, Interpretation& out);
// `path` without its interpretation: the file (or image sequence, or Photoshop layer) as it is.
std::string uninterpretedPath(const std::string& path);
// `path` read with `i` (any interpretation it carried replaced; none when `i` is empty).
std::string interpretedPath(const std::string& path, const Interpretation& i);
// The interpretation a media item is read with.
Interpretation interpretationOf(const MediaItem& m);

// Names for the choices, as the dialog, the project file and MCP use them.
bool validAlphaMode(const std::string& alpha);
bool validFieldOrder(const std::string& fields);
// One stretch of a 2:3 cadence (Interpretation::pulldown).
struct PulldownStretch {
    int64_t start = 0;     // the file frame it starts at
    int position = 0;      // that frame's place in the cadence: 0 A A, 1 B B, 2 B C, 3 C D, 4 D D; -1 video with no pulldown
                           // ("v": one frame in five left out, so it keeps time)
    bool topFirst = true;  // the top field first (the cadence's fields counted from it)
    bool operator==(const PulldownStretch&) const = default;
};
// The stretches a pulldown value lists, in order; false for a value that is not one (empty for "").
bool parsePulldown(const std::string& value, std::vector<PulldownStretch>& out);
std::string pulldownValue(const std::vector<PulldownStretch>& stretches);
bool validRawHighlights(const std::string& mode);
bool validStereoLayout(const std::string& layout);  // "", "none", "sbs", "sbs_half", "tb", "tb_half", "mv"
// The eyes' packing a media file is read with: the file's own (`detected`, from its stereo metadata) unless the
// interpretation says otherwise; "" for a flat picture.
std::string stereoLayout(const std::string& detected, const Interpretation& i);
// `path` with the right (1) or left (0) eye of a stereoscopic file read from it.
std::string eyePath(const std::string& path, int eye);
// `path` with one layer of a multi-layer OpenEXR read from it ("" the main picture).
std::string layerPath(const std::string& path, const std::string& layer);
// `path` read as a gain-map JPEG's HDR rendition, or its SDR picture.
std::string gainMapPath(const std::string& path, bool hdr);

}  // namespace montage
