// Montage — Open Media Transport (OMT): live video, sound and metadata over the network, as vMix and OBS send and
// receive it. Written from the protocol as libomtnet (MIT, Open Media Transport Contributors) implements it: a sender
// listens on a TCP port from 6400 and is found by DNS-SD (_omt._tcp, as "HOST (Name)") or through an OMT discovery
// server; a receiver connects, subscribes to video, sound and metadata, and gets frames of a 16-byte little-endian
// header, an extended header (video's size, rate, flags and colour space; sound's rate, samples and channels), the
// data and XML metadata. Pictures are VMX, the visually lossless intra-frame codec OMT sends (libvmx, MIT), 8-bit or
// 16-bit 4:2:2; sound is 32-bit float, one channel after another, silent channels left out.
//
// Montage adds per-frame metadata other receivers pass over: the frame's timecode and NDI's colour element (transfer,
// matrix and primaries; OMT's own header has only BT.601 or BT.709).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace montage::omt {

constexpr int kPortStart = 6400, kPortEnd = 6600, kDiscoveryPort = 6399;
constexpr const char* kServiceType = "_omt._tcp";

enum FrameType : uint8_t { Metadata = 1, Video = 2, Audio = 4 };
enum VideoFlags : int32_t { Interlaced = 1, Alpha = 2, PreMultiplied = 4, Preview = 8, HighBitDepth = 16 };

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}
constexpr uint32_t kVmx1 = fourcc('V', 'M', 'X', '1'), kFpa1 = fourcc('F', 'P', 'A', '1');

// The 16-byte header every frame starts with.
struct Header {
    uint8_t version = 1;
    uint8_t type = Metadata;
    int64_t timestamp = 0;        // 100 ns units (10,000,000 a second)
    uint16_t metadataLength = 0;  // the per-frame XML at the end of the data, with its NUL
    int32_t dataLength = 0;       // everything after this header: extended header, data and metadata
};
constexpr int kHeaderBytes = 16, kVideoHeaderBytes = 32, kAudioHeaderBytes = 24;
// The largest picture VMX codes (and so OMT carries), and the sound a frame can hold.
constexpr int kMaxWidth = 7680, kMaxHeight = 4320, kMaxSamples = 65536, kMaxChannels = 32;
void writeHeader(const Header& h, uint8_t* out);
bool readHeader(const uint8_t* in, Header& h);  // false: not version 1 or an unknown type

struct VideoHeader {
    uint32_t codec = kVmx1;
    int32_t width = 0, height = 0;
    int32_t rateN = 25, rateD = 1;
    float aspect = 16.0f / 9.0f;  // display aspect ratio
    int32_t flags = 0;
    int32_t colorSpace = 709;  // 601, 709 or 0 (undefined: 601 below 720 lines, else 709)
};
void writeVideoHeader(const VideoHeader& v, uint8_t* out);
VideoHeader readVideoHeader(const uint8_t* in);

struct AudioHeader {
    uint32_t codec = kFpa1;
    int32_t sampleRate = 48000, samplesPerChannel = 0, channels = 0;
    uint32_t activeChannels = 0;  // a bit for each channel whose samples are in the data (silent ones are left out)
    int32_t reserved = 0;
};
void writeAudioHeader(const AudioHeader& a, uint8_t* out);
AudioHeader readAudioHeader(const uint8_t* in);
// Whether a received frame's extended header and data make sense (a picture VMX can code; sound whose data holds
// every active channel's samples, at a usual rate).
bool plausible(const VideoHeader& v);
bool plausible(const AudioHeader& a, size_t dataBytes);

// The commands receivers send (exact strings: they are matched, not parsed; the tally's "==" is the protocol's).
std::string subscribeCommand(FrameType type);
std::string tallyCommand(bool preview, bool program);
std::string previewCommand(bool on);
enum class Quality { Default, Low, Medium, High };
std::string qualityCommand(Quality q);

// FPA1: `channels` planes of `samples` floats, the silent ones left out (`active` marks those kept); and back.
std::vector<uint8_t> encodeFpa1(const float* planar, int channels, int samples, uint32_t* active);
// Into `channels` planes of `samples` (zeros for those left out); false when the data is short.
bool decodeFpa1(const uint8_t* data, size_t size, int channels, int samples, uint32_t active, float* planar);

// The machine's name as OMT gives it (upper case), and a source's full name: "HOST (Name)", 63 characters at most.
std::string machineName();
std::string fullName(const std::string& name);

// ---------------------------------------------------------------------------
// VMX

enum class PictureFormat { Uyvy, P216 };  // 8-bit UYVY, or 16-bit 4:2:2 (a Y plane, then interleaved Cb Cr)
size_t pictureBytes(PictureFormat f, int width, int height);

// VMX's profiles OMT uses: Low, Medium (the default) and High.
int vmxProfile(Quality q);
// libvmx runs on this processor (its x86 code needs LZCNT and BMI1, any x86 processor since 2013); else why not.
bool vmxAvailable(std::string* why = nullptr);

class VmxEncoder {
public:
    VmxEncoder() = default;
    ~VmxEncoder();
    VmxEncoder(const VmxEncoder&) = delete;
    VmxEncoder& operator=(const VmxEncoder&) = delete;
    bool open(int width, int height, int profile, int colorSpace, double fps);
    void close();
    bool isOpen() const { return inst_ != nullptr; }
    int profile() const { return profile_; }
    int width() const { return width_; }
    int height() const { return height_; }
    // One picture, `stride` bytes a row (P216: both planes'); its encoded bytes appended to `out`.
    bool encode(PictureFormat f, const uint8_t* data, int stride, bool interlaced, std::vector<uint8_t>& out);
    // The bytes of the last picture that make its preview (an eighth of its size), from its start.
    int previewLength() const;

private:
    void* inst_ = nullptr;
    int width_ = 0, height_ = 0, profile_ = 0;
};

