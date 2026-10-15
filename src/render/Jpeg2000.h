// Montage — JPEG 2000 through OpenJPEG itself (rather than FFmpeg's wrapper, which can only ask for the cinema
// profiles): the IMF profiles of ISO 15444-1 Amendment 8 that IMF Application #2E masters are coded in, lossless (the
// reversible 5-3 profiles, as Netflix and most studios ask) or lossy at a capped bit rate, and decoding for checks.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace montage {

// Built with OpenJPEG 2.5 or later.
bool openJpegAvailable();

// The IMF profile, main level and sub level a w x h, `components`-component picture at `fps` needs: the 2K profile up
// to 2048 x 1556, else 4K (to 4096 x 3112) or 8K; the reversible (lossless) profiles, or the lossy ones at
// `megabitsPerSecond`. The value is the codestream's Rsiz; 0 when the picture is too large or too fast.
uint16_t imfRsiz(int width, int height, int components, double fps, bool lossless, double megabitsPerSecond = 0);
// An HTJ2K codestream's Rsiz (ISO 15444-15: bit 14, no Part 1 profile).
constexpr uint16_t kHtj2kRsiz = 0x4000;
// The picture coding label (SMPTE ST 422) for that Rsiz, as 16 bytes; all zero when it is not an IMF profile (or HTJ2K).
void imfPictureCoding(uint16_t rsiz, uint8_t out[16]);

// Encodes w x h RGB (three interleaved samples per pixel, `bits` 8 to 16) as a bare codestream in the IMF profile
// `rsiz`; lossy profiles keep each frame within `maxBytes` (0: no cap). Rsiz 3 or 4 is the DCI 2K or 4K cinema profile
// for 12-bit X'Y'Z' (no component transform, each component within four fifths of `maxBytes`).
bool encodeJpeg2000(const uint16_t* rgb, int width, int height, int bits, uint16_t rsiz, size_t maxBytes, std::vector<uint8_t>& out,
                    std::string* error = nullptr);
// High-Throughput JPEG 2000 (ISO 15444-15) through OpenJPH (built with OpenEXR): the HTJ2K coding IMF Application #2E
// takes since ST 2067-21:2023, lossless only (OpenJPH has no rate control): the reversible 5-3 wavelet and component
// transform, 32 x 32 code-blocks, 128 x 128 precincts at the lowest resolution and 256 x 256 above, CPRL, a tile-part per
// component with a TLM marker, Rsiz 0x4000 and a CAP marker. OpenJPEG's decoder reads it back.
bool htj2kAvailable();
bool encodeHtj2k(const uint16_t* rgb, int width, int height, int bits, std::vector<uint8_t>& out, std::string* error = nullptr);
// The HTJ2K picture coding label (ST 422: HTJ2KPictureCodingSchemeGeneric).
void htj2kPictureCoding(uint8_t out[16]);
// Decodes a codestream to interleaved samples (up to four components).
bool decodeJpeg2000(const uint8_t* data, size_t size, std::vector<uint16_t>& samples, int& width, int& height, int& components,
                    int& bits, std::string* error = nullptr);

}  // namespace montage
