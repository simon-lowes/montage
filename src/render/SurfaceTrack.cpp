#include "SurfaceTrack.h"

#include <QByteArray>
#include <QFileInfo>
#include <QDir>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <list>
#include <set>
#include <mutex>
#include <sstream>
#include <thread>

#include "FollowFocus.h"
#include "Processing.h"
#include "core/LinearSolve.h"
#include "media/Decoder.h"
#include "media/Exr.h"
#include "media/MediaPool.h"

namespace montage {

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

double signedArea(const MeshPoint& a, const MeshPoint& b, const MeshPoint& c) {
    return 0.5 * ((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]));
}

// fn(i) for i in [0, n): on this thread, the shared workers or `threads` of its own; each i on its own, so the result
// is the same however it is shared out.
void forEach(int n, int threads, const std::function<void(int)>& fn) {
    if (n <= 0) return;
    if (threads == 1 || n == 1) {
        for (int i = 0; i < n; ++i) fn(i);
        return;
    }
    if (threads <= 0) {
        parallelRows(n, [&](int a, int b) {
            for (int i = a; i < b; ++i) fn(i);
        });
        return;
    }
    std::vector<std::thread> pool;
    const int k = std::min(threads, n);
    for (int t = 0; t < k; ++t)
        pool.emplace_back([&, t] {
            for (int i = n * t / k; i < n * (t + 1) / k; ++i) fn(i);
        });
    for (std::thread& th : pool) th.join();
}

// Catmull-Rom (bicubic) in an RGBA picture at mesh coordinates, clamped to its edge: sharper than bilinear, so a
// picture taken off the surface and put back loses little; alpha kept to 0..1.
void sampleCubic(const Image& src, double x, double y, float o[4]) {
    x = std::clamp(x - 0.5, 0.0, double(src.width - 1));
    y = std::clamp(y - 0.5, 0.0, double(src.height - 1));
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const double fx = x - ix, fy = y - iy;
    auto weights = [](double t, double w[4]) {
        const double t2 = t * t, t3 = t2 * t;
        w[0] = -0.5 * t3 + t2 - 0.5 * t;
        w[1] = 1.5 * t3 - 2.5 * t2 + 1;
        w[2] = -1.5 * t3 + 2 * t2 + 0.5 * t;
        w[3] = 0.5 * t3 - 0.5 * t2;
    };
    double wx[4], wy[4], acc[4] = {0, 0, 0, 0};
    weights(fx, wx), weights(fy, wy);
    for (int j = 0; j < 4; ++j) {
        const int yy = std::clamp(iy - 1 + j, 0, src.height - 1);
        for (int i = 0; i < 4; ++i) {
            const float* p = src.at(std::clamp(ix - 1 + i, 0, src.width - 1), yy);
            const double w = wx[i] * wy[j];
            for (int k = 0; k < 4; ++k) acc[k] += w * p[k];
        }
    }
    for (int k = 0; k < 4; ++k) o[k] = float(acc[k]);
    o[3] = std::clamp(o[3], 0.0f, 1.0f);
}

// Finding the triangle a point is in, on a grid of cells over the mesh where it is.
struct Locator {
    const WarpMesh* m = nullptr;
    const std::vector<MeshPoint>* at = nullptr;
    double x0 = 0, y0 = 0, cell = 1;
    int gw = 0, gh = 0;
    std::vector<std::vector<int>> cells;
    Locator(const WarpMesh& mesh, const std::vector<MeshPoint>& pos) : m(&mesh), at(&pos) {
        if (mesh.tris.empty()) return;
        double x1 = -1e300, y1 = -1e300;
        x0 = y0 = 1e300;
        for (const MeshPoint& p : pos) x0 = std::min(x0, p[0]), y0 = std::min(y0, p[1]), x1 = std::max(x1, p[0]), y1 = std::max(y1, p[1]);
        cell = std::max(1.0, std::sqrt((x1 - x0) * (y1 - y0) / double(mesh.tris.size())) * 2);
        gw = std::clamp(int((x1 - x0) / cell) + 1, 1, 4096), gh = std::clamp(int((y1 - y0) / cell) + 1, 1, 4096);
        cells.assign(size_t(gw) * size_t(gh), {});
        size_t entries = 0;  // (a mesh of triangles each across the whole grid is not looked through)
        for (size_t t = 0; t < mesh.tris.size(); ++t) {
            double a0 = 1e300, b0 = 1e300, a1 = -1e300, b1 = -1e300;
            for (int k : mesh.tris[t]) {
                const MeshPoint& p = pos[size_t(k)];
                a0 = std::min(a0, p[0]), b0 = std::min(b0, p[1]), a1 = std::max(a1, p[0]), b1 = std::max(b1, p[1]);
            }
            for (int gy = std::max(0, int((b0 - y0) / cell)); gy <= std::min(gh - 1, int((b1 - y0) / cell)); ++gy)
                for (int gx = std::max(0, int((a0 - x0) / cell)); gx <= std::min(gw - 1, int((a1 - x0) / cell)); ++gx) {
                    if (++entries > (size_t(4) << 20)) {
                        cells.clear();
                        return;
                    }
                    cells[size_t(gy) * size_t(gw) + size_t(gx)].push_back(int(t));
                }
        }
    }
    // The triangle and the point's barycentric weights in it; false outside the mesh.
    bool find(double x, double y, int& tri, double l[3]) const {
        if (cells.empty() || !(x >= x0 && y >= y0)) return false;
        const double fx = (x - x0) / cell, fy = (y - y0) / cell;
        if (!(fx < gw && fy < gh)) return false;  // (also NaN, and far off: compared before it is made an int)
        const int gx = int(fx), gy = int(fy);
        for (int t : cells[size_t(gy) * size_t(gw) + size_t(gx)]) {
            const auto& tr = m->tris[size_t(t)];
            const MeshPoint &a = (*at)[size_t(tr[0])], &b = (*at)[size_t(tr[1])], &c = (*at)[size_t(tr[2])];
            const double area = signedArea(a, b, c);
            if (std::fabs(area) < 1e-12) continue;
            const MeshPoint p{x, y};
            l[0] = signedArea(p, b, c) / area, l[1] = signedArea(a, p, c) / area, l[2] = signedArea(a, b, p) / area;
            if (l[0] >= -1e-9 && l[1] >= -1e-9 && l[2] >= -1e-9) {
                tri = t;
                return true;
            }
        }
        return false;
    }
};

// A feature held by the mesh: its triangle and place in it, and where it should be.
struct Hold {
    int tri = 0;
    double l[3] = {0, 0, 0};
    double x = 0, y = 0;
    double w = 1;  // how much it counts
};

// v^T Q v for the form over `loc`'s unknowns, into `entries` (core/LinearSolve.h: one off-diagonal entry for both).
void addForm(std::vector<SparseCholesky::Entry>& entries, const int* loc, const double* q, int n, double w) {
    for (int k = 0; k < n; ++k) {
        if (q[k] == 0) continue;
        entries.push_back({loc[k], loc[k], w * q[k] * q[k]});
        for (int l = k + 1; l < n; ++l) {
            if (q[l] == 0) continue;
            entries.push_back({loc[k], loc[l], loc[k] == loc[l] ? 2 * w * q[k] * q[l] : w * q[k] * q[l]});
        }
    }
}

