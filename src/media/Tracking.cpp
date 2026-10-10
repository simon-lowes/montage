#include "Tracking.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <sstream>

#include "Decoder.h"

namespace montage {

namespace {

constexpr int kLevels = 3;       // pyramid levels for Lucas–Kanade
constexpr int kWin = 7;          // half window (15 x 15)
constexpr int kIterations = 20;
constexpr double kFbError = 1.0;  // forward-backward tolerance, px
constexpr int kAnalysisWidth = 640;

GrayImage downsample(const GrayImage& s) {
    GrayImage d;
    d.width = std::max(1, s.width / 2);
    d.height = std::max(1, s.height / 2);
    d.px.resize(size_t(d.width) * size_t(d.height));
    for (int y = 0; y < d.height; ++y)
        for (int x = 0; x < d.width; ++x) {
            const int x0 = std::min(2 * x, s.width - 1), x1 = std::min(2 * x + 1, s.width - 1);
            const int y0 = std::min(2 * y, s.height - 1), y1 = std::min(2 * y + 1, s.height - 1);
            d.px[size_t(y) * size_t(d.width) + size_t(x)] = 0.25f * (s.at(x0, y0) + s.at(x1, y0) + s.at(x0, y1) + s.at(x1, y1));
        }
    return d;
}

std::vector<GrayImage> pyramid(const GrayImage& img) {
    std::vector<GrayImage> p{img};
    for (int l = 1; l < kLevels && p.back().width > 32 && p.back().height > 32; ++l) p.push_back(downsample(p.back()));
    return p;
}

// One point through the pyramid from `a` to `b` (window half-size `win` <= kWin).
bool lkPoint(const std::vector<GrayImage>& A, const std::vector<GrayImage>& B, Point2 p, Point2& out, int win = kWin,
             int iterations = kIterations) {
    double gx = 0, gy = 0;  // displacement at the current level
    for (int l = int(A.size()) - 1; l >= 0; --l) {
        const GrayImage& a = A[size_t(l)];
        const GrayImage& b = B[size_t(l)];
        const double k = 1.0 / double(1 << l);
        const double px = p.x * k, py = p.y * k;
        if (l < int(A.size()) - 1) {
            gx *= 2;
            gy *= 2;
        }
        // Template gradients and the structure tensor.
        double gxx = 0, gxy = 0, gyy = 0;
        float tpl[(2 * kWin + 1) * (2 * kWin + 1)], ix[(2 * kWin + 1) * (2 * kWin + 1)], iy[(2 * kWin + 1) * (2 * kWin + 1)];
        int n = 0;
        for (int dy = -win; dy <= win; ++dy)
            for (int dx = -win; dx <= win; ++dx, ++n) {
                const double x = px + dx, y = py + dy;
                tpl[n] = a.sample(x, y);
                ix[n] = 0.5f * (a.sample(x + 1, y) - a.sample(x - 1, y));
                iy[n] = 0.5f * (a.sample(x, y + 1) - a.sample(x, y - 1));
                gxx += double(ix[n]) * ix[n];
                gxy += double(ix[n]) * iy[n];
                gyy += double(iy[n]) * iy[n];
            }
        const double det = gxx * gyy - gxy * gxy;
        if (det < 1e-9) return false;
        for (int it = 0; it < iterations; ++it) {
            double bx = 0, by = 0;
            n = 0;
            for (int dy = -win; dy <= win; ++dy)
                for (int dx = -win; dx <= win; ++dx, ++n) {
                    const double diff = double(tpl[n]) - b.sample(px + dx + gx, py + dy + gy);
                    bx += diff * ix[n];
                    by += diff * iy[n];
                }
            const double vx = (gyy * bx - gxy * by) / det, vy = (gxx * by - gxy * bx) / det;
            gx += vx;
            gy += vy;
            if (vx * vx + vy * vy < 1e-4) break;
        }
        if (!std::isfinite(gx) || !std::isfinite(gy)) return false;
    }
    out = {p.x + gx, p.y + gy};
    const GrayImage& b0 = B.front();
    return out.x >= 0 && out.y >= 0 && out.x < b0.width && out.y < b0.height;
}

// Frames for analysis: decoded small, as luma.
struct FrameSource {
    VideoDecoder dec;
    double fps = 25;
    int w = 0, h = 0;
    bool open(const std::string& path, std::string* error) {
        if (!dec.open(path, error)) return false;
        fps = dec.fps() > 0 ? dec.fps() : 25;
        const int mw = std::max(1, dec.displayWidth()), mh = std::max(1, dec.displayHeight());
        w = std::min(kAnalysisWidth, mw);
        h = std::max(16, int(std::lround(double(w) * mh / mw)));
        return true;
    }
    bool frame(double t, GrayImage& out) {
        Frame16Ptr f = dec.frameAt(t, w, h);
        if (!f) return false;
        out = toGray(*f);
        return true;
    }
};

void meanPoint(const std::vector<Point2>& pts, const std::vector<int>& idx, double& mx, double& my) {
    mx = my = 0;
    for (int i : idx) {
        mx += pts[size_t(i)].x;
        my += pts[size_t(i)].y;
    }
    mx /= double(idx.size());
    my /= double(idx.size());
}

// Least-squares fit of the model to the chosen pairs.
Similarity solve(const std::vector<Point2>& a, const std::vector<Point2>& b, const std::vector<int>& idx, MotionModel model) {
    double ax, ay, bx, by;
    meanPoint(a, idx, ax, ay);
    meanPoint(b, idx, bx, by);
    Similarity s;
    if (model == MotionModel::Translation || idx.size() < 2) {
        s.tx = bx - ax;
        s.ty = by - ay;
        return s;
    }
    double num = 0, cross = 0, den = 0;
    for (int i : idx) {
        const double x = a[size_t(i)].x - ax, y = a[size_t(i)].y - ay;
        const double u = b[size_t(i)].x - bx, v = b[size_t(i)].y - by;
        num += x * u + y * v;
        cross += x * v - y * u;
        den += x * x + y * y;
    }
    if (den < 1e-12) {
        s.tx = bx - ax;
        s.ty = by - ay;
        return s;
    }
    double c = num / den, d = model == MotionModel::Similarity ? cross / den : 0;
    s.scale = std::sqrt(c * c + d * d);
    s.angle = std::atan2(d, c);
    // t = mean(b) - sR * mean(a)
    s.tx = bx - (c * ax - d * ay);
    s.ty = by - (d * ax + c * ay);
    return s;
}

}  // namespace

float GrayImage::sample(double x, double y) const {
    x = std::clamp(x, 0.0, double(width - 1));
    y = std::clamp(y, 0.0, double(height - 1));
    const int x0 = int(x), y0 = int(y);
    const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    const float fx = float(x - x0), fy = float(y - y0);
    const float a = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
    const float b = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
    return a + (b - a) * fy;
}

GrayImage toGray(const Frame16& f) {
    GrayImage g;
    g.width = f.width;
    g.height = f.height;
    g.px.resize(size_t(f.width) * size_t(f.height));
    const float k = 1.0f / 65535.0f;
    for (size_t i = 0; i < g.px.size(); ++i) {
        const uint16_t* p = &f.px[i * 4];
        g.px[i] = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) * k;
    }
    return g;
}

