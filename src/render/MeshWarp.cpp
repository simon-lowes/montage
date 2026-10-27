#include "MeshWarp.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <numeric>

#include "FollowFocus.h"
#include "core/Effects.h"
#include "core/LinearSolve.h"
#include "media/Tracking.h"

namespace montage {

namespace {

constexpr double kPi = 3.14159265358979323846;
// A pin's pull against a triangle's wish to keep its shape: soft, so one factored matrix serves wherever the pins go,
// and strong enough that a pin lands within a hundredth of a pixel of where it is put.
constexpr double kPinWeight = 1e6;
// A fully stiff triangle's weight against an ordinary one's.
constexpr double kStiffWeight = 1000;
// How faintly step 1 keeps each edge unturned and its length (so a part left free to turn or grow, as two shapes touching
// at one vertex are, still solves, and moves as little as it can).
constexpr double kLooseWeight = 1e-9;
// Passes of as-rigid-as-possible refinement after the two steps.
constexpr int kRefine = 4;

double cross(const MeshPoint& a, const MeshPoint& b) { return a[0] * b[1] - a[1] * b[0]; }
MeshPoint sub(const MeshPoint& a, const MeshPoint& b) { return {a[0] - b[0], a[1] - b[1]}; }

// How much of a pin's effect reaches a distance d: all of it at the pin, none at its radius, smoothly between.
double reach(double d, double r) {
    if (r <= 0 || d >= r) return 0;
    const double q = 1 - (d / r) * (d / r);
    return q * q;
}
// A Stiff pin's: all of it over the inner three quarters, fading out to its radius.
double stiffReach(double d, double r) {
    if (r <= 0 || d >= r) return 0;
    if (d <= 0.75 * r) return 1;
    const double q = (r - d) / (0.25 * r);
    return q * q * (3 - 2 * q);
}

MeshPoint centroid(const WarpMesh& m, int t) {
    const auto& tr = m.tris[size_t(t)];
    const MeshPoint &a = m.rest[size_t(tr[0])], &b = m.rest[size_t(tr[1])], &c = m.rest[size_t(tr[2])];
    return {(a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3};
}

// A lattice of cols x rows squares over w x h, each split in two (the diagonals alternating, so it bends alike both ways).
WarpMesh lattice(int cols, int rows, double w, double h) {
    WarpMesh m;
    m.rest.reserve(size_t(cols + 1) * size_t(rows + 1));
    for (int r = 0; r <= rows; ++r)
        for (int c = 0; c <= cols; ++c) m.rest.push_back({w * c / cols, h * r / rows});
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const int a = r * (cols + 1) + c, b = a + 1, d = a + cols + 1, e = d + 1;
            if ((r + c) % 2 == 0) {
                m.tris.push_back({a, b, e});
                m.tris.push_back({a, e, d});
            } else {
                m.tris.push_back({a, b, d});
                m.tris.push_back({b, e, d});
            }
        }
    return m;
}

// Where a pin holds on to the mesh: the triangle it sits in at rest, by barycentric weights (or, off the mesh, the
// nearest vertex).
struct Attach {
    std::array<int, 3> v{};
    std::array<double, 3> b{1, 0, 0};
    MeshPoint offset{0, 0};  // off the mesh: from the point to the vertex it holds (so the gap between them is kept)
};
Attach attach(const WarpMesh& m, const MeshPoint& p) {
    Attach out;
    double nearest = std::numeric_limits<double>::infinity();
    bool snapped = false;
    for (size_t t = 0; t < m.tris.size(); ++t) {
        const auto& tr = m.tris[t];
        const MeshPoint &a = m.rest[size_t(tr[0])], &b = m.rest[size_t(tr[1])], &c = m.rest[size_t(tr[2])];
        const double den = cross(sub(b, a), sub(c, a));
        if (std::fabs(den) < 1e-12) continue;
        const double lb = cross(sub(p, a), sub(c, a)) / den, lc = cross(sub(b, a), sub(p, a)) / den, la = 1 - lb - lc;
        if (la >= -1e-9 && lb >= -1e-9 && lc >= -1e-9) {
            out.v = tr;
            out.b = {la, lb, lc};
            return out;
        }
        for (int k = 0; k < 3; ++k) {
            const MeshPoint& q = m.rest[size_t(tr[size_t(k)])];
            const double d = std::hypot(q[0] - p[0], q[1] - p[1]);
            if (!snapped || d < nearest) nearest = d, snapped = true, out.v = {tr[size_t(k)], tr[size_t(k)], tr[size_t(k)]}, out.b = {1, 0, 0};
        }
    }
    if (snapped) out.offset = sub(m.rest[size_t(out.v[0])], p);
    return out;
}

// v^T Q v for the quadratic form over `loc`'s unknowns (each q_k q_l), added to `entries`.
void addForm(std::vector<SparseCholesky::Entry>& entries, const int* loc, const double* q, int n, double w) {
    for (int k = 0; k < n; ++k) {
        if (q[k] == 0) continue;
        entries.push_back({loc[k], loc[k], w * q[k] * q[k]});
        for (int l = k + 1; l < n; ++l) {
            if (q[l] == 0) continue;
            // (one entry stands for both halves of an off-diagonal pair; on the diagonal it is both)
            entries.push_back({loc[k], loc[l], loc[k] == loc[l] ? 2 * w * q[k] * q[l] : w * q[k] * q[l]});
        }
    }
}

// The best rotation (radians) taking points `from` onto `to` about their middles (Procrustes in 2D).
double bestRotation(const std::vector<MeshPoint>& from, const std::vector<MeshPoint>& to) {
    MeshPoint ca{0, 0}, cb{0, 0};
    for (size_t i = 0; i < from.size(); ++i) ca[0] += from[i][0], ca[1] += from[i][1], cb[0] += to[i][0], cb[1] += to[i][1];
    const double n = double(std::max<size_t>(1, from.size()));
    ca[0] /= n, ca[1] /= n, cb[0] /= n, cb[1] /= n;
    double s = 0, c = 0;
    for (size_t i = 0; i < from.size(); ++i) {
        const MeshPoint p = sub(from[i], ca), q = sub(to[i], cb);
        c += p[0] * q[0] + p[1] * q[1];
        s += p[0] * q[1] - p[1] * q[0];
    }
    return std::atan2(s, c);
}

// The two steps' matrices for one mesh and its pins (where the pins sit, and the stiff areas), factored.
struct Solver {
    WarpSolve how = WarpSolve::Rigid;
    struct Hold {
        Attach a;
        int pin = 0;  // the pin it is (a Bend pin holding a part with no Position pin where it is)
    };
    std::vector<Hold> holds;
    std::vector<int> comp;            // each vertex's connected part
    std::vector<int> idx1, idx2;      // each vertex's unknown in step 1 (x at 2i, y at 2i + 1) and step 2 (-1: none)
    std::vector<int> triComp;
    std::vector<double> weight;       // each triangle's
    std::vector<int> group;           // the Stiff pin whose area a triangle is in (-1: none) ...
    std::vector<double> groupReach;   // ... and how far in
    std::vector<std::array<double, 6>> local;  // step 1: each vertex of a triangle in the frame of the other two (x, y)
    SparseCholesky step1, step2;
    bool rigid = false;  // step 1 factored
    bool ok = false;
};

uint64_t mix(uint64_t h, const void* p, size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ULL;
    return h;
}

uint64_t solverKey(const WarpMesh& m, const std::vector<WarpPin>& pins, WarpSolve how) {
    uint64_t h = 1469598103934665603ULL;
    const int mode = int(how);
    h = mix(h, &mode, sizeof mode);
    h = mix(h, m.rest.data(), m.rest.size() * sizeof(MeshPoint));
    h = mix(h, m.tris.data(), m.tris.size() * sizeof(m.tris[0]));
    // (every pin's kind, in order, as the holds refer to pins by their place in the list)
    const size_t n = pins.size();
    h = mix(h, &n, sizeof n);
    for (const WarpPin& p : pins) {
        const int kind = int(p.kind);
        h = mix(h, &kind, sizeof kind);
        if (p.kind == PinKind::Overlap) continue;
        h = mix(h, p.at.data(), sizeof p.at);
        if (p.kind == PinKind::Stiff) {
            h = mix(h, &p.radius, sizeof p.radius);
            h = mix(h, &p.amount, sizeof p.amount);
        }
    }
    return h;
}

std::shared_ptr<const Solver> makeSolver(const WarpMesh& m, const std::vector<WarpPin>& pins, WarpSolve how) {
    auto s = std::make_shared<Solver>();
    s->how = how;
    const int nv = int(m.rest.size()), nt = int(m.tris.size());
    // Connected parts.
    std::vector<int> parent(static_cast<size_t>(nv));
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](int x) {
        while (parent[size_t(x)] != x) x = parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
        return x;
    };
    for (const auto& tr : m.tris)
        for (int k = 1; k < 3; ++k) parent[size_t(find(tr[size_t(k)]))] = find(tr[0]);
    s->comp.resize(size_t(nv));
    for (int v = 0; v < nv; ++v) s->comp[size_t(v)] = find(v);
    s->triComp.resize(size_t(nt));
    for (int t = 0; t < nt; ++t) s->triComp[size_t(t)] = s->comp[size_t(m.tris[size_t(t)][0])];
    // What holds each part: its Position pins, or where it has none, its Bend pins where they are. A part is held in
    // as many places as it has holds half a pixel or more apart (two pins on one spot hold it in one).
    std::map<int, std::vector<MeshPoint>> places;
    auto hold = [&](size_t i) {
        Solver::Hold hd{attach(m, pins[i].at), int(i)};
        s->holds.push_back(hd);
        MeshPoint at{0, 0};
        for (int k = 0; k < 3; ++k)
            at[0] += hd.a.b[size_t(k)] * m.rest[size_t(hd.a.v[size_t(k)])][0], at[1] += hd.a.b[size_t(k)] * m.rest[size_t(hd.a.v[size_t(k)])][1];
        auto& seen = places[s->comp[size_t(hd.a.v[0])]];
        if (std::none_of(seen.begin(), seen.end(), [&](const MeshPoint& q) { return std::hypot(q[0] - at[0], q[1] - at[1]) < 0.5; }))
            seen.push_back(at);
    };
    for (size_t i = 0; i < pins.size(); ++i)
        if (pins[i].kind == PinKind::Position) hold(i);
    const auto positioned = places;
    for (size_t i = 0; i < pins.size(); ++i)
        if (pins[i].kind == PinKind::Bend && !positioned.count(s->comp[size_t(attach(m, pins[i].at).v[0])])) hold(i);
    std::map<int, int> holding;
    for (const auto& [part, at] : places) holding[part] = int(at.size());
    // Unknowns: step 1 for parts held in two places or more (the rigid solve), step 2 for parts held at all.
    s->idx1.assign(size_t(nv), -1);
    s->idx2.assign(size_t(nv), -1);
    int n1 = 0, n2 = 0;
    for (int v = 0; v < nv; ++v) {
        const auto it = holding.find(s->comp[size_t(v)]);
        if (it == holding.end()) continue;
        s->idx2[size_t(v)] = n2++;
        if (it->second >= 2 && how == WarpSolve::Rigid) s->idx1[size_t(v)] = n1++;
    }
    // Stiff areas: each triangle in the stiff area that reaches its middle furthest, weighted by how far in.
    s->weight.assign(size_t(nt), 1.0);
    s->group.assign(size_t(nt), -1);
    s->groupReach.assign(size_t(nt), 0.0);
    for (int t = 0; t < nt; ++t) {
        const MeshPoint c = centroid(m, t);
        for (size_t i = 0; i < pins.size(); ++i) {
            if (pins[i].kind != PinKind::Stiff) continue;
            const double f = stiffReach(std::hypot(c[0] - pins[i].at[0], c[1] - pins[i].at[1]), pins[i].radius) *
                             std::clamp(pins[i].amount, 0.0, 1.0);
            if (f > s->groupReach[size_t(t)]) s->groupReach[size_t(t)] = f, s->group[size_t(t)] = int(i);
        }
        s->weight[size_t(t)] = 1 + (kStiffWeight - 1) * s->groupReach[size_t(t)];
    }
    // Step 1: each vertex of a triangle kept where it was in the frame of the other two (so each triangle stays similar
    // to itself).
    std::vector<SparseCholesky::Entry> e1, e2;
    s->local.resize(size_t(nt));
    for (int t = 0; t < nt; ++t) {
        const auto& tr = m.tris[size_t(t)];
        for (int k = 0; k < 3; ++k) {
            const MeshPoint &a = m.rest[size_t(tr[size_t(k)])], &b = m.rest[size_t(tr[size_t((k + 1) % 3)])],
                            &c = m.rest[size_t(tr[size_t((k + 2) % 3)])];
            const MeshPoint d = sub(b, a), f = sub(c, a);
            const double dd = d[0] * d[0] + d[1] * d[1];
            // c = a + x (b - a) + y R90 (b - a), R90 (u, v) = (v, -u)
            s->local[size_t(t)][size_t(2 * k)] = dd > 0 ? (f[0] * d[0] + f[1] * d[1]) / dd : 0;
            s->local[size_t(t)][size_t(2 * k + 1)] = dd > 0 ? (f[0] * d[1] - f[1] * d[0]) / dd : 0;
        }
        if (s->idx1[size_t(tr[0])] < 0) continue;
        for (int k = 0; k < 3; ++k) {
            const int ia = s->idx1[size_t(tr[size_t(k)])], ib = s->idx1[size_t(tr[size_t((k + 1) % 3)])],
                      ic = s->idx1[size_t(tr[size_t((k + 2) % 3)])];
            const double x = s->local[size_t(t)][size_t(2 * k)], y = s->local[size_t(t)][size_t(2 * k + 1)];
            const int loc[6] = {2 * ia, 2 * ia + 1, 2 * ib, 2 * ib + 1, 2 * ic, 2 * ic + 1};
            // The residual c' - a' - x (b' - a') - y R90 (b' - a'), its x and y rows.
            const double rx[6] = {x - 1, y, -x, -y, 1, 0};
            const double ry[6] = {-y, x - 1, y, -x, 0, 1};
            addForm(e1, loc, rx, 6, s->weight[size_t(t)]);
            addForm(e1, loc, ry, 6, s->weight[size_t(t)]);
        }
    }
    // Step 2: each edge of each triangle kept as long as it was, turned as step 1 turned the triangle (x and y apart,
    // the same matrix).
    for (int t = 0; t < nt; ++t) {
        const auto& tr = m.tris[size_t(t)];
        if (s->idx2[size_t(tr[0])] < 0) continue;
        for (int k = 0; k < 3; ++k) {
            const int loc[2] = {s->idx2[size_t(tr[size_t(k)])], s->idx2[size_t(tr[size_t((k + 1) % 3)])]};
            const double q[2] = {-1, 1};
            addForm(e2, loc, q, 2, s->weight[size_t(t)]);
        }
    }
    for (const Solver::Hold& hd : s->holds) {
        if (s->idx1[size_t(hd.a.v[0])] >= 0) {
            int lx[3], ly[3];
            for (int k = 0; k < 3; ++k) lx[k] = 2 * s->idx1[size_t(hd.a.v[size_t(k)])], ly[k] = lx[k] + 1;
            addForm(e1, lx, hd.a.b.data(), 3, kPinWeight);
            addForm(e1, ly, hd.a.b.data(), 3, kPinWeight);
        }
        int l2[3];
        for (int k = 0; k < 3; ++k) l2[k] = s->idx2[size_t(hd.a.v[size_t(k)])];
        addForm(e2, l2, hd.a.b.data(), 3, kPinWeight);
    }
    for (int t = 0; t < nt; ++t) {
        const auto& tr = m.tris[size_t(t)];
        if (s->idx1[size_t(tr[0])] < 0) continue;
        for (int k = 0; k < 3; ++k) {
            const int i = s->idx1[size_t(tr[size_t(k)])], j = s->idx1[size_t(tr[size_t((k + 1) % 3)])];
            const double q[2] = {-1, 1};
            const int lx[2] = {2 * i, 2 * j}, ly[2] = {2 * i + 1, 2 * j + 1};
            addForm(e1, lx, q, 2, kLooseWeight);
            addForm(e1, ly, q, 2, kLooseWeight);
        }
    }
    // (without step 1, the triangles keep their turns at rest)
    s->rigid = s->step1.factor(2 * n1, e1);
    s->ok = s->step2.factor(n2, e2);
    return s;
}

std::shared_ptr<const Solver> cachedSolver(const WarpMesh& m, const std::vector<WarpPin>& pins, WarpSolve how) {
    static std::mutex lock;
    static std::list<std::pair<uint64_t, std::shared_ptr<const Solver>>> cache;  // most recent first
    const uint64_t key = solverKey(m, pins, how);
    {
        std::lock_guard<std::mutex> g(lock);
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (it->first == key) {
                cache.splice(cache.begin(), cache, it);
                return it->second;
            }
    }
    std::shared_ptr<const Solver> s = makeSolver(m, pins, how);
    std::lock_guard<std::mutex> g(lock);
    cache.emplace_front(key, s);
    while (cache.size() > 12) cache.pop_back();
    return s;
}

// Draws a line over the picture (for Show Mesh and Show Grid).
void drawLine(Image& img, MeshPoint a, MeshPoint b, const float colour[4]) {
    const double len = std::hypot(b[0] - a[0], b[1] - a[1]);
    if (!(len < 1e5)) return;  // (a mesh thrown far off the picture)
    const int n = std::max(1, int(std::ceil(len * 2)));
    for (int i = 0; i <= n; ++i) {
        const double f = double(i) / n;
        const int x = int(std::floor(a[0] + (b[0] - a[0]) * f)), y = int(std::floor(a[1] + (b[1] - a[1]) * f));
        if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
        float* p = img.at(x, y);
        for (int k = 0; k < 4; ++k) p[k] = colour[k] + p[k] * (1 - colour[3]);
    }
}

// Renders `draw(time, out)` at clip frame t, or averaged over the shutter when the mesh moves through it (as many
// samples as pixels it moves, 2 to 32).
template <class Moved, class Draw>
void throughShutter(Image& img, double t, bool blur, double shutter, Moved moved, Draw draw) {
    const Image src = img;
    int n = 1;
    if (blur && shutter > 0) {
        const std::vector<MeshPoint> a = moved(t - shutter / 2), b = moved(t + shutter / 2);
        double most = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) most = std::max(most, std::hypot(b[i][0] - a[i][0], b[i][1] - a[i][1]));
        if (most >= 0.5) n = std::clamp(int(std::ceil(most)), 2, 32);
    }
    if (n == 1) {
        Image out(img.width, img.height);
        draw(src, t, out);
        img = std::move(out);
        return;
    }
    std::vector<float> sum(img.px.size(), 0.f);
    for (int i = 0; i < n; ++i) {
        Image out(img.width, img.height);
        draw(src, t + ((i + 0.5) / n - 0.5) * shutter, out);
        for (size_t k = 0; k < sum.size(); ++k) sum[k] += out.px[k];
    }
    for (size_t k = 0; k < sum.size(); ++k) img.px[k] = sum[k] / float(n);
}