// The vertices that best keep the features where they are asked to be and each triangle a turned and scaled copy of
// itself in `shape`, near `prior` (lightly, so a part with no features stays put), with `fixed` held.
std::vector<MeshPoint> solveVertices(const WarpMesh& m, const std::vector<MeshPoint>& shape, const std::vector<Hold>& holds,
                                     double stiffness, const std::vector<MeshPoint>& prior, const std::map<int, MeshPoint>& fixed) {
    const int n = int(m.rest.size());
    std::vector<SparseCholesky::Entry> e;
    std::vector<double> rhs(size_t(2 * n), 0.0);
    e.reserve(size_t(n) * 2 + holds.size() * 12 + m.tris.size() * 3 * 30);
    for (const Hold& h : holds) {
        const auto& tr = m.tris[size_t(h.tri)];
        for (int axis = 0; axis < 2; ++axis) {
            const int loc[3] = {2 * tr[0] + axis, 2 * tr[1] + axis, 2 * tr[2] + axis};
            addForm(e, loc, h.l, 3, h.w);
            for (int k = 0; k < 3; ++k) rhs[size_t(loc[k])] += h.w * h.l[k] * (axis ? h.y : h.x);
        }
    }
    // Each vertex of each triangle where the other two (as `shape` has them) put it: k = i + s (j - i) + r R(j - i),
    // R turning a quarter (x, y) -> (-y, x).
    for (const auto& tr : m.tris)
        for (int rot = 0; rot < 3; ++rot) {
            const int i = tr[size_t(rot)], j = tr[size_t((rot + 1) % 3)], k = tr[size_t((rot + 2) % 3)];
            const MeshPoint &Ti = shape[size_t(i)], &Tj = shape[size_t(j)], &Tk = shape[size_t(k)];
            const double ex = Tj[0] - Ti[0], ey = Tj[1] - Ti[1], dx = Tk[0] - Ti[0], dy = Tk[1] - Ti[1];
            const double len = ex * ex + ey * ey;
            if (len < 1e-12) continue;
            const double s = (dx * ex + dy * ey) / len, r = (dy * ex - dx * ey) / len;
            // x: x_k - (1 - s) x_i - s x_j + r y_j - r y_i
            {
                const int loc[5] = {2 * k, 2 * i, 2 * j, 2 * j + 1, 2 * i + 1};
                const double q[5] = {1, -(1 - s), -s, r, -r};
                addForm(e, loc, q, 5, stiffness);
            }
            // y: y_k - (1 - s) y_i - s y_j - r x_j + r x_i
            {
                const int loc[5] = {2 * k + 1, 2 * i + 1, 2 * j + 1, 2 * j, 2 * i};
                const double q[5] = {1, -(1 - s), -s, -r, r};
                addForm(e, loc, q, 5, stiffness);
            }
        }
    constexpr double kPrior = 1e-4, kFixed = 1e6;
    for (int v = 0; v < n; ++v) {
        const auto f = fixed.find(v);
        const double w = f != fixed.end() ? kFixed : kPrior;
        const MeshPoint& p = f != fixed.end() ? f->second : prior[size_t(v)];
        for (int axis = 0; axis < 2; ++axis) {
            e.push_back({2 * v + axis, 2 * v + axis, w});
            rhs[size_t(2 * v + axis)] += w * p[size_t(axis)];
        }
    }
    SparseCholesky solver;
    if (!solver.factor(2 * n, e)) return prior;
    solver.solve(rhs);
    std::vector<MeshPoint> out(static_cast<size_t>(n));
    for (int v = 0; v < n; ++v) out[size_t(v)] = {rhs[size_t(2 * v)], rhs[size_t(2 * v + 1)]};
    return out;
}

bool folded(const WarpMesh& m, const std::vector<MeshPoint>& v) {
    for (const auto& tr : m.tris) {
        const double rest = signedArea(m.rest[size_t(tr[0])], m.rest[size_t(tr[1])], m.rest[size_t(tr[2])]);
        const double now = signedArea(v[size_t(tr[0])], v[size_t(tr[1])], v[size_t(tr[2])]);
        if (!(now * (rest < 0 ? -1 : 1) > 1e-6)) return true;
    }
    return false;
}

// Solved, robustly (features far from where the rest put them left out, once), and stiffer until nothing folds.
std::vector<MeshPoint> solveHeld(const WarpMesh& m, const std::vector<MeshPoint>& shape, std::vector<Hold> holds, double stiffness,
                                 const std::vector<MeshPoint>& prior, const std::map<int, MeshPoint>& fixed) {
    std::vector<MeshPoint> v = solveVertices(m, shape, holds, stiffness, prior, fixed);
    std::vector<Hold> used = holds;  // (less those far from the solve, should a fold need it solved again)
    if (holds.size() > 8) {
        std::vector<double> err(holds.size());
        for (size_t i = 0; i < holds.size(); ++i) {
            const auto& tr = m.tris[size_t(holds[i].tri)];
            double x = 0, y = 0;
            for (int k = 0; k < 3; ++k) x += holds[i].l[k] * v[size_t(tr[size_t(k)])][0], y += holds[i].l[k] * v[size_t(tr[size_t(k)])][1];
            err[i] = std::hypot(x - holds[i].x, y - holds[i].y);
        }
        std::vector<double> sorted = err;
        std::nth_element(sorted.begin(), sorted.begin() + long(sorted.size() / 2), sorted.end());
        const double limit = std::max(0.75, 3 * sorted[sorted.size() / 2]);
        std::vector<Hold> kept;
        for (size_t i = 0; i < holds.size(); ++i)
            if (err[i] <= limit) kept.push_back(holds[i]);
        if (kept.size() != holds.size() && kept.size() >= 6) {
            used = std::move(kept);
            v = solveVertices(m, shape, used, stiffness, prior, fixed);
        }
    }
    for (int tries = 0; tries < 6 && folded(m, v); ++tries) {
        stiffness *= 4;
        v = solveVertices(m, shape, used, stiffness, prior, fixed);
    }
    return v;
}

// Covers: whether each pixel centre of a w x h picture is inside the mesh at `pos` (mesh coordinates).
std::vector<char> coverage(const WarpMesh& m, const std::vector<MeshPoint>& pos, int w, int h) {
    const std::vector<float> map = meshMap(m, pos, pos, w, h);
    std::vector<char> in(size_t(w) * size_t(h));
    for (size_t i = 0; i < in.size(); ++i) in[i] = !std::isnan(map[i * 2]);
    return in;
}