std::vector<Point2> goodFeatures(const GrayImage& img, int maxCorners, double minDistance, double x0, double y0, double x1,
                                 double y1) {
    const int W = img.width, H = img.height;
    if (x1 <= x0 || y1 <= y0) {
        x0 = y0 = 0;
        x1 = W;
        y1 = H;
    }
    const int bx0 = std::max(kWin + 2, int(x0)), by0 = std::max(kWin + 2, int(y0));
    const int bx1 = std::min(W - kWin - 2, int(std::ceil(x1))), by1 = std::min(H - kWin - 2, int(std::ceil(y1)));
    if (bx1 <= bx0 || by1 <= by0) return {};
    // Minimum eigenvalue of the structure tensor over a 5 x 5 window.
    const int rw = bx1 - bx0, rh = by1 - by0;
    std::vector<float> ixx(size_t(rw) * rh), ixy(size_t(rw) * rh), iyy(size_t(rw) * rh), score(size_t(rw) * rh, 0.0f);
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x) {
            const int X = bx0 + x, Y = by0 + y;
            const float gx = 0.5f * (img.at(X + 1, Y) - img.at(X - 1, Y));
            const float gy = 0.5f * (img.at(X, Y + 1) - img.at(X, Y - 1));
            ixx[size_t(y) * rw + x] = gx * gx;
            ixy[size_t(y) * rw + x] = gx * gy;
            iyy[size_t(y) * rw + x] = gy * gy;
        }
    float best = 0;
    for (int y = 2; y < rh - 2; ++y)
        for (int x = 2; x < rw - 2; ++x) {
            double a = 0, b = 0, c = 0;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const size_t i = size_t(y + dy) * rw + size_t(x + dx);
                    a += ixx[i];
                    b += ixy[i];
                    c += iyy[i];
                }
            const float l = float((a + c) / 2 - std::sqrt((a - c) * (a - c) / 4 + b * b));
            score[size_t(y) * rw + x] = l;
            best = std::max(best, l);
        }
    if (best <= 1e-8f) return {};
    struct Cand {
        float s;
        int x, y;
    };
    std::vector<Cand> cands;
    const float thr = best * 0.01f;
    for (int y = 3; y < rh - 3; ++y)
        for (int x = 3; x < rw - 3; ++x) {
            const float s = score[size_t(y) * rw + x];
            if (s < thr) continue;
            bool peak = true;
            for (int dy = -1; dy <= 1 && peak; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if ((dx || dy) && score[size_t(y + dy) * rw + size_t(x + dx)] > s) {
                        peak = false;
                        break;
                    }
            if (peak) cands.push_back({s, bx0 + x, by0 + y});
        }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.s > b.s; });
    // Keep the strongest, at least minDistance apart (grid buckets).
    const double cell = std::max(1.0, minDistance);
    const int gw = int(W / cell) + 1, gh = int(H / cell) + 1;
    std::vector<std::vector<Point2>> grid(size_t(gw) * gh);
    std::vector<Point2> out;
    for (const Cand& c : cands) {
        const int gx = int(c.x / cell), gy = int(c.y / cell);
        bool near = false;
        for (int yy = std::max(0, gy - 1); yy <= std::min(gh - 1, gy + 1) && !near; ++yy)
            for (int xx = std::max(0, gx - 1); xx <= std::min(gw - 1, gx + 1) && !near; ++xx)
                for (const Point2& q : grid[size_t(yy) * gw + xx])
                    if ((q.x - c.x) * (q.x - c.x) + (q.y - c.y) * (q.y - c.y) < minDistance * minDistance) {
                        near = true;
                        break;
                    }
        if (near) continue;
        grid[size_t(gy) * gw + gx].push_back({double(c.x), double(c.y)});
        out.push_back({double(c.x), double(c.y)});
        if (int(out.size()) >= maxCorners) break;
    }
    return out;
}