// The mesh each Mesh Warp was last drawn with over its picture's alpha (by effect id, with the picture's size), for
// showing and placing its pins on the mesh the picture was drawn with.
struct LastMesh {
    int w = 0, h = 0;
    std::shared_ptr<const WarpMesh> mesh;
};
std::mutex lastMeshLock;
std::map<Id, LastMesh> lastMeshes;

void rememberMesh(const Effect& e, int w, int h, const WarpMesh& m) {
    std::lock_guard<std::mutex> g(lastMeshLock);
    if (lastMeshes.size() > 64 && !lastMeshes.count(e.id)) lastMeshes.clear();
    LastMesh& last = lastMeshes[e.id];
    if (last.w == w && last.h == h && last.mesh && last.mesh->rest == m.rest && last.mesh->tris == m.tris) return;
    last = {w, h, std::make_shared<const WarpMesh>(m)};
}

WarpMesh meshFor(const Effect& e, int w, int h) {
    if (std::lround(e.p("mesh_from", 0, 0)) == 0) {
        LastMesh last;
        {
            std::lock_guard<std::mutex> g(lastMeshLock);
            if (const auto it = lastMeshes.find(e.id); it != lastMeshes.end()) last = it->second;
        }
        if (last.mesh && last.w > 0 && last.h > 0) {
            // (drawn at another size, a preview's: scaled to this one)
            WarpMesh m = *last.mesh;
            for (MeshPoint& q : m.rest) q = {q[0] * w / last.w, q[1] * h / last.h};
            return m;
        }
    }
    return buildWarpMesh(w, h, int(std::lround(e.p("density", 0, 2000))));
}

}  // namespace