// Corners in `g` inside the mesh at `pos`, at least `reach` pixels in, with their places in their triangles.
std::vector<Hold> featuresIn(const GrayImage& g, const WarpMesh& m, const std::vector<MeshPoint>& pos, int maxCount, double spacing,
                             int reach) {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const MeshPoint& p : pos) x0 = std::min(x0, p[0]), y0 = std::min(y0, p[1]), x1 = std::max(x1, p[0]), y1 = std::max(y1, p[1]);
    const std::vector<Point2> corners = goodFeatures(g, maxCount, std::max(2.0, spacing), x0 - 0.5, y0 - 0.5, x1 - 0.5, y1 - 0.5);
    const std::vector<char> in = coverage(m, pos, g.width, g.height);
    auto inside = [&](double x, double y) {
        const int ix = int(std::floor(x)), iy = int(std::floor(y));
        return ix >= 0 && iy >= 0 && ix < g.width && iy < g.height && in[size_t(iy) * size_t(g.width) + size_t(ix)];
    };
    const Locator loc(m, pos);
    std::vector<Hold> out;
    for (const Point2& c : corners) {
        const double x = c.x + 0.5, y = c.y + 0.5;
        if (!inside(x, y) || !inside(x - reach, y - reach) || !inside(x + reach, y - reach) || !inside(x - reach, y + reach) ||
            !inside(x + reach, y + reach) || !inside(x, y - reach) || !inside(x, y + reach) || !inside(x - reach, y) || !inside(x + reach, y))
            continue;
        Hold h;
        if (!loc.find(x, y, h.tri, h.l)) continue;
        h.x = x, h.y = y;
        out.push_back(h);
    }
    return out;
}

void pointOf(const WarpMesh& m, const std::vector<MeshPoint>& v, const Hold& h, double& x, double& y) {
    const auto& tr = m.tris[size_t(h.tri)];
    x = y = 0;
    for (int k = 0; k < 3; ++k) x += h.l[k] * v[size_t(tr[size_t(k)])][0], y += h.l[k] * v[size_t(tr[size_t(k)])][1];
}

// meshMap, carried on past the mesh's edge for `band` pixels by the nearest triangle's own mapping (its barycentric
// weights extrapolated), so a window on the mesh's edge sees what is round it as the mesh would carry it.
std::vector<float> reachingMap(const WarpMesh& m, const std::vector<MeshPoint>& moved, const std::vector<MeshPoint>& to, int W, int H,
                               double band) {
    std::vector<float> map = meshMap(m, moved, to, W, H);
    std::vector<float> nearest(size_t(W) * size_t(H), std::numeric_limits<float>::infinity());
    std::vector<char> covered(nearest.size());
    for (size_t i = 0; i < covered.size(); ++i) covered[i] = !std::isnan(map[i * 2]);
    struct Tri {
        MeshPoint c;
        double radius;
        int x0, x1, y0, y1;
    };
    std::vector<Tri> tris(m.tris.size());
    for (size_t ti = 0; ti < m.tris.size(); ++ti) {
        const auto& tr = m.tris[ti];
        Tri& d = tris[ti];
        d.c = {0, 0};
        for (int k : tr) d.c[0] += moved[size_t(k)][0] / 3, d.c[1] += moved[size_t(k)][1] / 3;
        d.radius = 0;
        double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
        for (int k : tr) {
            const MeshPoint& p = moved[size_t(k)];
            d.radius = std::max(d.radius, std::hypot(p[0] - d.c[0], p[1] - d.c[1]));
            minX = std::min(minX, p[0]), maxX = std::max(maxX, p[0]), minY = std::min(minY, p[1]), maxY = std::max(maxY, p[1]);
        }
        d.x0 = std::max(0, int(std::floor(minX - band))), d.x1 = std::min(W - 1, int(std::ceil(maxX + band)));
        d.y0 = std::max(0, int(std::floor(minY - band))), d.y1 = std::min(H - 1, int(std::ceil(maxY + band)));
    }
    parallelRows(H, [&](int ya, int yb) {
        for (size_t ti = 0; ti < m.tris.size(); ++ti) {
            const Tri& d = tris[ti];
            const auto& tr = m.tris[ti];
            const MeshPoint &a = moved[size_t(tr[0])], &b = moved[size_t(tr[1])], &c = moved[size_t(tr[2])];
            const double area = signedArea(a, b, c);
            if (std::fabs(area) < 1e-9) continue;
            for (int y = std::max(d.y0, ya); y <= std::min(d.y1, yb - 1); ++y)
                for (int x = d.x0; x <= d.x1; ++x) {
                    const size_t i = size_t(y) * size_t(W) + size_t(x);
                    if (covered[i]) continue;
                    const MeshPoint p{x + 0.5, y + 0.5};
                    const double dist = std::hypot(p[0] - d.c[0], p[1] - d.c[1]);
                    if (dist > d.radius + band || dist >= nearest[i]) continue;
                    nearest[i] = float(dist);
                    const double l0 = signedArea(p, b, c) / area, l1 = signedArea(a, p, c) / area, l2 = 1 - l0 - l1;
                    const MeshPoint &r0 = to[size_t(tr[0])], &r1 = to[size_t(tr[1])], &r2 = to[size_t(tr[2])];
                    map[i * 2] = float(l0 * r0[0] + l1 * r1[0] + l2 * r2[0]);
                    map[i * 2 + 1] = float(l0 * r0[1] + l1 * r1[1] + l2 * r2[1]);
                }
        }
    });
    return map;
}

// Each vertex (on the picture) as a feature of its own, at half weight.
std::vector<Hold> vertexHolds(const WarpMesh& m, const std::vector<MeshPoint>& at, int W, int H) {
    std::vector<int> firstTri(m.rest.size(), -1), corner(m.rest.size(), 0);
    for (size_t ti = 0; ti < m.tris.size(); ++ti)
        for (int k = 0; k < 3; ++k) {
            const size_t v = size_t(m.tris[ti][size_t(k)]);
            if (firstTri[v] < 0) firstTri[v] = int(ti), corner[v] = k;
        }
    std::vector<Hold> out;
    for (size_t v = 0; v < m.rest.size(); ++v) {
        const MeshPoint& p = at[v];
        if (firstTri[v] < 0 || p[0] < 1 || p[1] < 1 || p[0] > W - 1 || p[1] > H - 1) continue;
        Hold h;
        h.tri = firstTri[v];
        h.l[corner[v]] = 1;
        h.x = p[0], h.y = p[1];
        h.w = 0.5;
        out.push_back(h);
    }
    return out;
}

std::map<int, MeshPoint> fixesAt(const SurfaceTrack& t, int frame) {
    const auto f = t.fixes.find(frame);
    std::map<int, MeshPoint> out;
    if (f == t.fixes.end()) return out;
    for (const auto& [v, p] : f->second)
        if (v >= 0 && v < int(t.mesh.rest.size())) out[v] = p;
    return out;
}

}  // namespace

// ---- The track ------------------------------------------------------------------

int SurfaceTrack::frameAt(double mediaSeconds) const {
    if (frameSeconds <= 0 || frames.empty()) return -1;
    const double k = (mediaSeconds - start) / frameSeconds;
    if (!std::isfinite(k)) return -1;
    const long i = std::lround(k);
    if (i < 0 || i >= long(frames.size()) || frames[size_t(i)].size() != mesh.rest.size() || mesh.rest.empty()) return -1;
    return int(i);
}

