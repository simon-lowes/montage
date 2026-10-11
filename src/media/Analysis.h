// Montage — media analysis: scene-cut detection and proxy generation.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace montage {

using AnalysisProgress = std::function<void(double fraction)>;

// Times (seconds) where a new shot starts, found by comparing consecutive
// frames' colour histograms and pixels against an adaptive threshold.
// `sensitivity` in 0..1: higher finds more (subtler) cuts.
std::vector<double> detectSceneCuts(const std::string& path, double sensitivity = 0.5,
                                    const AnalysisProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                                    std::string* error = nullptr);

// Writes a fast-seeking, low-resolution H.264 proxy of a video (no audio).
bool createProxy(const std::string& source, const std::string& proxyPath, int maxWidth = 960,
                 const AnalysisProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                 std::string* error = nullptr);

// A Super Scale copy of a video or still, `factor` (2 to 4) times its size: a
// video with its sound (ProRes 422 HQ for .mov, else high-quality H.264), a
// still as an image file of the type `dest` names (.png, .jpg, .tif).
bool createSuperScaled(const std::string& source, const std::string& dest, int factor, double strength = 1,
                       const AnalysisProgress& progress = {}, const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

}  // namespace montage