void trackPoints(const GrayImage& a, const GrayImage& b, const std::vector<Point2>& from, std::vector<Point2>& to,
                 std::vector<bool>& ok) {
    const auto A = pyramid(a), B = pyramid(b);
    to.assign(from.size(), {});
    ok.assign(from.size(), false);
    for (size_t i = 0; i < from.size(); ++i) {
        Point2 fwd, back;
        if (!lkPoint(A, B, from[i], fwd) || !lkPoint(B, A, fwd, back)) continue;
        const double e = std::hypot(back.x - from[i].x, back.y - from[i].y);
        to[i] = fwd;
        ok[i] = e < kFbError;
    }
}

FlowField denseFlow(const GrayImage& a, const GrayImage& b, int step) {
    FlowField f;
    f.step = std::max(1, step);
    f.gw = a.width / f.step + 1;
    f.gh = a.height / f.step + 1;
    f.v.assign(size_t(f.gw) * f.gh, {});
    std::vector<char> known(f.v.size(), 0);
    const auto A = pyramid(a), B = pyramid(b);
    parallelRows(f.gh, [&](int y0, int y1) {
        for (int gy = y0; gy < y1; ++gy)
            for (int gx = 0; gx < f.gw; ++gx) {
                const Point2 p{double(std::min(gx * f.step, a.width - 1)), double(std::min(gy * f.step, a.height - 1))};
                Point2 q;
                if (lkPoint(A, B, p, q, 5, 10)) {
                    f.v[size_t(gy) * f.gw + gx] = {q.x - p.x, q.y - p.y};
                    known[size_t(gy) * f.gw + gx] = 1;
                }
            }
    });
    // Fill untextured points from known neighbours, then smooth once.
    for (int pass = 0; pass < std::max(f.gw, f.gh); ++pass) {
        bool missing = false;
        std::vector<char> next = known;
        for (int gy = 0; gy < f.gh; ++gy)
            for (int gx = 0; gx < f.gw; ++gx) {
                const size_t i = size_t(gy) * f.gw + gx;
                if (known[i]) continue;
                double sx = 0, sy = 0;
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int x = gx + dx, y = gy + dy;
                        if (x < 0 || y < 0 || x >= f.gw || y >= f.gh || !known[size_t(y) * f.gw + x]) continue;
                        sx += f.v[size_t(y) * f.gw + x].x;
                        sy += f.v[size_t(y) * f.gw + x].y;
                        ++n;
                    }
                if (n) {
                    f.v[i] = {sx / n, sy / n};
                    next[i] = 1;
                } else {
                    missing = true;
                }
            }
        known.swap(next);
        if (!missing) break;
    }
    std::vector<Point2> sm = f.v;
    for (int gy = 0; gy < f.gh; ++gy)
        for (int gx = 0; gx < f.gw; ++gx) {
            double sx = 0, sy = 0;
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int x = std::clamp(gx + dx, 0, f.gw - 1), y = std::clamp(gy + dy, 0, f.gh - 1);
                    sx += f.v[size_t(y) * f.gw + x].x;
                    sy += f.v[size_t(y) * f.gw + x].y;
                    ++n;
                }
            sm[size_t(gy) * f.gw + gx] = {sx / n, sy / n};
        }
    f.v.swap(sm);
    return f;
}