std::string surfaceTrackToString(const SurfaceTrack& t) {
    std::ostringstream o;
    o.precision(17);
    o << "surface 1\nsize " << t.width << ' ' << t.height << "\ntime " << t.start << ' ' << t.frameSeconds << "\nreference "
      << t.reference << "\nvertices " << t.mesh.rest.size();
    char buf[64];
    for (const MeshPoint& p : t.mesh.rest) {
        std::snprintf(buf, sizeof buf, " %.4f,%.4f", p[0], p[1]);
        o << buf;
    }
    o << "\ntriangles " << t.mesh.tris.size();
    for (const auto& tr : t.mesh.tris) o << ' ' << tr[0] << ',' << tr[1] << ',' << tr[2];
    o << "\nframes " << t.frames.size() << '\n';
    std::vector<float> f;
    for (const auto& v : t.frames) {
        if (v.empty()) {
            o << "-\n";
            continue;
        }
        f.resize(v.size() * 2);
        for (size_t i = 0; i < v.size(); ++i) f[i * 2] = float(v[i][0]), f[i * 2 + 1] = float(v[i][1]);
        o << QByteArray(reinterpret_cast<const char*>(f.data()), qsizetype(f.size() * sizeof(float))).toBase64().toStdString() << '\n';
    }
    for (const auto& [frame, vs] : t.fixes)
        for (const auto& [v, p] : vs) {
            std::snprintf(buf, sizeof buf, "fix %d %d %.4f %.4f\n", frame, v, p[0], p[1]);
            o << buf;
        }
    return o.str();
}

bool surfaceTrackFromString(const std::string& text, SurfaceTrack& out) {
    out = {};
    std::istringstream in(text);
    std::string word;
    int version = 0;
    if (!(in >> word >> version) || word != "surface" || version != 1) return false;
    size_t count = 0;
    if (!(in >> word >> out.width >> out.height) || word != "size" || out.width <= 0 || out.height <= 0 || out.width > 8192 ||
        out.height > 8192 || int64_t(out.width) * out.height > 16 * 1024 * 1024)
        return false;
    // (Points far off the frame are not a track: kept out so nothing sized or indexed by them goes astray.)
    auto near = [&](double x, double y) {
        return std::isfinite(x) && std::isfinite(y) && std::fabs(x) <= 10.0 * out.width && std::fabs(y) <= 10.0 * out.height;
    };
    if (!(in >> word >> out.start >> out.frameSeconds) || word != "time" || !std::isfinite(out.start) || !(out.frameSeconds > 0))
        return false;
    if (!(in >> word >> out.reference) || word != "reference") return false;
    // (meshes made here have at most about 10,000 vertices: surfaceMesh)
    if (!(in >> word >> count) || word != "vertices" || count > 20000) return false;
    out.mesh.rest.resize(count);
    for (MeshPoint& p : out.mesh.rest) {
        char comma = 0;
        if (!(in >> p[0] >> comma >> p[1]) || comma != ',' || !near(p[0], p[1])) return false;
    }
    if (!(in >> word >> count) || word != "triangles" || count > 40000) return false;
    out.mesh.tris.resize(count);
    for (auto& tr : out.mesh.tris) {
        char c1 = 0, c2 = 0;
        if (!(in >> tr[0] >> c1 >> tr[1] >> c2 >> tr[2]) || c1 != ',' || c2 != ',') return false;
        for (int k : tr)
            if (k < 0 || size_t(k) >= out.mesh.rest.size()) return false;
    }
    // (each frame takes at least two characters)
    if (!(in >> word >> count) || word != "frames" || count > 10000000 || count > text.size() / 2) return false;
    out.frames.resize(count);
    const size_t n = out.mesh.rest.size();
    for (auto& v : out.frames) {
        if (!(in >> word)) return false;
        if (word == "-") continue;
        const QByteArray raw = QByteArray::fromBase64(QByteArray::fromStdString(word));
        if (size_t(raw.size()) != n * 2 * sizeof(float)) return false;
        std::vector<float> f(n * 2);
        std::memcpy(f.data(), raw.constData(), size_t(raw.size()));
        v.resize(n);
        for (size_t i = 0; i < n; ++i) {
            if (!near(f[i * 2], f[i * 2 + 1])) return false;
            v[i] = {f[i * 2], f[i * 2 + 1]};
        }
    }
    while (in >> word) {
        if (word != "fix") return false;
        int frame = 0, v = 0;
        MeshPoint p{};
        if (!(in >> frame >> v >> p[0] >> p[1]) || v < 0 || size_t(v) >= n || frame < 0 || size_t(frame) >= out.frames.size() || !near(p[0], p[1]))
            return false;
        out.fixes[frame][v] = p;
    }
    if (out.reference < 0 || (!out.frames.empty() && size_t(out.reference) >= out.frames.size())) return false;
    return true;
}

std::shared_ptr<const SurfaceTrack> cachedSurfaceTrack(const Effect& e) {
    // (The text read in place: a long clip's track runs to megabytes.)
    static const std::string none;
    const auto it = e.strings.find("track");
    return cachedSurfaceTrack(it == e.strings.end() ? none : it->second);
}

std::shared_ptr<const SurfaceTrack> cachedSurfaceTrack(const std::string& text) {
    static std::mutex lock;
    static std::list<std::pair<std::string, std::shared_ptr<const SurfaceTrack>>> kept;  // most recent first
    if (text.empty()) return nullptr;
    {
        std::lock_guard<std::mutex> g(lock);
        for (auto it = kept.begin(); it != kept.end(); ++it)
            if (it->first == text) {
                kept.splice(kept.begin(), kept, it);
                return kept.front().second;
            }
    }
    auto t = std::make_shared<SurfaceTrack>();
    if (!surfaceTrackFromString(text, *t)) t = nullptr;
    std::lock_guard<std::mutex> g(lock);
    kept.emplace_front(text, t);
    while (kept.size() > 8) kept.pop_back();
    return t;
}

// ---- The mesh ---------------------------------------------------------------------

WarpMesh surfaceMesh(const std::vector<MeshPoint>& outline, int w, int h, int vertices) {
    if (w <= 0 || h <= 0 || outline.size() < 3) return {};
    // The outline's inside (even-odd, at pixel centres), row by row.
    Image alpha(w, h);
    double area = 0;
    for (size_t i = 0; i < outline.size(); ++i) {
        const MeshPoint &a = outline[i], &b = outline[(i + 1) % outline.size()];
        area += a[0] * b[1] - b[0] * a[1];
    }
    area = std::fabs(area) / 2;
    if (area < 16) return {};
    std::vector<double> xs;
    for (int y = 0; y < h; ++y) {
        const double py = y + 0.5;
        xs.clear();
        for (size_t i = 0; i < outline.size(); ++i) {
            const MeshPoint &a = outline[i], &b = outline[(i + 1) % outline.size()];
            if ((a[1] <= py) == (b[1] <= py)) continue;
            xs.push_back(a[0] + (py - a[1]) / (b[1] - a[1]) * (b[0] - a[0]));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2)
            for (int x = std::max(0, int(std::ceil(xs[i] - 0.5))); x < std::min(w, int(std::ceil(xs[i + 1] - 0.5))); ++x) alpha.at(x, y)[3] = 1;
    }
    const double full = std::clamp(double(vertices) * double(w) * double(h) / area, 4.0, 10000.0);
    return buildWarpMesh(w, h, int(full), &alpha, 0);
}

