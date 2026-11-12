// Montage — ProRes frames coded and decoded by Montage itself, from SMPTE RDD 36 (the ProRes bitstream): 12-bit Y'CbCr
// in, 4:4:4 for 4444 and 4444 XQ (4:2:2 for the 422 profiles), with 16-bit alpha where it is wanted, progressive or as
// two fields, sliced eight macroblocks wide and coded slice by slice on several threads, each frame kept within its
// profile's target size. (FFmpeg's encoder, which codes 10 bits, makes Montage's 10-bit ProRes: render/ProRes.h.)
//
// The coefficients are those of the orthonormal 8 x 8 DCT of the samples less half their range (2048 at 12 bits), each
// divided by the frame's quantization matrix entry and the slice's scale and rounded; the DC values are coded as
// differences between neighbouring blocks, the rest as runs and levels across every block of the slice in scan order,
// each with the adaptive Rice and exponential Golomb codes RDD 36 gives. Every frame is coded on its own: the same
// picture always gives the same bytes, on any number of threads.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace montage::prores {

// One picture for Montage's encoder: Y', Cb and Cr as 12-bit codes (0 to 4095) in 16-bit samples, the colour
// differences full width for 4444 and 4444 XQ and half width (rounded up) for the 422 profiles; and, when the encoder
// takes alpha, its plane (16 bits, straight; null: opaque). Strides in samples.
struct PlanarPicture {
    const uint16_t* y = nullptr;
    const uint16_t* cb = nullptr;
    const uint16_t* cr = nullptr;
    const uint16_t* alpha = nullptr;
    std::ptrdiff_t yStride = 0, cStride = 0, aStride = 0;
};

struct OwnSettings {
    int width = 0, height = 0;  // the frame's (interlaced: both fields)
    int profile = 5;            // 1 422 Proxy ... 4 422 HQ, 5 4444, 6 4444 XQ (as render/ProRes.h numbers them)
    int fields = 0;             // 0 progressive, 1 two fields with the top first, 2 the bottom first
    int primaries = 2, transfer = 2, matrix = 2;  // the frame header's colour codes (2: unspecified)
    bool alpha = false;         // a 16-bit alpha channel (4444 and 4444 XQ)
    int frameRateCode = 0;      // RDD 36's frame_rate_code (frameRateCode below; 0 unknown)
    int aspectCode = 0;         // aspect_ratio_information: 0 unknown, 1 square pixels, 2 4:3, 3 16:9
    size_t targetBytes = 0;     // the most a frame's headers and colour take (alpha, lossless, comes on top); 0: the profile's
    int threads = 0;            // 0: one a core
};

// The size a profile's frames are held to at this picture size: the data rates published for each profile at 1920 x 1080
// and 29.97 fps (45, 102, 147, 220, 330 and 500 Mbit/s from Proxy to 4444 XQ, without alpha), as bits a pixel of a
// frame, so the same at every frame rate.
size_t targetFrameBytes(int profile, int width, int height);
// frame_rate_code for a rate (23.976 1, 24 2, 25 3, 29.97 4, 30 5, 50 6, 59.94 7, 60 8, 100 9, 119.88 10, 120 11; 0 other).
int frameRateCode(double fps);

class OwnEncoder {
public:
    OwnEncoder();
    ~OwnEncoder();
    OwnEncoder(const OwnEncoder&) = delete;
    OwnEncoder& operator=(const OwnEncoder&) = delete;
    // Up to 8192 x 8192 (interlaced: an even height); alpha with 4444 and 4444 XQ only.
    bool open(const OwnSettings& settings, std::string* error = nullptr);
    bool encode(const PlanarPicture& picture, std::vector<uint8_t>& out, std::string* error = nullptr);
    const OwnSettings& settings() const { return s_; }
    // The last frame's: its slices' quantization indexes (1 the finest, 224 the coarsest), the bytes its alpha took.
    const std::vector<uint8_t>& lastQuantization() const { return quant_; }
    size_t lastAlphaBytes() const { return alphaBytes_; }

private:
    OwnSettings s_;
    bool open_ = false;
    std::vector<uint8_t> quant_;
    size_t alphaBytes_ = 0;
};

// A frame decoded by Montage's own decoder: Y', Cb and Cr as 12-bit codes (the colour differences full width for
// 4:4:4, half width rounded up for 4:2:2; 4 to 4091) and, when the frame has it, alpha (16 bits; an 8-bit alpha widened).
struct OwnDecoded {
    int width = 0, height = 0;
    int chroma = 0;     // 2 4:2:2, 3 4:4:4
    int interlace = 0;  // 0 progressive, 1 the top field first, 2 the bottom first
    int alphaType = 0;  // 0 none, 1 8 bits, 2 16 bits
    int chromaWidth() const { return chroma == 3 ? width : (width + 1) / 2; }
    std::vector<uint16_t> y, cb, cr, alpha;
};
bool decodeOwn(const uint8_t* data, size_t size, OwnDecoded& out, std::string* error = nullptr);

}  // namespace montage::prores