Point2 FlowField::at(double x, double y) const {
    if (v.empty()) return {};
    const double gx = std::clamp(x / step, 0.0, double(gw - 1)), gy = std::clamp(y / step, 0.0, double(gh - 1));
    const int x0 = int(gx), y0 = int(gy), x1 = std::min(x0 + 1, gw - 1), y1 = std::min(y0 + 1, gh - 1);
    const double fx = gx - x0, fy = gy - y0;
    auto g = [&](int xx, int yy) { return v[size_t(yy) * gw + xx]; };
    const Point2 a = g(x0, y0), b = g(x1, y0), c = g(x0, y1), d = g(x1, y1);
    return {(a.x * (1 - fx) + b.x * fx) * (1 - fy) + (c.x * (1 - fx) + d.x * fx) * fy,
            (a.y * (1 - fx) + b.y * fx) * (1 - fy) + (c.y * (1 - fx) + d.y * fx) * fy};
}

GrayImage toGray(const Image& img, int maxWidth) {
    GrayImage g;
    const int k = std::max(1, int(std::ceil(double(img.width) / std::max(1, maxWidth))));
    g.width = std::max(1, img.width / k);
    g.height = std::max(1, img.height / k);
    g.px.resize(size_t(g.width) * g.height);
    for (int y = 0; y < g.height; ++y)
        for (int x = 0; x < g.width; ++x) {
            double sum = 0;
            for (int dy = 0; dy < k; ++dy)
                for (int dx = 0; dx < k; ++dx) {
                    const float* p = img.at(std::min(x * k + dx, img.width - 1), std::min(y * k + dy, img.height - 1));
                    sum += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
                }
            g.px[size_t(y) * g.width + x] = float(sum / (k * k));
        }
    return g;
}

Point2 Similarity::apply(Point2 p) const {
    const double c = scale * std::cos(angle), s = scale * std::sin(angle);
    return {c * p.x - s * p.y + tx, s * p.x + c * p.y + ty};
}

bool fitMotion(const std::vector<Point2>& from, const std::vector<Point2>& to, MotionModel model, Similarity& out,
               int* inliersOut) {
    const int n = int(std::min(from.size(), to.size()));
    const int need = model == MotionModel::Translation ? 1 : 2;
    if (n < need) return false;
    std::mt19937 rng(12345);  // deterministic: the same footage gives the same result
    std::uniform_int_distribution<int> pick(0, n - 1);
    const double thr = 1.5;
    std::vector<int> best;
    for (int it = 0; it < 200; ++it) {
        std::vector<int> sample = {pick(rng)};
        if (need == 2) {
            int j = pick(rng);
            for (int guard = 0; j == sample[0] && guard < 8; ++guard) j = pick(rng);
            if (j == sample[0]) continue;
            sample.push_back(j);
        }
        const Similarity m = solve(from, to, sample, model);
        std::vector<int> in;
        for (int i = 0; i < n; ++i) {
            const Point2 p = m.apply(from[size_t(i)]);
            if (std::hypot(p.x - to[size_t(i)].x, p.y - to[size_t(i)].y) < thr) in.push_back(i);
        }
        if (in.size() > best.size()) best = std::move(in);
        if (int(best.size()) == n) break;
    }
    if (int(best.size()) < std::max(need, std::min(n, 3))) return false;
    out = solve(from, to, best, model);
    if (inliersOut) *inliersOut = int(best.size());
    return std::isfinite(out.tx) && std::isfinite(out.ty) && std::isfinite(out.angle) && out.scale > 0.2 && out.scale < 5;
}

