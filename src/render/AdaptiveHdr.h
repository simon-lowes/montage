// Montage — adaptive HDR metadata (SMPTE ST 2094-50) in pictures and files: read from a clip's HEVC SEI or AV1
// metadata OBUs and honoured as it enters a sequence (its reference white kept at the sequence's, its own gain curves
// tone mapping it to the sequence's headroom), honoured by the viewers at the display's headroom, and made for PQ and
// HLG exports from the grade: each scene's SDR rendition (its SDR trim, or Montage's own HDR-to-SDR mapping) fitted
// by a gain curve on the brightest component. The message itself and its maths are in core/AdaptiveHdrMetadata.h.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/AdaptiveHdrMetadata.h"
#include "core/Model.h"
#include "media/Image.h"
#include "render/ColorSpace.h"

struct AVPacket;

namespace montage {

struct Lut3D;

// ---- In files -----------------------------------------------------------------------------------------------------
// Each ITU-T T.35 message (from the country code on) in an HEVC access unit's SEI (`lengthSize` the NAL length size
// of MP4-style packets, 0 for start codes) or an AV1 temporal unit's metadata OBUs.
void forEachT35(const uint8_t* data, size_t size, bool av1, int lengthSize, const std::function<void(const uint8_t*, size_t)>& fn);
// A video file's adaptive HDR metadata, scene by scene in its frames (a message lasts until the next one). False with
// nothing when it has none: only HEVC and AV1 carry it, and a file without it in its first frames is taken to have
// none. Only its first ten minutes are read unless `whole` (on import, quickly); the last scene of a file read no
// further is marked (its key kAdaptiveHdrMore), and adaptiveHdrAt reads the rest when a frame past it is asked for.
bool readAdaptiveHdr(const std::string& path, std::vector<AdaptiveHdrScene>& scenes, std::string* error = nullptr, bool whole = false);
inline constexpr const char* kAdaptiveHdrMore = "more";
// The metadata of a media item's frame at `seconds` (null: none).
const AdaptiveHdr* adaptiveHdrAt(const MediaItem& m, double seconds);

// ---- In pictures --------------------------------------------------------------------------------------------------
// The headroom (log2 of the peak over reference white) a picture in `space` can show: 0 for SDR, the mastering peak
// over 203 nits for PQ, 1000 nits over 203 for HLG.
double spaceHeadroom(const ColorSpace& space, double peakNits);
// The gain application primaries from linear light in `p` and back.
void adaptiveGainMatrices(const AdaptiveHdr& md, Primaries p, double toGain[9], double fromGain[9]);
// Whether a picture in `from` carrying `md` converts into `to` differently from the plain conversion (its reference
// white is not 203 nits, or its curves tone map it to `to`'s headroom). Display-referred spaces only.
bool adaptiveConverts(const AdaptiveHdr& md, const ColorSpace& from, const ColorSpace& to, double peakNits);
// One pixel (code values) of a picture in `from` with `md` into `to`: to light relative to its reference white, in the
// gain application primaries; tone mapped to `to`'s headroom; into `to`'s primaries and code values, clipped to its
// headroom. Without curves, HDR into SDR rolls off as convertColor does.
void convertPixelAdaptive(float rgb[3], const ColorSpace& from, const AdaptiveHdr& md, const ColorSpace& to, double peakNits);
// That conversion as a 65³ LUT (kept for the last few pictures' metadata), and on a whole premultiplied picture.
std::shared_ptr<const Lut3D> adaptiveConversionLut(const ColorSpace& from, const AdaptiveHdr& md, const ColorSpace& to, double peakNits);
void convertColorAdaptive(Image& img, const ColorSpace& from, const AdaptiveHdr& md, const ColorSpace& to, double peakNits);
// For the HDR viewer: a picture as hdrViewImage makes it (light relative to SDR white in Rec.709 primaries), tone
// mapped in place by `md` to a display with `headroom` (its peak over SDR white, 1 = SDR) and clipped to it.
void adaptiveViewImage(std::vector<uint16_t>& rgbaHalf, const AdaptiveHdr& md, double headroom);

// ---- Making it ----------------------------------------------------------------------------------------------------
// Fits metadata to an HDR picture and its SDR rendition, pixel for pixel (premultiplied, the same size): the HDR
// picture's headroom, and the SDR rendition as an alternate at headroom 0, its gain curve on the brightest component
// fitted to the measured gains with up to 32 points. Gains are applied in the SDR rendition's primaries when they are
// Rec.709, P3 or Rec.2020 (Rec.2020 otherwise).
class AdaptiveHdrFitter {
public:
    AdaptiveHdrFitter(const ColorSpace& hdr, const ColorSpace& sdr);
    void add(const Image& hdr, const Image& sdr);
    AdaptiveHdr result() const;
    size_t samples() const { return samples_; }

private:
    const ColorSpace& hdr_;
    const ColorSpace& sdr_;
    AdaptiveHdr base_;
    double toGainHdr_[9], toGainSdr_[9];
    std::vector<uint32_t> peak_;              // brightest-component histogram, 1/64 stop bins
    std::vector<std::vector<std::pair<float, float>>> gains_;  // (log2 light, log2 gain) in 1/8 stop bins of light
    size_t samples_ = 0;
};

// The SDR trim target a sequence's SDR renditions come from (its first SDR target), or null: Montage's own mapping.
const TrimTarget* adaptiveSdrTarget(const Sequence& s);
// A fingerprint of what frames [from, to) look like to the analysis: the pictures (as HDR10+'s key) and the SDR trims.
std::string adaptiveHdrFramesKey(const Project& p, const Sequence& s, FrameTime from, FrameTime to, const ColorSpace& out, double peakNits);
// Measures [from, to) of `s` as exported to `out` (PQ or HLG), scene by scene (the cut's edits): a few frames of each
// rendered, with their SDR renditions, and fitted. `hdrTrim` (an HDR trim target, null: the master) is what the export
// delivers. False with `error` if `out` is not HDR, the range is empty or it was cancelled.
bool analyseAdaptiveHdr(const Project& p, const Sequence& s, FrameTime from, FrameTime to, const ColorSpace& out, double peakNits,
                        std::vector<AdaptiveHdrScene>& scenes, std::string* error = nullptr,
                        const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                        const TrimTarget* hdrTrim = nullptr);
// The scenes for [from, to) as an export needs them: the sequence's stored scenes that still match (when `reuse`) and
// the rest measured now; `measured` says how many frames had to be.
bool adaptiveHdrForRange(const Project& p, const Sequence& s, FrameTime from, FrameTime to, const ColorSpace& out, double peakNits,
                         bool reuse, std::vector<AdaptiveHdrScene>& scenes, std::string* error = nullptr,
                         const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                         const TrimTarget* hdrTrim = nullptr, FrameTime* measured = nullptr);
// New scenes into the stored ones, replacing those they overlap.
void mergeAdaptiveHdrScenes(std::vector<AdaptiveHdrScene>& stored, const std::vector<AdaptiveHdrScene>& fresh);
// The stored scene for frame `t` of `s` if it still matches the cut (checked once per project snapshot), else null.
const AdaptiveHdr* storedAdaptiveHdrAt(const std::shared_ptr<const Project>& p, const Sequence& s, FrameTime t);

}  // namespace montage
