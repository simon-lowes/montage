// Montage — ProRes RAW (Apple's RAW video: Atomos recorders behind Sony, Canon, Nikon, Panasonic and Fujifilm cameras,
// DJI's Zenmuse X9 and Ronin 4D, the iPhone 17 Pro). FFmpeg 9 decodes it to a linear Bayer mosaic and hands over each
// frame's sensor range, white balance, camera-to-XYZ matrix and gain (AV_FRAME_DATA_RAW_COLOR_PARAMS); this develops
// that into a picture, with the same RAW controls as camera RAW stills and CinemaDNG (exposure, white balance as the
// light's temperature and tint, half size).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "CameraRaw.h"

// FFmpeg with ProRes RAW's colour parameters (in the media library, which is built against FFmpeg's headers).
#if __has_include(<libavutil/raw_color_params.h>)
#define MONTAGE_PRORES_RAW 1
#endif

struct AVFrame;

namespace montage {

// Whether this FFmpeg can decode ProRes RAW with its colour parameters (FFmpeg 9 and later).
bool proResRawAvailable();

struct ProResRawColor {
    double black = 256.0 / 65535, white = 61568.0 / 65535;  // the sensor's range, as fractions of 16 bits
    double wbRed = 1, wbBlue = 1;                            // as shot (green 1)
    unsigned cct = 0;                                        // the as-shot white balance in kelvin, 0 if not said
    double camToXyz[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};        // white-balanced camera RGB to CIE XYZ (D65), rows
    double gain = 1;                                         // to scene-linear light
};

// The developed picture's size: the mosaic less its recommended crop (left, top, right, bottom) exactly; with `half`,
// whole 2x2 cells of it, halved.
void proResRawSize(int width, int height, const int crop[4], bool half, int& outWidth, int& outHeight);

// The white balance multipliers on the mosaic (red, green, blue): as shot, or for `settings`' light (its white as the
// camera sees it once balanced as shot, made white) and tint (on green).
void proResRawBalance(const ProResRawColor& color, const RawSettings& settings, double mul[3]);

// Develops an RGGB mosaic (16-bit samples, `stride` samples a row) into 16-bit RGB encoded as ACEScct (AP1
// primaries), so highlights far above white keep their stops: sensor range to 0-1, the white balance on the mosaic
// (proResRawBalance), each channel clipped where it saturates (so blown areas are white, not magenta, whatever the
// balance; bright saturated colours lose what lies past the first channel's saturation, as LibRaw's Clip does), a
// bilinear demosaic (or, with settings.half, each 2x2 cell one pixel), the camera matrix, the gain and
// `settings.exposure`. `crop` is cut away; `rgb` holds proResRawSize()'s picture, `rgbStride` samples a row.
void developProResRaw(const uint16_t* mosaic, int width, int height, ptrdiff_t stride, const ProResRawColor& color,
                      const RawSettings& settings, const int crop[4], uint16_t* rgb, ptrdiff_t rgbStride);

struct DevelopedRaw {
    int width = 0, height = 0;
    std::vector<uint16_t> rgb;  // width * height * 3
};
void developProResRaw(const uint16_t* mosaic, int width, int height, ptrdiff_t stride, const ProResRawColor& color,
                      const RawSettings& settings, const int crop[4], DevelopedRaw& out);

// A decoded ProRes RAW frame's colour and recommended crop (left, top, right, bottom); false if it carries none.
bool proResRawColorOf(const AVFrame* frame, ProResRawColor& color, int crop[4]);
// A decoded ProRes RAW frame (its decoder opened with apply_cropping off) developed into a new RGB48 frame (ACEScct
// AP1, full range, cropped), half size when it is shown at `targetWidth` x `targetHeight` or smaller; null if it is
// not one. The caller frees it.
AVFrame* developProResRawFrame(const AVFrame* in, const RawSettings& settings, int targetWidth, int targetHeight);

// A ProRes RAW file's first frame: its picture size (after the recommended crop) and colour as shot. False if the
// file is not ProRes RAW, or this FFmpeg cannot read its colour.
struct ProResRawInfo {
    int width = 0, height = 0;
    int crop[4] = {0, 0, 0, 0};
    ProResRawColor color;
};
bool probeProResRaw(const std::string& path, ProResRawInfo& out, std::string* error = nullptr);
// The same from a ProRes RAW frame's own header (a packet as the file holds it); false if it is not one.
bool parseProResRawFrame(const uint8_t* data, size_t size, ProResRawInfo& out);

}  // namespace montage
