// Montage — HDR stills with a gain map (ISO 21496-1, and Adobe's gain map XMP beside it): an SDR picture that every
// viewer shows, and a map of how much brighter each pixel is in the HDR rendition, which HDR screens apply as far as
// their headroom allows. Written as a JPEG whose second picture (MPF) is the map; read back as the HDR rendition.
//
// This product includes Gain Map technology under license by Adobe.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "media/Image.h"

class QImage;

namespace montage {

struct ColorSpace;

// The map's metadata (one set for all three channels, as Montage writes it): log2 of the content boost the map's 0
// and 1 stand for, the map's gamma, the offsets added to the SDR and HDR linear light before the ratio is taken, and
// log2 of the display headroom where the map starts and stops being applied.
struct GainMapMetadata {
    double gainMapMin[3] = {0, 0, 0}, gainMapMax[3] = {1, 1, 1};
    double gamma[3] = {1, 1, 1};
    double offsetSdr[3] = {1 / 64.0, 1 / 64.0, 1 / 64.0}, offsetHdr[3] = {1 / 64.0, 1 / 64.0, 1 / 64.0};
    double hdrCapacityMin = 0, hdrCapacityMax = 1;
    bool baseIsHdr = false;
    bool useBaseColorSpace = true;
};

// What a JPEG with a gain map holds: its metadata (the ISO 21496-1 block, else the XMP), where the map's JPEG is and
// how long. False for a JPEG without one.
struct GainMapJpeg {
    GainMapMetadata metadata;
    bool iso = false, xmp = false;  // which metadata it has
    long long mapOffset = 0, mapLength = 0;
};

// Whether a JPEG has a gain map, and where: its metadata segments alone are read (not the picture).
bool readGainMapJpeg(const std::string& path, GainMapJpeg& out);

// The HDR rendition of a base picture and its map at full boost (`headroom` log2, below the metadata's maximum: less):
// linear light relative to SDR white (1.0 = 203 nits in HDR), in the base's primaries.
void applyGainMap(const QImage& base, const QImage& map, const GainMapMetadata& m, Image& linear, double headroom = 1e9);
// A file's HDR rendition: 16-bit code values of Rec.2100 PQ (BT.2020), RGBA (opaque) in host byte order, and its
// brightest pixel in nits. Decoded once per version of the file; nullptr with `error` when it cannot be.
struct GainMapPicture {
    int width = 0, height = 0;
    std::vector<uint16_t> rgba;
    double maxNits = 0;
};
std::shared_ptr<const GainMapPicture> decodeGainMapJpeg(const std::string& path, std::string* error = nullptr);

// Writes `sdr` (code values in `sdrSpace`, an SDR display space) as the base and the map to `hdr` (code values in
// `hdrSpace`, PQ or HLG), as a JPEG with both kinds of metadata. Pictures premultiplied over black, the same size; the
// SDR one in Rec.709 or Display P3 primaries (what a JPEG states).
bool writeGainMapJpeg(const Image& sdr, const ColorSpace& sdrSpace, const Image& hdr, const ColorSpace& hdrSpace, int quality,
                      const std::string& path, std::string* error = nullptr, GainMapMetadata* written = nullptr);

}  // namespace montage
