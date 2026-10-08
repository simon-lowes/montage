#include "Reframe.h"

#include <algorithm>
#include <cmath>

#include "Decoder.h"
#include "Tracking.h"

namespace montage {

namespace {

constexpr int kAnalysisWidth = 160;

// Box blur of radius r, run twice (close to a Gaussian), on one w x h channel.
void blur(std::vector<float>& v, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<float> tmp(v.size());
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < h; ++y) {
            const float* src = &v[size_t(y) * size_t(w)];
            float* dst = &tmp[size_t(y) * size_t(w)];
            double acc = 0;
            int n = 0;
            for (int x = -r; x <= r; ++x)
                if (x >= 0 && x < w) {
                    acc += src[x];
                    ++n;
                }
            for (int x = 0; x < w; ++x) {
                dst[x] = float(acc / n);
                const int out = x - r, in = x + r + 1;
                if (out >= 0) {
                    acc -= src[out];
                    --n;
                }
                if (in < w) {
                    acc += src[in];
                    ++n;
                }
            }
        }
        for (int x = 0; x < w; ++x) {
            double acc = 0;
            int n = 0;
            for (int y = -r; y <= r; ++y)
                if (y >= 0 && y < h) {
                    acc += tmp[size_t(y) * size_t(w) + size_t(x)];
                    ++n;
                }
            for (int y = 0; y < h; ++y) {
                v[size_t(y) * size_t(w) + size_t(x)] = float(acc / n);
                const int out = y - r, in = y + r + 1;
                if (out >= 0) {
                    acc -= tmp[size_t(out) * size_t(w) + size_t(x)];
                    --n;
                }
                if (in < h) {
                    acc += tmp[size_t(in) * size_t(w) + size_t(x)];
                    ++n;
                }
            }
        }
    }
}

// Scales a map so its 99th percentile is 1 (and clamps), unless it is flat.
void normalise(std::vector<float>& v) {
    if (v.empty()) return;
    std::vector<float> s = v;
    auto it = s.begin() + std::ptrdiff_t(double(s.size() - 1) * 0.99);
    std::nth_element(s.begin(), it, s.end());
    const float top = *it;
    if (top < 1e-4f) {
        std::fill(v.begin(), v.end(), 0.f);
        return;
    }
    for (float& x : v) x = std::min(1.f, x / top);
}

GrayImage grayOf(const Image& img) {
    GrayImage g;
    g.width = img.width;
    g.height = img.height;
    g.px.resize(size_t(img.width) * size_t(img.height));
    for (size_t i = 0; i < g.px.size(); ++i) {
        const float* p = &img.px[i * 4];
        g.px[i] = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
    }
    return g;
}

}  // namespace

SubjectPoint frameSubject(const Image& frame, const Image& previous) {
    SubjectPoint out;
    const int w = frame.width, h = frame.height;
    if (w < 8 || h < 8) return out;
    const size_t n = size_t(w) * size_t(h);
    // Centre-surround contrast in opponent channels: brightness, red-green and yellow-blue.
    std::vector<float> contrast(n, 0.f);
    {
        std::vector<float> fine[3], coarse[3];
        for (auto& c : fine) c.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const float* p = &frame.px[i * 4];
            fine[0][i] = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            fine[1][i] = p[0] - p[1];
            fine[2][i] = 0.5f * (p[0] + p[1]) - p[2];
        }
        for (int c = 0; c < 3; ++c) {
            coarse[c] = fine[c];
            blur(fine[c], w, h, 1);
            blur(coarse[c], w, h, std::max(2, w / 10));
        }
        for (size_t i = 0; i < n; ++i) {
            double d = 0;
            for (int c = 0; c < 3; ++c) d += double(fine[c][i] - coarse[c][i]) * double(fine[c][i] - coarse[c][i]);
            contrast[i] = float(std::sqrt(d));
        }
        blur(contrast, w, h, 2);
        normalise(contrast);
    }
    // What moves once the camera's own motion is taken out.
    std::vector<float> motion;
    if (previous.width == w && previous.height == h) {
        const GrayImage a = grayOf(previous), b = grayOf(frame);
        Similarity m;  // previous -> this frame
        const auto pts = goodFeatures(a, 200, 5);
        std::vector<Point2> moved;
        std::vector<bool> ok;
        trackPoints(a, b, pts, moved, ok);
        std::vector<Point2> fa, fb;
        for (size_t k = 0; k < pts.size(); ++k)
            if (ok[k]) {
                fa.push_back(pts[k]);
                fb.push_back(moved[k]);
            }
        if (fa.size() < 6 || !fitMotion(fa, fb, MotionModel::Similarity, m)) m = Similarity{};
        // Where each pixel was in the previous frame under the camera's motion.
        const double c = std::cos(-m.angle) / m.scale, s = std::sin(-m.angle) / m.scale;
        motion.resize(n);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const double dx = x - m.tx, dy = y - m.ty;
                const double px = c * dx - s * dy, py = s * dx + c * dy;
                const bool inside = px >= 0 && py >= 0 && px <= w - 1 && py <= h - 1;
                motion[size_t(y) * size_t(w) + size_t(x)] = inside ? std::fabs(b.at(x, y) - a.sample(px, py)) : 0.f;
            }
        blur(motion, w, h, 2);
        normalise(motion);
    }
    // Together, with a mild pull towards the middle of the frame.
    std::vector<float> sal(n);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const double ux = (x + 0.5) / w - 0.5, uy = (y + 0.5) / h - 0.5;
            const double prior = 0.6 + 0.4 * std::exp(-(ux * ux + uy * uy) / (2 * 0.3 * 0.3));
            sal[i] = float((contrast[i] + (motion.empty() ? 0.0 : 1.5 * motion[i])) * prior);
        }
    // The centre of the most salient tenth.
    std::vector<float> sorted = sal;
    auto cut = sorted.begin() + std::ptrdiff_t(double(n - 1) * 0.9);
    std::nth_element(sorted.begin(), cut, sorted.end());
    const float thr = *cut;
    double sx = 0, sy = 0, sw = 0, top = 0, all = 0;
    int topN = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = sal[size_t(y) * size_t(w) + size_t(x)];
            all += v;
            if (v < thr || v <= 0) continue;
            const double wt = double(v) * v;
            sx += wt * (x + 0.5);
            sy += wt * (y + 0.5);
            sw += wt;
            top += v;
            ++topN;
        }
    if (sw <= 0) return out;
    out.x = sx / sw / w;
    out.y = sy / sw / h;
    // How much the top stands out from the frame as a whole.
    const double ratio = (top / topN) / std::max(1e-6, all / double(n));
    out.weight = std::clamp((ratio - 1.5) / 4.0, 0.05, 1.0);
    return out;
}

