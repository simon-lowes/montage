// Montage — live streaming packages: a recording (media/Capture.h, any input: SRT, RIST, OMT, ST 2110, a camera) fed
// as it is made to a ladder of renditions, packaged for HLS (a sliding window of segments, EXT-X-MEDIA-SEQUENCE moving
// on and the oldest files removed) and DASH (a dynamic MPD whose time-shift buffer is the same window). With low
// latency, segments are made of parts: each a CMAF chunk (moof + mdat), written as an LL-HLS part file (EXT-X-PART,
// a PRELOAD-HINT for the next, blocking playlist reloads, rendition reports and playlist delta updates) and appended
// to the segment as it grows, which the origin sends to a DASH player as it comes (chunked transfer,
// availabilityTimeOffset). HLS can have MPEG-TS segments instead (render/TsSegments.h; not with low latency). The sound
// can be AAC, Opus, AC-3 or E-AC-3, the AC-3 family stating the programme's loudness as measured so far (dialnorm,
// updated each segment); each sound stream of the feed is a rendition (an adaptation set) in its language. The feed's
// CEA-608 and CTA-708 captions become WebVTT segments for HLS and IMSC 1 (TTML in fragmented MP4) for DASH; SCTE 35
// cues, from the feed or placed while it records, become EXT-X-DATERANGE tags and DASH events (emsg in the segments)
// on a keyframe at the splice; chapters marked while it records are a WebVTT chapters file. LiveOrigin serves the
// package's folder over HTTP as players need it (what static file servers cannot: the blocking reloads, the delta
// updates and the growing segments).
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
    // Each sound stream's language, by track (LivePackager::soundTracks; track 0 the main sound's): "" or past the
    // list the feed's own (else audioLanguage for track 0, else und). RFC 5646 or ISO 639-2 codes.
    std::vector<std::string> languages;
    // The feed's captions (CEA-608 channel 1, or CTA-708 service 1 where it has pop-on captions): "auto" listed when
    // its first segment carries caption data, "always" listed from the start, "none".
    std::string captions = "auto";
    std::string captionLanguage = "en", captionName;  // ("": the language's)
    // Cues placed while recording (LivePackager::cue): splice_insert or time_signal with segmentation descriptors (a
    // placement opportunity's start and end).
    std::string cueCommand = "splice_insert";
};

// A sound stream of the feed (CaptureSettings::SoundTrack): its language and title as the feed states them.
struct LiveSoundTrack {
    std::string language, title;
    int channels = 0;
};

// An ad cue: an out point (a break's start, with its length if known), the return (`returning`: the break's end), or
// another SCTE 35 command (time_signal without a break's segmentation; `other`).
struct LiveCue {
    double at = -1;          // seconds into the recording (its first picture at 0); -1: the next picture recorded
    double duration = 0;     // the break's length (seconds; 0 not known)
    bool returning = false, other = false;
    uint32_t eventId = 0;    // the feed's splice_event_id (or segmentation_event_id), matching a return to its out
    std::vector<uint8_t> scte35;  // the feed's splice_info_section, as it came ("" for a cue placed here: made)
};

struct LiveCueStatus {
    int id = 0;               // the date range's (cue-1, cue-2...) and the DASH events' (2 x id, + 1 the return)
    int64_t frame = 0;        // the splice's picture, from the presentation's first
    double seconds = 0;       // and its time
    double duration = 0;
    bool fromFeed = false, late = false;  // came in the feed / was asked for a picture already encoded (placed on the next)
    bool keyframe = false;    // the first rung's picture there was coded as a keyframe
    bool returned = false;    // the break's end passed (its return listed)
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
    int soundRenditions = 0;
    bool captioned = false;             // a caption rendition is listed
    int64_t captionSegments = 0, captionCues = 0;  // its segments written, and the cues in them
    std::vector<LiveCueStatus> cues;
    int chapters = 0;
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
    // A picture's CEA-608 and CTA-708 caption data (AV_FRAME_DATA_A53_CC) are its captions.
    void picture(const AVFrame* frame, int fpsNum, int fpsDen);
    // The feed's sound streams, before their sound (track 0 the main sound; CaptureSettings::onSoundTracks). Without
    // it there is one, track 0. A track whose sound begins a segment after the pictures is left out.
    void soundTracks(const std::vector<LiveSoundTrack>& tracks);
    void sound(const AVFrame* frame, int track = 0);
    // An ad cue, from any thread: placed on its picture, made a keyframe (a cue for a picture already encoded goes on
    // the next), listed as an EXT-X-DATERANGE (SCTE35-OUT, then SCTE35-IN once the break is over) and sent to DASH
    // players as an event in the segments (emsg, urn:scte:scte35:2013:bin). A cue placed here is made a splice_insert
    // (or time_signal) at the presentation's 90 kHz time.
    void cue(const LiveCue& cue);
    // A SCTE 35 message from the feed (CaptureSettings::onCue): splice_insert and time_signal become cues at their
    // splice time on the feed's clock (immediate ones when they came); splice_null and those it cannot read are passed
    // over.
    void cueFromFeed(const std::vector<uint8_t>& section, double feedZero, double at);
    // A chapter from here (at < 0: the next picture recorded; else seconds into the recording) to the next: the
    // package's chapters.vtt (named in the multivariant playlist).
    void chapter(const std::string& title, double at = -1);
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
// not yet made (_HLS_msn, _HLS_part) are held until it is (or three target durations, then 503); a delta update
// (_HLS_skip=YES, of a playlist stating CAN-SKIP-UNTIL) has the segments before its skip boundary replaced by
// EXT-X-SKIP (playlistDeltaUpdate); a file not yet there
// is waited for (up to `waitSeconds`, when it is a playlist, segment or MPD in a folder there); a file still growing is
// sent as it grows (chunked); an MPD's UTCTiming (urn:mpeg:dash:utc:direct:2014) is the time it is sent; /time is the
// time (ISO 8601). A connection that has not sent its whole request in 10 s is closed, and with 256 open the oldest
// still sending one makes room for the next.
// A playlist delta update (HLS 6.2.5.1) of a media playlist: the segments wholly before its skip boundary
// (CAN-SKIP-UNTIL seconds from its end) replaced by EXT-X-SKIP:SKIPPED-SEGMENTS, their date ranges kept; the playlist
// as it is when it states no CAN-SKIP-UNTIL or nothing is that old. `skipped` gets the count.
std::string playlistDeltaUpdate(const std::string& playlist, int* skipped = nullptr);

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
