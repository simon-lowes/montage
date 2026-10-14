// Montage — live remote review: the Program monitor streamed to reviewers elsewhere as it plays or parks, at low
// latency. It is an output interface like NDI's (render/ReferenceOutput.h): the frames playback renders are encoded
// (HEVC 10-bit or H.264 8-bit, on NVENC where there is one, else x265 or x264, tuned for zero latency, a key frame
// every second), with the sound mixed to stereo (AAC), in an MPEG transport stream sent over SRT (AES-256 with a
// passphrase; Montage listening for a viewer to call, or calling a relay that viewers watch) or UDP. Each picture
// carries its timecode burnt in and, as SEI user data, the sequence frame it shows. Viewers watch in Montage (its
// review viewer below), VLC or ffplay; a Montage viewer sends markers back over a TCP side channel that asks for the
// same passphrase, and they land on the sequence as review notes do. No relay, service or account of Montage's own.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/Model.h"
#include "render/ReferenceOutput.h"

namespace montage::output {

struct StreamSettings {
    // Where it goes: "srt://0.0.0.0:9000" (listening: any of this computer's addresses) or "srt://relay:8890?streamid=…"
    // (calling a server), or "udp://239.1.1.1:5000" (a local network, not encrypted).
    std::string address = "srt://0.0.0.0:9000";
    bool listen = true;          // SRT: wait for a viewer to call (one at a time); else call the address
    std::string passphrase;      // SRT's key (10 to 79 characters), asked of viewers and of the marker channel
    std::string codec = "hevc";  // "hevc" (10-bit) or "h264" (8-bit, for players without HEVC)
    bool hardware = true;        // NVENC where there is one
    int bitrateKbps = 8000;
    int audioBitrateKbps = 192;
    int latencyMs = 120;         // SRT's buffer against loss and jitter
    bool burnTimecode = true;
    int markersPort = 0;         // the marker channel's TCP port: 0 the stream's port + 1, -1 none
};

// Why the settings cannot be used ("" when they can).
std::string checkStreamSettings(const StreamSettings& s);
// A passphrase of 20 letters and digits, none that look alike.
std::string generatePassphrase();
// The scheme, host and port of an address ("srt", "0.0.0.0", 9000); false when it is not one.
bool parseStreamAddress(const std::string& address, std::string& scheme, std::string& host, int& port);
// The marker channel's port for the settings (-1 when none).
int markersPortFor(const StreamSettings& s);

// What each picture carries: the sequence frame shown and its timecode.
struct StreamTag {
    FrameTime frame = -1;
    std::string timecode;  // "01:00:10:05" (";" before the frames in drop frame)
    Rational fps{0, 1};    // the stream's (a sequence goes out at its own rate)
    Id sequence = 0;       // the sequence streamed
};
// SEI user data (unregistered): Montage's UUID, then the tag as JSON.
std::vector<uint8_t> streamTagPayload(const StreamTag& t);
// The tag from SEI user data (the UUID found at its start, or after an encoder's own, as x265 writes it).
bool parseStreamTag(const uint8_t* data, size_t size, StreamTag& out);

// A 4:2:0 picture of 10-bit codes (limited range).
struct Planes420 {
    int width = 0, height = 0;
    std::vector<uint16_t> y, u, v;  // u and v (width + 1) / 2 by (height + 1) / 2
    Planes420() = default;
    Planes420(int w, int h);
};
// A packed frame (P216 or UYVY) as 4:2:0 10-bit codes: chroma rows averaged in pairs.
Planes420 planesFrom(const VideoFrame& f);
// The box the timecode is burnt into (on even pixels), for `chars` characters, at the bottom centre.
Rect burnInBox(int width, int height, size_t chars);
// The timecode in white on a black box (digits, ":" and ";" drawn from a font of Montage's own).
void burnTimecode(Planes420& p, const std::string& text);

// A marker a reviewer sent.
struct RemoteMarker {
    FrameTime frame = 0, duration = 0;
    Id sequence = 0;  // the stream tag's (0: the one streamed now)
    std::string timecode, author, text, drawing, id;  // drawing: core/ReviewPage.h's JSON ("" none)
};

class StreamBackend : public Backend {
public:
    StreamBackend();
    ~StreamBackend() override;
    std::string name() const override { return "Stream"; }
    bool available(std::string* why = nullptr) override;
    std::vector<Device> devices() override;
    // Taken by the next `open`.
    void setSettings(const StreamSettings& s);
    // The sequence each picture's tag names.
    void setSequence(Id id);
    StreamSettings settings() const;
    bool open(const Device& device, const Mode& mode, const Signal& signal, int audioChannels, int audioRate,
              std::string* error) override;
    bool send(const VideoFrame& frame, const Audio& audio) override;
    void close() override;
    void wake() override;
    // 1 while a viewer (or the server called) is connected, else 0.
    int receivers() override;

    struct Stats {
        bool connected = false;
        int64_t encoded = 0;       // pictures sent since the viewer connected
        int64_t skipped = 0;       // ticks with no picture sent because encoding fell behind
        double kbps = 0;           // the last few seconds' rate
        std::string encoder;       // "libx265", "hevc_nvenc"
        std::string error;         // the last connection's ("" none)
        int markersPort = -1;      // listening for markers on it (-1 none)
        int markers = 0;           // received since opened
    };
    Stats stats() const;
    // Markers received since the last call.
    std::vector<RemoteMarker> takeMarkers();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Watches a stream: connects (calling a Montage that listens, or listening for one that calls) on a thread of its
// own, and again whenever the stream drops, until stopped; decodes its pictures (the latest kept) and sound.
class ReviewReceiver {
public:
    ReviewReceiver();
    ~ReviewReceiver();
    ReviewReceiver(const ReviewReceiver&) = delete;
    ReviewReceiver& operator=(const ReviewReceiver&) = delete;

    bool start(const std::string& address, const std::string& passphrase, int latencyMs = 120, bool listen = false,
               std::string* error = nullptr);
    void stop();

    struct Picture {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;  // 8-bit RGBA, for showing
        std::vector<uint16_t> y;    // the decoded luma codes
        int depth = 8;              // their bits
        bool tagged = false;
        StreamTag tag;
        int64_t serial = 0;         // counts pictures decoded
    };
    // The newest picture decoded after picture `after`; false when there is none.
    bool latest(Picture& out, int64_t after = 0) const;
    // The tags of the pictures decoded, in order (the last 2000).
    std::vector<StreamTag> tags() const;
    // Sound decoded since the last call: stereo, interleaved, at 48 kHz.
    std::vector<float> takeAudio();

    struct Status {
        bool connected = false;
        std::string error;      // why the last connection failed or dropped
        int64_t pictures = 0;
        std::string video;      // "hevc 1920x1080 10-bit"
        bool audio = false;
    };
    Status status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// Sends markers to a Montage that streams (its marker channel at `host`:`port`): each side proves it has the
// passphrase (a key derived from it, never the passphrase itself), and each marker is signed. False and why when it
// would not take them, or could not prove it has the passphrase.
bool sendRemoteMarkers(const std::string& host, int port, const std::string& passphrase, const std::vector<RemoteMarker>& markers,
                       std::string* error = nullptr, int timeoutMs = 5000);

}  // namespace montage::output
