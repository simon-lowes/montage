// Montage — ProRes pictures one at a time (SMPTE RDD 36 frames, through FFmpeg's encoder and decoder), for track files
// that wrap them as SMPTE RDD 44 maps them: IMF Application ProRes (RDD 45) and Application DPP (RDD 59-1, render/Imf.h).
// A picture goes in as full-range R'G'B' and is coded as Y'CbCr (4:2:2 or 4:4:4 by the profile) through the colour's
// matrix, narrow range (QE.1) or full range (QE.2: the decoded R'G'B' from 0 to the top code), as one progressive frame
// or as two fields of it (the top or the bottom one first), with the colour codes RDD 45 Table 3 gives in the frame
// header; 4444 and 4444 XQ can carry 16-bit alpha. Decoding gives full-range R'G'B' (and alpha) back, 4:4:4 at the 12
// bits FFmpeg decodes it at.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace montage::prores {

// The profiles, numbered as RDD 44's coding labels number them (byte 15): 1 422 Proxy, 2 422 LT, 3 422, 4 422 HQ,
// 5 4444, 6 4444 XQ. Names: "proxy", "lt", "422", "hq", "4444", "4444xq".
int profileOf(const std::string& name);  // 0 when unknown
std::string profileName(int profile);
std::string profileTitle(int profile);  // "ProRes 422 HQ"
inline bool fourFourFour(int profile) { return profile >= 5; }

// A frame's header (RDD 36 frame() and frame_header()).
struct FrameHeader {
    uint32_t frameSize = 0;
    int width = 0, height = 0;
    int chroma = 0;     // 2 4:2:2, 3 4:4:4
    int interlace = 0;  // 0 progressive
    int primaries = 0, transfer = 0, matrix = 0;
    int alpha = 0;      // alpha_channel_type
};
bool parseFrameHeader(const uint8_t* data, size_t size, FrameHeader& out);

// RDD 45 Table 3's colour codes for an IMF colour ("rec709" COLOR.3, "p3d65-pq" COLOR.6, "rec2020-pq" COLOR.7,
// "rec2020-hlg" COLOR.8).
struct ColourCodes {
    int primaries = 0, transfer = 0, matrix = 0;
};
bool colourCodes(const std::string& imfColour, ColourCodes& out);

bool encoderAvailable();

// How pictures are coded beyond their profile and colour.
struct Coding {
    int fields = 0;          // 0 progressive, 1 two fields with the top first (interlace_mode 1), 2 the bottom first (2)
    bool fullRange = false;  // QE.2: Y'CbCr over the whole range, so the decoded R'G'B' is too
    bool alpha = false;      // 4444 and 4444 XQ: a 16-bit alpha channel (alpha_channel_type 2)
};

class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    bool open(int width, int height, int profile, const std::string& imfColour, std::string* error = nullptr) {
        return open(width, height, profile, imfColour, Coding{}, error);
    }
    // Interlaced pictures need an even height; alpha needs a 4:4:4 profile.
    bool open(int width, int height, int profile, const std::string& imfColour, const Coding& coding, std::string* error = nullptr);
    // One picture: interleaved R'G'B', 16 bits a sample, full range; with alpha, `alpha` is its plane (16 bits, straight,
    // width x height; null: opaque). Coded as two fields, each field is the picture's lines of its parity.
    bool encode(const uint16_t* rgb, std::vector<uint8_t>& out, std::string* error = nullptr) { return encode(rgb, nullptr, out, error); }
    bool encode(const uint16_t* rgb, const uint16_t* alpha, std::vector<uint8_t>& out, std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// A frame decoded to interleaved full-range R'G'B' of 16 bits a sample (from its Y'CbCr, by the matrix its header gives;
// narrow range unless `fullRange`, which the frame does not say: the track file's descriptor does). 4:4:4 frames are
// decoded at 12 bits, 4:2:2 at 10. With `alpha`, a frame with alpha gives its plane (16 bits; empty when it has none).
bool decode(const uint8_t* data, size_t size, std::vector<uint16_t>& rgb, int& width, int& height, std::string* error = nullptr);
bool decode(const uint8_t* data, size_t size, bool fullRange, std::vector<uint16_t>& rgb, std::vector<uint16_t>* alpha, int& width, int& height,
            std::string* error = nullptr);

}  // namespace montage::prores