WarpMesh buildWarpMesh(int w, int h, int vertices, const Image* alpha, double grow) {
    if (w <= 0 || h <= 0) return {};
    vertices = std::clamp(vertices, 4, 1000000);
    const int cols = std::max(1, int(std::lround(std::sqrt(double(vertices) * w / h))) - 1);
    const int rows = std::max(1, int(std::lround(double(vertices) / (cols + 1))) - 1);
    WarpMesh full = lattice(cols, rows, w, h);
    if (!alpha || alpha->width != w || alpha->height != h) return full;
    // Which lattice squares have any of the picture, then which come within `grow` of one that has.
    const double cw = double(w) / cols, ch = double(h) / rows;
    std::vector<char> has(size_t(cols) * size_t(rows), 0);
    for (int y = 0; y < h; ++y) {
        const int r = std::min(rows - 1, int(y / ch));
        for (int x = 0; x < w; ++x)
            if (alpha->at(x, y)[3] > 1.0f / 255) has[size_t(r) * size_t(cols) + size_t(std::min(cols - 1, int(x / cw)))] = 1;
    }
    std::vector<char> keepSquare(has.size(), 0);
    const int reachX = int(std::ceil(std::max(0.0, grow) / cw)), reachY = int(std::ceil(std::max(0.0, grow) / ch));
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            if (!has[size_t(r) * size_t(cols) + size_t(c)]) continue;
            for (int rr = std::max(0, r - reachY); rr <= std::min(rows - 1, r + reachY); ++rr)
                for (int cc = std::max(0, c - reachX); cc <= std::min(cols - 1, c + reachX); ++cc) keepSquare[size_t(rr) * size_t(cols) + size_t(cc)] = 1;
        }
    WarpMesh m;
    std::vector<int> remap(full.rest.size(), -1);
    for (size_t t = 0; t < full.tris.size(); ++t) {
        const size_t square = t / 2;
        if (!keepSquare[square]) continue;
        std::array<int, 3> tri{};
        for (int k = 0; k < 3; ++k) {
            int& v = remap[size_t(full.tris[t][size_t(k)])];
            if (v < 0) {
                v = int(m.rest.size());
                m.rest.push_back(full.rest[size_t(full.tris[t][size_t(k)])]);
            }
            tri[size_t(k)] = v;
        }
        m.tris.push_back(tri);
    }
    return m;
}