std::vector<TrackRegion> trackRegion(const std::string& path, double from, double to, const TrackRegion& start,
                                     MotionModel model, const TrackProgress& progress, const std::atomic<bool>* cancel,
                                     std::string* error) {
    FrameSource src;
    if (!src.open(path, error)) return {};
    std::vector<TrackRegion> out{start};
    const double dir = to >= from ? 1 : -1;
    const int frames = int(std::floor(std::fabs(to - from) * src.fps + 1e-6));
    GrayImage cur, next;
    if (!src.frame(from, cur)) {
        if (error) *error = "Cannot read the clip's frames";
        return {};
    }
    const double W = src.w, H = src.h;
    // Region in analysis pixels.
    double cx = start.x * W, cy = start.y * H, rw = start.w * W, rh = start.h * H, rot = start.rotation;
    for (int i = 1; i <= frames; ++i) {
        if (cancel && cancel->load()) break;
        if (!src.frame(from + dir * i / src.fps, next)) break;
        // Features inside the region's bounding box.
        const double r = std::hypot(rw, rh) / 2;
        const double ex = std::min(r, std::fabs(rw / 2 * std::cos(rot * M_PI / 180)) + std::fabs(rh / 2 * std::sin(rot * M_PI / 180)));
        const double ey = std::min(r, std::fabs(rw / 2 * std::sin(rot * M_PI / 180)) + std::fabs(rh / 2 * std::cos(rot * M_PI / 180)));
        const auto pts = goodFeatures(cur, 120, std::max(3.0, std::min(rw, rh) / 12), cx - ex, cy - ey, cx + ex, cy + ey);
        if (pts.size() < 3) break;  // nothing to hold on to
        std::vector<Point2> moved;
        std::vector<bool> ok;
        trackPoints(cur, next, pts, moved, ok);
        std::vector<Point2> a, b;
        for (size_t k = 0; k < pts.size(); ++k)
            if (ok[k]) {
                a.push_back(pts[k]);
                b.push_back(moved[k]);
            }
        Similarity m;
        if (a.size() < 3 || !fitMotion(a, b, model, m)) break;  // lost
        const Point2 c = m.apply({cx, cy});
        cx = c.x;
        cy = c.y;
        rw *= m.scale;
        rh *= m.scale;
        rot += m.angle * 180 / M_PI;
        out.push_back({cx / W, cy / H, rw / W, rh / H, rot});
        std::swap(cur, next);
        if (progress && frames > 0) progress(double(i) / frames);
    }
    return out;
}

// ---- Planar tracking -------------------------------------------------------------

Point2 Homography::apply(Point2 p) const {
    const double w = h[6] * p.x + h[7] * p.y + h[8];
    if (std::fabs(w) < 1e-12) return {1e12, 1e12};
    return {(h[0] * p.x + h[1] * p.y + h[2]) / w, (h[3] * p.x + h[4] * p.y + h[5]) / w};
}

namespace {

// Moves the points' centroid to the origin and their mean distance from it to
// sqrt(2), which keeps the least-squares system well conditioned (Hartley).
void normaliser(const std::vector<Point2>& pts, const std::vector<int>& idx, double& cx, double& cy, double& k) {
    meanPoint(pts, idx, cx, cy);
    double d = 0;
    for (int i : idx) d += std::hypot(pts[size_t(i)].x - cx, pts[size_t(i)].y - cy);
    d /= double(idx.size());
    k = d > 1e-12 ? std::sqrt(2.0) / d : 1.0;
}

// The homography through the chosen pairs (exact for 4, least squares for more).
bool solveHomography(const std::vector<Point2>& a, const std::vector<Point2>& b, const std::vector<int>& idx, Homography& out) {
    if (idx.size() < 4) return false;
    double ax, ay, ak, bx, by, bk;
    normaliser(a, idx, ax, ay, ak);
    normaliser(b, idx, bx, by, bk);
    // Normal equations of the DLT with h8 = 1: two rows per pair.
    double M[8][9] = {};
    for (int i : idx) {
        const double x = (a[size_t(i)].x - ax) * ak, y = (a[size_t(i)].y - ay) * ak;
        const double u = (b[size_t(i)].x - bx) * bk, v = (b[size_t(i)].y - by) * bk;
        const double r1[9] = {x, y, 1, 0, 0, 0, -x * u, -y * u, u};
        const double r2[9] = {0, 0, 0, x, y, 1, -x * v, -y * v, v};
        for (const double* r : {r1, r2})
            for (int j = 0; j < 8; ++j)
                for (int k = 0; k < 9; ++k) M[j][k] += r[j] * r[k];
    }
    // Gaussian elimination with partial pivoting.
    for (int c = 0; c < 8; ++c) {
        int piv = c;
        for (int r = c + 1; r < 8; ++r)
            if (std::fabs(M[r][c]) > std::fabs(M[piv][c])) piv = r;
        if (std::fabs(M[piv][c]) < 1e-12) return false;  // degenerate (three points in a line)
        if (piv != c)
            for (int k = 0; k < 9; ++k) std::swap(M[c][k], M[piv][k]);
        for (int r = 0; r < 8; ++r) {
            if (r == c) continue;
            const double f = M[r][c] / M[c][c];
            for (int k = c; k < 9; ++k) M[r][k] -= f * M[c][k];
        }
    }
    double hn[9];
    for (int j = 0; j < 8; ++j) hn[j] = M[j][8] / M[j][j];
    hn[8] = 1;
    // Undo the normalisations: H = Tb^-1 * Hn * Ta.
    const double Ta[9] = {ak, 0, -ak * ax, 0, ak, -ak * ay, 0, 0, 1};
    const double Tbi[9] = {1 / bk, 0, bx, 0, 1 / bk, by, 0, 0, 1};
    double t[9], h[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            t[r * 3 + c] = 0;
            for (int k = 0; k < 3; ++k) t[r * 3 + c] += hn[r * 3 + k] * Ta[k * 3 + c];
        }
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            h[r * 3 + c] = 0;
            for (int k = 0; k < 3; ++k) h[r * 3 + c] += Tbi[r * 3 + k] * t[k * 3 + c];
        }
    if (std::fabs(h[8]) < 1e-12) return false;
    for (int k = 0; k < 9; ++k) {
        out.h[k] = h[k] / h[8];
        if (!std::isfinite(out.h[k])) return false;
    }
    return true;
}

