#include "MaskPath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace montage {

namespace {

const char* const kComponents[6] = {"x", "y", "ix", "iy", "ox", "oy"};

std::string paramName(int i, int component) { return "mask.p" + std::to_string(i) + "." + kComponents[component]; }

double& component(PathPoint& p, int c) {
    double* f[6] = {&p.x, &p.y, &p.ix, &p.iy, &p.ox, &p.oy};
    return *f[c];
}
double component(const PathPoint& p, int c) { return component(const_cast<PathPoint&>(p), c); }

void erasePathParams(Effect& e) {
    for (auto it = e.params.begin(); it != e.params.end();)
        it = isMaskPathParam(it->first) ? e.params.erase(it) : std::next(it);
}

double cubic(double p0, double p1, double p2, double p3, double s) {
    const double u = 1 - s;
    return u * u * u * p0 + 3 * u * u * s * p1 + 3 * u * s * s * p2 + s * s * s * p3;
}

double lerp(double a, double b, double s) { return a + (b - a) * s; }

}  // namespace

MaskBox maskBox(const Effect& e, FrameTime t) {
    return {e.p("mask.x", t, 0.5), e.p("mask.y", t, 0.5), e.p("mask.w", t, 0.4), e.p("mask.h", t, 0.4), e.p("mask.rotation", t)};
}

bool isMaskPathParam(const std::string& name) {
    if (name.rfind("mask.p", 0) != 0) return false;
    size_t i = 6;
    const size_t digits = i;
    while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) ++i;
    if (i == digits || i >= name.size() || name[i] != '.') return false;
    const std::string c = name.substr(i + 1);
    return std::any_of(std::begin(kComponents), std::end(kComponents), [&c](const char* k) { return c == k; });
}

int maskPathCount(const Effect& e) {
    int n = 0;
    while (e.params.count(paramName(n, 0))) ++n;
    return n;
}

std::vector<std::string> maskPathParams(const Effect& e) {
    std::vector<std::string> out;
    const int n = maskPathCount(e);
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 6; ++c) out.push_back(paramName(i, c));
    return out;
}

std::vector<PathPoint> maskPath(const Effect& e, FrameTime t) {
    std::vector<PathPoint> pts(size_t(maskPathCount(e)));
    for (size_t i = 0; i < pts.size(); ++i)
        for (int c = 0; c < 6; ++c) component(pts[i], c) = e.p(paramName(int(i), c), t);
    return pts;
}

bool maskPathAnimated(const Effect& e) {
    auto it = e.params.find(kMaskPathParam);
    return it != e.params.end() && it->second.animated();
}

std::vector<FrameTime> maskPathKeyTimes(const Effect& e) {
    std::set<FrameTime> times;
    for (const auto& [name, p] : e.params)
        if (isMaskPathParam(name))
            for (const Keyframe& k : p.keys) times.insert(k.t);
    return {times.begin(), times.end()};
}

void setMaskPath(Effect& e, FrameTime t, const std::vector<PathPoint>& pts) {
    const bool animated = maskPathAnimated(e);
    for (int i = int(pts.size()), n = maskPathCount(e); i < n; ++i)
        for (int c = 0; c < 6; ++c) e.params.erase(paramName(i, c));
    for (size_t i = 0; i < pts.size(); ++i)
        for (int c = 0; c < 6; ++c) {
            Param& p = e.params[paramName(int(i), c)];
            if (animated && !p.animated()) p.addKey(t, component(pts[i], c));
            else p.set(t, component(pts[i], c));
        }
}

void setMaskPathAnimated(Effect& e, FrameTime t, bool on) {
    for (const std::string& name : maskPathParams(e)) {
        Param& p = e.params[name];
        if (on) {
            if (!p.keyAt(t)) p.addKey(t, p.at(t));
        } else {
            const double v = p.at(t);
            p.keys.clear();
            p.value = v;
        }
    }
}

void editMaskPath(Effect& e, FrameTime t, const std::function<void(std::vector<PathPoint>&)>& fn) {
    if (!maskPathAnimated(e)) {
        std::vector<PathPoint> pts = maskPath(e, t);
        fn(pts);
        erasePathParams(e);
        setMaskPath(e, t, pts);
        return;
    }
    struct Snapshot {
        FrameTime t;
        Interp interp;
        std::vector<PathPoint> pts;
    };
    std::vector<Snapshot> keys;
    const Param& rep = e.params.at(kMaskPathParam);
    for (FrameTime k : maskPathKeyTimes(e)) {
        const Keyframe* kf = rep.keyAt(k);
        keys.push_back({k, kf ? kf->interp : Interp::Linear, maskPath(e, k)});
        fn(keys.back().pts);
    }
    erasePathParams(e);
    for (const Snapshot& s : keys)
        for (size_t i = 0; i < s.pts.size(); ++i)
            for (int c = 0; c < 6; ++c) e.params[paramName(int(i), c)].addKey(s.t, component(s.pts[i], c), s.interp);
}

