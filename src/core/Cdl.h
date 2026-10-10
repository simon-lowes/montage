// Montage — ASC CDL (the American Society of Cinematographers' Color Decision List, v1.2): slope, offset and power
// per channel and one saturation, the grade that travels with dailies from set to editorial to the colourist. Read
// and written as .cc (one correction), .ccc (a collection) and .cdl (a decision list), in EDL comments (*ASC_SOP,
// *ASC_SAT) and in ALE columns (core/Ale.h); a clip carries one as its "cdl" effect.
#pragma once

#include <string>
#include <vector>

#include "Model.h"

namespace montage {

struct Cdl {
    std::string id;  // the correction's id (often the clip, reel or file name)
    std::string description;
    double slope[3] = {1, 1, 1};
    double offset[3] = {0, 0, 0};
    double power[3] = {1, 1, 1};
    double saturation = 1;
    bool identity() const;
    bool sameGrade(const Cdl& o) const;  // the numbers alike (to six decimals), whatever the id
};

// One pixel, in the space the CDL was made in, as v1.2 specifies: slope, offset, clamped to 0-1, power; then
// saturation about Rec. 709 luma, clamped to 0-1.
void applyCdl(const Cdl& c, float& r, float& g, float& b);

// "(s s s)(o o o)(p p p)", as EDLs and ALEs carry it, and back (false if it is not that).
std::string cdlSopText(const Cdl& c);
bool parseCdlSop(const std::string& text, Cdl& out);
std::string cdlNumber(double v);  // six decimals, as colour tools write them

// Every ColorCorrection in a .cc, .ccc or .cdl file (with its id and description). `error` says why when there is
// none, or the file is not XML, and also names corrections that were left out (unreadable numbers, or references to
// corrections in another file) when others were read.
std::vector<Cdl> parseCdlXml(const std::string& xml, std::string* error = nullptr);
// The correction for something known by these names (a clip's name, its media's name, file and tape): one whose id is
// one of them exactly, else one whose id matches one without its media file extension; null if none.
const Cdl* matchCdl(const std::vector<Cdl>& cdls, const std::vector<std::string>& names);
// A name without a media file extension (".mov", ".mxf", ".braw"...; "Sc12.1" keeps its ".1").
std::string withoutMediaExtension(const std::string& name);
enum class CdlFormat { Cc, Ccc, Cdl };
CdlFormat cdlFormatFor(const std::string& path);  // by extension (.cc, .ccc, else .cdl)
std::string writeCdlXml(const std::vector<Cdl>& cdls, CdlFormat format);

// A clip's CDL: its "cdl" effect (the first one switched on) at frame t; false if it has none.
bool clipCdl(const Clip& c, FrameTime t, Cdl& out);
// Gives a clip this CDL: its CDL effect is set (static values), or one is added first in its chain (before any
// transform out of the space the CDL was made in).
void setClipCdl(Project& p, Clip& c, const Cdl& cdl);
// A CDL kept on a media item (from an ALE, or set by hand), in its metadata as "asc_sop" and "asc_sat".
bool mediaCdl(const MediaItem& m, Cdl& out);
void setMediaCdl(MediaItem& m, const Cdl& cdl);

}  // namespace montage
