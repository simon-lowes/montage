// Montage — VFX pulls (Resolve's Individual Clips render, Avid's DPX/EXR
// pulls): each chosen shot's source frames, untouched, at the footage's own
// size and rate, with handles either side, as an image sequence a compositor
// can work on (OpenEXR half float in scene-linear light, 10-bit DPX, or
// 16-bit TIFF), numbered so the first frame of the cut is 1001, plus a pull
// list (CSV) saying what came from where. An OpenEXR pull can carry more:
// the source render's other layers and Cryptomattes, and the clip's masks as
// mattes, in any of OpenEXR's compressions (HTJ2K and zstd too). A deep render is pulled deep: its samples copied.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "Exporter.h"
#include "core/Model.h"

namespace montage {

struct VfxPullOptions {
    std::string folder;          // a folder per shot is made in it
    std::string format = "exr";  // exr, dpx or tiff
    int handles = 8;             // frames either side, as far as the footage goes
    int cutIn = 1001;            // the frame number of the first frame of the cut
    std::string colorSpace;      // EXR: "" = Linear Rec.709; others: "" = the footage's own (graded scene-referred: the working space's)
    bool amf = true;             // an ACES Metadata File beside each shot's frames (render/Amf.h): their space, the grade's looks
    // An ASC Framing Decision List beside each shot's frames (render/Fdl.h): the plate's canvas, with the footage's own
    // framing decisions (scaled to the plate) and one for what the cut shows of it.
    bool fdl = true;
    // OpenEXR, with the OpenEXR library (media/Exr.h):
    std::string compression;     // "" = ZIP (16 lines); or any of exrCompressions(), HTJ2K and zstd among them
    bool sourceLayers = false;   // a multi-layer OpenEXR source's other layers (AOVs) and Cryptomattes, copied as they are
    bool mattes = false;         // the clip's masks, each a one-channel layer "matte_<effect>" (A, 1 inside)
    // A shot whose clip has a Lens Correction: ST maps for its lens (32-bit OpenEXR, media/LensCalibration.h) beside the
    // plate, <shot>_undistort.exr (at the overscan size) and <shot>_redistort.exr, so the vendor's comp undistorts the
    // plate as Lens Correction does with no fit (all of the picture, at the recording's scale) and redistorts its
    // renders to match it.
    bool stMaps = false;
    // The shots' names: "" the clip's; or a pattern of {sequence}, {shot} (010, 020... in the cut's order), {clip} and
    // {layer} (the clip's track), as "{sequence}_{shot}_{layer}".
    std::string namePattern;
    // A compositing script (Nuke's .nk, render/VfxShots.h) beside each shot's frames: the plate read, written to
    // `returns` as the shot's first version, named by `returnsPattern` ("" = "{shot}_comp_v###").
    bool scripts = false;
    std::string returns, returnsPattern;
};

struct VfxShot {
    std::string name;            // the folder's and files' name
    std::string media;           // source file
    std::string folder;
    int firstFrame = 0, lastFrame = 0;  // file numbers, handles included
    int cutIn = 0, cutOut = 0;          // file numbers of the cut's first and last frames
    double sourceIn = 0, sourceOut = 0;  // seconds of the source pulled (handles included), out exclusive
    int headHandle = 0, tailHandle = 0;  // the handles there was footage for
    std::string amf;                    // its ACES Metadata File, when written
    std::string fdl;                    // its Framing Decision List, when written
    std::vector<std::string> layers;    // OpenEXR: the layers and Cryptomattes written beside the plate (its own is the main picture)
    std::string undistortMap, redistortMap;  // its ST maps, when written
    int overscanWidth = 0, overscanHeight = 0;  // the undistorted plate's size (its undistort map's)
    bool deep = false;                  // a deep render's samples, copied as they are (deep OpenEXR)
    Id clip = 0;                        // the clip pulled
    std::string plate;                  // its frames ("<folder>/<name>.####.exr")
    std::string colorSpace;             // theirs (ColorSpace.h id)
    int width = 0, height = 0;
    Rational fps{24, 1};
    double cutSource = 0;               // the cut's first frame in the footage (sequence frames, as Clip::sourceIn)
    std::string script;                 // its compositing script, when written
};

// The name a pattern gives a clip of the cut as a shot (VfxPullOptions::namePattern), made safe for files; {shot}
// numbers it by its place among the cut's footage clips.
std::string vfxShotName(const std::string& pattern, const Project& p, const Sequence& seq, const Clip& c);

// Pulls the clips (video clips of footage; titles and other generated clips
// are skipped), writes "pull_list.csv" in the folder, and returns the shots.
bool exportVfxPulls(const Project& p, const Sequence& seq, const std::vector<Id>& clips, const VfxPullOptions& options,
                    std::vector<VfxShot>* shots, const ExportProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                    std::string* error = nullptr);

}  // namespace montage