std::vector<float> meshMap(const WarpMesh& m, const std::vector<MeshPoint>& moved, const std::vector<MeshPoint>& to, int W, int H) {
    std::vector<float> out(size_t(std::max(0, W)) * size_t(std::max(0, H)) * 2, kNaN);
    if (W <= 0 || H <= 0 || moved.size() != m.rest.size() || to.size() != m.rest.size()) return out;
    // As render/MeshWarp.cpp drawWarp: edge functions worked out from each edge's lower-numbered vertex, so the two
    // triangles either side of an edge share its pixels out exactly.
    struct Tri {
        double A[3], B[3], C[3], at[3];
        bool own[3];
        int x0, x1, y0, y1;
        MeshPoint r[3];
    };
    std::vector<Tri> tris;
    tris.reserve(m.tris.size());
    // (triangles that overlap over and over, as only a crafted mesh's do, are drawn up to 16 frames' worth of pixels)
    double covered = 0;
    for (const auto& tr : m.tris) {
        Tri d{};
        bool ok = true;
        double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
        for (int k = 0; k < 3; ++k) {
            int i = tr[size_t((k + 1) % 3)], j = tr[size_t((k + 2) % 3)];
            if (i > j) std::swap(i, j);
            const MeshPoint &P = moved[size_t(i)], &Q = moved[size_t(j)];
            d.A[k] = -(Q[1] - P[1]);
            d.B[k] = Q[0] - P[0];
            d.C[k] = -d.A[k] * P[0] - d.B[k] * P[1];
            const MeshPoint& V = moved[size_t(tr[size_t(k)])];
            d.at[k] = d.A[k] * V[0] + d.B[k] * V[1] + d.C[k];
            if (!(std::fabs(d.at[k]) > 1e-12) || !std::isfinite(d.at[k])) ok = false;
            d.own[k] = d.at[k] > 0;
            d.r[k] = to[size_t(tr[size_t(k)])];
            minX = std::min(minX, V[0]), maxX = std::max(maxX, V[0]), minY = std::min(minY, V[1]), maxY = std::max(maxY, V[1]);
        }
        if (!ok || !(minX < W && maxX > -1 && minY < H && maxY > -1)) continue;
        d.x0 = int(std::floor(std::max(minX - 0.5, 0.0))), d.x1 = int(std::ceil(std::min(maxX - 0.5, double(W - 1))));
        d.y0 = int(std::floor(std::max(minY - 0.5, 0.0))), d.y1 = int(std::ceil(std::min(maxY - 0.5, double(H - 1))));
        if (d.x0 > d.x1 || d.y0 > d.y1) continue;
        if ((covered += double(d.x1 - d.x0 + 1) * double(d.y1 - d.y0 + 1)) > std::max(16.0 * W * H, 64.0 * double(m.tris.size()))) break;
        tris.push_back(d);
    }
    parallelRows(H, [&](int y0, int y1) {
        for (const Tri& d : tris) {
            const int ya = std::max(d.y0, y0), yb = std::min(d.y1, y1 - 1);
            for (int y = ya; y <= yb; ++y) {
                const double py = y + 0.5;
                for (int x = d.x0; x <= d.x1; ++x) {
                    const double px = x + 0.5;
                    double l[3];
                    bool in = true;
                    for (int k = 0; k < 3 && in; ++k) {
                        const double e = d.A[k] * px + d.B[k] * py + d.C[k];
                        l[k] = e / d.at[k];
                        in = l[k] > 0 || (e == 0 && d.own[k]);
                    }
                    if (!in) continue;
                    float* o = &out[(size_t(y) * size_t(W) + size_t(x)) * 2];
                    o[0] = float(l[0] * d.r[0][0] + l[1] * d.r[1][0] + l[2] * d.r[2][0]);
                    o[1] = float(l[0] * d.r[0][1] + l[1] * d.r[1][1] + l[2] * d.r[2][1]);
                }
            }
        }
    });
    return out;
}

// ---- Tracking ----------------------------------------------------------------------

std::vector<float> meshMapReaching(const WarpMesh& m, const std::vector<MeshPoint>& moved, const std::vector<MeshPoint>& to, int w, int h,
                                   double reach) {
    return reachingMap(m, moved, to, w, h, reach);
}

