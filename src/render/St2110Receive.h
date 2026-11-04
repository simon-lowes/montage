// Montage — receiving SMPTE ST 2110: a facility's IP feeds recorded as SDI capture cards record theirs.
//  - The streams are found from their SDP files (as a sender or an NMOS controller gives them): ST 2110-20 video at
//    any sampling and depth render/St2110 sends, -30 audio (L24 or L16, 1 ms or 125 µs packets) and -40 ancillary data.
//  - ST 2022-7: a stream that arrives on two paths (an SDP's "a=group:DUP") is merged by RTP sequence number, each packet
//    taken once from whichever path brought it.
//  - Pictures, sound and ancillary data are put together by their RTP timestamps, which ST 2110 takes from PTP time: a
//    frame comes with the samples of its own time span, its time code (ST 12-2), captions and HDR metadata.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "render/St2110.h"

namespace montage::st2110 {

// One stream an SDP describes: what it carries and where it arrives (with ST 2022-7, on two paths).
struct SdpStream {
    std::string media;     // "video" or "audio"
    std::string encoding;  // "raw", "smpte291", "L24" or "L16"
    int payloadType = -1;
    Endpoint path[2];       // (path[1].port 0: one path)
    std::string source[2];  // the sender's address as its source filter gives it ("" not said)
    VideoFormat video;      // raw (all of it) and smpte291 (its frame rate)
    AudioFormat audio;      // L24 and L16
    bool fullRange = false; // the video's RANGE=FULL (its codes from 0, not 16 at 8 bits)
    bool redundant() const { return path[1].port > 0; }
};
// The streams in an SDP (one or several; two media descriptions of a "DUP" group are one stream's two paths).
std::vector<SdpStream> parseSdp(const std::string& sdp, std::string* error = nullptr);
// A stream as an SDP again (parseSdp's inverse, for what it reads: an NMOS controller's connection written down).
std::string sdpText(const SdpStream& s);

// The time code a frame's ancillary data carries (as timecodePackets sends it: LTC, else VITC; above 30 frames a
// second, its frame pair's with the second frame's field mark); false when none.
bool frameTimecode(const std::vector<AncPacket>& anc, Rational fps, output::Timecode& out);

// ST 2022-7: the packets of one stream from one or two paths, each taken once, out in the order of their RTP sequence
// numbers (RFC 4175's extended ones for video, with `extended`). A gap is waited on while `window` later packets
// arrive, for the other path to fill it.
class PathMerger {
public:
    explicit PathMerger(bool extended, size_t window = 512) : extended_(extended), window_(window) {}
    void setWindow(size_t window) { window_ = std::max<size_t>(16, window); }
    void push(std::vector<uint8_t> packet, int path, std::vector<std::vector<uint8_t>>& out);
    // Everything held, in order (a pause in the stream: what is missing will not come).
    void flush(std::vector<std::vector<uint8_t>>& out);
    int64_t received[2] = {0, 0};
    int64_t duplicates = 0, lost = 0, late = 0;

private:
    bool extended_;
    size_t window_;
    bool started_ = false;
    int64_t next_ = 0;
    int64_t lateRun_ = 0;  // packets in a row from before the window (a sender restarted, or one jumped ahead of)
    std::map<int64_t, std::vector<uint8_t>> held_;
    void release(std::vector<std::vector<uint8_t>>& out);
};

// A frame as received.
struct InputFrame {
    Raster picture;
    uint32_t rtpTimestamp = 0;
    int64_t ptpNs = 0;      // when it began (PTP time)
    int64_t number = 0;     // its frame count since the PTP epoch
    std::vector<float> audio;  // its time span's samples, interleaved (silence where packets were missing)
    int channels = 0;
    std::vector<AncPacket> anc;  // its ancillary packets (both fields')
    bool hasTimecode = false;
    output::Timecode timecode;
    int64_t lostPackets = 0;  // of its picture, after merging the paths
};

struct InputOptions {
    std::string interfaceAddress[2];  // where to join multicast groups on each path ("" as routing chooses)
    int waitMs = 200;                 // how long a frame waits for its sound and ancillary data before going without
};

struct InputStats {
    int64_t frames = 0, incomplete = 0, dropped = 0;  // (dropped: frames not taken in time)
    int64_t packets[3][2] = {};                       // video, audio, ancillary on each path
    int64_t duplicates = 0, lost = 0;                 // after merging
};

// Receives the streams (one video, and at most one audio and one ancillary data stream) on threads of their own.
class Input {
public:
    Input();
    ~Input();
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    bool open(const std::vector<SdpStream>& streams, const InputOptions& o = {}, std::string* error = nullptr);
    // The next frame within `ms`; false when none came (or closed).
    bool next(InputFrame& out, int ms);
    void close();
    bool isOpen() const;
    VideoFormat video() const { return video_; }
    AudioFormat audio() const { return audio_; }
    bool hasAudio() const { return hasAudio_; }
    InputStats stats() const;

private:
    struct Stream;
    struct Pending {
        Raster picture;
        uint32_t ts = 0;
        int64_t lost = 0;
        std::chrono::steady_clock::time_point at;
    };
    void receive(Stream* s, int path);
    void take(Stream& s, const std::vector<uint8_t>& packet);
    bool ready(const Pending& p, std::chrono::steady_clock::time_point now);
    void build(Pending& p, InputFrame& out);

    VideoFormat video_;
    AudioFormat audio_;
    bool hasAudio_ = false, hasAnc_ = false, ancSeen_ = false, joined_ = false;
    size_t pendingLimit_ = 16;  // frames held for the reader (fewer of large pictures)
    uint32_t ancFirst_ = 0, ancLatest_ = 0;  // the first and latest ancillary data's RTP timestamps
    InputOptions options_;
    std::vector<std::unique_ptr<Stream>> streams_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stop_{false};
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<Pending> pending_;
    std::map<uint32_t, std::vector<AncPacket>> anc_;  // by RTP timestamp
    std::deque<float> sound_;                         // samples from soundStart_ on
    int64_t soundStart_ = -1;
    InputStats stats_;
};

}  // namespace montage::st2110