std::vector<SubjectPoint> findSubject(const std::string& path, double from, double to, double step,
                                      const ReframeProgress& progress, const std::atomic<bool>* cancel,
                                      std::string* error) {
    VideoDecoder dec;
    if (!dec.open(path, error)) return {};
    const int mw = std::max(1, dec.displayWidth()), mh = std::max(1, dec.displayHeight());
    const int w = std::min(kAnalysisWidth, mw), h = std::max(8, int(std::lround(double(w) * mh / mw)));
    step = std::max(1e-3, step);
    const double dir = to >= from ? 1 : -1;
    std::vector<double> times;
    for (int i = 0; from + dir * i * step <= std::max(from, to) + 1e-9 && from + dir * i * step >= std::min(from, to) - 1e-9; ++i)
        times.push_back(from + dir * i * step);
    if (std::fabs(times.back() - to) > step * 0.25) times.push_back(to);  // always the last frame too
    const int count = int(times.size());
    std::vector<SubjectPoint> out;
    Image prev;
    for (int i = 0; i < count; ++i) {
        if (cancel && cancel->load()) break;
        const double t = times[size_t(i)];
        Frame16Ptr f = dec.frameAt(t, w, h);
        if (!f) break;
        Image img = toImage(*f);
        SubjectPoint p = frameSubject(img, prev);
        p.t = t;
        out.push_back(p);
        prev = std::move(img);
        if (progress) progress(double(i + 1) / count);
    }
    if (out.empty() && error && error->empty()) *error = "Cannot read the clip's frames";
    return out;
}

std::vector<SubjectPoint> smoothSubjectPath(const std::vector<SubjectPoint>& points, double smoothSeconds, double still) {
    std::vector<SubjectPoint> out = points;
    if (points.size() < 2) return out;
    const double sigma = std::max(1e-3, smoothSeconds);
    // A weighted local line through the neighbours (not a plain average), so steady
    // movement keeps its full extent at the ends of the shot instead of being pulled in.
    for (size_t i = 0; i < points.size(); ++i) {
        double s0 = 0, s1 = 0, s2 = 0, sx = 0, sy = 0, sxt = 0, syt = 0;
        for (const SubjectPoint& q : points) {
            const double dt = q.t - points[i].t, d = dt / sigma;
            if (std::fabs(d) > 3) continue;
            const double wt = std::exp(-0.5 * d * d) * std::max(0.05, q.weight);
            s0 += wt;
            s1 += wt * dt;
            s2 += wt * dt * dt;
            sx += wt * q.x;
            sy += wt * q.y;
            sxt += wt * q.x * dt;
            syt += wt * q.y * dt;
        }
        if (s0 <= 0) continue;
        const double den = s0 * s2 - s1 * s1;
        if (den > 1e-12 * s0 * s0) {
            out[i].x = (s2 * sx - s1 * sxt) / den;  // the line's value at this point
            out[i].y = (s2 * sy - s1 * syt) / den;
        } else {
            out[i].x = sx / s0;
            out[i].y = sy / s0;
        }
        out[i].x = std::clamp(out[i].x, 0.0, 1.0);
        out[i].y = std::clamp(out[i].y, 0.0, 1.0);
    }
    // Barely moving: a locked-off frame looks better than drifting.
    for (double SubjectPoint::*axis : {&SubjectPoint::x, &SubjectPoint::y}) {
        double lo = 1e9, hi = -1e9, mean = 0, ws = 0;
        for (const SubjectPoint& p : out) {
            lo = std::min(lo, p.*axis);
            hi = std::max(hi, p.*axis);
            mean += p.*axis * std::max(0.05, p.weight);
            ws += std::max(0.05, p.weight);
        }
        if (hi - lo < still)
            for (SubjectPoint& p : out) p.*axis = mean / ws;
    }
    return out;
}

}  // namespace montage
