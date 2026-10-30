// Montage — SMPTE ST 2110 over IP: the program as a facility's IP plant takes it, as DeckLink IP and AJA's IP cards
// send it.
//  - ST 2110-20 video: RFC 4175 pixel groups, 4:2:2 at 10 or 8 bits, progressive or interlaced (each field on its own),
//    one sample row range per packet (the general packing mode), sent spread over each frame's time (wide pacing).
//  - ST 2110-30 audio: AES67's L24, 1 ms packets, up to 8 channels at 48 kHz.
//  - ST 2110-40 ancillary data (RFC 8331): ancillary time code (ST 12-2), CEA-608/708 captions in a CDP (ST 334) and
//    HDR static metadata (ST 2108-1), a packet each frame or field even with none.
//  - An SDP file for each stream, with the parameters ST 2110-20, -30 and -40 require.
// Timing: RTP timestamps are taken from PTP time (ST 2059-1's epoch, TAI), read from the system clock as linuxptp
// (ptp4l and phc2sys) or the operating system keeps it in step with the grandmaster: Linux's TAI clock, else UTC plus
// the leap seconds. Packets go through the operating system's UDP sockets, unicast or multicast.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <memory>
#include <string>
#include <vector>

#include "core/Model.h"
#include "render/ReferenceOutput.h"

