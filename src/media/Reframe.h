// Montage — finding the subject of a shot, for Auto Reframe: where the eye
// goes in each frame, from what moves against the camera's own motion and
// from centre-surround contrast in brightness and colour, and a smoothed path
// for a crop window to follow.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "Image.h"

namespace montage {

struct SubjectPoint {
    double t = 0;              // media seconds
    double x = 0.5, y = 0.5;   // fractions of the frame
    double weight = 0;         // how clearly something stands out (0..1)
};

// The subject of one frame (premultiplied float RGBA), with `previous` (the
// frame before, same size, or empty) for motion.
SubjectPoint frameSubject(const Image& frame, const Image& previous);

using ReframeProgress = std::function<void(double fraction)>;
// The subject every `step` seconds from media time `from` to `to` (either order).
std::vector<SubjectPoint> findSubject(const std::string& path, double from, double to, double step,
                                      const ReframeProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                                      std::string* error = nullptr);

// The path a crop window should follow: Gaussian-smoothed over
// `smoothSeconds` (weighted by how clear the subject is), and held still when
// it would move less than `still` (a fraction of the frame) in all.
std::vector<SubjectPoint> smoothSubjectPath(const std::vector<SubjectPoint>& points, double smoothSeconds,
                                            double still = 0.04);

}  // namespace montage