std::pair<double, double> segmentPoint(const PathPoint& a, const PathPoint& b, double s) {
    return {cubic(a.x, a.x + a.ox, b.x + b.ix, b.x, s), cubic(a.y, a.y + a.oy, b.y + b.iy, b.y, s)};
}

int insertMaskPoint(Effect& e, FrameTime t, int seg, double s) {
    const int n = maskPathCount(e);
    if (n < 2 || seg < 0 || seg >= n) return -1;
    s = std::clamp(s, 0.01, 0.99);
    editMaskPath(e, t, [seg, s](std::vector<PathPoint>& pts) {
        if (seg >= int(pts.size())) return;
        PathPoint& a = pts[size_t(seg)];
        PathPoint& b = pts[size_t(seg + 1) % pts.size()];
        PathPoint mid;
        if (!a.smooth() && !b.smooth()) {
            // A straight segment gets a corner on it.
            const auto [x, y] = segmentPoint(a, b, s);
            mid.x = x;
            mid.y = y;
        } else {
            // de Casteljau: the two halves trace the same curve.
            const double p0x = a.x, p0y = a.y, p1x = a.x + a.ox, p1y = a.y + a.oy;
            const double p2x = b.x + b.ix, p2y = b.y + b.iy, p3x = b.x, p3y = b.y;
            const double ax = lerp(p0x, p1x, s), ay = lerp(p0y, p1y, s);
            const double bx = lerp(p1x, p2x, s), by = lerp(p1y, p2y, s);
            const double cx = lerp(p2x, p3x, s), cy = lerp(p2y, p3y, s);
            const double dx = lerp(ax, bx, s), dy = lerp(ay, by, s);
            const double ex = lerp(bx, cx, s), ey = lerp(by, cy, s);
            mid.x = lerp(dx, ex, s);
            mid.y = lerp(dy, ey, s);
            mid.ix = dx - mid.x;
            mid.iy = dy - mid.y;
            mid.ox = ex - mid.x;
            mid.oy = ey - mid.y;
            a.ox = ax - p0x;
            a.oy = ay - p0y;
            b.ix = cx - p3x;
            b.iy = cy - p3y;
        }
        pts.insert(pts.begin() + seg + 1, mid);
    });
    return seg + 1;
}

bool removeMaskPoint(Effect& e, FrameTime t, int index) {
    if (index < 0 || index >= maskPathCount(e) || maskPathCount(e) <= 3) return false;
    editMaskPath(e, t, [index](std::vector<PathPoint>& pts) {
        if (index < int(pts.size())) pts.erase(pts.begin() + index);
    });
    return true;
}

void toggleMaskPointSmooth(Effect& e, FrameTime t, int index) {
    if (index < 0 || index >= maskPathCount(e)) return;
    const bool wasSmooth = maskPath(e, t)[size_t(index)].smooth();
    editMaskPath(e, t, [index, wasSmooth](std::vector<PathPoint>& pts) {
        PathPoint& p = pts[size_t(index)];
        if (wasSmooth) p.ix = p.iy = p.ox = p.oy = 0;
        else p = smoothed(pts, index);
    });
}

PathPoint smoothed(const std::vector<PathPoint>& pts, int i, bool closed) {
    const int n = int(pts.size());
    PathPoint p = pts[size_t(i)];
    if (n < 2) return p;
    const PathPoint& prev = i > 0 ? pts[size_t(i - 1)] : closed ? pts[size_t(n - 1)] : p;
    const PathPoint& next = i + 1 < n ? pts[size_t(i + 1)] : closed ? pts[0] : p;
    const double tx = (next.x - prev.x) / 6, ty = (next.y - prev.y) / 6;
    p.ox = tx;
    p.oy = ty;
    p.ix = -tx;
    p.iy = -ty;
    return p;
}

