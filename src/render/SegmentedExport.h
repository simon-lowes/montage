// Montage — one export split into segments (Compressor's job segmenting): the range cut, at cuts near equal lengths,
// into parts that render nodes encode at once (render/RenderFarm), each checked, then joined into one file by copying
// their coded video one after another (no second encode), with the sound, captions, chapters and timecode made once for
// the whole range on the node that joins, so no segment edge touches them.
//  - Intra-frame codecs (ProRes, DNxHR, FFV1, CineForm, uncompressed, Motion JPEG) join frame for frame as one render
//    would have made them. Long-GOP ones (H.264, HEVC, AV1, MPEG-2, and hardware H.264 and HEVC, whose segments all go
//    to nodes with the same encoder) start each segment on a keyframe of a closed GOP, and their rate control works per
//    segment, unless the export takes two passes (x264, x265): each segment's first pass measures its pictures and the
//    bits are shared out across the segments by what they measured (twoPassBitrates).
//  - Joined into MOV, MP4, MKV, MXF (broadcast MXF too: joined, then rewrapped as the delivery asks) or MPEG-TS (one
//    timeline, its continuity counters and clock reference running on); MPEG-2's GOP timecode follows the whole range.
//  - An image sequence's segments write their own frames, numbered on from the whole one's first: the join checks them.
//  - IMF and DCP packages (render/Imf.h, render/Dcp.h) are split by their pictures: each segment codes its share as
//    JPEG 2000 codestreams (codestreamFile), and the join wraps them with the sound and the rest of the package.
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
// range in one go: spatial video, HDR10+ and adaptive HDR analysis, CEA-608, a frame rate conversion, captions inside
// MXF or MPEG-TS, Content Credentials).
bool exportSplits(const ExportSettings& s, std::string* why = nullptr);
// Coded across frames (H.264, HEVC, AV1, MPEG-2, hardware H.264 and HEVC): rate control works per segment.
bool exportSegmentsLongGop(const ExportSettings& s);
// Numbered frames (PNG, TIFF, OpenEXR, DPX).
bool exportIsImageSequence(const ExportSettings& s);

// The export's range in `n` segments (fewer when it is shorter), each boundary on the cut nearest an equal share when
// one is within an eighth of a segment's length, else on that share's frame; MPEG-2's then on the nearest of its GOPs
// (exportMpeg2Gop), so every GOP but the last is as long as a single render's.
std::vector<ExportSegment> planExportSegments(const Sequence& seq, const ExportSettings& s, int n);
// The settings a segment renders with: only its frames, without what the join makes for the whole (sound, embedded
// captions, chapters, timecode, reports); a video file at `path`, an image sequence's frames where the whole one's go,
// numbered on from its first. MPEG-2's GOP timecode counts on from where the segment falls (at the sequence's `fps`); a
// broadcast MXF delivery's segments are plain MXF.
ExportSettings segmentExportSettings(const ExportSettings& s, const ExportSegment& segment, const std::string& path, Rational fps = {0, 1});
// Two passes across segments: each segment's bitrate, from what its first pass took at constant quality (`measured`,
// any unit, a segment each), so the frames that need more get more while the whole spends `bitrate` on average (a
// segment kept within four times the average either way).
std::vector<int64_t> twoPassBitrates(int64_t bitrate, const std::vector<ExportSegment>& parts, const std::vector<int64_t>& measured);
// A segment's file name: "segment-003.mov" (1 based, the output's extension).
std::string segmentFileName(const ExportSettings& s, int index);

// A package segment's picture for composition frame `frame` (the right eye's for a stereo DCP): one JPEG 2000 codestream
// a file, in the folder a farm's segments share, which the join wraps into the package as it is.
std::string codestreamFile(const std::string& folder, int64_t frame, bool right = false);

struct SegmentCheck;
// A package segment's codestreams for frames [from, to) (both eyes' with `stereo`), counted until the first missing and
// hashed in order. False with `error` only when one cannot be read.
bool checkCodestreams(const std::string& folder, int64_t from, int64_t to, bool stereo, SegmentCheck& out, std::string* error = nullptr,
                      const std::atomic<bool>* cancel = nullptr, const std::function<void(double)>& progress = {});

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
                        std::string* error = nullptr, HrdReport* hrd = nullptr);
// (A stream that states a buffer model, H.264's and HEVC's HRD or MPEG-2's constant-rate VBV, is kept one model across
// the joins: render/HrdCheck.h's HrdSplicer, the segments after the first started lower (segmentExportSettings). The
// joined file is then checked, `hrd` given the report; one that does not conform is warned about and kept.)

}  // namespace montage
