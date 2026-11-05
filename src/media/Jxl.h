// Montage — JPEG XL, read and written with libjxl (BSD-3-Clause) directly rather than through FFmpeg, so float, HDR and
// alpha come through as they are: stills, numbered frames and animations, 8 to 16-bit integer, half and float samples,
// straight or premultiplied alpha, the file's colour encoding (or ICC profile) and intensity target, and its
// orientation. A JPEG can be recompressed into one losslessly and rebuilt from it byte for byte.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace montage {

struct ColorSpace;

bool jxlAvailable();  // built with libjxl
bool jxlWritesPremultiplied();  // its libjxl (0.9 or later) writes premultiplied alpha
bool jxlConvertsProfiles();     // built with Little CMS: colour that is none of Montage's spaces is converted, not refused
bool isJxlPath(const std::string& file);

struct JxlInfo {
    int width = 0, height = 0;  // as shown: after its orientation
    int bits = 8;               // per sample: 8 to 16 integer, 16 half float, 32 float
    int exponentBits = 0;       // 0 integer, 5 half, 8 float
    bool gray = false;
    bool alpha = false;
    bool alphaPremultiplied = false;
    int orientation = 1;     // EXIF's 1 to 8
    // The Montage space (render/ColorSpace.h id) its samples are read in: its own, or linear Rec.709 display light
    // ("linear-rec709+display") for colour that is none of them, converted from its ICC profile (with Little CMS; a
    // file it cannot convert, CMYK say, is refused).
    std::string colorSpace;
    bool icc = false;        // its colour stated by an ICC profile
    double intensityTarget = 0;  // nits (the libjxl default, 255, for SDR)
    bool jpeg = false;           // holds a JPEG, rebuilt byte for byte by jxlToJpeg
    // An animation: each shown frame's duration in ticks of tpsDen/tpsNum seconds (a still: one frame, no ticks).
    bool animated = false;
    uint32_t tpsNum = 0, tpsDen = 1;
    std::vector<uint32_t> durations;
    int frames() const { return animated ? int(durations.size()) : 1; }
    double seconds() const;
    // Where frame `k` starts, in seconds, and the frame shown at `t` seconds.
    double frameStart(int k) const;
    int frameAt(double t) const;
};
bool readJxlInfo(const std::string& file, JxlInfo& out, std::string* error = nullptr);
// Frame `k` of a file (0 for a still) as float RGBA as it is stored: integer samples as 0..1, floats as they are,
// colour premultiplied when the file says so (`premultiplied`), oriented.
bool readJxlFrame(const std::string& file, int k, int& width, int& height, std::vector<float>& rgba, bool* premultiplied = nullptr,
                  std::string* error = nullptr);

// An animation's frames, read in order without decoding from the start for each (a frame before the last one read
// starts again from the first).
class JxlAnimationReader {
public:
    JxlAnimationReader();
    ~JxlAnimationReader();
    bool open(const std::string& file, std::string* error = nullptr);
    const JxlInfo& info() const;
    bool frame(int k, int& width, int& height, std::vector<float>& rgba, std::string* error = nullptr);

private:
    struct State;
    std::unique_ptr<State> s_;
};

struct JxlWriteOptions {
    int bits = 16;            // 8 or 16 integer (or 9 to 15), 16 with halfFloat, or 32 (float)
    bool halfFloat = false;
    double distance = 0;      // 0 lossless; else the encoder's distance (1 is visually lossless; 0.1 to 25)
    int effort = 7;           // 1 (fastest) to 9
    bool alpha = false;
    bool premultiplied = false;        // written premultiplied (else straight)
    bool sourcePremultiplied = false;  // the pixels given are premultiplied (as program frames are)
    std::string colorSpace;            // the Montage space the pixels are in ("" = sRGB): its encoding is stated
    std::vector<uint8_t> iccProfile;   // else an ICC profile stated as the pixels' colour (its bytes)
    double intensityTarget = 0;        // nits; 0 = the default
    int orientation = 1;
};
// Whether a Montage space can be stated in a JPEG XL file's colour encoding (display spaces and linear light; not
// camera log).
bool jxlCanState(const ColorSpace& space);
// Writes `frames` (float RGBA, width * height * 4 each) as a still (one frame) or an animation (with `durations`
// in ticks of tpsDen/tpsNum seconds). False with `error` when it cannot be written.
bool writeJxl(const std::string& file, int width, int height, const std::vector<const float*>& frames, const JxlWriteOptions& options,
              std::string* error = nullptr, const std::vector<uint32_t>& durations = {}, uint32_t tpsNum = 0, uint32_t tpsDen = 1);
bool writeJxl(const std::string& file, int width, int height, const float* rgba, const JxlWriteOptions& options,
              std::string* error = nullptr);
// The same to memory.
bool encodeJxl(std::vector<uint8_t>& out, int width, int height, const std::vector<const float*>& frames, const JxlWriteOptions& options,
               std::string* error = nullptr, const std::vector<uint32_t>& durations = {}, uint32_t tpsNum = 0, uint32_t tpsDen = 1);

// A JPEG recompressed losslessly (its coefficients kept, with what rebuilds the file), and the JPEG rebuilt.
bool jpegToJxl(const std::string& jpeg, const std::string& jxl, std::string* error = nullptr, int effort = 7);
bool jxlToJpeg(const std::string& jxl, std::vector<uint8_t>& jpeg, std::string* error = nullptr);

}  // namespace montage
