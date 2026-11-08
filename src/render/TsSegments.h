// Montage — MPEG-TS segments for HLS: one stream's packets cut into transport stream files, each starting with the
// programme tables (PAT, PMT) and, for pictures, a keyframe, written through one FFmpeg muxer for the whole stream so
// the continuity counters run on from segment to segment as a player joining them expects. Times are moved on by
// kTsOffset (the presentation's 0 at 10 s), so no decode time before the first presentation time falls below 0, and
// WebVTT segments map their cues there (X-TIMESTAMP-MAP=MPEGTS:900000).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AVCodecParameters;
struct AVFormatContext;
struct AVPacket;
struct AVRational;

namespace montage {

constexpr int64_t kTsOffset = 900000;  // 90 kHz

// Whether MPEG-TS segments carry this codec (H.264, AAC, AC-3, E-AC-3; FFmpeg's names). HEVC goes in CMAF segments, as
// HLS players take it only there.
bool tsSegmentsCarry(const std::string& codecName);

class TsSegmenter {
public:
    TsSegmenter() = default;
    TsSegmenter(const TsSegmenter&) = delete;
    TsSegmenter& operator=(const TsSegmenter&) = delete;
    ~TsSegmenter();
    bool open(const AVCodecParameters* par, std::string* error);
    // The next segment starts (its tables first).
    bool begin(std::string* error);
    bool started() const { return started_; }
    // A packet, its times in `num/den` (moved on by kTsOffset).
    bool write(AVPacket* pkt, int num, int den, std::string* error);
    // The segment's bytes (what was buffered flushed into it).
    bool end(std::vector<uint8_t>& bytes, std::string* error);
    // The segment just ended: its first sample's end (the I-frame range: tables and the keyframe), its presentation
    // span (90 kHz, with kTsOffset), and whether it began on a keyframe.
    int64_t iframeBytes = 0, start = 0, finish = 0;
    bool keyFirst = false;

private:
    AVFormatContext* oc_ = nullptr;
    bool started_ = false, first_ = false;
};

}  // namespace montage
