// Montage — camera RAW stills (as Resolve 21's Photo page, Premiere and Final
// Cut take them): Canon CR2 / CR3, Nikon NEF, Sony ARW, Fujifilm RAF,
// Panasonic RW2, Olympus ORF, Pentax PEF, Leica, Hasselblad, Adobe DNG and the
// other formats LibRaw (LGPL-2.1 / CDDL-1.0) reads. Each is developed once,
// with the camera's white balance, demosaiced (AHD) and turned upright, into
// 16-bit Rec.709 RGB with the Rec.709 curve, so it edits like any other still.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace montage {

// Built with LibRaw.
bool rawAvailable();
// Whether a file is named as a camera RAW (by its extension).
bool isRawPath(const std::string& path);

struct RawInfo {
    int width = 0, height = 0;  // as developed (upright)
    std::string camera;         // "Canon EOS 5D Mark IV"
};
bool probeRaw(const std::string& path, RawInfo& info, std::string* error = nullptr);

// Develops the picture: RGB, 16 bits a channel, rows top to bottom.
struct RawImage {
    int width = 0, height = 0;
    std::vector<uint16_t> rgb;
};
bool developRaw(const std::string& path, RawImage& out, std::string* error = nullptr);

}  // namespace montage
