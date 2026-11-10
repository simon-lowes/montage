// Montage — receiving SMPTE ST 2110: a facility's IP feeds recorded as SDI capture cards record theirs.
//  - The streams are found from their SDP files (as a sender or an NMOS controller gives them): ST 2110-20 video at
//    any sampling and depth render/St2110 sends, -22's JPEG XS (RFC 9134, either packetization mode; a lost packet
//    damaging only the lines of its slices), -30 audio (L24 or L16, 1 ms or 125 µs packets) and -40 ancillary data.
//  - ST 2022-7: a stream that arrives on two paths (an SDP's "a=group:DUP") is merged by RTP sequence number, each packet
//    taken once from whichever path brought it.
//  - Pictures, sound and ancillary data are put together by their RTP timestamps, which ST 2110 takes from PTP time: a
//    frame comes with the samples of its own time span, its time code (ST 12-2), captions and HDR metadata.
//  - Several sound streams (-30, and -31's AES3 subframes kept bit for bit), ancillary data streams and -41 fast
//    metadata streams go with one picture.
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
    std::string encoding;  // "raw", "jxsv" (ST 2110-22's JPEG XS), "smpte291", "L24", "L16", "AM824" (ST 2110-31) or "ST2110-41"
    int payloadType = -1;
    Endpoint path[2];       // (path[1].port 0: one path)
    std::string source[2];  // the sender's address as its source filter gives it ("" not said)
    VideoFormat video;      // raw and jxsv (all of it) and smpte291 (its frame rate)
    AudioFormat audio;      // L24, L16 and AM824 (its `aes3`)
    std::vector<uint32_t> metadataTypes;  // ST2110-41's data item types (its DIT)
    bool fullRange = false; // the video's RANGE=FULL (its codes from 0, not 16 at 8 bits)
    std::string interfaceAddress[2];  // where this stream joins on each path ("" as InputOptions say; not in its SDP)
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
// arrive (and no more than `maxBytes` of them are held), for the other path to fill it.
// Only RTP packets of the stream's payload type (`setPayloadType`, -1 any) and of one sender on each path (its SSRC:
// the two paths' may differ) take a place. Another sender's are passed over until a window's worth of them has come
// with fewer than a quarter as many of the one taken between: then it is taken instead, so a real stream is not kept
// out by a stray sender heard first and trickling on (an any-source join), and a sender that starts again with a new
// SSRC is followed.
class PathMerger {
public:
    explicit PathMerger(bool extended, size_t window = 512, size_t maxBytes = size_t(256) << 20)
        : extended_(extended), window_(window), maxBytes_(maxBytes) {}
    void setWindow(size_t window) { window_ = std::max<size_t>(16, window); }
    void setPayloadType(int type) { payloadType_ = type; }
    void push(std::vector<uint8_t> packet, int path, std::vector<std::vector<uint8_t>>& out);
    // Everything held, in order (a pause in the stream: what is missing will not come).
    void flush(std::vector<std::vector<uint8_t>>& out);
    size_t heldBytes() const { return heldBytes_; }
    int64_t received[2] = {0, 0};
    int64_t duplicates = 0, lost = 0, late = 0;
    int64_t rejected = 0;  // not RTP, of another payload type, or another sender's

private:
    bool extended_;
    size_t window_, maxBytes_;
    int payloadType_ = -1;
    bool started_ = false;
    // Each path's sender: its SSRC, its packets since the others' were last counted from, and theirs (by SSRC, a few);
    // with none yet, those that would be its first but are numbered far from the stream, and the other path's count
    // when each of them was first heard.
    struct Heard {
        int64_t count = 0;
        int64_t otherAt = 0;
        int64_t last = 0;  // (when last heard, by heardClock_)
    };
    int64_t heardClock_ = 0;
    struct Sender {
        bool have = false;
        uint32_t ssrc = 0;
        int64_t own = 0;
        std::map<uint32_t, Heard> others;
    } sender_[2];
    int64_t next_ = 0;
    int64_t lateRun_ = 0;     // packets in a row from before the window (a sender restarted, or one jumped ahead of)
    int restartedBy_ = -1;    // the path whose new sender last started the count again
    int64_t sinceRestart_ = 0;  // packets taken since
    size_t heldBytes_ = 0;     // what the packets held take (their buffers, and a little for each)
    std::map<int64_t, std::vector<uint8_t>> held_;
    void release(std::vector<std::vector<uint8_t>>& out);
};

