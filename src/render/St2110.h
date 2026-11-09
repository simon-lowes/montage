// Montage — SMPTE ST 2110 over IP: the program as a facility's IP plant takes it, as DeckLink IP and AJA's IP cards
// send it.
//  - ST 2110-20 video: RFC 4175 pixel groups, YCbCr 4:2:2 or 4:4:4 or RGB at 8, 10 or 12 bits, progressive or
//    interlaced (each field on its own), one sample row range per packet (the general packing mode), sent spread over
//    each frame's time (wide pacing).
//  - ST 2110-30 audio: AES67's L24 or L16, 1 ms or 125 µs packets, up to 8 channels at 48 kHz.
//  - ST 2110-31 AES3 transparent audio: each AES3 subframe whole (its 24 bits, validity, user and channel status bits)
//    in an AM824 word, so a payload that is not PCM (an ST 337 data burst) goes through bit for bit.
//  - ST 2110-41 fast metadata: data items (a 22-bit type, its K bit and contents in 32-bit words) with each frame.
//  - ST 2022-7: each stream sent on two paths at once (and merged again by RTP sequence number where received).
//  - ST 2110-40 ancillary data (RFC 8331): ancillary time code (ST 12-2), CEA-608/708 captions in a CDP (ST 334) and
//    HDR static metadata (ST 2108-1), a packet each frame or field even with none.
//  - An SDP file for each stream, with the parameters ST 2110-20, -30 and -40 require.
// Timing: RTP timestamps are taken from PTP time (ST 2059-1's epoch, TAI), read from the system clock as linuxptp
// (ptp4l and phc2sys) or the operating system keeps it in step with the grandmaster: Linux's TAI clock, else UTC plus
// the leap seconds. Packets go through the operating system's UDP sockets, unicast or multicast.
#pragma once

#include <array>
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
// ST 2110-20's samplings: YCbCr 4:2:2 (Cb Y Cr Y for each pair of pixels), 4:4:4 (Cb Y Cr for each) and RGB (R G B).
enum class Sampling { Ycc422, Ycc444, Rgb };
std::string samplingName(Sampling s);  // as the SDP names it ("YCbCr-4:2:2", "YCbCr-4:4:4", "RGB")
bool parseSampling(const std::string& name, Sampling& out);
struct VideoFormat {
    int width = 1920, height = 1080;
    Rational fps{25, 1};  // frames a second (an interlaced format's frames are its field pairs)
    bool interlaced = false;
    Sampling sampling = Sampling::Ycc422;
    int depth = 10;       // 8, 10 or 12
    std::string colorimetry = "BT709";  // BT709, BT2020 or BT2100
    std::string tcs = "SDR";            // SDR, PQ or HLG
};
struct AudioFormat {
    int channels = 2;
    int rate = 48000;
    int bits = 24;          // L24, or L16
    int packetUs = 1000;    // 1 ms packets (ST 2110-30 level A), or 125 µs (level C)
    bool aes3 = false;      // ST 2110-31: AES3 subframes as AM824 words (`bits` not used)
    int packetSamples() const { return int(int64_t(rate) * packetUs / 1000000); }
};
struct SdpOptions {
    std::string source;          // the sender's address ("" = 0.0.0.0)
    std::string name = "Montage";
    std::string refclk = "ptp=IEEE1588-2008:traceable";  // ts-refclk (a grandmaster's "ptp=IEEE1588-2008:<id>:<domain>")
    uint64_t session = 1;
    int ttl = 16;
    std::string source2;  // the second path's sender address (ST 2022-7)
};
// A stream's SDP; with a second destination `to2` (ST 2022-7), both as one session's two media descriptions grouped as
// duplicates (RFC 7104's "a=group:DUP"), the second from `o.source2`.
std::string videoSdp(const VideoFormat& f, const Endpoint& to, int payloadType, const SdpOptions& o, const Endpoint* to2 = nullptr);
std::string audioSdp(const AudioFormat& f, const Endpoint& to, int payloadType, const SdpOptions& o, const Endpoint* to2 = nullptr);
// (`ids`: the DID/SDID pairs it carries)
std::string ancSdp(const VideoFormat& f, const Endpoint& to, int payloadType, const SdpOptions& o,
                   const std::vector<std::pair<int, int>>& ids = {{0x60, 0x60}, {0x61, 0x01}}, const Endpoint* to2 = nullptr);
