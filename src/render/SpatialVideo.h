// Montage — spatial video out: a stereoscopic sequence's two eyes coded as MV-HEVC, the left eye the base view (which
// any HEVC player shows) and the right eye a second view predicted from it, as Apple's spatial video carries them. The
// encoder is x265's own multiview one (built with Montage: distribution builds of x265 leave it out, and FFmpeg's
// wrapper passes a single view). The MOV or MP4 around it gets the second view's configuration (lhvC) and Apple's
// stereo boxes (vexu, hfov) once FFmpeg has written it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace montage {

// Whether this build has the multiview encoder.
bool spatialVideoAvailable();

struct SpatialVideoSettings {
    int width = 0, height = 0;  // each eye's (even)
    int fpsNum = 25, fpsDen = 1;
    int crf = 23;          // when no bitrate is asked for
    int64_t bitrate = 0;   // bits a second, 0 = constant quality
    std::string preset;    // x265's ("" = medium)
    int keyint = 0;        // frames between keyframes, 0 = two seconds
    int primaries = 1, transfer = 1, matrix = 1;  // the colour the pictures are in (H.273 codes)
};

class SpatialVideoEncoder {
public:
    struct Packet {
        std::vector<uint8_t> data;  // both views' NAL units, Annex B
        int64_t pts = 0, dts = 0;
        bool key = false;
    };
    static std::unique_ptr<SpatialVideoEncoder> open(const SpatialVideoSettings& s, std::string* error = nullptr);
    ~SpatialVideoEncoder();
    // The base view's parameter sets and its stereo SEI messages, Annex B: the stream's extradata, from which the
    // muxer writes the hvcC.
    const std::vector<uint8_t>& baseHeaders() const { return base_; }
    // The second view's parameter sets as an L-HEVC decoder configuration record (ISO/IEC 14496-15), the lhvC box's
    // contents.
    const std::vector<uint8_t>& lhvcRecord() const { return lhvc_; }
    // One picture: the eyes side by side (left then right) in 8-bit 4:2:0, its planes one after another with no
    // padding (2 x width by height, `size` bytes). Coded pictures come out in decoding order, some frames later.
    bool encode(const uint8_t* sideBySide, size_t size, int64_t pts, std::vector<Packet>& out, std::string* error = nullptr);
    // The pictures still held, once there are no more.
    bool flush(std::vector<Packet>& out, std::string* error = nullptr);

private:
    SpatialVideoEncoder() = default;
    struct Impl;
    std::unique_ptr<Impl> d_;
    std::vector<uint8_t> base_, lhvc_;
};

// The boxes Apple's players read added to the first video track's sample entry of an MP4/MOV file: lhvC (`lhvc`),
// and vexu (both eyes present, the left the hero eye, the cameras' baseline in millimetres, no disparity adjustment)
// with hfov (the horizontal field of view in degrees).
bool writeSpatialVideoBoxes(const std::string& path, const std::vector<uint8_t>& lhvc, double baselineMm, double fieldOfView,
                            std::string* error = nullptr);

}  // namespace montage
