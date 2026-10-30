// Montage — one export split into segments (Compressor's job segmenting): the range cut, at cuts near equal lengths,
// into parts that render nodes encode at once (render/RenderFarm), each checked, then joined into one file by copying
// their coded video one after another (no second encode), with the sound, captions, chapters and timecode made once for
// the whole range on the node that joins, so no segment edge touches them.
//  - Intra-frame codecs (ProRes, DNxHR, FFV1, CineForm, uncompressed, Motion JPEG) join frame for frame as one render
//    would have made them. Long-GOP ones (H.264, HEVC, AV1) start each segment on a keyframe of a closed GOP, and their
//    rate control works per segment (a bitrate is kept within each, not across the whole).
//  - An image sequence's segments write their own frames, numbered on from the whole one's first: the join checks them.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"
#include "render/Exporter.h"

namespace montage {

struct ExportSegment {
    FrameTime in = 0, out = 0;  // sequence frames [in, out)
};

// Whether an export can be split; false with `why` when it cannot (a codec, file type or feature made across the whole
// range in one go: spatial video, HDR10+ and adaptive HDR analysis, CEA-608, a frame rate conversion, broadcast MXF,
// Content Credentials).
bool exportSplits(const ExportSettings& s, std::string* why = nullptr);
// Coded across frames (H.264, HEVC, AV1): rate control works per segment.
bool exportSegmentsLongGop(const ExportSettings& s);
// Numbered frames (PNG, TIFF, OpenEXR, DPX).
bool exportIsImageSequence(const ExportSettings& s);

// The export's range in `n` segments (fewer when it is shorter), each boundary on the cut nearest an equal share when
// one is within an eighth of a segment's length, else on that share's frame.
std::vector<ExportSegment> planExportSegments(const Sequence& seq, const ExportSettings& s, int n);
// The settings a segment renders with: only its frames, without what the join makes for the whole (sound, embedded
// captions, chapters, timecode, reports); a video file at `path`, an image sequence's frames where the whole one's go,
// numbered on from its first.
ExportSettings segmentExportSettings(const ExportSettings& s, const ExportSegment& segment, const std::string& path);
// A segment's file name: "segment-003.mov" (1 based, the output's extension).
std::string segmentFileName(const ExportSettings& s, int index);

// A segment as written: its frames (a video file's video packets, an image sequence's files) and a SHA-256 of its bytes.
struct SegmentCheck {
    int64_t frames = 0, bytes = 0;
    std::string sha256;
};
// Reads a segment written with `segment` (segmentExportSettings's) and counts and hashes it. False with `error` when it
// cannot be read.
bool checkExportSegment(const ExportSettings& segment, SegmentCheck& out, std::string* error = nullptr, const std::atomic<bool>* cancel = nullptr,
                        const std::function<void(double)>& progress = {});

// The join: the export `s` of [parts.front().in, parts.back().out) made from its segments' files (`files`, a video
// file each, in order; none for an image sequence, whose frames are where they go already): the sound, captions and
// chapters rendered once, the segments' video copied after it in order, the timecode and HDR light levels (the highest
// of the segments') stated; then `stems` (StemGroups) as renderExport makes them. False with `error` when a segment
// does not fit the others (another encoder's, or not the frames it should hold).
bool joinExportSegments(const Project& p, const Sequence& seq, const ExportSettings& s, int stems, const std::vector<std::string>& files,
                        const std::vector<ExportSegment>& parts, const ExportProgress& progress, const std::atomic<bool>* cancel,
                        std::string* error = nullptr);

}  // namespace montage