// ST 2110-41 fast metadata's stream (`types`: the data item types it carries, its fmtp's DIT).
std::string metadataSdp(const Endpoint& to, int payloadType, const SdpOptions& o, const std::vector<uint32_t>& types, const Endpoint* to2 = nullptr);
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

// A picture's samples as ST 2110-20's pixel groups order them (see Sampling), each its code at `depth` bits, rows in
// turn.
struct Raster {
    int width = 0, height = 0;
    Sampling sampling = Sampling::Ycc422;
    int depth = 10;
    std::vector<uint16_t> samples;
    Raster() = default;
    Raster(int w, int h, Sampling s, int d) : width(w), height(h), sampling(s), depth(d), samples(size_t(rowSamples()) * size_t(std::max(h, 0))) {}
    int rowSamples() const { return sampling == Sampling::Ycc422 ? width * 2 : width * 3; }
    uint16_t* row(int r) { return samples.data() + size_t(r) * size_t(rowSamples()); }
    const uint16_t* row(int r) const { return samples.data() + size_t(r) * size_t(rowSamples()); }
    bool operator==(const Raster& o) const = default;
};
// A pixel group: the fewest pixels whose samples fill whole bytes (RFC 4175 and ST 2110-20's table).
struct Pgroup {
    int pixels = 2, bytes = 5;
};
Pgroup pgroup(Sampling s, int depth);
// Whether ST 2110-20 carries a picture of this width in whole pixel groups at this sampling and depth.
bool videoFormatOk(const VideoFormat& f, std::string* why = nullptr);

// One picture (a frame, or with `field` 1 or 2 an interlaced frame's field: its rows of that parity) as ST 2110-20
// packets: `format` V210 or UYVY rows (the reference output's), `stride` bytes apart, cut into packets of at most
// `maxPayload` bytes of pixels (whole pixel groups, a row range each), the last marked.
using PacketSink = std::function<bool(const uint8_t* data, size_t size)>;
bool packVideo(const uint8_t* data, size_t stride, output::PixelFormat format, int width, int height, int field, uint32_t timestamp,
               RtpState& rtp, const PacketSink& send, int maxPayload = 1200);
// The same from a raster, at its sampling and depth.
bool packVideo(const Raster& picture, int field, uint32_t timestamp, RtpState& rtp, const PacketSink& send, int maxPayload = 1200);
// Packets a picture makes (for pacing).
int videoPacketCount(int width, int height, int depth, bool interlaced, int maxPayload = 1200, Sampling sampling = Sampling::Ycc422);
// Rebuilds pictures from ST 2110-20 packets (fields woven into their frame): into rows of V210 or UYVY, or a raster.
class VideoAssembler {
public:
    VideoAssembler(int width, int height, output::PixelFormat format, bool interlaced);
    VideoAssembler(int width, int height, Sampling sampling, int depth, bool interlaced);
    // A packet; true when it completed a frame (`frame()` or `raster()` then holds it, and `timestamp()` its first
    // field's).
    bool add(const RtpPacket& p);
    const std::vector<uint8_t>& frame() const { return done_; }
    const Raster& raster() const { return doneRaster_; }
    uint32_t timestamp() const { return doneTimestamp_; }
    int64_t lost() const { return lost_; }  // packets missing by their sequence numbers
    // Packets missing from the frame last completed (0: whole).
    int64_t lostInFrame() const { return doneLost_; }

private:
    int width_, height_;
    output::PixelFormat format_ = output::PixelFormat::V210;
    bool bytes_ = true;  // (rows of `format_`, else a raster)
    bool interlaced_;
    size_t stride_ = 0;
    Pgroup pg_;
    std::vector<uint8_t> done_;
    Raster building_, doneRaster_;
    uint32_t timestamp_ = 0, doneTimestamp_ = 0;
    bool started_ = false;
    int fieldsDone_ = 0;
    int64_t lost_ = 0, frameLost_ = 0, doneLost_ = 0;
    int64_t lastSeq_ = -1;
    void finish();
};

