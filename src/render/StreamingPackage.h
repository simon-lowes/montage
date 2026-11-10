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
    std::string segmentFormat = "cmaf"; // or "ts": HLS in MPEG-TS segments (H.264; AAC, AC-3 or E-AC-3), DASH in CMAF
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
    // Rungs set per shot: each shot (between the sequence's cuts and shot changes, a second at the least) starts a
    // segment, is encoded first at several quality settings (CRF) for each rung, and scored by VMAF at the rung's
    // size; the rung then gives each shot the setting from those rate–quality points that reaches vmafTarget for the
    // fewest bits, or, with no target, that spends the rung's bit rate over the range for the most VMAF. A rung's bit
    // rate is then its cap: no shot's mean goes past it (and its peaks stay within 1.5 times it). Needs VMAF (and, for
    // HDR, an HDR VMAF model). The package gets ladder.csv, each shot's rate and score on each rung.
    bool perShot = false;
    double vmafTarget = 0;              // 0: fill each rung's bit rate instead
};

// One quality setting tried on a shot: the encoder's CRF, the bits it took and the VMAF it scored (its frames' mean).
struct ShotTrialPoint {
    double crf = 0;
    int64_t bits = 0;
    double vmaf = 0;
};
// Which of each shot's trial points a rung uses. With `target` > 0: the fewest bits reaching it within the shot's cap
// (`capBits`), else the highest VMAF within the cap, else the fewest bits. With no target: the points on each shot's
// convex hull (bits against frames x VMAF) that give the most VMAF over all for `budgetBits`, each within its cap, all
// starting from the cheapest. `frames` weighs each shot. Returns an index into each shot's points.
std::vector<size_t> chooseShotPoints(const std::vector<std::vector<ShotTrialPoint>>& shots, const std::vector<FrameTime>& frames,
                                     const std::vector<int64_t>& capBits, double target, int64_t budgetBits);

struct StreamingAdBreak {
    FrameTime at = 0, duration = 0;  // from the range's start
    uint32_t id = 0;                 // splice_event_id / segmentation_event_id: 1, 2, ... in time order
    std::string name;
};

// A shot of a per-shot rung: its frames (from the range's start), the CRF it was given, its bits a second and VMAF as
// delivered (decoded from the rendition, against the render at the rung's size), and the VMAF its trial expected.
struct StreamingShot {
    FrameTime first = 0, end = 0;
    double crf = 0;
    int64_t bitrate = 0;
    double vmaf = 0, trialVmaf = 0;
};
struct StreamingRungShots {
    std::string rendition;
    int64_t bitrate = 0;  // over the range
    double vmaf = 0;      // the mean of its frames
    std::vector<StreamingShot> shots;
};

struct StreamingResult {
    std::string hls, dash;              // the multivariant playlist and the MPD written ("" when not asked for)
    std::vector<FrameTime> segments;    // each segment's first frame, from the range's start
    std::vector<StreamingAdBreak> breaks;
    std::vector<std::string> renditions;  // folder names, video then audio then captions
    std::vector<StreamingRungShots> perShot;  // each video rung's shots, when set per shot
    std::string ladderReport;            // ladder.csv, when set per shot
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
// Whether a folder holds nothing but what a rendition of a package writes (its segments, init segment and playlists):
// one that may be replaced.
bool streamingRenditionFolder(const std::string& path);

bool exportStreamingPackage(const Project& p, const Sequence& seq, const StreamingSettings& s, const ExportProgress& progress,
                            const std::atomic<bool>* cancel, std::string* error, StreamingResult* result = nullptr);

}  // namespace montage
