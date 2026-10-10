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
// The picture coding label (SMPTE ST 422) for that Rsiz, as 16 bytes; all zero when it is not an IMF profile.
void imfPictureCoding(uint16_t rsiz, uint8_t out[16]);

// Encodes w x h RGB (three interleaved samples per pixel, `bits` 8 to 16) as a bare codestream in the IMF profile
// `rsiz`; lossy profiles keep each frame within `maxBytes` (0: no cap). Rsiz 3 or 4 is the DCI 2K or 4K cinema profile
// for 12-bit X'Y'Z' (no component transform, each component within four fifths of `maxBytes`).
bool encodeJpeg2000(const uint16_t* rgb, int width, int height, int bits, uint16_t rsiz, size_t maxBytes, std::vector<uint8_t>& out,
                    std::string* error = nullptr);
// Decodes a codestream to interleaved samples (up to four components).
bool decodeJpeg2000(const uint8_t* data, size_t size, std::vector<uint16_t>& samples, int& width, int& height, int& components,
                    int& bits, std::string* error = nullptr);

}  // namespace montage
