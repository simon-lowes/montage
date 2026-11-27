// Montage — Regions Coded Finer: parts of the picture an export spends more of its bits on (faces found in it, a
// clip's masks as they are tracked, or a box), each frame's given to the encoder as FFmpeg's regions of interest
// (AV_FRAME_DATA_REGIONS_OF_INTEREST side data: rectangles in the coded picture, each with a quantiser offset). In
// FFmpeg 6.1 three software encoders read them: x264 and x265 (an offset a 16 by 16 block, x265's 8 by 8 when its
// quantisation groups are; adaptive quantisation must be on, as it is by default) and libvpx (VP9's and VP8's
// segments, at most seven offsets besides none; VP9's only in its real-time mode at speed 5 or more with its own
// adaptive quantisation off, which an export with regions then uses). Where regions overlap, the stronger wins.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

struct Image;

struct CodingRegion {
    // "box": a rectangle of the frame; "mask": a clip's effect masks (ellipses, rectangles and drawn paths, with their
    // keys, as a tracker moves them), each mask's bounds through the clip's transform; "faces": each face found in the
    // picture (YuNet), a little larger than the face.
    std::string kind = "box";
    // How much finer than the rest, 0 to 1: a quantiser offset of a fifth of the encoder's range at 1 (x264's and
    // x265's 10 steps of QP, VP9's 13 of its quantiser index).
    double strength = 0.5;
    double x = 0, y = 0, w = 1, h = 1;  // a box's top left and size, as fractions of the frame
    Id clip = 0;    // a mask's clip (0: every clip with masks)
    Id effect = 0;  // one of its effects' mask (0: every masked effect of the clip)
    bool operator==(const CodingRegion&) const = default;
};

// A region in one coded picture: pixels from (left, top) up to, not including, (right, bottom), and its offset as
// FFmpeg takes it (-1 to 1; negative is finer).
struct CodedRegion {
    int left = 0, top = 0, right = 0, bottom = 0;
    double qoffset = 0;
    bool operator==(const CodedRegion&) const = default;
};

// The video encoders (ExportSettings::videoCodec) that read regions: libx264, libx265 and libvpx-vp9.
bool encoderTakesRegions(const std::string& videoCodec);
// Each region well formed: its kind known, its strength 0 to 1, a box within the frame and not empty.
bool checkCodingRegions(const std::vector<CodingRegion>& regions, std::string* error = nullptr);
// The quantiser offset of a strength.
double codingRegionOffset(double strength);

// The regions at sequence frame `f` in a picture coded `width` by `height` (the sequence's frame scaled to it), the
// strongest first. `picture` (the frame as rendered, any size) is where faces are found; without it, or without the
// face model, faces give none.
std::vector<CodedRegion> codedRegionsAt(const Project& p, const Sequence& seq, const std::vector<CodingRegion>& regions, FrameTime f, int width,
                                        int height, const Image* picture = nullptr);

// As MCP and saved export settings write them: [{"kind": "box", "strength": 0.5, "x": 0, "y": 0, "w": 0.5, "h": 1},
// {"kind": "mask", "clip": 12}, {"kind": "faces"}].
std::string codingRegionsToJson(const std::vector<CodingRegion>& regions);
bool codingRegionsFromJson(const std::string& json, std::vector<CodingRegion>& out, std::string* error = nullptr);

// What the regions did to an export: the file as made, and the same export made again without them, each decoded
// and measured against the picture they were both coded from (a lossless render at the coded size): luma PSNR inside
// the regions (every frame's own rectangles) and outside them, and VMAF of the whole picture (VMAF has no measure of
// a part of a frame that moves). VMAF is left at -1 when libvmaf is not built in.
struct RegionReport {
    int64_t bytes = 0, bytesWithout = 0;
    double psnrInside = 0, psnrOutside = 0, psnrInsideWithout = 0, psnrOutsideWithout = 0;
    double vmaf = -1, vmafWithout = -1;
    int64_t framesWithRegions = 0;
    double insideShare = 0;  // of the pixels over all frames
    std::string summary() const;
};
struct ExportSettings;
// Measures an export already made at s.path with its regions (`given`: each frame's rectangles, from the export's
// first, as it gave them to the encoder), making the export without them and a lossless reference beside it, both
// removed after.
bool regionCodingReport(const Project& p, const Sequence& seq, const ExportSettings& s, const std::vector<std::vector<CodedRegion>>& given,
                        RegionReport& out, const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);
// Luma PSNR of `coded` against `reference` (two files of the same size and frames), inside and outside each frame's
// rectangles (frame k's from given[k]; a frame with none is all outside).
bool regionPsnr(const std::string& reference, const std::string& coded, const std::vector<std::vector<CodedRegion>>& given, double& inside,
                double& outside, double* insideShare = nullptr, std::string* error = nullptr);

}  // namespace montage
