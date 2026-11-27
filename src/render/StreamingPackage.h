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
#include "render/Av1FilmGrain.h"
#include "render/Exporter.h"
#include "render/GrainMatch.h"

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
    // Rungs set per shot: each shot (between the sequence's cuts and shot changes, streamingShotChanges, or, where the
    // range has none, the scene changes found in the render; a second at the least) starts a segment, is encoded
    // first at several quality settings (CRF) for each rung, and scored by VMAF at the display size: the rung's
    // pictures scaled to it (bicubic) against the render at that size, so one target is one quality on every rung.
    // The rung then gives each shot the setting from those rate–quality points that reaches vmafTarget for the
    // fewest bits, or, with no target, that spends the rung's bit rate over the range for the most VMAF. A rung's bit
    // rate is then its cap: no shot's mean in the delivered files goes past it (a shot that would is encoded again,
    // coarser), and its peaks are held within 1.5 times it. Needs VMAF (and, for HDR, an HDR VMAF model). The package
    // gets ladder.csv: how it was scored, and each shot's rate and score on each rung.
    bool perShot = false;
    double vmafTarget = 0;              // 0: fill each rung's bit rate instead
    // The size VMAF is scored at: 0 by 0, the ladder's largest rung; a height of 0, in proportion to the width.
    int displayWidth = 0, displayHeight = 0;
    // The lowest CRF a shot may be given (its best quality), for H.264 and HEVC; AV1's, whose scale runs to 63, is two
    // more. 0: 18.
    double crfFloor = 0;
    // The trials score every such frame (what is delivered is scored on every frame). 0: 4.
    int trialVmafEvery = 0;
    // A shot's frames are kept in memory while it is tried, up to this many bytes: a longer shot is taken in parts,
    // each from a segment's start. 0: 1 GiB.
    int64_t shotMemory = 0;
    // Carry Grain as Parameters (render/Av1GrainExport.h), for a ladder of AV1 rungs: "measure" (each shot's grain
    // measured) or a grain profile's text (a saved profile's name made its text first, resolveFilmGrainSetting); "" or
    // "off": none. The render is made once with each shot's grain taken out, every rung is encoded from it, and each
    // rung's frame headers carry the shots' grain fitted again for the rung's size, with a new seed every frame. Set per
    // shot, the trials and what is delivered are scored against the cleaned render (the picture a decoder shows with its
    // grain left off), and a shot's cap allows for the parameters' bits.
    std::string filmGrain;
    // Regions coded finer (render/CodingRegions.h) in the H.264 and HEVC rungs, each at its own size: boxes and clips'
    // masks (faces are found in an export's own render, not here). AV1 rungs take none.
    std::vector<CodingRegion> regions;
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

// A shot of a per-shot rung: its frames (from the range's start), the CRF it was given, its bits (as the file's samples
// carry them) and bits a second, its VMAF as delivered (decoded from the rendition and scaled to the display size,
// against the render at that size, every frame scored), the same at the rung's own size (against the render scaled
// to the rung: what a viewer of a window that size sees), the VMAF its trial expected, what held it back ("floor": the
// lowest CRF allowed; "cap": the rung's rate; "": neither), and how many settings it was encoded at.
struct StreamingShot {
    FrameTime first = 0, end = 0;
    double crf = 0;
    int64_t bits = 0, bitrate = 0;
    double vmaf = 0, ownVmaf = 0, trialVmaf = 0;
    std::string stopped;
    int trials = 0;
    int64_t grainBits = 0;  // of `bits`, what grain parameters added to its frame headers
    int capEncodes = 0;     // encodes of it made again, coarser, because one came out past its cap
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
    // Set per shot: the size VMAF was scored at, the scaler to it ("bicubic"), the model, how many frames apart the
    // trials scored, the CRF floor (H.264 and HEVC's), how many times the render was decoded for the encodes (once to
    // a target, twice filling the rungs' rates) and how many encodes of a shot were made in all.
    int displayWidth = 0, displayHeight = 0;
    std::string scaler, vmafModel;
    int trialVmafEvery = 0;
    double crfFloor = 0;
    int renderDecodes = 0;
    int64_t shotEncodes = 0;
    // With grain carried as parameters: each shot's grain (as measured or given, and why none where there is none), and
    // for each rung the parameters each shot was given at its size, the frames given grain and the bytes they added.
    struct RungGrain {
        std::string rendition;
        std::vector<Av1FilmGrain> shots;  // (each shot's, its seed its first frame's)
        int64_t framesWithGrain = 0, bytesAdded = 0;
    };
    std::vector<GrainProfile> grainProfiles;
    std::vector<std::string> grainNotes;
    std::vector<RungGrain> grain;
    std::vector<std::string> warnings;
};

// A ladder for a picture of this size: rungs from it down to 360 lines (16:9 sizes scaled to its shape), bit rates by
// pixel count (HEVC and AV1 at about two thirds of H.264's, HDR a quarter more).
std::vector<StreamingRung> defaultLadder(int width, int height, const std::string& codec = "libx264", bool hdr = false);

// Where the picture changes shot, for rungs set per shot: the cuts and marked shot changes captions keep clear of
// (core/CaptionTools captionShotChanges), and edits from one generator to another (cards, bars, titles on nothing)
// where no clip lies under them and no transition crosses them. In time order.
std::vector<FrameTime> streamingShotChanges(const Sequence& seq);

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