namespace montage::st2110 {

struct Endpoint {
    std::string address;  // IPv4, unicast or multicast
    int port = 0;
};
// "239.100.0.1:5004" (or an address alone, taking `port`); false when it is not one.
bool parseEndpoint(const std::string& text, Endpoint& out, int port = 5004);
std::string endpointText(const Endpoint& e);
bool isMulticast(const std::string& address);

// PTP time: nanoseconds since 1970-01-01 TAI, and where it came from ("TAI clock", or "system clock + 37 s" when
// nothing sets the TAI offset, so no PTP daemon keeps the clock).
int64_t ptpNowNs(std::string* source = nullptr);

// ---- What the streams carry.
struct VideoFormat {
    int width = 1920, height = 1080;
    Rational fps{25, 1};  // frames a second (an interlaced format's frames are its field pairs)
    bool interlaced = false;
    int depth = 10;       // 10 or 8
    std::string colorimetry = "BT709";  // BT709, BT2020 or BT2100
    std::string tcs = "SDR";            // SDR, PQ or HLG
};
struct AudioFormat {
    int channels = 2;
    int rate = 48000;
};
struct SdpOptions {
    std::string source;          // the sender's address ("" = 0.0.0.0)
    std::string name = "Montage";
    std::string refclk = "ptp=IEEE1588-2008:traceable";  // ts-refclk (a grandmaster's "ptp=IEEE1588-2008:<id>:<domain>")
    uint64_t session = 1;
    int ttl = 16;
};
std::string videoSdp(const VideoFormat& f, const Endpoint& to, int payloadType, const SdpOptions& o);
std::string audioSdp(const AudioFormat& f, const Endpoint& to, int payloadType, const SdpOptions& o);
// (`ids`: the DID/SDID pairs it carries)
std::string ancSdp(const VideoFormat& f, const Endpoint& to, int payloadType, const SdpOptions& o,
                   const std::vector<std::pair<int, int>>& ids = {{0x60, 0x60}, {0x61, 0x01}});
// An SDP's media format parameters (its a=fmtp line's "name=value;" pairs) and rtpmap encoding ("raw/90000").
std::map<std::string, std::string> sdpParameters(const std::string& sdp, std::string* encoding = nullptr);

// ---- RTP.
struct RtpState {
    uint8_t payloadType = 96;
    uint32_t ssrc = 0;
    uint32_t sequence = 0;  // RFC 4175's extended sequence number above the RTP header's 16 bits
};
// An RTP packet as received: header fields and payload.
struct RtpPacket {
    bool marker = false;
    uint8_t payloadType = 0;
    uint16_t sequence = 0;
    uint32_t timestamp = 0, ssrc = 0;
    const uint8_t* payload = nullptr;
    size_t size = 0;
};
bool parseRtp(const uint8_t* data, size_t size, RtpPacket& out);

// One picture (a frame, or with `field` 1 or 2 an interlaced frame's field: its rows of that parity) as ST 2110-20
// packets: `format` V210 or UYVY rows (the reference output's), `stride` bytes apart, cut into packets of at most
// `maxPayload` bytes of pixels (whole pixel groups, a row range each), the last marked.
using PacketSink = std::function<bool(const uint8_t* data, size_t size)>;
bool packVideo(const uint8_t* data, size_t stride, output::PixelFormat format, int width, int height, int field, uint32_t timestamp,
               RtpState& rtp, const PacketSink& send, int maxPayload = 1200);
// Packets a picture makes (for pacing).
int videoPacketCount(int width, int height, int depth, bool interlaced, int maxPayload = 1200);
// Rebuilds pictures from ST 2110-20 packets into rows of the same format (fields woven into their frame).
class VideoAssembler {
public:
    VideoAssembler(int width, int height, output::PixelFormat format, bool interlaced);
    // A packet; true when it completed a frame (`frame()` then holds it, and `timestamp()` its first field's).
    bool add(const RtpPacket& p);
    const std::vector<uint8_t>& frame() const { return done_; }
    uint32_t timestamp() const { return doneTimestamp_; }
    int64_t lost() const { return lost_; }  // packets missing by their sequence numbers

private:
    int width_, height_;
    output::PixelFormat format_;
    bool interlaced_;
    size_t stride_;
    std::vector<uint8_t> done_;
    uint32_t timestamp_ = 0, doneTimestamp_ = 0;
    bool started_ = false;
    int fieldsDone_ = 0;
    int64_t lost_ = 0;
    int64_t lastSeq_ = -1;
    std::vector<uint16_t> components_;  // the picture being built: Cb Y Cr Y a row
    void finish();
};

// Audio as L24 (24-bit big-endian, interleaved), one packet of `frames` samples.
bool packAudio(const float* samples, int frames, int channels, uint32_t timestamp, RtpState& rtp, const PacketSink& send);
// The samples of an L24 payload, as floats (exactly what was sent, to 24 bits).
std::vector<float> unpackAudio(const uint8_t* payload, size_t size, int channels);

// ---- Ancillary data (ST 291 packets), as RFC 8331 carries them.
struct AncPacket {
    uint8_t did = 0, sdid = 0;
    std::vector<uint8_t> data;  // its user data words' 8 bits (parity added when sent)
    // Where SDI would carry it: RFC 8331's "any line of the vertical blanking" (7FEh) and "in VANC" (FFDh) unless
    // said (exact lines would need the SDP's VPID_Code).
    int line = 0x7FE;
    int horizontalOffset = 0xFFD;
    bool chroma = false;
};
// The packets of one picture (or field) as one RTP packet (marked); empty `packets` sends an empty one, as
// ST 2110-40 wants every frame to have one.
bool packAnc(const std::vector<AncPacket>& packets, int field, uint32_t timestamp, RtpState& rtp, const PacketSink& send);
// Back from a payload (checksums and parity checked: false when one is wrong).
bool unpackAnc(const uint8_t* payload, size_t size, std::vector<AncPacket>& out, int* field = nullptr);
// ST 12-2 ancillary time code (DID 60h, SDID 60h): a time address as LTC (type 0) or VITC1 / VITC2 (1, 2), with its
// drop frame flag and (VITC) field mark, which a 25-frame count keeps in its bit 75 and a 30 or 24-frame count in bit 35.
enum class AtcType { Ltc = 0, Vitc1 = 1, Vitc2 = 2 };
AncPacket atcPacket(const output::Timecode& tc, AtcType type = AtcType::Ltc, bool fieldMark = false, int frameCount = 30);
bool parseAtc(const AncPacket& p, output::Timecode& out, AtcType* type = nullptr, bool* fieldMark = nullptr, int frameCount = 30);
// The time code packets of a frame (`field` 0) or an interlaced frame's field (1, 2) of time code `tc` at `fps`: LTC once a
// frame, and VITC1 in the first field and VITC2 in the second; above 30 frames a second, the count of frame pairs (one
// time address for two frames, LTC and VITC1 with the first, VITC2 with its field mark on the second, ST 12-1's 50p and
// 60p).
std::vector<AncPacket> timecodePackets(const output::Timecode& tc, Rational fps, int field);
// ST 2108-1 static HDR metadata (DID 41h, SDID 0Ch): the mastering display (ST 2086: its primaries, D65 white, peak and
// minimum) and the content light levels, as HEVC's SEI messages carry them; and back (primaries R, G, B: x, y).
AncPacket hdrMetadataPacket(const output::Signal& s);
struct HdrMetadata {
    double primaries[3][2] = {}, white[2] = {};
    double maxLuminance = 0, minLuminance = 0;
    int maxCll = 0, maxFall = 0;
    bool display = false, lightLevel = false;
};
bool parseHdrMetadata(const AncPacket& p, HdrMetadata& out);
// CEA-608/708 captions: a CDP (core/Cea708.h's captionCdp) in DID 61h, SDID 01h.
AncPacket captionPacket(const std::vector<uint8_t>& cdp);

// ---- A sender of the three streams.
struct SenderOptions {
    Endpoint video{"239.100.0.1", 5004}, audio{"239.100.0.2", 5004}, anc{"239.100.0.3", 5004};
    std::string interfaceAddress;  // the address to send from ("" = as routing chooses)
    int sourcePort = 0;            // the port to send from (0: any free)
    int ttl = 16;
    bool sendVideo = true, sendAudio = true, sendAnc = true;
    int videoPayload = 96, audioPayload = 97, ancPayload = 100;
    int maxPayload = 1200;  // bytes of pixels a packet
    std::string refclk = "ptp=IEEE1588-2008:traceable";
};
// Options the reference output's ST 2110 backend uses when it opens (the app's settings or MCP set them).
void setOutputOptions(const SenderOptions& o);
SenderOptions outputOptions();

class Sender {
public:
    Sender();
    ~Sender();
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    bool open(const SenderOptions& o, const VideoFormat& video, const AudioFormat& audio, std::string* error = nullptr);
    // A frame (V210 or UYVY rows), its audio (interleaved, any number of samples: sent in 1 ms packets as they fill),
    // its time code (as timecodePackets makes it) and other ancillary packets (with the frame, or an interlaced frame's
    // first field), sent over the frame's time from the next frame boundary of PTP time; returns when it has gone. False
    // when sending failed (or `stop`).
    bool send(const uint8_t* data, size_t stride, output::PixelFormat format, const float* audio, int audioFrames,
              const output::Timecode* timecode, const std::vector<AncPacket>& anc, const std::atomic<bool>* stop = nullptr);
    // Ancillary packets sent with every frame from now (ST 2108-1's HDR metadata), and named in the SDP.
    void setFrameAnc(const std::vector<AncPacket>& anc) { frameAnc_ = anc; }
    void close();
    bool isOpen() const;
    std::string sdpVideo() const, sdpAudio() const, sdpAnc() const;
    std::string clockSource() const { return clock_; }
    const SenderOptions& options() const { return o_; }
    const VideoFormat& videoFormat() const { return video_; }
    const AudioFormat& audioFormat() const { return audio_; }
    int64_t framesSent() const { return frames_; }

private:
    struct Sockets;
    std::unique_ptr<Sockets> s_;
    SenderOptions o_;
    VideoFormat video_;
    AudioFormat audio_;
    RtpState rtpVideo_, rtpAudio_, rtpAnc_;
    int64_t nextFrame_ = 0;       // the frame number (PTP time / frame period) sent next
    int64_t audioSample_ = 0;     // the next audio packet's first sample (PTP time at the audio rate)
    std::vector<float> audioFifo_;
    std::vector<AncPacket> frameAnc_;
    std::string clock_, source_;
    int64_t frames_ = 0;
};

// What an ST 2110 output (output::backends()'s "ST 2110") sends now: its streams' formats and where they go; false when
// `b` is another interface or is not open.
bool openStreams(output::Backend& b, VideoFormat& video, AudioFormat& audio, SenderOptions& options);

// Receives UDP datagrams on an endpoint (joining it when multicast), for tests and checking a stream.
class Receiver {
public:
    Receiver();
    ~Receiver();
    bool open(const Endpoint& e, const std::string& interfaceAddress = {}, std::string* error = nullptr);
    // The next datagram within `ms`; false when none came.
    bool receive(std::vector<uint8_t>& out, int ms);
    void close();
    int port() const;  // the port it is bound to (an endpoint's port 0 binds a free one)

private:
    struct Socket;
    std::unique_ptr<Socket> s_;
};

}  // namespace montage::st2110