// Audio as L24 (24-bit big-endian, interleaved) or with `bits` 16 L16, one packet of `frames` samples.
bool packAudio(const float* samples, int frames, int channels, uint32_t timestamp, RtpState& rtp, const PacketSink& send, int bits = 24);
// The samples of an L24 (or L16) payload, as floats (exactly what was sent, to its bits).
std::vector<float> unpackAudio(const uint8_t* payload, size_t size, int channels, int bits = 24);

// ---- ST 2110-31: AES3 subframes as IEC 61883-6's AM824 words: a label byte above the subframe's 24 bits. The label
// (an IEC 60958 conformant subframe's): bit 5 the start of a channel status block (AES3's Z preamble), bit 4 the first
// subframe of a frame, bit 3 the subframe's parity, bits 2 to 0 its channel status (C), user (U) and validity (V) bits.
uint32_t am824Word(int32_t data24, bool blockStart, bool frameStart, bool c, bool u = false, bool v = false);
inline int32_t am824Data(uint32_t word) { return int32_t(word << 8) >> 8; }  // (the 24 bits, signed)
inline bool am824BlockStart(uint32_t word) { return (word >> 29) & 1; }
inline bool am824C(uint32_t word) { return (word >> 26) & 1; }
// Whether a word's parity bit gives even parity over its 24 bits and V, U, C and P (AES3's rule).
bool am824ParityOk(uint32_t word);
// Professional channel status: byte 0's bit 0 set, its bit 1 when the subframes carry something other than linear PCM
// (an ST 337 data burst, say), byte 23 the CRC (CRC-8/AES over bytes 0 to 22).
std::array<uint8_t, 24> aes3ChannelStatus(bool nonAudio);
uint8_t aes3Crc(const uint8_t* bytes, size_t n);
// A channel's status as its C bits carry it, from the first block start in `frames` frames of `channels` interleaved
// words; false when no whole block is there.
bool aes3ReadChannelStatus(const uint32_t* words, size_t frames, int channels, int channel, std::array<uint8_t, 24>& out);
// Linear PCM as AM824 words: 24 bits each, the channel status `status` sent block by block (`blockFrame`: the frame
// within the 192-frame block the next is, kept from one call to the next).
std::vector<uint32_t> aes3FromPcm(const float* samples, int frames, int channels, int& blockFrame, const std::array<uint8_t, 24>& status);
// One packet of `frames` frames of `channels` AM824 words, big-endian; and back.
bool packAm824(const uint32_t* words, int frames, int channels, uint32_t timestamp, RtpState& rtp, const PacketSink& send);
std::vector<uint32_t> unpackAm824(const uint8_t* payload, size_t size, int channels);

