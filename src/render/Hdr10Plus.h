// Montage — HDR10+ dynamic metadata (SMPTE ST 2094-40, as Resolve Studio, Amazon and Samsung deliveries use it): each
// scene of a PQ programme measured in linear light (its brightest red, green and blue, the average and the spread of
// each pixel's brightest channel), so an HDR10+ display can tone map scene by scene instead of to one programme-wide
// peak. The scenes are the cut's edits plus picture changes within clips; the measures go into HEVC exports as
// HDR10+ SEI messages and AV1 exports as metadata OBUs on every frame, and into the JSON that x265, hdr10plus_tool
// and mastering tools read.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"
#include "render/ColorSpace.h"
#include "render/LightLevel.h"

struct AVPacket;

namespace montage {

// Accumulates frames into scenes.
class Hdr10PlusMeter {
public:
    explicit Hdr10PlusMeter(const ColorSpace& space);
    bool valid() const { return valid_; }  // PQ only: HDR10+ is HDR10's
    // Adds the frame at `f` (frames in order, consecutive). A scene starts at `cut` (an edit), or where the picture
    // changes as at a cut (scenes within a clip) once the scene is long enough.
    void add(const Image& img, FrameTime f, bool cut);
    // The scenes so far, the last one finished.
    std::vector<Hdr10PlusScene> scenes();
    int minSceneFrames = 12;

private:
    void close();
    bool valid_ = false;
    std::vector<float> nits_;                  // per 16-bit code value
    std::vector<uint64_t> hist_, frame_;       // the current scene's and frame's maxRGB per 16-bit code value
    std::vector<double> coarse_, lastCoarse_;  // the frame's and the previous frame's maxRGB spread (cut detection)
    Hdr10PlusScene cur_;
    double sum_ = 0;
    uint64_t pixels_ = 0;
    FrameTime next_ = -1;
    std::vector<Hdr10PlusScene> done_;
};

// Where the cut's scenes change in [from, to): the starts and ends of video clips (and transitions' middles) inside it.
std::vector<FrameTime> hdr10PlusCuts(const Sequence& s, FrameTime from, FrameTime to);
// A fingerprint of frames [from, to) of `s` as exports render them to `out` (from the render cache's frame keys).
std::string hdr10PlusFramesKey(const Project& p, const Sequence& s, FrameTime from, FrameTime to, const ColorSpace& out,
                               double peakNits);
// Renders [from, to) at full size into `out` (PQ) and measures its scenes, each with its key, and (when given) its
// HDR10 light levels in the same pass. False with `error` if `out` is not PQ, the range is empty or it was cancelled.
bool analyseHdr10Plus(const Project& p, const Sequence& s, FrameTime from, FrameTime to, const ColorSpace& out, double peakNits,
                      std::vector<Hdr10PlusScene>& scenes, std::string* error = nullptr,
                      const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                      LightLevels* light = nullptr);
// The sequence's stored scenes for [from, to) (clipped to it) when every scene there still matches the cut; empty if
// any part is unanalysed or has changed.
std::vector<Hdr10PlusScene> storedHdr10Plus(const Project& p, const Sequence& s, FrameTime from, FrameTime to,
                                            const ColorSpace& out, double peakNits);

// ---- Writing ---------------------------------------------------------------------------------------------------------
// The scene as an ITU-T T.35 message (from the country code on): HDR10+ profile A, one window, no targeted display.
std::vector<uint8_t> hdr10PlusT35(const Hdr10PlusScene& scene);
// Adds the message to an HEVC access unit as a prefix SEI before its first slice. `lengthSize` is the NAL length
// field's size for length-prefixed packets (MP4 style), 0 for Annex B start codes. False if the packet has no slice.
bool addHdr10PlusSei(AVPacket* pkt, const std::vector<uint8_t>& t35, int lengthSize);
// Adds it to an AV1 temporal unit as a metadata OBU before its first frame. False if there is none.
bool addHdr10PlusObu(AVPacket* pkt, const std::vector<uint8_t>& t35);
// The scenes of frames [from, to) as hdr10plus_tool's JSON (frame indexes from `from`), as x265's --dhdr10-info reads.
std::string hdr10PlusJson(const std::vector<Hdr10PlusScene>& scenes, FrameTime from, FrameTime to);
bool writeHdr10PlusJson(const std::string& path, const std::vector<Hdr10PlusScene>& scenes, FrameTime from, FrameTime to,
                        std::string* error = nullptr);
// Reads such JSON back into scenes (frames from 0), for checks and for footage delivered with it.
bool readHdr10PlusJson(const std::string& path, std::vector<Hdr10PlusScene>& scenes, std::string* error = nullptr);

}  // namespace montage