double quadArea(const Point2 q[4]) {
    double a = 0;
    for (int i = 0; i < 4; ++i) a += q[i].x * q[(i + 1) % 4].y - q[(i + 1) % 4].x * q[i].y;
    return a / 2;
}

bool insideQuad(const Point2 q[4], Point2 p) {
    bool in = false;  // even-odd crossings, so a bow-tie still works
    for (int i = 0, j = 3; i < 4; j = i++)
        if ((q[i].y > p.y) != (q[j].y > p.y) && p.x < (q[j].x - q[i].x) * (p.y - q[i].y) / (q[j].y - q[i].y) + q[i].x)
            in = !in;
    return in;
}

}  // namespace

bool fitHomography(const std::vector<Point2>& from, const std::vector<Point2>& to, Homography& out, int* inliersOut) {
    const int n = int(std::min(from.size(), to.size()));
    if (n < 4) return false;
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> pick(0, n - 1);
    const double thr = 1.5;
    auto inliers = [&](const Homography& H) {
        std::vector<int> in;
        for (int i = 0; i < n; ++i) {
            const Point2 p = H.apply(from[size_t(i)]);
            if (std::hypot(p.x - to[size_t(i)].x, p.y - to[size_t(i)].y) < thr) in.push_back(i);
        }
        return in;
    };
    std::vector<int> best;
    for (int it = 0; it < 400; ++it) {
        std::vector<int> sample;
        for (int guard = 0; sample.size() < 4 && guard < 32; ++guard) {
            const int j = pick(rng);
            if (std::find(sample.begin(), sample.end(), j) == sample.end()) sample.push_back(j);
        }
        Homography H;
        if (sample.size() < 4 || !solveHomography(from, to, sample, H)) continue;
        auto in = inliers(H);
        if (in.size() > best.size()) best = std::move(in);
        if (int(best.size()) == n) break;
    }
    if (int(best.size()) < std::max(4, std::min(n, 6))) return false;
    // Refine on the inliers, then once more on the inliers of the refined fit.
    Homography H;
    if (!solveHomography(from, to, best, H)) return false;
    auto again = inliers(H);
    if (again.size() >= best.size() && solveHomography(from, to, again, H)) best = std::move(again);
    out = H;
    if (inliersOut) *inliersOut = int(best.size());
    return true;
}

