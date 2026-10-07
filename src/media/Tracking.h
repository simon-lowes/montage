// Montage — motion tracking and stabilisation analysis: Shi–Tomasi corners,
// pyramidal Lucas–Kanade optical flow with a forward–backward check, and a
// RANSAC similarity fit (translation, rotation, uniform scale). Used to make
// masks follow what they cover and to measure camera shake.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "Image.h"

namespace montage {

struct GrayImage {
    int width = 0, height = 0;
    std::vector<float> px;  // luma 0..1
    float at(int x, int y) const { return px[size_t(y) * size_t(width) + size_t(x)]; }
    float sample(double x, double y) const;  // bilinear, clamped to the edges
};
GrayImage toGray(const Frame16& f);

struct Point2 {
    double x = 0, y = 0;
};

// Up to `maxCorners` good features to track, strongest first, at least
// `minDistance` px apart, inside [x0, x1) x [y0, y1) (all of it when empty).
std::vector<Point2> goodFeatures(const GrayImage& img, int maxCorners, double minDistance, double x0 = 0, double y0 = 0,
                                 double x1 = 0, double y1 = 0);

// Follows points from `a` to `b` (pyramidal Lucas–Kanade). ok[i] is false
// for points lost or failing the forward–backward check.
void trackPoints(const GrayImage& a, const GrayImage& b, const std::vector<Point2>& from, std::vector<Point2>& to,
                 std::vector<bool>& ok);

// Dense optical flow from `a` to `b` on a grid every `step` pixels: flow[gy *
// gw + gx] is the motion of the point (gx * step, gy * step). Points without
// texture take their neighbours' motion.
struct FlowField {
    int gw = 0, gh = 0, step = 1;
    std::vector<Point2> v;
    Point2 at(double x, double y) const;  // bilinear, in pixels of the analysed images
};
FlowField denseFlow(const GrayImage& a, const GrayImage& b, int step);
GrayImage toGray(const Image& img, int maxWidth);  // premultiplied float RGBA, downscaled to at most maxWidth

// p' = s * R(angle) * p + t.
struct Similarity {
    double tx = 0, ty = 0, angle = 0, scale = 1;
    Point2 apply(Point2 p) const;
};
enum class MotionModel { Translation, TranslationScale, Similarity };
// Robust fit of from -> to (RANSAC, then least squares on the inliers).
bool fitMotion(const std::vector<Point2>& from, const std::vector<Point2>& to, MotionModel model, Similarity& out,
               int* inliers = nullptr);

// ---- Region tracking (masks) -------------------------------------------------
// A rotated rectangle in fractions of the frame (as mask.x/y/w/h/rotation are).
struct TrackRegion {
    double x = 0.5, y = 0.5, w = 0.4, h = 0.4, rotation = 0;  // rotation in degrees
};
using TrackProgress = std::function<void(double fraction)>;
// Follows `start` (the region at media time `from`) frame by frame to `to`
// (seconds; earlier than `from` tracks backwards). One region per frame,
// the first being `start`. Stops early (shorter result) if the region is lost.
std::vector<TrackRegion> trackRegion(const std::string& path, double from, double to, const TrackRegion& start,
                                     MotionModel model, const TrackProgress& progress = {},
                                     const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

// ---- Stabilisation -------------------------------------------------------------
struct CameraMotion {
    double fps = 0;       // media frame rate the samples are at
    double start = 0;     // media time of the first sample
    std::vector<Similarity> steps;  // frame i-1 -> i, in fractions of the frame width (steps[0] identity)
};
// Compact text form, stored in a Stabilize effect.
std::string cameraMotionToString(const CameraMotion& m);
bool cameraMotionFromString(const std::string& s, CameraMotion& m);

CameraMotion analyzeCameraMotion(const std::string& path, double from, double to, const TrackProgress& progress = {},
                                 const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

// The per-frame correction that moves the shaky path onto its smoothed
// version (Gaussian, `smoothSeconds` wide; 0 locks the camera), as transforms
// in fractions of the frame width.
std::vector<Similarity> stabilizationCorrections(const CameraMotion& m, double smoothSeconds, MotionModel model);
// The zoom that keeps the frame covered under every correction (`aspect` is height / width).
double stabilizationZoom(const std::vector<Similarity>& corrections, double aspect);

}  // namespace montage