bool trackSurface(SurfaceTrack& t, int count, int from, const SurfaceFrames& frames, const SurfaceTrackOptions& options,
                  const std::function<bool(double)>& progress, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    const WarpMesh& m = t.mesh;
    const int n = int(m.rest.size());
    if (n < 3 || m.tris.empty()) return fail("The surface's outline is too small to track");
    if (count <= 0 || from < 0 || from >= count || t.reference < 0 || t.reference >= count) return fail("Nothing to track");
    t.frames.resize(size_t(count));
    GrayImage ref;
    if (!frames(t.reference, ref) || ref.width != t.width || ref.height != t.height) return fail("Cannot read the reference frame");
    // The lattice's spacing, for how far apart features are taken.
    double edges = 0;
    for (const auto& tr : m.tris) edges += std::hypot(m.rest[size_t(tr[1])][0] - m.rest[size_t(tr[0])][0], m.rest[size_t(tr[1])][1] - m.rest[size_t(tr[0])][1]);
    const double spacing = edges / double(m.tris.size());
    const int maxFeatures = std::clamp(n * 6, 64, 20000);

    // The frame it starts from: the reference at rest, or what was tracked there, with that frame's fixes put in.
    {
        std::vector<MeshPoint>& start = t.frames[size_t(from)];
        if (from == t.reference) start = m.rest;
        if (start.size() != size_t(n)) return fail("Track the surface out from the reference frame first");
        const std::map<int, MeshPoint> fixed = fixesAt(t, from);
        if (!fixed.empty()) start = solveHeld(m, start, {}, options.stiffness, start, fixed);
    }
    // Features on the reference, for matching every frame back to it.
    // (to the mesh's edge, the reference being warped a little past it; and its vertices)
    std::vector<Hold> refFeatures = featuresIn(ref, m, m.rest, maxFeatures, spacing / 3, 2);
    for (Hold h : vertexHolds(m, m.rest, t.width, t.height)) refFeatures.push_back(h);
    const std::vector<GrayImage> refLevel{ref};

    const int forward = count - 1 - from, backward = from;
    const int total = (from >= t.reference ? forward : 0) + (from <= t.reference ? backward : 0);
    int done = 0;
    bool trackedAny = from != t.reference;  // (a frame already tracked: kept, with its fixes, whatever follows)
    for (int dir : {1, -1}) {
        if ((dir > 0 && from < t.reference) || (dir < 0 && from > t.reference)) continue;
        GrayImage prev;
        if (!frames(from, prev) || prev.width != t.width || prev.height != t.height) return fail("Cannot read the clip's frames");
        int q = from + dir;
        for (; q >= 0 && q < count; q += dir) {
            if (progress && !progress(total ? double(done) / total : 1.0)) return fail("Cancelled");
            ++done;
            t.frames[size_t(q)].clear();
            const std::vector<MeshPoint> P = t.frames[size_t(q - dir)];
            GrayImage cur;
            if (!frames(q, cur) || cur.width != t.width || cur.height != t.height) break;
            const std::map<int, MeshPoint> fixed = fixesAt(t, q);
            // Frame to frame: features followed by pyramidal Lucas–Kanade, checked back.
            // (to the mesh's edge, so its outer triangles are held too; and the vertices themselves, so a corner
            // whose triangle has no corner of the picture in it is followed as well)
            std::vector<Hold> holds = featuresIn(prev, m, P, maxFeatures, spacing / 3, 2);
            for (Hold h : vertexHolds(m, P, t.width, t.height)) holds.push_back(h);
            {
                const std::vector<GrayImage> A = grayPyramid(prev), B = grayPyramid(cur);
                std::vector<char> ok(holds.size(), 0);
                forEach(int(holds.size()), options.threads, [&](int i) {
                    Hold& h = holds[size_t(i)];
                    Point2 to, back;
                    if (!lucasKanade(A, B, {h.x - 0.5, h.y - 0.5}, to) || !lucasKanade(B, A, to, back)) return;
                    if (std::hypot(back.x - (h.x - 0.5), back.y - (h.y - 0.5)) > 0.5) return;
                    h.x = to.x + 0.5, h.y = to.y + 0.5;
                    ok[size_t(i)] = 1;
                });
                std::vector<Hold> kept;
                for (size_t i = 0; i < holds.size(); ++i)
                    if (ok[i]) kept.push_back(holds[i]);
                holds.swap(kept);
            }
            if (holds.size() < 6 && fixed.size() < 3) break;  // lost
            // Light pull towards the last frame moved by the features' mean motion (for parts with none).
            std::vector<MeshPoint> prior = P;
            {
                double mx = 0, my = 0;
                for (const Hold& h : holds) {
                    double x, y;
                    pointOf(m, P, h, x, y);
                    mx += h.x - x, my += h.y - y;
                }
                if (!holds.empty()) mx /= double(holds.size()), my /= double(holds.size());
                for (MeshPoint& p : prior) p[0] += mx, p[1] += my;
            }
            std::vector<MeshPoint> V = solveHeld(m, P, holds, options.stiffness, prior, fixed);
            // Back to the reference: it warped by the mesh on to this frame, its features matched there again.
            for (int pass = 0; pass < options.refine && refFeatures.size() >= 6; ++pass) {
                const std::vector<float> map = reachingMap(m, V, m.rest, t.width, t.height, 12);
                GrayImage warped = cur;
                for (size_t i = 0; i < warped.px.size(); ++i)
                    if (!std::isnan(map[i * 2])) warped.px[i] = ref.sample(map[i * 2] - 0.5, map[i * 2 + 1] - 0.5);
                const std::vector<GrayImage> A{warped}, B{cur};
                std::vector<Hold> matched(refFeatures.size());
                std::vector<char> ok(refFeatures.size(), 0);
                forEach(int(refFeatures.size()), options.threads, [&](int i) {
                    Hold h = refFeatures[size_t(i)];
                    double x, y;
                    pointOf(m, V, h, x, y);
                    // Its whole window inside the warped reference.
                    for (int dy : {-8, 0, 8})
                        for (int dx : {-8, 0, 8}) {
                            const int ix = int(std::floor(x + dx)), iy = int(std::floor(y + dy));
                            if (ix < 0 || iy < 0 || ix >= t.width || iy >= t.height || std::isnan(map[(size_t(iy) * size_t(t.width) + size_t(ix)) * 2]))
                                return;
                        }
                    Point2 to, back;
                    if (!lucasKanade(A, B, {x - 0.5, y - 0.5}, to) || !lucasKanade(B, A, to, back)) return;
                    if (std::hypot(back.x - (x - 0.5), back.y - (y - 0.5)) > 0.25 || std::hypot(to.x - (x - 0.5), to.y - (y - 0.5)) > 4) return;
                    h.x = to.x + 0.5, h.y = to.y + 0.5;
                    matched[size_t(i)] = h;
                    ok[size_t(i)] = 1;
                });
                std::vector<Hold> kept;
                for (size_t i = 0; i < matched.size(); ++i)
                    if (ok[i]) kept.push_back(matched[i]);
                if (kept.size() < std::max<size_t>(6, refFeatures.size() / 5)) break;  // (too changed to match: kept as followed)
                // With the features followed from the frame before counting less (they hold the mesh's edge, where
                // the reference's are too near the outside to match).
                for (Hold h : holds) {
                    h.w *= 0.25;
                    kept.push_back(h);
                }
                V = solveHeld(m, P, kept, options.stiffness, V, fixed);
            }
            t.frames[size_t(q)] = std::move(V);
            trackedAny = true;
            prev = std::move(cur);
        }
        // Lost: nothing beyond (not what an earlier track had there).
        for (; q >= 0 && q < count; q += dir) t.frames[size_t(q)].clear();
    }
    if (progress) progress(1.0);
    if (!trackedAny && count > 1) return fail("The surface was lost at once");
    return true;
}

// ---- Using it ----------------------------------------------------------------------

void drawOnSurface(const SurfaceTrack& t, int frame, const Image& src, Image& out, bool stabilise) {
    if (frame < 0 || size_t(frame) >= t.frames.size() || t.frames[size_t(frame)].size() != t.mesh.rest.size() || out.empty() ||
        src.empty() || t.width <= 0 || t.height <= 0)
        return;
    const double sx = double(out.width) / t.width, sy = double(out.height) / t.height;
    auto scaled = [&](const std::vector<MeshPoint>& v) {
        std::vector<MeshPoint> s = v;
        for (MeshPoint& p : s) p[0] *= sx, p[1] *= sy;
        return s;
    };
    const std::vector<MeshPoint> on = scaled(t.frames[size_t(frame)]), rest = scaled(t.mesh.rest);
    // Each pixel drawn: where it is on `src`.
    const std::vector<float> map = stabilise ? meshMap(t.mesh, rest, on, out.width, out.height) : meshMap(t.mesh, on, rest, out.width, out.height);
    const double kx = double(src.width) / out.width, ky = double(src.height) / out.height;
    parallelRows(out.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < out.width; ++x) {
                const float* m = &map[(size_t(y) * size_t(out.width) + size_t(x)) * 2];
                if (std::isnan(m[0])) continue;
                float s[4];
                sampleCubic(src, m[0] * kx, m[1] * ky, s);
                float* o = out.at(x, y);
                for (int k = 0; k < 4; ++k) o[k] = s[k] + o[k] * (1 - s[3]);
            }
    });
}

