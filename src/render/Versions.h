// Montage — delivering a cut in several shapes at once (Premiere's Auto Reframe with Media Encoder, CapCut's and
// Descript's multi-format export): 16:9 for YouTube, 9:16 for Shorts, Reels and TikTok, 4:5 for feeds and 1:1, each
// a copy of the sequence reframed to follow each shot's subject (render/ClipAnalysis.h), its captions raised clear of
// the platforms' buttons when the frame is tall, rendered with the same settings: captions burned in if asked, and
// the sound brought to a loudness target.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "core/Model.h"
#include "render/Exporter.h"

namespace montage {

struct VersionShape {
    int aspectW = 16, aspectH = 9;
    std::string label;  // "16x9"
};
// "16:9", "9:16", "4:5", "1:1" (also "16x9", or a frame size like "1080x1920"); false for anything else.
bool parseVersionShape(const std::string& text, VersionShape& out);
// The standard shapes, landscape first.
std::vector<VersionShape> standardVersionShapes();

// The sequences for each shape: the source itself when it already has that shape, else a reframed copy named
// "<name> <w>x<h>" (an existing one of that name is replaced). Their ids in `out`, in the shapes' order. False with
// `error` if the footage cannot be read or when cancelled.
bool makeVersionSequences(Project& p, Id source, const std::vector<VersionShape>& shapes, std::vector<Id>& out, int speed = 1,
                          const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                          std::string* error = nullptr);

// The export settings for a version: `base` with its file named "<folder>/<sequence name>.<ext>", captions burned in
// when asked and the sequence has any, and the loudness target (0 = left alone).
ExportSettings versionSettings(const Sequence& version, const ExportSettings& base, const std::string& folder, bool burnCaptions,
                               double loudnessLufs);

}  // namespace montage
