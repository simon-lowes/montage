// Montage — live streaming packages: a recording (media/Capture.h, any input: SRT, RIST, OMT, ST 2110, a camera) fed
// as it is made to a ladder of renditions, packaged for HLS (a sliding window of segments, EXT-X-MEDIA-SEQUENCE moving
// on and the oldest files removed) and DASH (a dynamic MPD whose time-shift buffer is the same window). With low
// latency, segments are made of parts: each a CMAF chunk (moof + mdat), written as an LL-HLS part file (EXT-X-PART,
// a PRELOAD-HINT for the next, blocking playlist reloads) and appended to the segment as it grows, which the origin
// sends to a DASH player as it comes (chunked transfer, availabilityTimeOffset). HLS can have MPEG-TS segments instead
// (render/TsSegments.h; not with low latency). The sound can be AAC, Opus, AC-3 or E-AC-3, the AC-3 family stating
// the programme's loudness as measured so far (dialnorm, updated each segment). LiveOrigin serves the package's folder
// over HTTP as players need it (what static file servers cannot: the blocking reloads and the growing segments).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "render/StreamingPackage.h"

struct AVFrame;

namespace montage {

struct LiveSettings {
    std::string folder;                 // the package's folder (made when missing; what an earlier live package left is replaced)
    std::vector<StreamingRung> rungs;   // none: the picture's size and, above 480 lines, half of it, in `codec`
    std::string codec = "libx264";      // libx264, libx265 or libsvtav1 (the default rungs')
    double segmentSeconds = 2;          // 1 to 6 (a whole number of parts, and of frames)
    int window = 6;                     // segments the playlists and the MPD's time-shift buffer hold, 3 to 100
    bool lowLatency = false;            // segments in parts of partSeconds, each sent as soon as it is made
    double partSeconds = 0.5;           // 0.1 to 1
    bool hls = true, dash = true;
    std::string segmentFormat = "cmaf";  // or "ts": HLS in MPEG-TS segments (DASH stays CMAF; not with low latency)
    std::string preset = "veryfast";    // the encoders' speed
    std::string audioCodec = "aac";     // aac, libopus, ac3, eac3 or none
    int64_t audioBitrate = 128000;      // a pair of channels'
    std::string audioName = "Main", audioLanguage = "und";
};

struct LiveStatus {
    bool running = false, finished = false;
    int64_t frames = 0, dropped = 0;    // pictures taken, and dropped while the encoders fell behind
    int64_t segments = 0;               // segments finished
    int64_t firstListed = 1;            // the playlists' first segment (EXT-X-MEDIA-SEQUENCE)
    double behind = 0;                  // seconds of pictures waiting for the encoders
    int dialnorm = 0;                   // AC-3 and E-AC-3: the dialogue level stated now (dB)
    std::string hls, dash;              // the multivariant playlist and the MPD
    std::vector<std::string> renditions;
    std::string error;                  // why it stopped
};

class LivePackager {
public:
    LivePackager();
    ~LivePackager();  // finishes
    LivePackager(const LivePackager&) = delete;
    LivePackager& operator=(const LivePackager&) = delete;

    bool start(const LiveSettings& settings, std::string* error = nullptr);
    // The recording's pictures and sound (CaptureSettings::onPicture and onSound): copied and queued, the encoding done
    // on a thread of the packager's own. The first picture is the presentation's 0, at the wall clock's now (DASH's
    // availabilityStartTime, HLS's program dates).
    void picture(const AVFrame* frame, int fpsNum, int fpsDen);
    void sound(const AVFrame* frame);
    // The feed ended: what was queued is encoded, the last segment finished, the playlists ended (EXT-X-ENDLIST) and
    // the MPD given its duration. Waits for that.
    void finish();
    LiveStatus status() const;
    // Whether a file of the package (its path in the folder, "v1_720p_h264/seg00012.m4s") is still being written.
    bool growing(const std::string& relative) const;

    struct State;

private:
    std::unique_ptr<State> s_;
};

// An HTTP origin for a package's folder, on a thread of its own: GET and HEAD of its files (no others: nothing
// outside, nothing hidden), with CORS for players on other pages. HLS playlist requests asking for a part or segment
// not yet made (_HLS_msn, _HLS_part) are held until it is (or three target durations, then 503); a file not yet there
// is waited for (up to `waitSeconds`); a file still growing is sent as it grows (chunked); an MPD's UTCTiming
// (urn:mpeg:dash:utc:direct:2014) is the time it is sent; /time is the time (ISO 8601).
class LiveOrigin {
public:
    LiveOrigin();
    ~LiveOrigin();  // stops
    LiveOrigin(const LiveOrigin&) = delete;
    LiveOrigin& operator=(const LiveOrigin&) = delete;
    // `growing` as LivePackager::growing (none: no file grows). Port 0: any free one. The address to listen on:
    // 127.0.0.1 (this computer only) or 0.0.0.0 (the network's).
    bool start(const std::string& folder, int port, std::function<bool(const std::string&)> growing, std::string* error = nullptr,
               const std::string& address = "127.0.0.1", double waitSeconds = 10);
    int port() const;
    void stop();

    struct State;

private:
    std::unique_ptr<State> s_;
};

}  // namespace montage
