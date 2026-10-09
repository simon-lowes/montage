// Montage — Interpret Footage (Premiere's Interpret Footage, Resolve's Clip Attributes, Avid's Source Settings, Final
// Cut's Conform Speed and anamorphic override): how a video file is read, set once on the media item for every clip
// made from it.
//  - Frame rate: each of the file's frames is shown for 1/fps of a second, so high-frame-rate footage plays as smooth
//    slow motion and 25 fps material conforms to 23.976; its sound conforms with it (varispeed, or pitch kept).
//  - Pixel aspect: the file's own replaced, for anamorphic footage that is not flagged (or flagged wrongly).
//  - Alpha: read as straight (the file's way) or premultiplied, ignored, or inverted.
//  - Fields: the file's flags overridden, to deinterlace footage flagged progressive or leave alone footage that is not
//    interlaced at all.
//  - Camera RAW (stills and CinemaDNG runs): exposure in stops, white balance as the light's temperature and tint
//    (or as shot), highlight recovery and a half-size decode for speed, as Resolve's Camera RAW panel and Lightroom
//    develop them.
// Like an image sequence's rate, the interpretation travels with the media path, after a separator no file name holds,
// so everything that opens media by path (decoders, the frame and audio caches, thumbnails, waveforms, proxies and
// analysis) reads it the same way: "clip.mov\x1d" "fps=24,1;file=120,1;par=2;alpha=ignore;fields=upper;pitch=1".
#pragma once

#include <string>

#include "Model.h"

namespace montage {

struct Interpretation {
    Rational fps{0, 1};      // the rate its frames play at; 0 = the file's
    Rational fileFps{0, 1};  // the file's own rate (kept with `fps`)
    double par = 0;          // pixel aspect ratio; 0 = the file's
    std::string alpha;       // "" as the file says (straight), "premultiplied", "ignore", "invert"
    std::string fields;      // "" as the frames are flagged, "progressive", "upper" (top field first), "lower"
    bool keepPitch = false;  // conformed sound keeps its pitch (time-stretched) rather than playing at the new speed
    double rawExposure = 0;     // stops
    double rawTemperature = 0;  // the light's colour temperature in kelvin; 0 = the camera's white balance (as shot)
    double rawTint = 0;         // + towards magenta, - towards green
    std::string rawHighlights;  // "" clipped, "blend", "rebuild"
    bool rawHalf = false;       // decoded at half size (faster)
    bool hasRaw() const { return rawExposure != 0 || rawTemperature > 0 || rawTint != 0 || !rawHighlights.empty() || rawHalf; }
    bool empty() const { return !conformed() && par <= 0 && (alpha.empty() || alpha == "straight") && fields.empty() && !hasRaw(); }
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
bool validRawHighlights(const std::string& mode);

}  // namespace montage