// ---- ST 2110-41 fast metadata: data items, each a 32-bit header (its type, 22 bits; the K bit; its length in 32-bit
// words, 9 bits) and its contents, as many as fit in a packet, the frame's last packet marked.
struct FastMetadata {
    uint32_t type = 0;  // (22 bits)
    bool k = false;
    std::vector<uint32_t> words;  // (at most 511)
    bool operator==(const FastMetadata& o) const = default;
};
bool packFastMetadata(const std::vector<FastMetadata>& items, uint32_t timestamp, RtpState& rtp, const PacketSink& send, int maxPayload = 1200);
// (false when an item runs past the payload)
bool unpackFastMetadata(const uint8_t* payload, size_t size, std::vector<FastMetadata>& out);

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
    // ST 2110-41 fast metadata (Sender::setNextMetadata), a packet each frame when on; the item types it names.
    bool sendMetadata = false;
    Endpoint metadata{"239.100.0.4", 5004}, metadata2{"239.101.0.4", 5004};
    int metadataPayload = 101;
    std::vector<uint32_t> metadataTypes;
    int maxPayload = 1200;  // bytes of pixels a packet
    std::string refclk = "ptp=IEEE1588-2008:traceable";
    // ST 2022-7: every packet also sent to these (from `interfaceAddress2`), when `redundant`.
    bool redundant = false;
    Endpoint video2{"239.101.0.1", 5004}, audio2{"239.101.0.2", 5004}, anc2{"239.101.0.3", 5004};
    std::string interfaceAddress2;
    // Each stream's (video, audio, ancillary data, fast metadata) paths sending: an NMOS controller may turn either of
    // a stream's two legs off.
    bool pathOn[4][2] = {{true, true}, {true, true}, {true, true}, {true, true}};
    // (for tests: a packet of stream 0 video, 1 audio, 2 ancillary data or 3 fast metadata, numbered from 0 as sent, left unsent on
    // path 0 or 1 when this says so)
    std::function<bool(int stream, int path, uint64_t index)> drop;
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
    // The same with the picture as a raster, at the video format's sampling and depth (4:4:4, RGB and 12 bits).
    bool send(const Raster& picture, const float* audio, int audioFrames, const output::Timecode* timecode,
              const std::vector<AncPacket>& anc, const std::atomic<bool>* stop = nullptr);
    // Ancillary packets sent with every frame from now (ST 2108-1's HDR metadata), and named in the SDP.
    void setFrameAnc(const std::vector<AncPacket>& anc) { frameAnc_ = anc; }
    // ST 2110-31: AES3 subframes as they are (AM824 words, interleaved), sent in place of `send`'s PCM (which an AES3
    // stream otherwise carries as linear PCM with professional channel status).
    void queueAes3(const uint32_t* words, int frames);
    // ST 2110-41: data items sent with the next frame.
    void setNextMetadata(std::vector<FastMetadata> items) { nextMetadata_ = std::move(items); }
    void close();
    bool isOpen() const;
    std::string sdpVideo() const, sdpAudio() const, sdpAnc() const, sdpMetadata() const;
    std::string clockSource() const { return clock_; }
    const SenderOptions& options() const { return o_; }
    const VideoFormat& videoFormat() const { return video_; }
    const AudioFormat& audioFormat() const { return audio_; }
    int64_t framesSent() const { return frames_; }
    // Frame slots passed over because `send` was called too late (over a fifth of a second behind): the frames after
    // go out from now, so their timestamps run that many frames ahead of their count.
    int64_t slotsSkipped() const { return skipped_; }
    int64_t packetsDropped() const;  // packets lost because the system's send queue stayed full
    // Packets path 0 or 1 could not send (its network down, say); with ST 2022-7 the other path goes on, and `send`
    // fails only when both do.
    int64_t sendErrors(int path) const;

private:
    bool sendFrame(const std::function<bool(int field, uint32_t ts, const PacketSink& keep)>& pack, const float* audio, int audioFrames,
                   const output::Timecode* timecode, const std::vector<AncPacket>& anc, const std::atomic<bool>* stop);
    struct Sockets;
    std::unique_ptr<Sockets> s_;
    SenderOptions o_;
    VideoFormat video_;
    AudioFormat audio_;
    RtpState rtpVideo_, rtpAudio_, rtpAnc_, rtpMeta_;
    int64_t nextFrame_ = 0;       // the frame number (PTP time / frame period) sent next
    int64_t audioSample_ = 0;     // the next audio packet's first sample (PTP time at the audio rate)
    std::vector<float> audioFifo_;
    std::vector<uint32_t> aesFifo_;  // (AM824 words: an AES3 stream's)
    int aesBlockFrame_ = 0;
    bool aesQueued_ = false;  // (queueAes3 used: send's PCM not sent)
    std::vector<AncPacket> frameAnc_;
    std::vector<FastMetadata> nextMetadata_;
    std::string clock_, source_, source2_;
    int64_t frames_ = 0;
    std::atomic<int64_t> skipped_{0};
};

// What an ST 2110 output (output::backends()'s "ST 2110") sends now: its streams' formats and where they go; false when
// `b` is another interface or is not open.
bool openStreams(output::Backend& b, VideoFormat& video, AudioFormat& audio, SenderOptions& options);

// Receives UDP datagrams on an endpoint (joining it when multicast), for tests and checking a stream.
class Receiver {
public:
    Receiver();
    ~Receiver();
    // (multicast from `source` alone when one is given and the network takes source-specific joins)
    bool open(const Endpoint& e, const std::string& interfaceAddress = {}, std::string* error = nullptr, const std::string& source = {});
    // The next datagram within `ms`; false when none came.
    bool receive(std::vector<uint8_t>& out, int ms);
    void close();
    int port() const;  // the port it is bound to (an endpoint's port 0 binds a free one)

private:
    struct Socket;
    std::unique_ptr<Socket> s_;
};

}  // namespace montage::st2110
