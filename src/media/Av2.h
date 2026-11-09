// Montage — AV2 read with VideoLAN's dav2d and written with AVM, AV2's reference encoder, both used directly (FFmpeg
// has no AV2): IVF files, bare OBU streams (AV2's low-overhead format, each OBU after its size) and MP4 as AV2's ISO
// BMFF binding has it while it is a working draft (the 'av02' sample entry and its 'av2C' box). 8 and 10 bits, 4:2:0,
// 4:2:2, 4:4:4 and monochrome in; 4:2:0 at 8 or 10 bits out; colour, PQ and HLG, HDR light levels and the mastering
// display read and written. Built only with -DMONTAGE_WITH_AV2=ON (both libraries' licences are flagged, so packages
// leave it out).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AVFrame;
struct AVPacket;
struct AVCodecParameters;

namespace montage {

bool av2Available();         // built with dav2d: AV2 is read
bool av2EncoderAvailable();  // built with AVM: AV2 is written
std::string av2Versions();   // "dav2d 0.0.1, AVM 1.0.0" (what is built in)

enum class Av2Container { None, Ivf, Obu, Mp4 };
// What holds AV2 at `file`, from its first bytes (an IVF of fourcc AV02, an MP4 with an AV2 sample entry, or OBUs
// starting with a temporal delimiter and a sequence header); None for anything else, AV1 included.
Av2Container av2Container(const std::string& file);
// The container a file name asks for: .ivf, .obu, else MP4 (.mp4, .m4v).
Av2Container av2ContainerForPath(const std::string& file);

struct Av2Info {
    Av2Container container = Av2Container::None;
    int width = 0, height = 0;  // the shown picture
    int bits = 8;               // 8, 10 or 12
    int chroma = 0;             // AV2's chroma_format_idc: 0 4:2:0, 1 monochrome, 2 4:4:4, 3 4:2:2
    int profile = 0, level = 0, tier = 0;
    bool monotonic = true;      // each temporal unit outputs its own frame, in decoding order
    // Colour (CICP code points), from the container ('colr') or the stream's content interpretation; 2 unspecified.
    int primaries = 2, transfer = 2, matrix = 2;
    bool fullRange = false;
    int maxCll = 0, maxFall = 0;  // nits, 0 = not stated
    // The mastering display (stated when masteringMax > 0): chromaticities as x, y (R, G, B, white) and luminances.
    double mastering[8] = {};
    double masteringMax = 0, masteringMin = 0;
    int fpsNum = 25, fpsDen = 1;  // the container's rate (a bare OBU stream: its timing info, else 25)
    int64_t frames = 0;           // frames shown
    double seconds() const { return fpsNum > 0 ? double(frames) * fpsDen / fpsNum : 0; }
    std::string colorSpace;       // the Montage space (render/ColorSpace.h id) its tags name ("" = Rec.709 or unknown)
};
bool readAv2Info(const std::string& file, Av2Info& out, std::string* error = nullptr);

// Frames of an AV2 file, decoded with dav2d: frame `n` (in showing order) into `into` as FFmpeg would hand it out
// (planar Y′CbCr or grey at 8, 10 or 12 bits, colour tagged). Frames after the last one asked for are decoded on from
// there; others from the closest key frame before them.
class Av2Reader {
public:
    Av2Reader();
    ~Av2Reader();
    Av2Reader(const Av2Reader&) = delete;
    Av2Reader& operator=(const Av2Reader&) = delete;
    bool open(const std::string& file, std::string* error = nullptr);
    const Av2Info& info() const;
    bool frame(int64_t n, AVFrame* into, std::string* error = nullptr);
    // Each temporal unit as a decoder takes it (a temporal delimiter, then, in MP4, the 'av2C' box's OBUs before each
    // key frame's, then the sample's OBUs), and whether it can be decoded from (a closed-loop key frame).
    int64_t temporalUnits() const;
    bool temporalUnit(int64_t i, std::vector<uint8_t>& out, bool* sync = nullptr, std::string* error = nullptr);

private:
    struct State;
    std::unique_ptr<State> s_;
};

struct Av2WriteSettings {
    Av2Container container = Av2Container::Mp4;
    int width = 0, height = 0;
    int bits = 8;                 // 8 or 10 (4:2:0)
    int fpsNum = 25, fpsDen = 1;
    int speed = 6;                // AVM's speed preset, 0 (slowest, best) to 6 (fastest)
    int qp = 0;                   // AVM's quantiser (0 = from crf: 0-63, mapped onto AVM's 0-255)
    int crf = 32;
    int64_t bitrate = 0;          // bits/s (variable rate); 0 = constant quality
    int gop = 0;                  // the most frames between key frames; 0 = two seconds
    int primaries = 1, transfer = 1, matrix = 1;  // CICP
    bool fullRange = false;
    int maxCll = 0, maxFall = 0;  // HDR light levels (nits; 0 = not stated)
    double masteringMax = 0, masteringMin = 0;    // the mastering display's luminances (nits; 0 = not stated), P3-D65
    int threads = 0;              // 0 = one a core, up to 8
};

// Writes AV2: frames given in showing order (planar 4:2:0, 8-bit or 10-bit little-endian, at the settings' size),
// encoded by AVM and written as they come. An MP4 can hold one audio stream beside the video (AAC, its packets
// handed over as an encoder made them). On a failure (or when destroyed unfinished) the file is removed.
class Av2Writer {
public:
    Av2Writer();
    ~Av2Writer();
    Av2Writer(const Av2Writer&) = delete;
    Av2Writer& operator=(const Av2Writer&) = delete;
    bool open(const std::string& path, const Av2WriteSettings& settings, std::string* error = nullptr);
    // (MP4, before the first frame) An audio stream: its parameters (AAC, with its AudioSpecificConfig) and the time
    // base its packets' times are in.
    bool addAudio(const AVCodecParameters* par, int timeBaseNum, int timeBaseDen, std::string* error = nullptr);
    bool encode(const AVFrame* frame, std::string* error = nullptr);
    bool audio(const AVPacket* packet, std::string* error = nullptr);
    // (MP4, before finish) The light levels its sample entry states, as measured once the pictures are coded (an IVF
    // or OBU stream keeps the settings' ones, stated with each key frame).
    void setLightLevels(int maxCll, int maxFall);
    bool finish(std::string* error = nullptr);
    int64_t framesWritten() const;
    const std::string& path() const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

}  // namespace montage
