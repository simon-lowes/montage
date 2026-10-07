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

// One point through the pyramid from `a` to `b`, starting from `guess` (full-resolution displacement).
bool lkPoint(const std::vector<GrayImage>& A, const std::vector<GrayImage>& B, Point2 p, Point2& out) {
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
        for (int dy = -kWin; dy <= kWin; ++dy)
            for (int dx = -kWin; dx <= kWin; ++dx, ++n) {
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
        for (int it = 0; it < kIterations; ++it) {
            double bx = 0, by = 0;
            n = 0;
            for (int dy = -kWin; dy <= kWin; ++dy)
                for (int dx = -kWin; dx <= kWin; ++dx, ++n) {
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