std::vector<TrackQuad> trackQuad(const std::string& path, double from, double to, const TrackQuad& start,
                                 const TrackProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    FrameSource src;
    if (!src.open(path, error)) return {};
    std::vector<TrackQuad> out{start};
    const double dir = to >= from ? 1 : -1;
    const int frames = int(std::floor(std::fabs(to - from) * src.fps + 1e-6));
    GrayImage cur, next;
    if (!src.frame(from, cur)) {
        if (error) *error = "Cannot read the clip's frames";
        return {};
    }
    const double W = src.w, H = src.h;
    Point2 q0[4], q[4];
    for (int k = 0; k < 4; ++k) q0[k] = q[k] = {start.p[k].x * W, start.p[k].y * H};
    const double area0 = std::fabs(quadArea(q0));
    if (area0 < 16) {
        if (error) *error = "The corners are too close together to track";
        return {};
    }
    // Every frame is fitted against the first (features' first-frame positions
    // to where they are now), so errors do not pile up from frame to frame and
    // points off the surface stand out more the further it moves.
    std::vector<Point2> ref, at;  // first-frame and current positions of the features followed
    Homography Hc;                // first frame -> current frame
    const double side = std::sqrt(area0);
    const double spacing = std::max(3.0, side / 16);
    auto seed = [&] {
        Point2 c{0, 0}, grown[4];
        for (const Point2& p : q) {
            c.x += p.x / 4;
            c.y += p.y / 4;
        }
        double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
        for (int k = 0; k < 4; ++k) {
            // The quad grown by a fifth about its centre: the surface and its edges (a blank screen's bezel).
            grown[k] = {c.x + (q[k].x - c.x) * 1.2, c.y + (q[k].y - c.y) * 1.2};
            x0 = std::min(x0, grown[k].x);
            y0 = std::min(y0, grown[k].y);
            x1 = std::max(x1, grown[k].x);
            y1 = std::max(y1, grown[k].y);
        }
        // Back to the first frame through the inverse (the adjugate) of Hc.
        const double* m = Hc.h;
        Homography inv;
        const double a[9] = {m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
                             m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
                             m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3]};
        std::copy(a, a + 9, inv.h);
        // The surface's own features; the margin only when the surface is too plain to hold (a blank screen).
        const auto found = goodFeatures(cur, 300, spacing, std::max(0.0, x0), std::max(0.0, y0), std::min(W, x1), std::min(H, y1));
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 1 && at.size() >= 20) break;
            for (const Point2& p : found) {
                if (pass == 0 ? !insideQuad(q, p) : (insideQuad(q, p) || !insideQuad(grown, p))) continue;
                bool taken = false;
                for (const Point2& o : at)
                    if (std::hypot(o.x - p.x, o.y - p.y) < spacing) {
                        taken = true;
                        break;
                    }
                if (taken) continue;
                at.push_back(p);
                ref.push_back(inv.apply(p));
            }
        }
    };
    seed();
    size_t wanted = at.size();
    for (int i = 1; i <= frames; ++i) {
        if (cancel && cancel->load()) break;
        if (!src.frame(from + dir * i / src.fps, next)) break;
        if (at.size() < std::max<size_t>(12, wanted * 2 / 3)) {
            seed();  // top up as features are lost or leave
            wanted = std::max(wanted, at.size());
        }
        if (at.size() < 6) {
            if (error && out.size() == 1) *error = "Nothing to track: put the corners on a surface with some detail";
            break;
        }
        std::vector<Point2> moved;
        std::vector<bool> ok;
        trackPoints(cur, next, at, moved, ok);
        std::vector<Point2> r, b;
        for (size_t k = 0; k < at.size(); ++k)
            if (ok[k]) {
                r.push_back(ref[k]);
                b.push_back(moved[k]);
            }
        Homography Hm;
        if (b.size() < 6 || !fitHomography(r, b, Hm)) break;  // lost
        Point2 nq[4];
        for (int k = 0; k < 4; ++k) nq[k] = Hm.apply(q0[k]);
        // A surface does not flip over or change size by half in one frame: that is a bad fit.
        const double area = quadArea(nq), was = quadArea(q);
        if (area * was <= 0 || std::fabs(area) > 2 * std::fabs(was) || std::fabs(area) < 0.5 * std::fabs(was)) break;
        // Points well off the fit are not on the surface (or have slipped): stop following them.
        ref.clear();
        at.clear();
        for (size_t k = 0; k < b.size(); ++k) {
            const Point2 e = Hm.apply(r[k]);
            if (std::hypot(e.x - b[k].x, e.y - b[k].y) < 3.0) {
                ref.push_back(r[k]);
                at.push_back(b[k]);
            }
        }
        Hc = Hm;
        TrackQuad t;
        for (int k = 0; k < 4; ++k) {
            q[k] = nq[k];
            t.p[k] = {q[k].x / W, q[k].y / H};
        }
        out.push_back(t);
        std::swap(cur, next);
        if (progress && frames > 0) progress(double(i) / frames);
    }
    return out;
}

std::string cameraMotionToString(const CameraMotion& m) {
    std::ostringstream o;
    o.precision(9);
    o << "v1 " << m.fps << ' ' << m.start << ' ' << m.steps.size();
    for (const Similarity& s : m.steps) o << ' ' << s.tx << ' ' << s.ty << ' ' << s.angle << ' ' << s.scale;
    return o.str();
}

bool cameraMotionFromString(const std::string& s, CameraMotion& m) {
    std::istringstream in(s);
    std::string tag;
    size_t n = 0;
    if (!(in >> tag >> m.fps >> m.start >> n) || tag != "v1" || m.fps <= 0 || n > 10000000) return false;
    m.steps.assign(n, {});
    for (Similarity& st : m.steps)
        if (!(in >> st.tx >> st.ty >> st.angle >> st.scale)) return false;
    return true;
}