std::vector<MeshPoint> solveWarp(const WarpMesh& m, const std::vector<WarpPin>& pins, WarpSolve how) {
    std::vector<MeshPoint> out = m.rest;
    if (m.tris.empty()) return out;
    const std::shared_ptr<const Solver> s = cachedSolver(m, pins, how);
    if (!s->ok) return out;
    const int nt = int(m.tris.size());
    if (std::any_of(s->holds.begin(), s->holds.end(), [&](const Solver::Hold& hd) { return hd.pin < 0 || size_t(hd.pin) >= pins.size(); }))
        return out;
    auto target = [&](const Solver::Hold& hd) {
        const WarpPin& p = pins[size_t(hd.pin)];
        const MeshPoint& to = p.kind == PinKind::Position ? p.to : p.at;
        return MeshPoint{to[0] + hd.a.offset[0], to[1] + hd.a.offset[1]};
    };
    // Step 1.
    std::vector<MeshPoint> first = m.rest;
    if (s->rigid && s->step1.size() > 0) {
        std::vector<double> b(size_t(s->step1.size()), 0.0);
        for (int t = 0; t < nt; ++t) {
            const auto& tr = m.tris[size_t(t)];
            if (s->idx1[size_t(tr[0])] < 0) continue;
            for (int k = 0; k < 3; ++k) {
                const int a = tr[size_t(k)], c = tr[size_t((k + 1) % 3)];
                const size_t i = size_t(s->idx1[size_t(a)]), j = size_t(s->idx1[size_t(c)]);
                const MeshPoint e = sub(m.rest[size_t(c)], m.rest[size_t(a)]);
                b[2 * j] += kLooseWeight * e[0], b[2 * j + 1] += kLooseWeight * e[1];
                b[2 * i] -= kLooseWeight * e[0], b[2 * i + 1] -= kLooseWeight * e[1];
            }
        }
        for (const Solver::Hold& hd : s->holds) {
            if (s->idx1[size_t(hd.a.v[0])] < 0) continue;
            const MeshPoint p = target(hd);
            for (int k = 0; k < 3; ++k) {
                const size_t i = size_t(2 * s->idx1[size_t(hd.a.v[size_t(k)])]);
                b[i] += kPinWeight * hd.a.b[size_t(k)] * p[0];
                b[i + 1] += kPinWeight * hd.a.b[size_t(k)] * p[1];
            }
        }
        s->step1.solve(b);
        for (size_t v = 0; v < m.rest.size(); ++v)
            if (const int i = s->idx1[v]; i >= 0) first[v] = {b[size_t(2 * i)], b[size_t(2 * i + 1)]};
    }
    // Each triangle's Bend pins' turn and scale, as its shape at rest is to be turned and scaled.
    std::vector<double> bend(static_cast<size_t>(nt), 0.0), grow(static_cast<size_t>(nt), 1.0);
    if (how == WarpSolve::Rigid)
        for (const WarpPin& p : pins) {
            if (p.kind != PinKind::Bend || (p.angle == 0 && p.scale == 1)) continue;
            for (int t = 0; t < nt; ++t) {
                const MeshPoint c = centroid(m, t);
                const double f = reach(std::hypot(c[0] - p.at[0], c[1] - p.at[1]), p.radius);
                if (f <= 0) continue;
                bend[size_t(t)] += f * p.angle * kPi / 180;
                grow[size_t(t)] *= std::pow(std::max(1e-3, p.scale), f);
            }
        }
    // A triangle's shape at rest so turned and scaled (about its first vertex: where does not matter to a turn).
    auto shaped = [&](int t) {
        const auto& tr = m.tris[size_t(t)];
        const double co = std::cos(bend[size_t(t)]) * grow[size_t(t)], si = std::sin(bend[size_t(t)]) * grow[size_t(t)];
        std::vector<MeshPoint> pts;
        const MeshPoint& o = m.rest[size_t(tr[0])];
        for (int k = 0; k < 3; ++k) {
            const MeshPoint e = sub(m.rest[size_t(tr[size_t(k)])], o);
            pts.push_back({o[0] + co * e[0] - si * e[1], o[1] + si * e[0] + co * e[1]});
        }
        return pts;
    };
    // Each triangle's turn: the one that best takes its shape (at rest in the first pass, as bent after) onto where the
    // last solve put it, a stiff area's one for all of it.
    std::vector<double> turn(static_cast<size_t>(nt), 0.0);
    auto turns = [&](const std::vector<MeshPoint>& now, bool firstPass) {
        std::map<int, std::pair<std::vector<MeshPoint>, std::vector<MeshPoint>>> areas;
        for (int t = 0; t < nt; ++t) {
            const auto& tr = m.tris[size_t(t)];
            turn[size_t(t)] = 0;
            if ((firstPass ? s->idx1 : s->idx2)[size_t(tr[0])] < 0) continue;
            std::vector<MeshPoint> from, to;
            if (firstPass)
                for (int k = 0; k < 3; ++k) from.push_back(m.rest[size_t(tr[size_t(k)])]);
            else
                from = shaped(t);
            for (int k = 0; k < 3; ++k) to.push_back(now[size_t(tr[size_t(k)])]);
            turn[size_t(t)] = bestRotation(from, to);
            if (s->group[size_t(t)] >= 0 && s->groupReach[size_t(t)] > 0) {
                auto& area = areas[s->group[size_t(t)]];
                area.first.insert(area.first.end(), from.begin(), from.end());
                area.second.insert(area.second.end(), to.begin(), to.end());
            }
        }
        std::map<int, double> areaTurn;
        for (const auto& [g, pts] : areas) areaTurn[g] = bestRotation(pts.first, pts.second);
        for (int t = 0; t < nt; ++t) {
            const auto it = areaTurn.find(s->group[size_t(t)]);
            if (it == areaTurn.end()) continue;
            const double d = std::remainder(it->second - turn[size_t(t)], 2 * kPi);
            turn[size_t(t)] += d * s->groupReach[size_t(t)];
        }
    };
    // Step 2: each triangle's edges as its shape has them, turned so; then (as-rigid-as-possible's own refinement, on the
    // same matrix) the turns fitted again to that and solved again, a few times.
    if (s->step2.size() == 0) return out;
    auto step2 = [&]() {
        std::vector<double> bx(size_t(s->step2.size()), 0.0), by(bx.size(), 0.0);
        for (int t = 0; t < nt; ++t) {
            const auto& tr = m.tris[size_t(t)];
            if (s->idx2[size_t(tr[0])] < 0) continue;
            const double r = turn[size_t(t)] + bend[size_t(t)];
            const double co = std::cos(r) * grow[size_t(t)], si = std::sin(r) * grow[size_t(t)];
            const double w = s->weight[size_t(t)];
            for (int k = 0; k < 3; ++k) {
                const int a = tr[size_t(k)], b = tr[size_t((k + 1) % 3)];
                const MeshPoint e = sub(m.rest[size_t(b)], m.rest[size_t(a)]);
                const double ux = co * e[0] - si * e[1], uy = si * e[0] + co * e[1];
                const size_t ia = size_t(s->idx2[size_t(a)]), ib = size_t(s->idx2[size_t(b)]);
                bx[ib] += w * ux, by[ib] += w * uy;
                bx[ia] -= w * ux, by[ia] -= w * uy;
            }
        }
        for (const Solver::Hold& hd : s->holds) {
            const MeshPoint p = target(hd);
            for (int k = 0; k < 3; ++k) {
                const size_t i = size_t(s->idx2[size_t(hd.a.v[size_t(k)])]);
                bx[i] += kPinWeight * hd.a.b[size_t(k)] * p[0];
                by[i] += kPinWeight * hd.a.b[size_t(k)] * p[1];
            }
        }
        s->step2.solve(bx);
        s->step2.solve(by);
        for (size_t v = 0; v < m.rest.size(); ++v)
            if (const int i = s->idx2[v]; i >= 0) out[v] = {bx[size_t(i)], by[size_t(i)]};
    };
    if (how == WarpSolve::Rigid && s->rigid) turns(first, true);
    step2();
    if (how == WarpSolve::Rigid)
        for (int pass = 0; pass < kRefine; ++pass) {
            turns(out, false);
            step2();
        }
    return out;
}