std::vector<float> surfaceStMap(const SurfaceTrack& t, int frame) {
    const int W = t.width, H = t.height;
    std::vector<float> st(size_t(std::max(0, W)) * size_t(std::max(0, H)) * 3, 0.0f);
    if (W <= 0 || H <= 0) return st;
    std::vector<float> map;
    if (frame >= 0 && size_t(frame) < t.frames.size() && t.frames[size_t(frame)].size() == t.mesh.rest.size())
        map = meshMap(t.mesh, t.frames[size_t(frame)], t.mesh.rest, W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = size_t(y) * size_t(W) + size_t(x);
            double rx = x + 0.5, ry = y + 0.5;
            if (!map.empty() && !std::isnan(map[i * 2])) rx = map[i * 2], ry = map[i * 2 + 1];
            st[i * 3] = float(rx / W);
            st[i * 3 + 1] = float(1 - ry / H);
        }
    return st;
}

std::vector<float> surfaceMotion(const SurfaceTrack& t, int frame, int step) {
    const int W = t.width, H = t.height;
    std::vector<float> mv(size_t(std::max(0, W)) * size_t(std::max(0, H)) * 2, 0.0f);
    const int other = frame + step;
    auto tracked = [&](int f) { return f >= 0 && size_t(f) < t.frames.size() && t.frames[size_t(f)].size() == t.mesh.rest.size(); };
    if (W <= 0 || H <= 0 || !tracked(frame) || !tracked(other)) return mv;
    const std::vector<float> map = meshMap(t.mesh, t.frames[size_t(frame)], t.frames[size_t(other)], W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = size_t(y) * size_t(W) + size_t(x);
            if (std::isnan(map[i * 2])) continue;
            mv[i * 2] = float(map[i * 2] - (x + 0.5));
            mv[i * 2 + 1] = float(map[i * 2 + 1] - (y + 0.5));
        }
    return mv;
}

std::vector<std::string> writeSurfaceExrs(const SurfaceTrack& t, const std::string& base, int first, int last, std::string* error) {
    std::vector<std::string> written;
    if (!exrAvailable()) {
        if (error) *error = "EXR writing is not available in this build";
        return {};
    }
    first = std::max(0, first), last = std::min(last, int(t.frames.size()) - 1);
    const QFileInfo info(QString::fromStdString(base));
    QDir().mkpath(info.absolutePath());
    const size_t px = size_t(t.width) * size_t(t.height);
    for (int f = first; f <= last; ++f) {
        if (t.frames[size_t(f)].size() != t.mesh.rest.size()) continue;
        ExrOutLayer st;
        st.channels = {"R", "G", "B"};
        st.data = surfaceStMap(t, f);
        st.fullFloat = true;
        ExrOutLayer fw, bw;
        fw.name = "forward", bw.name = "backward";
        fw.channels = bw.channels = {"u", "v"};
        fw.data = surfaceMotion(t, f, 1);
        bw.data = surfaceMotion(t, f, -1);
        fw.fullFloat = bw.fullFloat = true;
        // Nuke's motion vectors point up the picture where it moves up (its y runs upwards).
        for (size_t i = 0; i < px; ++i) fw.data[i * 2 + 1] = -fw.data[i * 2 + 1], bw.data[i * 2 + 1] = -bw.data[i * 2 + 1];
        char num[16];
        std::snprintf(num, sizeof num, ".%04d.exr", f);
        const std::string file = base + num;
        if (!writeExr(file, t.width, t.height, {st, fw, bw}, "zip", {{"montage:surface_frame", std::to_string(f)}}, error)) return {};
        written.push_back(file);
    }
    if (written.empty() && error) *error = "No frame there was tracked";
    return written;
}


std::vector<MeshPoint> surfaceCarry(const SurfaceTrack& t, int from, int to, const std::vector<MeshPoint>& points) {
    auto tracked = [&](int f) { return f >= 0 && size_t(f) < t.frames.size() && t.frames[size_t(f)].size() == t.mesh.rest.size(); };
    if (!tracked(from) || !tracked(to) || t.mesh.rest.empty()) return points;
    const std::vector<MeshPoint>&A = t.frames[size_t(from)], &B = t.frames[size_t(to)];
    // (kept for the frames last asked from: each paint stroke drawn on a frame asks from it, every frame)
    // (told apart by where the points are, so a track made again in the same memory is not taken for the last)
    struct Known {
        const WarpMesh* mesh;
        const std::vector<MeshPoint>* frame;  // (the Locator reads through it)
        const MeshPoint* at;
        size_t points, tris;
        uint64_t sum;
        std::shared_ptr<const Locator> loc;
    };
    uint64_t sum = 1469598103934665603ull;
    for (const MeshPoint& p : A)
        for (double v : p) {
            uint64_t bits;
            std::memcpy(&bits, &v, 8);
            sum = (sum ^ bits) * 1099511628211ull;
        }
    for (const auto& tr : t.mesh.tris)
        for (int k : tr) sum = (sum ^ uint64_t(uint32_t(k))) * 1099511628211ull;
    thread_local std::deque<Known> known;
    std::shared_ptr<const Locator> found;
    for (const Known& k : known)
        if (k.mesh == &t.mesh && k.frame == &A && k.at == A.data() && k.points == A.size() && k.tris == t.mesh.tris.size() && k.sum == sum) found = k.loc;
    if (!found) {
        found = std::make_shared<const Locator>(t.mesh, A);
        known.push_front({&t.mesh, &A, A.data(), A.size(), t.mesh.tris.size(), sum, found});
        if (known.size() > 8) known.pop_back();
    }
    const Locator& loc = *found;
    std::vector<MeshPoint> out(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        const MeshPoint& p = points[i];
        int tri = 0;
        double l[3];
        if (loc.find(p[0], p[1], tri, l)) {
            const auto& tr = t.mesh.tris[size_t(tri)];
            out[i] = {0, 0};
            for (int k = 0; k < 3; ++k) out[i][0] += l[k] * B[size_t(tr[size_t(k)])][0], out[i][1] += l[k] * B[size_t(tr[size_t(k)])][1];
            continue;
        }
        size_t best = 0;
        double bd = 1e300;
        for (size_t v = 0; v < A.size(); ++v)
            if (const double d = std::hypot(A[v][0] - p[0], A[v][1] - p[1]); d < bd) bd = d, best = v;
        out[i] = {p[0] + B[best][0] - A[best][0], p[1] + B[best][1] - A[best][1]};
    }
    return out;
}

// ---- The effect ----------------------------------------------------------------------

