#include "MaskPath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

namespace montage {

namespace {

const char* const kComponents[6] = {"x", "y", "ix", "iy", "ox", "oy"};

const std::string kMask = "mask.";
std::string paramName(const std::string& prefix, int i, int component) { return prefix + "p" + std::to_string(i) + "." + kComponents[component]; }

double& component(PathPoint& p, int c) {
    double* f[6] = {&p.x, &p.y, &p.ix, &p.iy, &p.ox, &p.oy};
    return *f[c];
}
double component(const PathPoint& p, int c) { return component(const_cast<PathPoint&>(p), c); }

// Whether `name` is prefix + "p<i>.<component>".
bool isPathParam(const std::string& name, const std::string& prefix) {
    if (name.rfind(prefix + "p", 0) != 0) return false;
    size_t i = prefix.size() + 1;
    const size_t digits = i;
    while (i < name.size() && std::isdigit(static_cast<unsigned char>(name[i]))) ++i;
    if (i == digits || i >= name.size() || name[i] != '.') return false;
    const std::string c = name.substr(i + 1);
    return std::any_of(std::begin(kComponents), std::end(kComponents), [&c](const char* k) { return c == k; });
}

void erasePathParams(Effect& e, const std::string& prefix) {
    for (auto it = e.params.begin(); it != e.params.end();)
        it = isPathParam(it->first, prefix) ? e.params.erase(it) : std::next(it);
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

bool isMaskPathParam(const std::string& name) { return isPathParam(name, kMask); }

int pathPointCount(const Effect& e, const std::string& prefix) {
    int n = 0;
    while (e.params.count(paramName(prefix, n, 0))) ++n;
    return n;
}
int maskPathCount(const Effect& e) { return pathPointCount(e, kMask); }

std::vector<std::string> maskPathParams(const Effect& e) {
    std::vector<std::string> out;
    const int n = maskPathCount(e);
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < 6; ++c) out.push_back(paramName(kMask, i, c));
    return out;
}

std::vector<PathPoint> pathPoints(const Effect& e, FrameTime t, const std::string& prefix) {
    std::vector<PathPoint> pts(size_t(pathPointCount(e, prefix)));
    for (size_t i = 0; i < pts.size(); ++i)
        for (int c = 0; c < 6; ++c) component(pts[i], c) = e.p(paramName(prefix, int(i), c), t);
    return pts;
}
std::vector<PathPoint> maskPath(const Effect& e, FrameTime t) { return pathPoints(e, t, kMask); }

bool pathAnimated(const Effect& e, const std::string& prefix) {
    auto it = e.params.find(paramName(prefix, 0, 0));
    return it != e.params.end() && it->second.animated();
}
bool maskPathAnimated(const Effect& e) { return pathAnimated(e, kMask); }

std::vector<FrameTime> pathKeyTimes(const Effect& e, const std::string& prefix) {
    std::set<FrameTime> times;
    for (const auto& [name, p] : e.params)
        if (isPathParam(name, prefix))
            for (const Keyframe& k : p.keys) times.insert(k.t);
    return {times.begin(), times.end()};
}
std::vector<FrameTime> maskPathKeyTimes(const Effect& e) { return pathKeyTimes(e, kMask); }

void setPathPoints(Effect& e, FrameTime t, const std::vector<PathPoint>& pts, const std::string& prefix) {
    const bool animated = pathAnimated(e, prefix);
    for (int i = int(pts.size()), n = pathPointCount(e, prefix); i < n; ++i)
        for (int c = 0; c < 6; ++c) e.params.erase(paramName(prefix, i, c));
    for (size_t i = 0; i < pts.size(); ++i)
        for (int c = 0; c < 6; ++c) {
            Param& p = e.params[paramName(prefix, int(i), c)];
            if (animated && !p.animated()) p.addKey(t, component(pts[i], c));
            else p.set(t, component(pts[i], c));
        }
}
void setMaskPath(Effect& e, FrameTime t, const std::vector<PathPoint>& pts) { setPathPoints(e, t, pts, kMask); }

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

void editPathPoints(Effect& e, FrameTime t, const std::function<void(std::vector<PathPoint>&)>& fn, const std::string& prefix) {
    if (!pathAnimated(e, prefix)) {
        std::vector<PathPoint> pts = pathPoints(e, t, prefix);
        fn(pts);
        erasePathParams(e, prefix);
        setPathPoints(e, t, pts, prefix);
        return;
    }
    struct Snapshot {
        FrameTime t;
        Interp interp;
        std::vector<PathPoint> pts;
    };
    std::vector<Snapshot> keys;
    const Param& rep = e.params.at(paramName(prefix, 0, 0));
    for (FrameTime k : pathKeyTimes(e, prefix)) {
        const Keyframe* kf = rep.keyAt(k);
        keys.push_back({k, kf ? kf->interp : Interp::Linear, pathPoints(e, k, prefix)});
        fn(keys.back().pts);
    }
    erasePathParams(e, prefix);
    for (const Snapshot& s : keys)
        for (size_t i = 0; i < s.pts.size(); ++i)
            for (int c = 0; c < 6; ++c) e.params[paramName(prefix, int(i), c)].addKey(s.t, component(s.pts[i], c), s.interp);
}
void editMaskPath(Effect& e, FrameTime t, const std::function<void(std::vector<PathPoint>&)>& fn) { editPathPoints(e, t, fn, kMask); }

std::pair<double, double> segmentPoint(const PathPoint& a, const PathPoint& b, double s) {
    return {cubic(a.x, a.x + a.ox, b.x + b.ix, b.x, s), cubic(a.y, a.y + a.oy, b.y + b.iy, b.y, s)};
}

int insertPathPoint(Effect& e, FrameTime t, int seg, double s, const std::string& prefix, bool closed) {
    const int n = pathPointCount(e, prefix);
    if (n < 2 || seg < 0 || seg >= (closed ? n : n - 1)) return -1;
    s = std::clamp(s, 0.01, 0.99);
    editPathPoints(e, t, [seg, s](std::vector<PathPoint>& pts) {
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
    }, prefix);
    return seg + 1;
}
int insertMaskPoint(Effect& e, FrameTime t, int seg, double s) { return insertPathPoint(e, t, seg, s, kMask, true); }

bool removePathPoint(Effect& e, FrameTime t, int index, const std::string& prefix, int fewest) {
    if (index < 0 || index >= pathPointCount(e, prefix) || pathPointCount(e, prefix) <= fewest) return false;
    editPathPoints(e, t, [index](std::vector<PathPoint>& pts) {
        if (index < int(pts.size())) pts.erase(pts.begin() + index);
    }, prefix);
    return true;
}
bool removeMaskPoint(Effect& e, FrameTime t, int index) { return removePathPoint(e, t, index, kMask, 3); }

void togglePathPointSmooth(Effect& e, FrameTime t, int index, const std::string& prefix, bool closed) {
    if (index < 0 || index >= pathPointCount(e, prefix)) return;
    const bool wasSmooth = pathPoints(e, t, prefix)[size_t(index)].smooth();
    editPathPoints(e, t, [index, wasSmooth, closed](std::vector<PathPoint>& pts) {
        PathPoint& p = pts[size_t(index)];
        if (wasSmooth) p.ix = p.iy = p.ox = p.oy = 0;
        else p = smoothed(pts, index, closed);
    }, prefix);
}
void toggleMaskPointSmooth(Effect& e, FrameTime t, int index) { togglePathPointSmooth(e, t, index, kMask, true); }

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
    erasePathParams(e, kMask);
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