std::vector<double> warpDepths(const WarpMesh& m, const std::vector<WarpPin>& pins) {
    std::vector<double> depth(m.tris.size(), 0.0);
    for (size_t t = 0; t < m.tris.size(); ++t) {
        const MeshPoint c = centroid(m, int(t));
        bool any = false;
        for (const WarpPin& p : pins) {
            if (p.kind != PinKind::Overlap || std::hypot(c[0] - p.at[0], c[1] - p.at[1]) >= p.radius) continue;
            depth[t] = any ? std::max(depth[t], p.depth) : p.depth;
            any = true;
        }
    }
    return depth;
}

void drawWarp(const Image& src, const WarpMesh& m, const std::vector<MeshPoint>& moved, const std::vector<double>& depth, Image& out) {
    const int W = out.width, H = out.height;
    if (W <= 0 || H <= 0 || moved.size() != m.rest.size()) return;
    // Each triangle's edges as edge functions A x + B y + C, worked out from the edge's lower-numbered vertex to its
    // other, so the two triangles either side of an edge see the same numbers and share its pixels out exactly.
    struct Tri {
        double A[3], B[3], C[3], at[3];  // edge opposite vertex k; its value at vertex k
        bool own[3];                     // a pixel centre exactly on edge k is this triangle's
        int x0, x1, y0, y1;
        MeshPoint r[3];
    };
    std::vector<Tri> tris;
    tris.reserve(m.tris.size());
    std::vector<int> order(m.tris.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return (depth.size() == m.tris.size() ? depth[size_t(a)] : 0) < (depth.size() == m.tris.size() ? depth[size_t(b)] : 0);
    });
    for (int t : order) {
        const auto& tr = m.tris[size_t(t)];
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
            d.r[k] = m.rest[size_t(tr[size_t(k)])];
            minX = std::min(minX, V[0]), maxX = std::max(maxX, V[0]), minY = std::min(minY, V[1]), maxY = std::max(maxY, V[1]);
        }
        if (!ok || !(minX < W && maxX > -1 && minY < H && maxY > -1)) continue;  // (off the picture, or not a number)
        d.x0 = int(std::floor(std::max(minX - 0.5, 0.0))), d.x1 = int(std::ceil(std::min(maxX - 0.5, double(W - 1))));
        d.y0 = int(std::floor(std::max(minY - 0.5, 0.0))), d.y1 = int(std::ceil(std::min(maxY - 0.5, double(H - 1))));
        if (d.x0 > d.x1 || d.y0 > d.y1) continue;
        tris.push_back(d);
    }
    auto sample = [&](double x, double y, float o[4]) {
        // Bilinear, clamped to the picture's edge (pixel centres at +0.5).
        x = std::clamp(x - 0.5, 0.0, double(src.width - 1));
        y = std::clamp(y - 0.5, 0.0, double(src.height - 1));
        const int x0 = std::min(int(x), src.width - 1), y0 = std::min(int(y), src.height - 1);
        const int x1 = std::min(x0 + 1, src.width - 1), y1 = std::min(y0 + 1, src.height - 1);
        const float fx = float(x - x0), fy = float(y - y0);
        const float *a = src.at(x0, y0), *b = src.at(x1, y0), *c = src.at(x0, y1), *e = src.at(x1, y1);
        for (int k = 0; k < 4; ++k) {
            const float top = a[k] + (b[k] - a[k]) * fx, bottom = c[k] + (e[k] - c[k]) * fx;
            o[k] = top + (bottom - top) * fy;
        }
    };
    if (src.empty()) return;
    parallelRows(H, [&](int y0, int y1) {
        for (const Tri& d : tris) {
            const int ya = std::max(d.y0, y0), yb = std::min(d.y1, y1 - 1);
            for (int y = ya; y <= yb; ++y) {
                const double py = y + 0.5;
                float* row = out.row(y);
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
                    float s[4];
                    sample(l[0] * d.r[0][0] + l[1] * d.r[1][0] + l[2] * d.r[2][0], l[0] * d.r[0][1] + l[1] * d.r[1][1] + l[2] * d.r[2][1], s);
                    float* o = row + size_t(x) * 4;
                    for (int k = 0; k < 4; ++k) o[k] = s[k] + o[k] * (1 - s[3]);
                }
            }
        }
    });
}