// A frame as received.
struct InputFrame {
    Raster picture;
    std::vector<uint8_t> codestream;  // ST 2110-22: the JPEG XS codestream it was decoded from (as rebuilt)
    uint32_t rtpTimestamp = 0;
    int64_t ptpNs = 0;      // when it began (PTP time)
    int64_t number = 0;     // its frame count since the PTP epoch
    std::vector<float> audio;  // its time span's samples, interleaved (silence where packets were missing): every
                               // sound stream's channels in turn
    int channels = 0;
    // With an AES3 stream (ST 2110-31) among them: the same samples as AM824 words, the AES3 streams' as they came
    // (their user, validity and channel status bits too), the others' their PCM's 24 bits under an empty label.
    std::vector<uint32_t> aes3;
    std::vector<FastMetadata> metadata;  // ST 2110-41 data items timed within the frame
    std::vector<AncPacket> anc;  // its ancillary packets (both fields')
    bool hasTimecode = false;
    output::Timecode timecode;
    int64_t lostPackets = 0;  // of its picture, after merging the paths
};

struct InputOptions {
    std::string interfaceAddress[2];  // where to join multicast groups on each path ("" as routing chooses), for the
                                      // streams that name none of their own
    int waitMs = 200;                 // how long a frame waits for its sound and ancillary data before going without
};

struct InputStats {
    int64_t frames = 0, incomplete = 0, dropped = 0;  // (dropped: frames not taken in time)
    int64_t packets[4][2] = {};                       // video, audio, ancillary, fast metadata on each path
    int64_t duplicates = 0, lost = 0;                 // after merging
    int64_t concealedSlices = 0;                      // ST 2110-22: slices taken from the frame before (packets lost)
};

// Receives the streams (one video, and any number of sound, ancillary data and fast metadata streams: sound streams at
// one rate, up to 64 channels in all) on threads of their own.
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
    // The sound: the first stream's format with every stream's channels.
    AudioFormat audio() const { return audio_; }
    bool hasAudio() const { return hasAudio_; }
    // Each sound stream's format, in the order their channels come.
    std::vector<AudioFormat> audioStreams() const;
    InputStats stats() const;

private:
    struct Stream;
    struct Pending {
        Raster picture;
        uint32_t ts = 0;
        int64_t lost = 0;
        std::chrono::steady_clock::time_point at;
        std::vector<uint8_t> codestream;
    };
    void receive(Stream* s, int path);
    void take(Stream& s, const std::vector<uint8_t>& packet);
    bool ready(const Pending& p, std::chrono::steady_clock::time_point now);
    void build(Pending& p, InputFrame& out);

    VideoFormat video_;
    AudioFormat audio_;
    bool hasAudio_ = false, hasAnc_ = false, ancSeen_ = false, joined_ = false;
    bool hasMeta_ = false, metaSeen_ = false, aes3_ = false;
    uint32_t metaLatest_ = 0;
    std::map<uint32_t, std::vector<FastMetadata>> meta_;  // by RTP timestamp
    // Each sound stream's samples from `start` on (AES3's as words too), and where its channels go.
    struct Sound {
        AudioFormat format;
        int offset = 0;
        std::deque<float> samples;
        std::deque<uint32_t> words;
        int64_t start = -1;
    };
    std::vector<Sound> sounds_;
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
    InputStats stats_;
};

}  // namespace montage::st2110