class VmxDecoder {
public:
    VmxDecoder() = default;
    ~VmxDecoder();
    VmxDecoder(const VmxDecoder&) = delete;
    VmxDecoder& operator=(const VmxDecoder&) = delete;
    // A picture of `width` x `height` into `out` (`stride` bytes a row; P216: both planes').
    bool decode(const uint8_t* data, size_t size, int width, int height, int colorSpace, PictureFormat f, uint8_t* out, int stride);

private:
    void* inst_ = nullptr;
    int width_ = 0, height_ = 0, colorSpace_ = 0;
};

// ---------------------------------------------------------------------------
// Sending

struct VideoOut {
    PictureFormat format = PictureFormat::P216;
    int width = 0, height = 0, stride = 0;
    const uint8_t* data = nullptr;
    int rateN = 25, rateD = 1;
    bool interlaced = false;
    int colorSpace = 709;
    int64_t timestamp = 0;  // 100 ns
    std::string metadata;   // per-frame XML ("" none)
};

// A source on the network: it listens on the first free port from 6400, is announced by DNS-SD (or to the discovery
// server set), and sends what it is given to every receiver subscribed to it (a receiver that falls four frames behind
// misses frames, as OMT does). Pictures are encoded once, at the best quality any receiver asks for.
class Sender {
public:
    Sender();
    ~Sender();
    Sender(const Sender&) = delete;
    Sender& operator=(const Sender&) = delete;

    struct Options {
        std::string name = "Montage";
        int portStart = kPortStart, portEnd = kPortEnd;
        bool announce = true;  // by DNS-SD or the discovery server
        std::string product = "Montage", manufacturer = "Montage", version;
    };
    bool start(const Options& o, std::string* error);
    void stop();
    bool running() const;
    int port() const;
    std::string fullName() const;        // "HOST (Name)"
    std::string announcement() const;    // how it is announced ("DNS-SD", "discovery server host:6399", "") and why not
    // Video goes to receivers subscribed to it; false when it could not be encoded.
    bool sendVideo(const VideoOut& v);
    // `channels` planes of `samples` (`stride` floats apart).
    bool sendAudio(const float* planar, int channels, int samples, int stride, int rate, int64_t timestamp);
    bool sendMetadata(const std::string& xml, int64_t timestamp = 0);
    int connections() const;  // receivers subscribed to video or sound
    // The receivers' tally, all of them taken together.
    bool tallyProgram() const;
    bool tallyPreview() const;
    int64_t framesDropped() const;

    struct Channel;
    struct State;

private:
    std::shared_ptr<State> s_;
};

// ---------------------------------------------------------------------------
// Receiving

// A frame as it came: its header, its extended header, the data (still VMX for video) and its metadata.
struct Frame {
    Header header;
    VideoHeader video;
    AudioHeader audio;
    std::vector<uint8_t> data;  // without the metadata
    std::string metadata;
};

// A source to receive: `omt://host:port`, or a source's full name ("HOST (Name)") found by DNS-SD or the discovery
// server. Video (with metadata) and sound come on connections of their own, as OMT's receivers do; when one drops it
// is made again each second until closed.
class Receiver {
public:
    Receiver();
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    struct Options {
        bool video = true, audio = true, metadata = true;
        bool preview = false;
        Quality quality = Quality::Default;
        int queue = 16;  // frames held for `next` (the oldest are kept; later ones dropped)
        double connectTimeout = 5;
    };
    bool open(const std::string& source, const Options& o, std::string* error);
    void close();
    // The next frame (video, audio or metadata), waiting up to `ms`; false when none came.
    bool next(Frame& out, int ms);
    bool connected() const;
    std::string address() const;  // host:port connected to
    std::string senderInfo() const;  // the sender's <OMTInfo .../>
    void setTally(bool preview, bool program);
    int64_t framesDropped() const;

    struct State;

private:
    std::shared_ptr<State> s_;
};

// The value of attribute `name` in the first element of `xml` that has it ("" none).
std::string xmlAttribute(const std::string& xml, const std::string& name);

// ---------------------------------------------------------------------------
// Discovery

struct Source {
    std::string name;  // "HOST (Name)"
    std::string host;  // a name or an address to connect to
    int port = 0;
    std::string via;   // "DNS-SD" or "discovery server"
};
// The sources announced on the network, listened for for `seconds`.
std::vector<Source> discover(double seconds = 1.5, std::string* why = nullptr);
// `source` (omt://host:port, or a full name) as host and port; false and why when it cannot be found.
bool resolve(const std::string& source, std::string& host, int& port, std::string* error, double seconds = 2);
// The discovery server set: MONTAGE_OMT_DISCOVERY_SERVER, else ~/.OMT/settings.xml's DiscoveryServer (as libomtnet
// reads it); "" for none (DNS-SD).
std::string discoveryServer();
// The XML a source is registered with on a discovery server (or taken away with, `removed`).
std::string addressXml(const std::string& name, int port, const std::vector<std::string>& addresses, bool removed);
bool parseAddressXml(const std::string& xml, Source& out, bool& removed);
// DNS-SD on this computer: Avahi (Linux, when its daemon runs), the system's (macOS) or Windows' own (10 and later).
bool dnsSdAvailable(std::string* why = nullptr);

// Announces a source until destroyed.
class Announcement {
public:
    virtual ~Announcement() = default;
    virtual std::string how() const = 0;
};
std::unique_ptr<Announcement> announce(const std::string& fullName, int port, std::string* why);

}  // namespace montage::omt