std::vector<WarpPin> meshWarpPins(const Effect& e, double t, int w, int h, double pixelScale) {
    std::vector<WarpPin> out;
    for (int k : meshPins(e)) {
        const std::string pre = "pin" + std::to_string(k) + ".";
        WarpPin p;
        p.kind = PinKind(meshPinKind(e, k));
        const double x = e.pAt(pre + "x", t, 0.5), y = e.pAt(pre + "y", t, 0.5);
        p.at = {x * w, y * h};
        p.to = {e.pAt(pre + "to_x", t, x) * w, e.pAt(pre + "to_y", t, y) * h};
        p.radius = std::max(0.0, e.pAt(pre + "radius", t, 80)) * pixelScale;
        p.amount = std::clamp(e.pAt(pre + "amount", t, 100) / 100, 0.0, 1.0);
        p.angle = e.pAt(pre + "angle", t, 0);
        p.scale = std::max(0.01, e.pAt(pre + "scale", t, 100) / 100);
        p.depth = e.pAt(pre + "depth", t, 50);
        out.push_back(p);
    }
    return out;
}

void meshWarp(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    if (img.empty()) return;
    const int W = img.width, H = img.height;
    const bool blur = e.p("motion_blur", t, 0) > 0.5;
    const double shutter = std::max(0.0, e.p("shutter", t, 180)) / 360;
    const bool show = e.p("show_mesh", t, 0) > 0.5;
    auto still = [](const std::vector<WarpPin>& ps) {
        for (const WarpPin& p : ps) {
            if (p.kind == PinKind::Position && p.to != p.at) return false;
            if (p.kind == PinKind::Bend && (p.angle != 0 || p.scale != 1)) return false;
        }
        return true;
    };
    // Pins where they are (and no bends) leave the picture exactly as it was.
    if (!show && still(meshWarpPins(e, double(t), W, H, pixelScale)) &&
        (!blur || (still(meshWarpPins(e, t - shutter / 2, W, H, pixelScale)) && still(meshWarpPins(e, t + shutter / 2, W, H, pixelScale)))))
        return;
    const WarpMesh mesh = buildWarpMesh(W, H, int(std::lround(e.p("density", t, 2000))),
                                        std::lround(e.p("mesh_from", t, 0)) == 0 ? &img : nullptr, std::max(0.0, e.p("expand", t, 3)) * pixelScale);
    if (std::lround(e.p("mesh_from", t, 0)) == 0) rememberMesh(e, W, H, mesh);
    auto moved = [&](double time) { return solveWarp(mesh, meshWarpPins(e, time, W, H, pixelScale)); };
    throughShutter(img, double(t), blur, shutter, moved, [&](const Image& src, double time, Image& out) {
        const std::vector<WarpPin> pins = meshWarpPins(e, time, W, H, pixelScale);
        drawWarp(src, mesh, solveWarp(mesh, pins), warpDepths(mesh, pins), out);
    });
    if (show) {
        const std::vector<MeshPoint> v = moved(double(t));
        const float colour[4] = {0.75f, 0.75f, 0.75f, 0.75f};
        for (const auto& tr : mesh.tris)
            for (int k = 0; k < 3; ++k) {
                const int a = tr[size_t(k)], b = tr[size_t((k + 1) % 3)];
                drawLine(img, v[size_t(a)], v[size_t(b)], colour);
            }
    }
}

