// Montage — VFX pulls (Resolve's Individual Clips render, Avid's DPX/EXR
// pulls): each chosen shot's source frames, untouched, at the footage's own
// size and rate, with handles either side, as an image sequence a compositor
// can work on (OpenEXR half float in scene-linear light, 10-bit DPX, or
// 16-bit TIFF), numbered so the first frame of the cut is 1001, plus a pull
// list (CSV) saying what came from where.
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
    std::string colorSpace;      // EXR: "" = Linear Rec.709; others: "" = the footage's own
};

struct VfxShot {
    std::string name;            // the folder's and files' name
    std::string media;           // source file
    std::string folder;
    int firstFrame = 0, lastFrame = 0;  // file numbers, handles included
    int cutIn = 0, cutOut = 0;          // file numbers of the cut's first and last frames
    double sourceIn = 0, sourceOut = 0;  // seconds of the source pulled (handles included), out exclusive
    int headHandle = 0, tailHandle = 0;  // the handles there was footage for
};

// Pulls the clips (video clips of footage; titles and other generated clips
// are skipped), writes "pull_list.csv" in the folder, and returns the shots.
bool exportVfxPulls(const Project& p, const Sequence& seq, const std::vector<Id>& clips, const VfxPullOptions& options,
                    std::vector<VfxShot>* shots, const ExportProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                    std::string* error = nullptr);

}  // namespace montage
