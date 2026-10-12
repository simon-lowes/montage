// Montage — pictures as video interfaces carry them: 4:2:2 limited-range Y'CbCr (BT.709 or BT.2020 non-constant
// luminance) from a rendered frame, and packed as SDI/HDMI cards and NDI take it: v210 (10-bit, DeckLink and AJA),
// UYVY (8-bit) and P216 (16-bit planar, NDI's high bit depth).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "media/Image.h"

namespace montage::video {

enum class YcbcrMatrix { Bt709, Bt2020 };

// 4:2:2 Y'CbCr at 16 bits (a 10-bit code times 64, an 8-bit one times 256): Y' = (16 + 219 Y) 256,
// Cb and Cr = (128 + 224 C) 256. Chroma is co-sited with the even luma samples, (width + 1) / 2 of them a row.
struct Ycbcr422 {
    int width = 0, height = 0;
    std::vector<uint16_t> y, cb, cr;
    Ycbcr422() = default;
    Ycbcr422(int w, int h);
    int chromaWidth() const { return (width + 1) / 2; }
};

// A rendered frame (its R'G'B' code values, premultiplied: over black) in Y'CbCr; chroma filtered [1 2 1] / 4 around
// each even sample, as SDI's co-siting has it.
Ycbcr422 toYcbcr422(const Image& rgb, YcbcrMatrix matrix);

// v210: six pixels in four little-endian 32-bit words (Cb Y Cr, Y Cb Y, Cr Y Cb, Y Cr Y, 10 bits each), rows padded
// to 48 pixels (128 bytes); codes kept within 4..1019 (0-3 and 1020-1023 are SDI's timing references). As FFmpeg's
// v210 encoder writes it, padding zeroed. Even widths.
size_t v210RowBytes(int width);
void packV210(const Ycbcr422& in, uint8_t* dst, size_t stride);
// UYVY: Cb Y0 Cr Y1 bytes, codes within 1..254.
void packUyvy(const Ycbcr422& in, uint8_t* dst, size_t stride);
// P216: a plane of 16-bit Y (little-endian), then one of interleaved 16-bit Cb Cr pairs as tall, starting `stride`
// times the height after the first; `stride` bytes a row in both.
void packP216(const Ycbcr422& in, uint8_t* dst, size_t stride);

}  // namespace montage::video
