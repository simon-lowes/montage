// Montage — streaming packages: the sequence rendered once, then encoded as a ladder of renditions (sizes, bit rates,
// H.264, HEVC or AV1) whose segments all start on keyframes at the same frames, packaged as CMAF fragmented MP4 for
// HLS (a multivariant playlist, media and I-frame playlists, CODECS, VIDEO-RANGE) and DASH (an MPD with a segment
// timeline). The sound comes as one rendition per language (the mix and the export's other streams), captions as
// WebVTT for both and IMSC 1.1 for DASH, one per caption track, chapters as a WebVTT chapters file, and ad-break
// markers as SCTE 35 cues (EXT-X-DATERANGE SCTE35-OUT and SCTE35-IN in HLS, an EventStream in DASH) with segments
// split at them. HLS can have MPEG-TS segments instead (render/TsSegments.h); the sound can be AC-3 or E-AC-3 with the
// metadata an export gives it (render/Ac3.h). Live packages, from a recording as it is made: render/LivePackage.h.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Model.h"
#include "render/Exporter.h"

namespace montage {

struct StreamingRung {
    int width = 0, height = 0;   // height 0: in proportion to the width
    int64_t bitrate = 0;         // video bits a second (peaks up to 1.5 times this)
    std::string codec = "libx264";  // libx264, libx265 or libsvtav1
    bool operator==(const StreamingRung&) const = default;
};

struct StreamingSettings {
    std::string folder;                 // the package's folder (made when missing)
    std::vector<StreamingRung> rungs;   // none: defaultLadder for the sequence's size
    double segmentSeconds = 4;          // 2 to 6
    bool cutAligned = false;            // segments end at the cut nearest each nominal end (within half a segment)
    bool hls = true, dash = true;
    bool iframePlaylists = true;        // HLS I-frame playlists (trick play)
    std::string segmentFormat = "cmaf"; // or "ts": HLS in MPEG-TS segments (H.264 or HEVC; AAC, AC-3 or E-AC-3), DASH in CMAF
    std::string preset = "fast";        // the encoders' speed
    std::string audioCodec = "aac";     // aac, libopus, ac3 (to 5.1) or eac3 (to 7.1) ("none": no sound)
    int64_t audioBitrate = 128000;
    std::string audioName = "Main", audioLanguage;  // the mix's ("": the first caption track's, or en)
    std::vector<ExportSettings::AudioStream> extraAudio;  // more languages (or descriptions), as an export's
    bool captions = true;               // each caption track with captions
    bool chapters = true;               // chapters.vtt from chapter markers
    bool adBreaks = true;               // SCTE 35 cues from ad-break markers
    std::string cueCommand = "splice_insert";  // or "time_signal" (segmentation descriptors)
    std::string colorSpace, toneMapping;  // as an export's ("": the sequence's)
    std::string startDate;              // EXT-X-PROGRAM-DATE-TIME of the first frame (ISO 8601 UTC; "": now)
    FrameTime in = -1, out = -1;
    bool useProxies = false;
    bool keepWork = false;              // keep the render and the sound it encoded from (in folder/work)
};

struct StreamingAdBreak {
    FrameTime at = 0, duration = 0;  // from the range's start
    uint32_t id = 0;                 // splice_event_id / segmentation_event_id: 1, 2, ... in time order
    std::string name;
};

struct StreamingResult {
    std::string hls, dash;              // the multivariant playlist and the MPD written ("" when not asked for)
    std::vector<FrameTime> segments;    // each segment's first frame, from the range's start
    std::vector<StreamingAdBreak> breaks;
    std::vector<std::string> renditions;  // folder names, video then audio then captions
    std::vector<std::string> warnings;
};

// A ladder for a picture of this size: rungs from it down to 360 lines (16:9 sizes scaled to its shape), bit rates by
// pixel count (HEVC and AV1 at about two thirds of H.264's, HDR a quarter more).
std::vector<StreamingRung> defaultLadder(int width, int height, const std::string& codec = "libx264", bool hdr = false);

// The ad-break markers inside [in, out), in time order, numbered.
std::vector<StreamingAdBreak> streamingAdBreaks(const Sequence& seq, FrameTime in, FrameTime out);

// Each segment's first frame (from `in`): every `seconds` (or at the cut nearest each such point when cutAligned),
// and at each frame in `splits` (from `in`); no segment shorter than a second unless a split asks for it.
std::vector<FrameTime> streamingSegments(const Sequence& seq, FrameTime in, FrameTime out, double seconds, bool cutAligned,
                                         const std::vector<FrameTime>& splits);

// RFC 6381 codec names from an MP4 sample entry's configuration ("avc1.640028", "hvc1.2.4.L120.B0", "av01.0.08M.10",
// "mp4a.40.2", "opus", "ac-3", "ec-3"); "" when the codec is not one of these.
std::string streamingCodecName(const std::string& codec, const std::vector<uint8_t>& extradata, int profile = -1);

bool exportStreamingPackage(const Project& p, const Sequence& seq, const StreamingSettings& s, const ExportProgress& progress,
                            const std::atomic<bool>* cancel, std::string* error, StreamingResult* result = nullptr);

}  // namespace montage
