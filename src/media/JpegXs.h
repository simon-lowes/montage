// Montage — JPEG XS (ISO/IEC 21122), coded and decoded through SVT-JPEG-XS (BSD-2-Clause-Patent), used directly as
// libjxl and dav2d are (the FFmpeg Montage builds with has no JPEG XS):
//  - pictures: 4:2:2 and 4:4:4 Y′CbCr and R′G′B′, 8 to 12 bits, at a chosen number of bits per pixel (a constant
//    number of bytes a picture), slices of 16 lines, each slice's bytes known (RFC 9134's slice packetisation);
//  - files: raw codestreams (.jxs: one picture, or a sequence of them one after the other) and MXF (SMPTE ST 2124,
//    frame-wrapped in OP1a through bmx, with the JPEG XS Picture SubDescriptor and PCM sound beside it), read and
//    written;
//  - a codestream's header read without the library (its profile, level, size, components and slice height), for
//    MXF's descriptor, SDP and NMOS.
// Built when SVT-JPEG-XS is found (SVTJPEGXS_ROOT, as scripts/build-svt-jpegxs.sh builds it, or pkg-config); without
// it, jpegXsAvailable() says so and nothing is coded.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct AVFrame;

namespace montage {

bool jpegXsAvailable();      // built with SVT-JPEG-XS
std::string jpegXsVersion(); // "SVT-JPEG-XS 0.10.0" ("" without it)

// ---- A codestream's header (its markers up to the first slice), as ISO/IEC 21122-1 lays them out.
struct JxsHeader {
    uint32_t lcod = 0;           // the codestream's bytes (PIH's Lcod; 0: not stated, a variable rate)
    uint16_t ppih = 0, plev = 0; // profile; level and sublevel
    int width = 0, height = 0;   // Wf, Hf
    int cw = 0;                  // precinct width (0: the picture's)
    int hsl = 0;                 // a slice's height in precincts
    int components = 0;          // Nc
    int depth = 0;               // the first component's bits (Bc)
    int sx[4] = {}, sy[4] = {};  // each component's sampling factors
    int cpih = 0;                // colour transform (1: the reversible one, so R′G′B′ coded)
    int decompV = 0;             // vertical decompositions (Nly): a precinct is 2^decompV lines
    std::vector<uint8_t> componentTable;  // the CDT marker segment as it is (ST 2124's JPEGXSComponentTable)
    size_t headerBytes = 0;      // bytes before the first slice (SOC to the first SLH)
    int sliceLines() const { return hsl << decompV; }
};
// False when `data` does not start with a JPEG XS codestream's header.
bool readJxsHeader(const uint8_t* data, size_t size, JxsHeader& out);
// ISO/IEC 21122-2's names: "Main 422.10", and the level and sublevel ("2k-1", "Sublev3bpp"); "" when not one of them.
std::string jxsProfileName(uint16_t ppih);
std::string jxsLevelName(uint16_t plev);
std::string jxsSublevelName(uint16_t plev);
// The bytes of the codestream `data` starts with (its Lcod, else found by reading its slices); 0 when it is not one or
// runs past `size`.
size_t jxsCodestreamSize(const uint8_t* data, size_t size);

// ---- Pictures.
enum class JxsSampling { Ycc422, Ycc444, Rgb };
std::string jxsSamplingName(JxsSampling s);  // as RFC 9134's SDP names it ("YCbCr-4:2:2", "YCbCr-4:4:4", "RGB")
// Planar samples: Y, Cb, Cr (or R, G, B), each its code (8 to 12 bits) in 16 bits, rows in turn; chroma half as wide
// in 4:2:2.
struct JxsPicture {
    int width = 0, height = 0, depth = 10;
    JxsSampling sampling = JxsSampling::Ycc422;
    std::vector<uint16_t> planes[3];
    JxsPicture() = default;
    JxsPicture(int w, int h, int d, JxsSampling s);
    int planeWidth(int c) const { return c && sampling == JxsSampling::Ycc422 ? (width + 1) / 2 : width; }
    bool operator==(const JxsPicture& o) const;
};

struct JxsEncodeSettings {
    int width = 1920, height = 1080;
    int depth = 10;                            // 8 to 12
    JxsSampling sampling = JxsSampling::Ycc422;
    int bppNum = 3, bppDen = 1;                // bits per pixel (as a fraction): bytes a picture = width x height x bpp / 8
    int sliceHeight = 16;                      // lines a slice
    bool slices = false;                       // say where each slice ends (RFC 9134's slice packetisation)
    int threads = 0;                           // 0: as SVT chooses
};
// Codes pictures at a constant number of bytes each.
class JxsEncoder {
public:
    JxsEncoder();
    ~JxsEncoder();
    JxsEncoder(const JxsEncoder&) = delete;
    JxsEncoder& operator=(const JxsEncoder&) = delete;
    bool open(const JxsEncodeSettings& s, std::string* error = nullptr);
    const JxsEncodeSettings& settings() const;
    uint32_t bytesPerPicture() const;
    // One picture (at the settings' size, sampling and depth) as a codestream; with `slices`, `units` gets the bytes of
    // each part RFC 9134's slice packetisation sends apart: the header, then each slice (the last with the EOC marker).
    bool encode(const JxsPicture& picture, std::vector<uint8_t>& out, std::vector<uint32_t>* units = nullptr,
                std::string* error = nullptr);
    void close();

private:
    struct State;
    std::unique_ptr<State> s_;
};

// Decodes codestreams (each whole); a codestream of another size or format starts the decoder again.
class JxsDecoder {
public:
    JxsDecoder();
    ~JxsDecoder();
    JxsDecoder(const JxsDecoder&) = delete;
    JxsDecoder& operator=(const JxsDecoder&) = delete;
    // `sampling`: what a 4:4:4 codestream without the colour transform holds (it does not say whether that is Y′CbCr or
    // R′G′B′: the container or the SDP does).
    bool decode(const uint8_t* data, size_t size, JxsPicture& out, std::string* error = nullptr,
                JxsSampling sampling444 = JxsSampling::Ycc444);
    void close();

private:
    struct State;
    std::unique_ptr<State> s_;
};

// ---- Files.
enum class JxsContainer { None, Codestream, Mxf };
// What holds JPEG XS at `file` (a raw codestream from its first bytes, an MXF from its essence container label).
JxsContainer jxsContainer(const std::string& file);
// The container a file name asks for: .mxf, else a raw codestream.
JxsContainer jxsContainerForPath(const std::string& file);

struct JxsFileInfo {
    JxsContainer container = JxsContainer::None;
    JxsHeader header;            // the first picture's
    JxsSampling sampling = JxsSampling::Ycc422;
    int fpsNum = 25, fpsDen = 1; // MXF's edit rate (a raw codestream: 25, or one picture alone a still)
    int64_t frames = 0;
    // Colour (CICP code points): MXF's descriptor, else Rec.709 Y′CbCr in narrow range (R′G′B′ full range).
    int primaries = 1, transfer = 1, matrix = 1;
    bool fullRange = false;
    std::string colorSpace;      // the Montage space (render/ColorSpace.h id) its tags name ("" = Rec.709)
    // MXF: its sound (PCM, read through FFmpeg as any MXF's), its start timecode ("" none) and JPEG XS Picture
    // SubDescriptor (whether there is one, and its values).
    int soundTracks = 0;
    std::string timecode;
    bool subDescriptor = false;
    uint16_t subPpih = 0, subPlev = 0;
    int subWf = 0, subHf = 0, subNc = 0, subCw = 0, subHsl = 0;
    uint32_t subMaxBitRate = 0;
    std::vector<uint8_t> subComponentTable;
    std::string essenceContainer;  // its label, as "urn:smpte:ul:..."
    std::string pictureCoding;     // the descriptor's picture essence coding label
    int horizontalSubsampling = 0, verticalSubsampling = 0, componentDepth = 0;
    double seconds() const { return fpsNum > 0 ? double(frames) * fpsDen / fpsNum : 0; }
};
bool readJxsFileInfo(const std::string& file, JxsFileInfo& out, std::string* error = nullptr);

// Pictures of a JPEG XS file: frame `n` as FFmpeg would hand it out (planar Y′CbCr or GBR at 8, 10 or 12 bits,
// colour tagged), or its codestream as it is.
class JxsReader {
public:
    JxsReader();
    ~JxsReader();
    JxsReader(const JxsReader&) = delete;
    JxsReader& operator=(const JxsReader&) = delete;
    bool open(const std::string& file, std::string* error = nullptr);
    const JxsFileInfo& info() const;
    bool codestream(int64_t n, std::vector<uint8_t>& out, std::string* error = nullptr);
    bool picture(int64_t n, JxsPicture& out, std::string* error = nullptr);
    bool frame(int64_t n, AVFrame* into, std::string* error = nullptr);

private:
    struct State;
    std::unique_ptr<State> s_;
};

struct JxsWriteSettings {
    JxsContainer container = JxsContainer::Mxf;
    JxsEncodeSettings encode;
    int fpsNum = 25, fpsDen = 1;
    int primaries = 1, transfer = 1, matrix = 1;  // CICP (MXF's descriptor)
    bool fullRange = false;
    int channels = 0, sampleRate = 48000, bits = 24;  // MXF's sound: a mono PCM track a channel
    std::string timecode = "00:00:00:00";          // MXF's start
    bool dropFrame = false;
    std::string clipName;
};
// Writes JPEG XS: pictures given in order, coded and written as they come (a raw codestream file, or MXF OP1a with
// the sound beside them). On a failure (or when destroyed unfinished) the file is removed.
class JxsWriter {
public:
    JxsWriter();
    ~JxsWriter();
    JxsWriter(const JxsWriter&) = delete;
    JxsWriter& operator=(const JxsWriter&) = delete;
    bool open(const std::string& path, const JxsWriteSettings& settings, std::string* error = nullptr);
    bool write(const JxsPicture& picture, std::string* error = nullptr);
    // A frame as FFmpeg holds it (planar Y′CbCr 4:2:2 or 4:4:4, or GBR, at the settings' depth; other formats converted).
    bool write(const AVFrame* frame, std::string* error = nullptr);
    // A codestream already coded (written as it is).
    bool writeCodestream(const uint8_t* data, size_t size, std::string* error = nullptr);
    // Sound for MXF: interleaved samples (any number: written with the pictures, a frame's worth each).
    bool audio(const float* samples, int frames, std::string* error = nullptr);
    bool finish(std::string* error = nullptr);
    int64_t framesWritten() const;
    const std::string& path() const;

private:
    struct State;
    std::unique_ptr<State> s_;
};

}  // namespace montage
