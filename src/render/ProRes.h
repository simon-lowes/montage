// Montage — ProRes pictures one at a time (SMPTE RDD 36 frames, through FFmpeg's encoder and decoder), for track files
// that wrap them as SMPTE RDD 44 maps them: IMF Application ProRes (RDD 45, render/Imf.h). A picture goes in as full-range
// R'G'B' and is coded as narrow-range Y'CbCr (4:2:2 or 4:4:4 by the profile) through the colour's matrix, with the colour
// codes RDD 45 Table 3 gives in the frame header; decoding gives full-range R'G'B' back.
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

class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    bool open(int width, int height, int profile, const std::string& imfColour, std::string* error = nullptr);
    // One picture: interleaved R'G'B', 16 bits a sample, full range.
    bool encode(const uint16_t* rgb, std::vector<uint8_t>& out, std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

// A frame decoded to interleaved full-range R'G'B' of 16 bits a sample (from its Y'CbCr, by the matrix and range its
// header gives).
bool decode(const uint8_t* data, size_t size, std::vector<uint16_t>& rgb, int& width, int& height, std::string* error = nullptr);

}  // namespace montage::prores
