// Montage — IMF masters (SMPTE ST 2067, Application #2E as ST 2067-21:2021 has it): the Interoperable Master Package
// Netflix, Amazon, Disney+, Apple and broadcasters ask for instead of a ProRes file (DaVinci Resolve Studio and Avid
// Media Composer export it; Premiere and Final Cut need a third-party tool).
//
// The picture is rendered at the sequence's size (or HD, UHD or 4K, fitted on black) in the colour the master is
// delivered in (Rec.709 SDR, P3-D65 or Rec.2020 PQ, Rec.2020 HLG), as full-range RGB 4:4:4 of 10 or 12 bits, and coded
// in the JPEG 2000 IMF profiles (render/Jpeg2000.h): lossless (the reversible 5-3 profiles, as studios ask) or capped
// at a bit rate, several frames at a time. The sound is the mix at 48 kHz, 24 bits, stereo, 5.1 or 7.1 DS with
// multichannel labels. Both go into AS-02 track files (render/DcpMxf.h), described by a Composition Playlist
// (ST 2067-3) whose essence descriptor list repeats each file's descriptors (as RegXML), with the Application #2E
// identification, a Packing List with each file's SHA-1 (ST 2067-2) and an Asset Map (ST 429-9). Netflix's Photon,
// the reference IMF checker, passes these packages.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"

namespace montage {

struct ImfSettings {
    std::string title;
    std::string kind = "feature";  // ST 2067-3 content kind: feature, episode, short, trailer, teaser, advertisement, test...
    std::string issuer = "Montage";
    std::string language = "en";   // the sound's and the annotations' (RFC 5646)
    std::string colour;            // "rec709", "p3d65-pq", "rec2020-pq", "rec2020-hlg"; empty: as the sequence is
    std::string size = "sequence";  // "sequence", "hd" (1920 x 1080), "uhd" (3840 x 2160), "4k" (4096 x 2160)
    int bits = 0;                  // 10 or 12 (0: 10 for SDR, 12 for HDR)
    bool lossless = true;          // the reversible profiles; else lossy within megabitsPerSecond
    double megabitsPerSecond = 400;
    double masteringPeak = 0;      // HDR: the mastering display's peak in cd/m^2 (0: the sequence's HDR peak, at least 1000)
    bool inOut = false;            // only In to Out (both included)
    int threads = 0;               // JPEG 2000 encoders at once (0: one per processor core)
};

struct ImfResult {
    std::string folder, cpl;
    int64_t frames = 0;
    uint32_t rateNum = 24, rateDen = 1;
    int width = 0, height = 0, bits = 0, channels = 0;
    uint16_t rsiz = 0;  // the JPEG 2000 profile the pictures declare
    std::string colour;
};

// The colour a sequence is mastered in, and whether IMF Application #2E takes its frame rate (24, 23.976, 25, 29.97,
// 30, 50, 59.94, 60, and 120 above HD). At 29.97 and 59.94 a master is padded with black and silence to a multiple of
// five frames, so the sound fills whole frames.
std::string defaultImfColour(const Sequence& s);
bool imfFrameRateAllowed(const Sequence& s);
// The same for a picture of width x height at `fps`; `canonical`, if given, receives the rate as IMF writes it
// (24000/1001 rather than 48000/2002).
bool imfFrameRateAllowed(Rational fps, int width, int height, Rational* canonical = nullptr);
// The picture size for `size`, or false (with the reason) when it is too large for Application #2E.
bool imfPictureSize(const Sequence& s, const std::string& size, int& width, int& height, std::string* error = nullptr);

// Writes the package into a new folder (named after the title and the date) under `parent`. `progress` (0..1) may
// return false to stop (the half-written folder is removed).
bool exportImf(const Project& p, const Sequence& s, const ImfSettings& settings, const std::string& parent, ImfResult* result = nullptr,
               const std::function<bool(double)>& progress = {}, std::string* error = nullptr);

// Checks a package: the asset map, every file at its size, the packing list's hashes, the composition's resources in
// the package with durations within them and their essence descriptors listed, the Application #2E identification, the
// picture track files JPEG 2000 in an IMF profile with as many frames as the composition says, the sound 24-bit 48 kHz
// with as many samples. An empty list means it passed; `progress` may return false to stop ("Stopped").
std::vector<std::string> verifyImf(const std::string& folder, const std::function<bool(double)>& progress = {});

// Frame `index` of an IMF picture track file decoded to RGB samples.
bool readImfFrame(const std::string& mxf, int index, std::vector<uint16_t>& rgb, int& width, int& height, int& bits,
                  std::string* error = nullptr);

}  // namespace montage