namespace {

constexpr int kSurfaceWidth = 960;  // the most the frames are tracked at

struct ClipFrames {
    std::string path;
    double first = 0, last = 0, fps = 25;
    int width = 0, height = 0, count = 0;
};

bool clipFrames(const Project& p, const Sequence& seq, const Clip& c, ClipFrames& out, VideoDecoder& dec, std::string* error) {
    const MediaItem* m = c.isGenerator() ? nullptr : p.findMedia(c.mediaId);
    if (!m || m->kind != MediaKind::Video || !clipMediaRange(p, seq, c, out.path, out.first, out.last)) {
        if (error) *error = "Surfaces are tracked in video clips";
        return false;
    }
    if (!dec.open(out.path, error)) return false;
    out.fps = dec.fps() > 0 ? dec.fps() : m->fps.valid() ? m->fps.toDouble() : 25;
    const int mw = std::max(1, dec.displayWidth()), mh = std::max(1, dec.displayHeight());
    out.width = std::min(kSurfaceWidth, mw);
    out.height = std::max(16, int(std::lround(double(out.width) * mh / mw)));
    out.count = std::clamp(int(std::floor((out.last - out.first) * out.fps + 1e-6)) + 1, 1, 1000000);
    return true;
}

}  // namespace

int surfaceFrameOf(const SurfaceTrack& t, const Sequence& seq, const Clip& c, FrameTime local) {
    if (c.duration <= 0) return -1;
    return t.frameAt(c.sourceFrameAt(c.start + std::clamp<FrameTime>(local, 0, c.duration - 1)) / seq.fpsValue());
}

bool trackClipSurface(const Project& p, const Sequence& seq, const Clip& c, const Effect& e, FrameTime at, std::string& track, bool again,
                      const std::function<bool(double)>& progress, std::string* error) {
    auto fail = [&](const char* why) {
        if (error && error->empty()) *error = why;
        return false;
    };
    if (c.duration <= 0) return fail("The clip is empty");
    VideoDecoder dec;
    ClipFrames cf;
    if (!clipFrames(p, seq, c, cf, dec, error)) return false;
    at = std::clamp<FrameTime>(at, 0, c.duration - 1);
    const double atSeconds = c.sourceFrameAt(c.start + at) / seq.fpsValue();
    const int from = std::clamp(int(std::lround((atSeconds - cf.first) * cf.fps)), 0, cf.count - 1);
    SurfaceTrack t;
    if (again) {
        if (!surfaceTrackFromString(e.s("track"), t) || t.width != cf.width || t.height != cf.height || int(t.frames.size()) != cf.count ||
            std::fabs(t.start - cf.first) > 0.5 / cf.fps)
            return fail("Track the surface first");
    } else {
        const std::vector<std::array<double, 2>> outline = maskOutline(e, at, cf.width, cf.height);
        if (outline.size() < 3) return fail("Draw a mask round the surface first (an ellipse, a rectangle or a shape)");
        t.width = cf.width, t.height = cf.height;
        t.start = cf.first, t.frameSeconds = 1.0 / cf.fps;
        t.reference = from;
        t.mesh = surfaceMesh(outline, cf.width, cf.height, int(std::lround(e.p("detail", at, 200))));
        if (t.mesh.tris.empty()) return fail("The mask is too small to track");
    }
    const SurfaceFrames frames = [&](int i, GrayImage& out) {
        Frame16Ptr f = dec.frameAt(cf.first + double(i) / cf.fps, cf.width, cf.height);
        if (!f || f->width != cf.width || f->height != cf.height) return false;
        out = toGray(*f);
        return true;
    };
    if (!trackSurface(t, cf.count, from, frames, {}, progress, error)) return false;
    track = surfaceTrackToString(t);
    return true;
}

void surfaceTrackEffect(const Effect& e, FrameTime t, Image& img, double sourceSeconds) {
    if (img.empty() || sourceSeconds < 0) return;
    const std::shared_ptr<const SurfaceTrack> tr = cachedSurfaceTrack(e);
    if (!tr) return;
    const int frame = tr->frameAt(sourceSeconds);
    if (frame < 0) return;
    const int mode = int(std::lround(e.p("mode", t, 0)));
    if (mode == 4) return;  // (the track only: for masks and paint that follow it)
    if (mode == 2 || mode == 3) {
        // Held still, the frame is kept so that, put back, what is round the surface is as it was.
        const SurfaceContext* sc = currentSurface();
        const Image src = img;
        if (mode == 2 && sc) sc->heldPlate = std::make_shared<const Image>(img);
        if (mode == 3 && sc && sc->heldPlate && sc->heldPlate->width == img.width && sc->heldPlate->height == img.height) img = *sc->heldPlate;
        drawOnSurface(*tr, frame, src, img, mode == 2);
        return;
    }
    if (mode == 1) {
        // The picture where it was put on the reference frame, carried on to this one.
        const std::string path = e.s("picture");
        Frame16Ptr f = path.empty() ? nullptr : MediaPool::instance().videoFrame(path, 0);
        if (!f || f->width <= 0 || f->height <= 0) return;
        const Image pic = toImage(*f);
        const double w = std::max(1.0, e.p("picture_w", t, 0.3) * img.width), h = w * pic.height / pic.width;
        const double x0 = e.p("picture_x", t, 0.5) * img.width - w / 2, y0 = e.p("picture_y", t, 0.5) * img.height - h / 2;
        const float opacity = float(std::clamp(e.p("opacity", t, 100) / 100.0, 0.0, 1.0));
        Image layer(img.width, img.height);
        parallelRows(img.height, [&](int ya, int yb) {
            for (int y = ya; y < yb; ++y)
                for (int x = 0; x < img.width; ++x) {
                    const double u = (x + 0.5 - x0) / w * pic.width, v = (y + 0.5 - y0) / h * pic.height;
                    if (u < 0 || v < 0 || u > pic.width || v > pic.height) continue;
                    float s[4];
                    sampleCubic(pic, u, v, s);
                    float* o = layer.at(x, y);
                    for (int k = 0; k < 4; ++k) o[k] = std::max(0.0f, s[k]) * opacity;
                }
        });
        drawOnSurface(*tr, frame, layer, img, false);
        return;
    }
    // The mesh, in grey.
    const double sx = double(img.width) / tr->width, sy = double(img.height) / tr->height;
    const std::vector<MeshPoint>& v = tr->frames[size_t(frame)];
    auto line = [&](const MeshPoint& a, const MeshPoint& b) {
        const double ax = a[0] * sx, ay = a[1] * sy, bx = b[0] * sx, by = b[1] * sy;
        const int steps = std::max(1, int(std::ceil(std::max(std::fabs(bx - ax), std::fabs(by - ay)))));
        for (int i = 0; i <= steps; ++i) {
            const int x = int(std::floor(ax + (bx - ax) * i / steps)), y = int(std::floor(ay + (by - ay) * i / steps));
            if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
            float* o = img.at(x, y);
            for (int k = 0; k < 3; ++k) o[k] = 0.75f * 0.75f + o[k] * 0.25f;
            o[3] = 0.75f + o[3] * 0.25f;
        }
    };
    std::set<std::pair<int, int>> edges;
    for (const auto& tri : tr->mesh.tris)
        for (int k = 0; k < 3; ++k) edges.insert(std::minmax(tri[size_t(k)], tri[size_t((k + 1) % 3)]));
    for (const auto& [a, b] : edges) line(v[size_t(a)], v[size_t(b)]);
}

}  // namespace montage
