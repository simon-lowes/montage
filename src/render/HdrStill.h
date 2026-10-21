// Montage — HDR stills: PNG with its HDR chunks (PNG Third Edition's cICP, mDCV and cLLI), and AVIF and HEIF at 10 bits
// in PQ or HLG with their light levels, written from a rendered frame; and read back.
#pragma once

#include <string>

#include "media/Image.h"

namespace montage {

struct ColorSpace;

// What a still's HDR metadata says: PNG's cICP chunk or AVIF's colr property (ITU-T H.273 code points: primaries 9
// BT.2020, 12 P3 D65; transfer 16 PQ, 18 HLG; -1 without one), the light levels of PNG's cLLI or AVIF's clli, and the
// mastering display's luminance from mDCV or mdcv, in nits (0 without). False for a file that is neither.
struct StillHdrInfo {
    int primaries = -1, transfer = -1, matrix = -1;
    bool fullRange = true;
    double maxCll = 0, maxFall = 0;
    double masteringMaxNits = 0, masteringMinNits = 0;
};
bool readStillHdrInfo(const std::string& path, StillHdrInfo& out);

// H.273 code points of Montage's display spaces (-1 for others).
int cicpPrimaries(const ColorSpace& space);
int cicpTransfer(const ColorSpace& space);

// `img`: code values in `space`, premultiplied over black (as program frames are). A 16-bit PNG; in an HDR space with
// cICP, mDCV (a P3 D65 display of `peakNits`) and cLLI. False with `error` when it cannot be written.
bool writeHdrPng(const Image& img, const ColorSpace& space, double peakNits, double maxCll, double maxFall, const std::string& path,
                 std::string* error = nullptr);
// An AVIF still (AV1, 4:2:0, full range, over black): 10 bits in an HDR space, with its mastering display and light
// levels (mdcv and clli properties; `stated` says whether they could be added), else 8. `quality` 0-100.
bool writeAvifStill(const Image& img, const ColorSpace& space, int quality, double peakNits, double maxCll, double maxFall,
                    const std::string& path, std::string* error = nullptr, bool* stated = nullptr);
// A HEIF still (HEVC, 4:2:0, full range, over black): 10 bits in an HDR space with its mastering display and light
// levels (mdcv and clli), else 8. Pictures larger than HEVC's levels take (or than `tile`, when given) go in a grid of
// tiles.
bool writeHeifStill(const Image& img, const ColorSpace& space, int quality, double peakNits, double maxCll, double maxFall,
                    const std::string& path, std::string* error = nullptr, int tile = 0);
// An AVIF with a gain map (ISO 21496-1, as libavif writes one): `sdr` (code values in `sdrSpace`, Rec.709 or Display P3
// primaries) as the 8-bit base, which every reader shows, and a map to `hdr` (in `hdrSpace`, PQ or HLG) in a tone-mapped
// derived item that HDR readers prefer, stating the HDR rendition's colour and light levels. Pictures premultiplied
// over black, the same size.
bool writeGainMapAvif(const Image& sdr, const ColorSpace& sdrSpace, const Image& hdr, const ColorSpace& hdrSpace, int quality,
                      double maxCll, double maxFall, const std::string& path, std::string* error = nullptr);
// The same as a HEIF, in HEVC (the map 4:2:0), the base and the map each in a grid of tiles when larger than HEVC's
// levels take (or than `tile`, when given).
bool writeGainMapHeif(const Image& sdr, const ColorSpace& sdrSpace, const Image& hdr, const ColorSpace& hdrSpace, int quality,
                      double maxCll, double maxFall, const std::string& path, std::string* error = nullptr, int tile = 0);

}  // namespace montage