std::vector<MeshPoint> meshWarpMoved(const Effect& e, double t, int w, int h, double pixelScale, const std::vector<MeshPoint>& rest) {
    if (w <= 0 || h <= 0) return rest;
    const WarpMesh mesh = meshFor(e, w, h);
    if (mesh.tris.empty()) return rest;
    const std::vector<MeshPoint> v = solveWarp(mesh, meshWarpPins(e, t, w, h, pixelScale));
    std::vector<MeshPoint> out;
    for (const MeshPoint& p : rest) {
        const Attach a = attach(mesh, p);
        MeshPoint q{-a.offset[0], -a.offset[1]};
        for (int k = 0; k < 3; ++k) q[0] += a.b[size_t(k)] * v[size_t(a.v[size_t(k)])][0], q[1] += a.b[size_t(k)] * v[size_t(a.v[size_t(k)])][1];
        out.push_back(q);
    }
    return out;
}

bool meshWarpRestOf(const Effect& e, double t, int w, int h, double pixelScale, const MeshPoint& shown, MeshPoint& rest) {
    if (w <= 0 || h <= 0) return false;
    const WarpMesh mesh = meshFor(e, w, h);
    const std::vector<WarpPin> pins = meshWarpPins(e, t, w, h, pixelScale);
    const std::vector<MeshPoint> v = solveWarp(mesh, pins);
    const std::vector<double> depth = warpDepths(mesh, pins);
    // (the one in front where the mesh is folded over itself)
    double best = -1e300;
    bool found = false;
    for (size_t t3 = 0; t3 < mesh.tris.size(); ++t3) {
        const auto& tr = mesh.tris[t3];
        const MeshPoint &a = v[size_t(tr[0])], &b = v[size_t(tr[1])], &c = v[size_t(tr[2])];
        const double den = cross(sub(b, a), sub(c, a));
        if (std::fabs(den) < 1e-12) continue;
        const double lb = cross(sub(shown, a), sub(c, a)) / den, lc = cross(sub(b, a), sub(shown, a)) / den, la = 1 - lb - lc;
        if (la < -1e-9 || lb < -1e-9 || lc < -1e-9 || (found && depth[t3] < best)) continue;
        const MeshPoint &ra = mesh.rest[size_t(tr[0])], &rb = mesh.rest[size_t(tr[1])], &rc = mesh.rest[size_t(tr[2])];
        rest = {la * ra[0] + lb * rb[0] + lc * rc[0], la * ra[1] + lb * rb[1] + lc * rc[1]};
        best = depth[t3];
        found = true;
    }
    return found;
}

MeshPoint gridWarpPointAt(const Effect& e, double t, int r, int c) {
    const int rows = std::clamp(int(std::lround(e.p("rows", 0, 3))), 1, 12), cols = std::clamp(int(std::lround(e.p("cols", 0, 3))), 1, 12);
    const std::string n = gridWarpPoint(r, c);
    return {double(c) / cols + e.pAt(n + ".dx", t, 0), double(r) / rows + e.pAt(n + ".dy", t, 0)};
}

void gridWarp(const Effect& e, FrameTime t, Image& img) {
    if (img.empty()) return;
    const int W = img.width, H = img.height;
    const int rows = std::clamp(int(std::lround(e.p("rows", 0, 3))), 1, 12), cols = std::clamp(int(std::lround(e.p("cols", 0, 3))), 1, 12);
    const bool smooth = std::lround(e.p("shape", t, 0)) == 0;
    const bool blur = e.p("motion_blur", t, 0) > 0.5;
    const double shutter = std::max(0.0, e.p("shutter", t, 180)) / 360;
    const bool show = e.p("show_grid", t, 0) > 0.5;
    // The points' moves (pixels) at a time.
    auto moves = [&](double time) {
        std::vector<MeshPoint> d(size_t(rows + 1) * size_t(cols + 1));
        for (int r = 0; r <= rows; ++r)
            for (int c = 0; c <= cols; ++c) {
                const std::string n = gridWarpPoint(r, c);
                d[size_t(r) * size_t(cols + 1) + size_t(c)] = {e.pAt(n + ".dx", time, 0) * W, e.pAt(n + ".dy", time, 0) * H};
            }
        return d;
    };
    auto still = [](const std::vector<MeshPoint>& d) {
        return std::all_of(d.begin(), d.end(), [](const MeshPoint& p) { return p[0] == 0 && p[1] == 0; });
    };
    if (!show && still(moves(double(t))) && (!blur || (still(moves(t - shutter / 2)) && still(moves(t + shutter / 2))))) return;
    // A fine lattice whose vertices follow the points: Catmull-Rom between them (the end points repeated), or straight.
    const int per = std::max(2, 64 / std::max(rows, cols));
    const WarpMesh mesh = lattice(cols * per, rows * per, W, H);
    auto moved = [&](double time) {
        const std::vector<MeshPoint> d = moves(time);
        auto at = [&](int r, int c) -> const MeshPoint& {
            return d[size_t(std::clamp(r, 0, rows)) * size_t(cols + 1) + size_t(std::clamp(c, 0, cols))];
        };
        auto spline = [](const MeshPoint& p0, const MeshPoint& p1, const MeshPoint& p2, const MeshPoint& p3, double f) {
            MeshPoint o;
            for (int k = 0; k < 2; ++k)
                o[size_t(k)] = 0.5 * (2 * p1[size_t(k)] + (p2[size_t(k)] - p0[size_t(k)]) * f +
                                      (2 * p0[size_t(k)] - 5 * p1[size_t(k)] + 4 * p2[size_t(k)] - p3[size_t(k)]) * f * f +
                                      (3 * p1[size_t(k)] - p0[size_t(k)] - 3 * p2[size_t(k)] + p3[size_t(k)]) * f * f * f);
            return o;
        };
        auto lerp = [](const MeshPoint& a, const MeshPoint& b, double f) { return MeshPoint{a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f}; };
        std::vector<MeshPoint> out = mesh.rest;
        for (int v = 0; v <= rows * per; ++v)
            for (int u = 0; u <= cols * per; ++u) {
                const int r = std::min(v / per, rows - 1), c = std::min(u / per, cols - 1);
                const double fy = double(v) / per - r, fx = double(u) / per - c;
                MeshPoint m;
                if (smooth) {
                    MeshPoint across[4];
                    for (int i = 0; i < 4; ++i)
                        across[i] = spline(at(r - 1 + i, c - 1), at(r - 1 + i, c), at(r - 1 + i, c + 1), at(r - 1 + i, c + 2), fx);
                    m = spline(across[0], across[1], across[2], across[3], fy);
                } else {
                    m = lerp(lerp(at(r, c), at(r, c + 1), fx), lerp(at(r + 1, c), at(r + 1, c + 1), fx), fy);
                }
                MeshPoint& p = out[size_t(v) * size_t(cols * per + 1) + size_t(u)];
                p[0] += m[0], p[1] += m[1];
            }
        return out;
    };
    const std::vector<double> flat;
    throughShutter(img, double(t), blur, shutter, moved,
                   [&](const Image& src, double time, Image& out) { drawWarp(src, mesh, moved(time), flat, out); });
    if (show) {
        const std::vector<MeshPoint> v = moved(double(t));
        const int stride = cols * per + 1;
        const float colour[4] = {0.75f, 0.75f, 0.75f, 0.75f};
        for (int r = 0; r <= rows * per; ++r)
            for (int c = 0; c <= cols * per; ++c) {
                const MeshPoint& p = v[size_t(r) * size_t(stride) + size_t(c)];
                if (r % per == 0 && c < cols * per) drawLine(img, p, v[size_t(r) * size_t(stride) + size_t(c + 1)], colour);
                if (c % per == 0 && r < rows * per) drawLine(img, p, v[size_t(r + 1) * size_t(stride) + size_t(c)], colour);
            }
    }
}