std::vector<std::pair<double, double>> flattenMaskPath(const std::vector<PathPoint>& pts, bool closed, int steps) {
    std::vector<std::pair<double, double>> out;
    const int n = int(pts.size());
    if (n == 0) return out;
    const int segments = closed ? n : n - 1;
    steps = std::max(1, steps);
    for (int i = 0; i < segments; ++i) {
        const PathPoint& a = pts[size_t(i)];
        const PathPoint& b = pts[size_t(i + 1) % size_t(n)];
        if (a.ox == 0 && a.oy == 0 && b.ix == 0 && b.iy == 0) {
            out.emplace_back(a.x, a.y);
            continue;
        }
        for (int k = 0; k < steps; ++k) out.push_back(segmentPoint(a, b, double(k) / steps));
    }
    if (!closed) out.emplace_back(pts.back().x, pts.back().y);
    return out;
}

void boxToFrame(const MaskBox& b, double frameW, double frameH, double bx, double by, double& u, double& v) {
    const double lx = bx * b.w * frameW, ly = by * b.h * frameH;
    const double r = b.rotation * M_PI / 180.0, c = std::cos(r), s = std::sin(r);
    u = b.x + (lx * c - ly * s) / frameW;
    v = b.y + (lx * s + ly * c) / frameH;
}

void frameToBox(const MaskBox& b, double frameW, double frameH, double u, double v, double& bx, double& by) {
    const double dx = (u - b.x) * frameW, dy = (v - b.y) * frameH;
    const double r = b.rotation * M_PI / 180.0, c = std::cos(r), s = std::sin(r);
    bx = (dx * c + dy * s) / std::max(1e-12, b.w * frameW);
    by = (-dx * s + dy * c) / std::max(1e-12, b.h * frameH);
}

bool fitMaskBox(MaskBox& box, std::vector<PathPoint>& pts, double frameW, double frameH) {
    if (pts.size() < 2) return false;
    const auto poly = flattenMaskPath(pts, true, 16);
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const auto& [x, y] : poly) {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    }
    const double sx = x1 - x0, sy = y1 - y0;
    if (sx < 1e-6 || sy < 1e-6) return false;
    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    double u = 0, v = 0;
    boxToFrame(box, frameW, frameH, cx, cy, u, v);
    box.x = u;
    box.y = v;
    box.w *= sx;
    box.h *= sy;
    for (PathPoint& p : pts) {
        p.x = (p.x - cx) / sx;
        p.y = (p.y - cy) / sy;
        p.ix /= sx;
        p.ox /= sx;
        p.iy /= sy;
        p.oy /= sy;
    }
    return true;
}

bool closeMaskPath(Effect& e, FrameTime t, double frameW, double frameH) {
    if (e.p("mask.open", t) <= 0.5 || maskPathCount(e) < 3) return false;
    e.params.erase("mask.open");
    static const char* const box[] = {"mask.x", "mask.y", "mask.w", "mask.h", "mask.rotation"};
    const bool boxAnimated = std::any_of(std::begin(box), std::end(box), [&e](const char* n) {
        auto it = e.params.find(n);
        return it != e.params.end() && it->second.animated();
    });
    if (boxAnimated || maskPathAnimated(e)) return true;
    MaskBox b = maskBox(e, t);
    std::vector<PathPoint> pts = maskPath(e, t);
    if (!fitMaskBox(b, pts, frameW, frameH)) return true;
    e.params["mask.x"] = Param(b.x);
    e.params["mask.y"] = Param(b.y);
    e.params["mask.w"] = Param(b.w);
    e.params["mask.h"] = Param(b.h);
    setMaskPath(e, t, pts);
    return true;
}

void setMaskPathFromFrame(Effect& e, std::vector<PathPoint> pts, double frameW, double frameH, bool smooth) {
    // The whole frame as the box first: box units are then the frame fractions less a half.
    MaskBox box{0.5, 0.5, 1, 1, 0};
    for (PathPoint& p : pts) {
        p.x -= 0.5;
        p.y -= 0.5;
    }
    if (smooth)
        for (size_t i = 0; i < pts.size(); ++i)
            if (!pts[i].smooth()) pts[i] = smoothed(pts, int(i));
    fitMaskBox(box, pts, frameW, frameH);
    erasePathParams(e);
    e.params.erase("mask.open");
    e.params["mask.shape"] = Param(5);
    e.params["mask.x"] = Param(box.x);
    e.params["mask.y"] = Param(box.y);
    e.params["mask.w"] = Param(box.w);
    e.params["mask.h"] = Param(box.h);
    e.params["mask.rotation"] = Param(box.rotation);
    setMaskPath(e, 0, pts);
}

}  // namespace montage
