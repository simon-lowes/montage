// Montage — HDR stills with a gain map (ISO 21496-1, and Adobe's gain map XMP beside it): an SDR picture that every
// viewer shows, and a map of how much brighter each pixel is in the HDR rendition, which HDR screens apply as far as
// their headroom allows. Written as a JPEG whose second picture (MPF) is the map; read back as the HDR rendition.
//
// This product includes Gain Map technology under license by Adobe.
#pragma once

#include <QByteArray>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "media/Image.h"
#include "render/ColorSpace.h"

class QImage;

namespace montage {

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
// A picture as 16-bit code values, RGBA (opaque) in host byte order: a file's HDR rendition in Rec.2100 PQ (BT.2020),
// with its brightest pixel in nits. Decoded once per version of the file; nullptr with `error` when it cannot be.
struct GainMapPicture {
    int width = 0, height = 0;
    std::vector<uint16_t> rgba;
    double maxNits = 0;
};
std::shared_ptr<const GainMapPicture> decodeGainMapJpeg(const std::string& path, std::string* error = nullptr);

// HEIF stills (HEIC, and AVIF, which is HEIF with AV1): their primary picture, AV1 or HEVC or a grid of tiles of
// either, turned and mirrored as the file says. What it is (size as shown, codec, H.273 colour code points from its
// nclx or what its ICC profile is closest to, 0 where not known; light levels; whether it has a gain map Montage
// reads), the meta box alone read; and its picture as 16-bit full-range code values in its own colour (decoded once
// per version of the file).
struct HeifStillInfo {
    int width = 0, height = 0;
    std::string codec;  // "hevc", "av1"
    bool grid = false;
    int primaries = 0, transfer = 0;
    double maxCll = 0, maxFall = 0;
    bool gainMap = false;
    double hdrMaxCll = 0, hdrMaxFall = 0;  // (the gain map's HDR rendition's, from its tone-mapped item)
};
bool readHeifStill(const std::string& path, HeifStillInfo& out);

// A still's file written whole: into a file beside it that then replaces it, else (where the system refuses that, as
// Windows has) over it in place; every byte checked as written, and a part-written file removed.
bool writeStillFile(const std::string& path, const QByteArray& bytes, std::string& why);
std::shared_ptr<const GainMapPicture> decodeHeifStill(const std::string& path, std::string* error = nullptr);

// A HEIF (AVIF or HEIC) with a gain map: a tone-mapped derived item ('tmap', ISO 21496-1's metadata) from its base (the
// primary item) and its map. Its metadata (the meta box alone is read; false for a gain map Montage does not read: to
// SDR from an HDR base, or in primaries or a curve it does not know), and its HDR rendition as decodeGainMapJpeg gives
// it.
bool readGainMapHeif(const std::string& path, GainMapMetadata* metadata = nullptr);
std::shared_ptr<const GainMapPicture> decodeGainMapHeif(const std::string& path, std::string* error = nullptr);
// A HEIF still's primary picture as 8-bit RGB (null when it is not one); its primary item's type ("av01", "hvc1"), coded
// data and property boxes (whole, in order), for writing it into another file.
QImage decodeHeifPicture(const std::string& path, std::string* error = nullptr);
struct AvifCodedItem {
    QByteArray type;
    QByteArray data;
    std::vector<QByteArray> properties;
};
bool readHeifCodedItem(const std::string& path, AvifCodedItem& out);

// For the writers: the map's metadata and target codes (0-255, three a pixel) for an HDR picture over a base as decoders
// see it (`seen`, 8-bit sRGB-curve code values in `basePrimaries`); the map encoded with what a decoder reads back of
// each attempt (`roundTrip` encodes `map` and decodes it into `back`) fed back into the codes, the closest kept; and
// ISO 21496-1's metadata block.
bool planGainMap(const QImage& seen, Primaries basePrimaries, const Image& hdr, const ColorSpace& hdrSpace, GainMapMetadata& meta,
                 std::vector<float>& target);
QByteArray encodeGainMap(const std::vector<float>& target, int width, int height,
                         const std::function<QByteArray(const QImage& map, QImage& back)>& roundTrip);
QByteArray isoGainMapMetadata(const GainMapMetadata& m);
// Adobe's gain map properties (hdrgm) as an XMP packet, for one set of channels.
QByteArray adobeGainMapXmp(const GainMapMetadata& m);

// Writes `sdr` (code values in `sdrSpace`, an SDR display space) as the base and the map to `hdr` (code values in
// `hdrSpace`, PQ or HLG), as a JPEG with both kinds of metadata. Pictures premultiplied over black, the same size; the
// SDR one in Rec.709 or Display P3 primaries (what a JPEG states).
bool writeGainMapJpeg(const Image& sdr, const ColorSpace& sdrSpace, const Image& hdr, const ColorSpace& hdrSpace, int quality,
                      const std::string& path, std::string* error = nullptr, GainMapMetadata* written = nullptr);

}  // namespace montage