CameraMotion analyzeCameraMotion(const std::string& path, double from, double to, const TrackProgress& progress,
                                 const std::atomic<bool>* cancel, std::string* error) {
    CameraMotion m;
    FrameSource src;
    if (!src.open(path, error)) return m;
    m.fps = src.fps;
    m.start = from;
    const int frames = std::max(1, int(std::floor((to - from) * src.fps + 1e-6)));
    GrayImage cur, next;
    if (!src.frame(from, cur)) {
        if (error) *error = "Cannot read the clip's frames";
        return m;
    }
    const double W = src.w;
    m.steps.push_back({});
    for (int i = 1; i < frames; ++i) {
        if (cancel && cancel->load()) {
            m.steps.clear();
            if (error) *error = "Cancelled";
            return m;
        }
        Similarity step;
        if (src.frame(from + i / src.fps, next)) {
            const auto pts = goodFeatures(cur, 300, W / 40);
            std::vector<Point2> moved;
            std::vector<bool> ok;
            trackPoints(cur, next, pts, moved, ok);
            std::vector<Point2> a, b;
            // About the frame centre, in fractions of the width.
            const double ox = cur.width / 2.0, oy = cur.height / 2.0;
            for (size_t k = 0; k < pts.size(); ++k)
                if (ok[k]) {
                    a.push_back({(pts[k].x - ox), (pts[k].y - oy)});
                    b.push_back({(moved[k].x - ox), (moved[k].y - oy)});
                }
            Similarity s;
            if (fitMotion(a, b, MotionModel::Similarity, s)) {
                step = s;
                step.tx /= W;
                step.ty /= W;
            }
            std::swap(cur, next);
        }
        m.steps.push_back(step);
        if (progress) progress(double(i) / frames);
    }
    return m;
}

std::vector<Similarity> stabilizationCorrections(const CameraMotion& m, double smoothSeconds, MotionModel model) {
    const size_t n = m.steps.size();
    std::vector<Similarity> out(n);
    if (n == 0) return out;
    // The camera path: accumulated position, rotation and log scale.
    std::vector<double> px(n), py(n), pa(n), ps(n);
    for (size_t i = 0; i < n; ++i) {
        const Similarity& s = m.steps[i];
        px[i] = (i ? px[i - 1] : 0) + s.tx;
        py[i] = (i ? py[i - 1] : 0) + s.ty;
        pa[i] = (i ? pa[i - 1] : 0) + (model == MotionModel::Similarity ? s.angle : 0);
        ps[i] = (i ? ps[i - 1] : 0) + (model != MotionModel::Translation ? std::log(s.scale) : 0);
    }
    // Smoothed with a Gaussian (edges mirrored); 0 seconds holds the first frame's framing.
    auto smooth = [&](const std::vector<double>& v) {
        std::vector<double> r(n);
        const double sigma = smoothSeconds * m.fps / 2;
        if (sigma <= 0.01) {
            std::fill(r.begin(), r.end(), v[0]);
            return r;
        }
        const int radius = int(std::ceil(3 * sigma));
        for (size_t i = 0; i < n; ++i) {
            double sum = 0, wsum = 0;
            for (int k = -radius; k <= radius; ++k) {
                long j = long(i) + k;
                if (j < 0) j = -j;
                if (j >= long(n)) j = 2 * long(n) - 2 - j;
                j = std::clamp<long>(j, 0, long(n) - 1);
                const double w = std::exp(-0.5 * k * k / (sigma * sigma));
                sum += v[size_t(j)] * w;
                wsum += w;
            }
            r[i] = sum / wsum;
        }
        return r;
    };
    const auto sx = smooth(px), sy = smooth(py), sa = smooth(pa), ss = smooth(ps);
    for (size_t i = 0; i < n; ++i) {
        out[i].tx = sx[i] - px[i];
        out[i].ty = sy[i] - py[i];
        out[i].angle = sa[i] - pa[i];
        out[i].scale = std::exp(ss[i] - ps[i]);
    }
    return out;
}

double stabilizationZoom(const std::vector<Similarity>& corrections, double aspect) {
    // The output frame (centred, width 1) zoomed by z must map, through the
    // inverse correction, inside the source frame for every frame.
    const double hw = 0.5, hh = aspect / 2;
    auto covered = [&](const Similarity& c, double z) {
        const double co = std::cos(-c.angle) / c.scale, si = std::sin(-c.angle) / c.scale;
        for (double sx : {-1.0, 1.0})
            for (double sy : {-1.0, 1.0}) {
                const double qx = sx * hw / z - c.tx, qy = sy * hh / z - c.ty;
                const double ux = co * qx - si * qy, uy = si * qx + co * qy;
                if (std::fabs(ux) > hw + 1e-9 || std::fabs(uy) > hh + 1e-9) return false;
            }
        return true;
    };
    double zoom = 1;
    for (const Similarity& c : corrections) {
        if (covered(c, zoom)) continue;
        double lo = zoom, hi = 4;
        if (!covered(c, hi)) return 4;
        for (int it = 0; it < 40; ++it) {
            const double mid = (lo + hi) / 2;
            (covered(c, mid) ? hi : lo) = mid;
        }
        zoom = hi;
    }
    return zoom;
}

}  // namespace montage