bool trackMeshPins(const Project& p, const Sequence& seq, const Clip& c, const Effect& e, FrameTime at,
                   std::map<int, std::pair<Param, Param>>& keys, const std::function<bool(double)>& progress, std::string* error) {
    auto fail = [&](const char* why) {
        if (error && error->empty()) *error = why;
        return false;
    };
    keys.clear();
    if (c.duration <= 0) return fail("The clip is empty");
    std::string path;
    double first = 0, last = 0;
    const MediaItem* m = c.isGenerator() ? nullptr : p.findMedia(c.mediaId);
    if (!m || !clipMediaRange(p, seq, c, path, first, last)) return fail("Pins are tracked in video clips");
    std::vector<int> pins;
    for (int k : meshPins(e))
        if (meshPinKind(e, k) == 0) pins.push_back(k);
    if (pins.empty()) return fail("Add a Position pin to track first");
    const double fps = seq.fpsValue();
    const double frameSeconds = m->fps.valid() ? 1.0 / m->fps.toDouble() : 1.0 / 30;
    at = std::clamp<FrameTime>(at, 0, c.duration - 1);
    auto mediaTime = [&](FrameTime local) {
        return std::clamp(c.sourceFrameAt(c.start + std::clamp<FrameTime>(local, 0, c.duration - 1)) / fps, 0.0,
                          std::max(0.0, m->duration - frameSeconds * 0.5));
    };
    const double from = mediaTime(at);
    const double aspect = m->height > 0 ? double(m->width) / m->height : 16.0 / 9;
    std::atomic<bool> cancelled{false};
    for (size_t i = 0; i < pins.size(); ++i) {
        const std::string pre = "pin" + std::to_string(pins[i]) + ".";
        const double x = e.p(pre + "x", at, 0.5), y = e.p(pre + "y", at, 0.5);
        TrackRegion start;
        start.x = e.p(pre + "to_x", at, x), start.y = e.p(pre + "to_y", at, y);
        auto stage = [&, i](double a, double b) {
            return [&, i, a, b](double f) {
                if (progress && !progress((double(i) + a + (b - a) * f) / double(pins.size()))) cancelled = true;
            };
        };
        // A small region round the pin first; where it has too little detail to hold on to and is lost, a larger one.
        std::vector<TrackRegion> ahead, behind;
        const size_t wantAhead = last > from ? size_t(std::floor((last - from) / frameSeconds + 1e-6)) + 1 : 1;
        const size_t wantBehind = first < from ? size_t(std::floor((from - first) / frameSeconds + 1e-6)) + 1 : 1;
        for (double size : {0.1, 0.2, 0.3}) {
            start.w = size, start.h = size * aspect;
            std::vector<TrackRegion> a = last > from ? trackRegion(path, from, last, start, MotionModel::TranslationScale, stage(0, 0.5), &cancelled, error)
                                                     : std::vector<TrackRegion>{start};
            std::vector<TrackRegion> b = first < from ? trackRegion(path, from, first, start, MotionModel::TranslationScale, stage(0.5, 1), &cancelled, error)
                                                      : std::vector<TrackRegion>{start};
            if (cancelled) return fail("Cancelled");
            if (a.size() + b.size() > ahead.size() + behind.size()) ahead = std::move(a), behind = std::move(b);
            if (ahead.size() >= wantAhead && behind.size() >= wantBehind) break;
        }
        Param px(start.x), py(start.y);
        for (FrameTime f = 0; f < c.duration; ++f) {
            const double k = (mediaTime(f) - from) / frameSeconds;
            const std::vector<TrackRegion>& run = k >= 0 ? ahead : behind;
            const long n = long(std::lround(std::fabs(k)));
            if (n >= long(run.size())) continue;  // lost by then: held
            px.addKey(f, run[size_t(n)].x);
            py.addKey(f, run[size_t(n)].y);
        }
        if (px.keys.size() < 2) return fail("A pin could not be tracked");
        keys[pins[i]] = {px, py};
    }
    if (progress) progress(1.0);
    return true;
}

}  // namespace montage
