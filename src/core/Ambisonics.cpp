#include "Ambisonics.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace montage {

namespace {

constexpr double kDeg = M_PI / 180;
constexpr double kHeadRadius = 0.0875;  // metres
constexpr double kSoundSpeed = 343.0;   // metres a second

// Unit direction in ambisonic axes (X ahead, Y to the left, Z up) from Montage's angle (positive to the right).
void direction(double angle, double elevation, double& x, double& y, double& z) {
    const double az = -angle * kDeg, el = elevation * kDeg;
    x = std::cos(az) * std::cos(el);
    y = std::sin(az) * std::cos(el);
    z = std::sin(el);
}

// The real spherical harmonics to third order at a unit direction, ACN order, SN3D (no Condon-Shortley phase).
void harmonics(int order, double x, double y, double z, double* out) {
    out[0] = 1;
    if (order < 1) return;
    out[1] = y, out[2] = z, out[3] = x;
    if (order < 2) return;
    const double s3 = std::sqrt(3.0);
    out[4] = s3 * x * y;
    out[5] = s3 * y * z;
    out[6] = 0.5 * (3 * z * z - 1);
    out[7] = s3 * x * z;
    out[8] = 0.5 * s3 * (x * x - y * y);
    if (order < 3) return;
    const double a = std::sqrt(5.0 / 8), b = std::sqrt(15.0), c = std::sqrt(3.0 / 8);
    out[9] = a * y * (3 * x * x - y * y);
    out[10] = b * x * y * z;
    out[11] = c * y * (5 * z * z - 1);
    out[12] = 0.5 * z * (5 * z * z - 3);
    out[13] = c * x * (5 * z * z - 1);
    out[14] = 0.5 * b * z * (x * x - y * y);
    out[15] = a * x * (x * x - 3 * y * y);
}

// Directions spread evenly over the sphere (a Fibonacci lattice), as unit vectors.
std::vector<std::array<double, 3>> evenGrid(int n) {
    std::vector<std::array<double, 3>> out;
    const double golden = M_PI * (3 - std::sqrt(5.0));
    for (int i = 0; i < n; ++i) {
        const double z = 1 - (2 * i + 1) / double(n), r = std::sqrt(std::max(0.0, 1 - z * z)), a = golden * i;
        out.push_back({r * std::cos(a), r * std::sin(a), z});
    }
    return out;
}

// Max-rE weights (Zotter and Frank): order l of an order-N decode scaled by P_l(cos(137.9° / (N + 1.51))).
double maxRe(int order, int l) {
    const double x = std::cos(137.9 * kDeg / (order + 1.51));
    switch (l) {
    case 0: return 1;
    case 1: return x;
    case 2: return 0.5 * (3 * x * x - 1);
    default: return 0.5 * (5 * x * x * x - 3 * x);
    }
}

int degreeOf(int acn) { return int(std::sqrt(double(acn) + 1e-9)); }

// In place inverse of an n × n matrix (Gauss-Jordan with pivoting); false if singular.
bool invert(std::vector<double>& m, int n) {
    std::vector<double> inv(size_t(n) * size_t(n), 0.0);
    for (int i = 0; i < n; ++i) inv[size_t(i * n + i)] = 1;
    for (int c = 0; c < n; ++c) {
        int p = c;
        for (int r = c + 1; r < n; ++r)
            if (std::fabs(m[size_t(r * n + c)]) > std::fabs(m[size_t(p * n + c)])) p = r;
        if (std::fabs(m[size_t(p * n + c)]) < 1e-12) return false;
        for (int k = 0; k < n; ++k) {
            std::swap(m[size_t(c * n + k)], m[size_t(p * n + k)]);
            std::swap(inv[size_t(c * n + k)], inv[size_t(p * n + k)]);
        }
        const double d = m[size_t(c * n + c)];
        for (int k = 0; k < n; ++k) m[size_t(c * n + k)] /= d, inv[size_t(c * n + k)] /= d;
        for (int r = 0; r < n; ++r) {
            if (r == c) continue;
            const double f = m[size_t(r * n + c)];
            if (f == 0) continue;
            for (int k = 0; k < n; ++k) m[size_t(r * n + k)] -= f * m[size_t(c * n + k)], inv[size_t(r * n + k)] -= f * inv[size_t(c * n + k)];
        }
    }
    m = std::move(inv);
    return true;
}

}  // namespace

int ambisonicOrderForChannels(int channels) {
    for (int order = kMaxAmbisonicOrder; order >= 1; --order)
        if (channels >= ambisonicChannels(order)) return order;
    return 0;
}

std::array<float, 4> foaEncode(double angle, double elevation, double focus) {
    std::array<float, 4> g;
    ambisonicEncode(1, angle, elevation, g.data(), focus);
    return g;
}

void ambisonicEncode(int order, double angle, double elevation, float* out, double focus) {
    order = std::clamp(order, 0, kMaxAmbisonicOrder);
    double x, y, z, h[16];
    direction(angle, elevation, x, y, z);
    harmonics(order, x, y, z, h);
    const double f = std::clamp(focus, 0.0, 1.0);
    for (int n = 0; n < ambisonicChannels(order); ++n) out[n] = float(h[n] * std::pow(f, degreeOf(n)));
}

FoaRotation foaRotation(double yaw, double pitch, double roll) {
    FoaRotation r;
    r.identity = std::fabs(yaw) < 1e-9 && std::fabs(pitch) < 1e-9 && std::fabs(roll) < 1e-9;
    const double cy = std::cos(yaw * kDeg), sy = std::sin(yaw * kDeg), cp = std::cos(pitch * kDeg), sp = std::sin(pitch * kDeg),
                 cr = std::cos(roll * kDeg), sr = std::sin(roll * kDeg);
    // Undo the view's turn: its yaw (about Z), then its pitch (about Y), then its roll (about X).
    const double Y[3][3] = {{cy, -sy, 0}, {sy, cy, 0}, {0, 0, 1}};
    const double P[3][3] = {{cp, 0, sp}, {0, 1, 0}, {-sp, 0, cp}};
    const double R[3][3] = {{1, 0, 0}, {0, cr, sr}, {0, -sr, cr}};
    double PY[3][3] = {}, M[3][3] = {};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) PY[i][j] += P[i][k] * Y[k][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) M[i][j] += R[i][k] * PY[k][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = float(M[i][j]);
    return r;
}

FoaRotation foaCompose(const FoaRotation& first, const FoaRotation& then) {
    if (first.identity) return then;
    if (then.identity) return first;
    FoaRotation r;
    r.identity = false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            double v = 0;
            for (int k = 0; k < 3; ++k) v += double(then.m[i][k]) * double(first.m[k][j]);
            r.m[i][j] = float(v);
        }
    return r;
}

void foaRotate(const FoaRotation& r, float* wyzx) {
    if (r.identity) return;
    const float x = wyzx[3], y = wyzx[1], z = wyzx[2];
    wyzx[3] = r.m[0][0] * x + r.m[0][1] * y + r.m[0][2] * z;
    wyzx[1] = r.m[1][0] * x + r.m[1][1] * y + r.m[1][2] * z;
    wyzx[2] = r.m[2][0] * x + r.m[2][1] * y + r.m[2][2] * z;
}

AmbisonicRotation ambisonicRotation(int order, const FoaRotation& turn) {
    AmbisonicRotation r;
    r.order = std::clamp(order, 1, kMaxAmbisonicOrder);
    r.identity = turn.identity;
    if (r.identity) return r;
    // Each order's harmonics of a turned direction are a fixed mix of the same order's harmonics of the direction:
    // that mix is fitted (least squares, exact for exact data) over a grid of directions. The fit's pseudo-inverse
    // depends only on the grid, so it is made once.
    static const std::vector<std::array<double, 3>> grid = evenGrid(64);
    static const auto pinv = [] {
        std::array<std::vector<double>, kMaxAmbisonicOrder + 1> out;  // per order: grid × (2l + 1)
        const int k = int(grid.size());
        for (int l = 1; l <= kMaxAmbisonicOrder; ++l) {
            const int w = 2 * l + 1, first = l * l;
            std::vector<double> a(size_t(w) * size_t(k));  // (2l + 1) × grid
            for (int j = 0; j < k; ++j) {
                double h[16];
                harmonics(l, grid[size_t(j)][0], grid[size_t(j)][1], grid[size_t(j)][2], h);
                for (int i = 0; i < w; ++i) a[size_t(i * k + j)] = h[first + i];
            }
            std::vector<double> aat(size_t(w) * size_t(w), 0.0);
            for (int i = 0; i < w; ++i)
                for (int c = 0; c < w; ++c)
                    for (int j = 0; j < k; ++j) aat[size_t(i * w + c)] += a[size_t(i * k + j)] * a[size_t(c * k + j)];
            invert(aat, w);
            out[size_t(l)].assign(size_t(k) * size_t(w), 0.0);  // Aᵀ (A Aᵀ)⁻¹
            for (int j = 0; j < k; ++j)
                for (int c = 0; c < w; ++c)
                    for (int i = 0; i < w; ++i) out[size_t(l)][size_t(j * w + c)] += a[size_t(i * k + j)] * aat[size_t(i * w + c)];
        }
        return out;
    }();
    const int k = int(grid.size());
    std::vector<double> turned(size_t(k) * 16);
    for (int j = 0; j < k; ++j) {
        const auto& d = grid[size_t(j)];
        double v[3];
        for (int i = 0; i < 3; ++i) v[i] = turn.m[i][0] * d[0] + turn.m[i][1] * d[1] + turn.m[i][2] * d[2];
        harmonics(r.order, v[0], v[1], v[2], &turned[size_t(j) * 16]);
    }
    for (int l = 1; l <= r.order; ++l) {
        const int w = 2 * l + 1, first = l * l;
        auto& m = r.band[l];
        m.assign(size_t(w) * size_t(w), 0.0f);
        for (int i = 0; i < w; ++i)
            for (int c = 0; c < w; ++c) {
                double s = 0;
                for (int j = 0; j < k; ++j) s += turned[size_t(j) * 16 + size_t(first + i)] * pinv[size_t(l)][size_t(j * w + c)];
                m[size_t(i * w + c)] = float(s);
            }
    }
    return r;
}

void ambisonicRotate(const AmbisonicRotation& r, float* field) {
    if (r.identity) return;
    float tmp[7];
    for (int l = 1; l <= r.order; ++l) {
        const int w = 2 * l + 1;
        float* f = field + l * l;
        const float* m = r.band[l].data();
        for (int i = 0; i < w; ++i) {
            float s = 0;
            for (int c = 0; c < w; ++c) s += m[i * w + c] * f[c];
            tmp[i] = s;
        }
        std::copy(tmp, tmp + w, f);
    }
}

void foaDecodeStereo(const float* wyzx, float& left, float& right) {
    // Cardioids (half omni, half figure of eight) at 60° left and right.
    constexpr float kx = 0.5f * 0.5f, ky = 0.5f * 0.8660254f;
    left = 0.5f * wyzx[0] + kx * wyzx[3] + ky * wyzx[1];
    right = 0.5f * wyzx[0] + kx * wyzx[3] - ky * wyzx[1];
}

std::vector<float> allradDecoder(int order, const std::vector<std::array<double, 2>>& directions, const std::vector<bool>& lfe) {
    order = std::clamp(order, 1, kMaxAmbisonicOrder);
    const int nch = ambisonicChannels(order), ns = int(directions.size());
    std::vector<float> out(size_t(ns) * size_t(nch), 0.0f);
    // The real speakers, then imaginary ones overhead and underneath, so the hull closes round the listener: what is
    // panned to the one overhead goes to its neighbours, what goes underneath is dropped.
    std::vector<std::array<double, 3>> pts;
    std::vector<int> speaker;
    for (int s = 0; s < ns; ++s) {
        if (s < int(lfe.size()) && lfe[size_t(s)]) continue;
        double x, y, z;
        direction(directions[size_t(s)][0], directions[size_t(s)][1], x, y, z);
        pts.push_back({x, y, z});
        speaker.push_back(s);
    }
    if (pts.size() < 3) return out;
    const int top = int(pts.size()), bottom = top + 1;
    pts.push_back({0, 0, 1});
    pts.push_back({0, 0, -1});
    // The convex hull's faces: triangles with every other point on one side (four or more on one circle give
    // overlapping triangles, any of which pans the same).
    struct Face {
        int v[3];
        double inv[9];
    };
    std::vector<Face> faces;
    const int np = int(pts.size());
    for (int a = 0; a < np; ++a)
        for (int b = a + 1; b < np; ++b)
            for (int c = b + 1; c < np; ++c) {
                const auto &A = pts[size_t(a)], &B = pts[size_t(b)], &C = pts[size_t(c)];
                const double u[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]}, v[3] = {C[0] - A[0], C[1] - A[1], C[2] - A[2]};
                const double n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
                if (std::hypot(n[0], n[1], n[2]) < 1e-9) continue;
                bool above = false, below = false;
                for (int q = 0; q < np && !(above && below); ++q) {
                    if (q == a || q == b || q == c) continue;
                    const double s = n[0] * (pts[size_t(q)][0] - A[0]) + n[1] * (pts[size_t(q)][1] - A[1]) + n[2] * (pts[size_t(q)][2] - A[2]);
                    if (s > 1e-9) above = true;
                    if (s < -1e-9) below = true;
                }
                if (above && below) continue;
                std::vector<double> m = {A[0], B[0], C[0], A[1], B[1], C[1], A[2], B[2], C[2]};
                if (!invert(m, 3)) continue;  // (a face through the middle: no direction is panned with it)
                Face f{{a, b, c}, {}};
                std::copy(m.begin(), m.end(), f.inv);
                faces.push_back(f);
            }
    // The overhead imaginary speaker's neighbours.
    std::vector<int> nearTop;
    for (const Face& f : faces)
        if (f.v[0] == top || f.v[1] == top || f.v[2] == top)
            for (int v : f.v)
                if (v != top && v != bottom && std::find(nearTop.begin(), nearTop.end(), v) == nearTop.end()) nearTop.push_back(v);
    // The virtual speakers: an even grid, each panned by VBAP (power 1, then what is underneath dropped).
    static const std::vector<std::array<double, 3>> grid = evenGrid(2000);
    std::vector<double> dec(size_t(ns) * size_t(nch), 0.0);
    const int nreal = top;
    std::vector<double> g(static_cast<size_t>(np));
    for (const auto& d : grid) {
        std::fill(g.begin(), g.end(), 0.0);
        for (const Face& f : faces) {
            double w[3];
            for (int i = 0; i < 3; ++i) w[i] = f.inv[i * 3] * d[0] + f.inv[i * 3 + 1] * d[1] + f.inv[i * 3 + 2] * d[2];
            if (w[0] < -1e-9 || w[1] < -1e-9 || w[2] < -1e-9) continue;
            for (int i = 0; i < 3; ++i) g[size_t(f.v[i])] = std::max(0.0, w[i]);
            break;
        }
        if (g[size_t(top)] > 0 && !nearTop.empty()) {
            for (int v : nearTop) g[size_t(v)] += g[size_t(top)] / std::sqrt(double(nearTop.size()));
            g[size_t(top)] = 0;
        }
        double power = 0;
        for (double v : g) power += v * v;
        if (power <= 0) continue;
        const double norm = 1 / std::sqrt(power);
        double h[16];
        harmonics(order, d[0], d[1], d[2], h);
        for (int i = 0; i < nreal; ++i) {
            const double gi = g[size_t(i)] * norm;
            if (gi == 0) continue;
            for (int n = 0; n < nch; ++n) {
                const int l = degreeOf(n);
                dec[size_t(speaker[size_t(i)] * nch + n)] += gi * (2 * l + 1) * maxRe(order, l) * h[n];
            }
        }
    }
    // Each speaker as loud in a diffuse field as the others: where speakers are sparse (overhead, say) one takes in a
    // wide patch of virtual speakers, which at low orders would outweigh a speaker nearer the sound. Then a sound's
    // power kept, on average over all directions.
    std::vector<double> diffuse(size_t(ns), 0.0);
    for (const auto& d : grid) {
        double h[16];
        harmonics(order, d[0], d[1], d[2], h);
        for (int s = 0; s < ns; ++s) {
            double v = 0;
            for (int n = 0; n < nch; ++n) v += dec[size_t(s * nch + n)] * h[n];
            diffuse[size_t(s)] += v * v;
        }
    }
    double mean = 0, energy = 0;
    for (int s : speaker) mean += diffuse[size_t(s)] / double(speaker.size());
    for (int s : speaker) {
        if (diffuse[size_t(s)] <= 0) continue;
        const double g = std::sqrt(mean / diffuse[size_t(s)]);
        for (int n = 0; n < nch; ++n) dec[size_t(s * nch + n)] *= g;
        energy += mean;
    }
    const double scale = energy > 0 ? 1 / std::sqrt(energy / double(grid.size())) : 0;
    for (size_t i = 0; i < dec.size(); ++i) out[i] = float(dec[i] * scale);
    return out;
}

const std::vector<std::array<double, 2>>& binauralDirections() {
    // Lebedev's 50-point grid (integrates the products of the harmonics to third order exactly), as angle, elevation.
    static const std::vector<std::array<double, 2>> dirs = [] {
        std::vector<std::array<double, 3>> v;
        for (int a = 0; a < 3; ++a)
            for (double s : {1.0, -1.0}) {
                std::array<double, 3> p{0, 0, 0};
                p[size_t(a)] = s;
                v.push_back(p);
            }
        const double h = std::sqrt(0.5);
        for (int a = 0; a < 3; ++a)
            for (double s1 : {h, -h})
                for (double s2 : {h, -h}) {
                    std::array<double, 3> p{0, 0, 0};
                    p[size_t((a + 1) % 3)] = s1, p[size_t((a + 2) % 3)] = s2;
                    v.push_back(p);
                }
        const double t = std::sqrt(1.0 / 3);
        for (double x : {t, -t})
            for (double y : {t, -t})
                for (double z : {t, -t}) v.push_back({x, y, z});
        const double l = 0.3015113445777636, m = 0.9045340337332909;
        for (int a = 0; a < 3; ++a)
            for (double s1 : {l, -l})
                for (double s2 : {l, -l})
                    for (double s3 : {m, -m}) {
                        std::array<double, 3> p;
                        p[size_t((a + 1) % 3)] = s1, p[size_t((a + 2) % 3)] = s2, p[size_t(a)] = s3;
                        v.push_back(p);
                    }
        std::vector<std::array<double, 2>> out;
        for (const auto& p : v) out.push_back({-std::atan2(p[1], p[0]) / kDeg, std::asin(std::clamp(p[2], -1.0, 1.0)) / kDeg});
        return out;
    }();
    return dirs;
}

namespace {

// Lebedev's weights for binauralDirections() (summing to 1), in the same order.
double binauralWeight(size_t i) {
    if (i < 6) return 0.0126984126984127;
    if (i < 18) return 0.02257495590828924;
    if (i < 26) return 0.02109375;
    return 0.02017333553791887;
}

}  // namespace

std::shared_ptr<const Hrirs> sphericalHeadHrirs(double sampleRate) {
    static std::mutex m;
    static std::vector<std::shared_ptr<const Hrirs>> made;
    std::lock_guard lock(m);
    const double sr = sampleRate > 0 ? sampleRate : 48000;
    for (const auto& h : made)
        if (h->sampleRate == sr) return h;
    auto out = std::make_shared<Hrirs>();
    out->name = "Spherical head model";
    out->sampleRate = sr;
    const double maxDelay = kHeadRadius / kSoundSpeed * (2 + M_PI / 2) * sr;
    out->length = int(std::ceil(maxDelay)) + int(std::ceil(64 * sr / 48000)) + 2;
    const double tau = kHeadRadius / (2 * kSoundSpeed), K = 2 * sr;
    for (const auto& d : binauralDirections()) {
        out->directions.push_back(d);
        double x, y, z;
        direction(d[0], d[1], x, y, z);
        for (int e = 0; e < 2; ++e) {
            // The angle between the ear's axis (left ear +Y, right ear -Y) and the sound; the delay round the head
            // (linear between two samples) and the shadow's shelf, rendered as taps.
            const double theta = std::acos(std::clamp(e == 0 ? y : -y, -1.0, 1.0));
            const double t = theta < M_PI / 2 ? -std::cos(theta) : theta - M_PI / 2;
            const double delay = (1 + t) * kHeadRadius / kSoundSpeed * sr;
            const double alpha = (1 + 0.05) + (1 - 0.05) * std::cos(theta / (150 * kDeg) * M_PI);
            const double norm = 1 + tau * K;
            const double b0 = (1 + alpha * tau * K) / norm, b1 = (1 - alpha * tau * K) / norm, a1 = (1 - tau * K) / norm;
            std::vector<float>& ir = e == 0 ? out->left : out->right;
            const size_t at = ir.size();
            ir.resize(at + size_t(out->length), 0.0f);
            const int whole = int(delay);
            const double frac = delay - whole;
            double x1 = 0, y1 = 0;
            for (int i = 0; i < out->length; ++i) {
                const double xin = (i == whole ? 1 - frac : 0) + (i == whole + 1 ? frac : 0);
                const double yout = b0 * xin + b1 * x1 - a1 * y1;
                x1 = xin, y1 = yout;
                ir[at + size_t(i)] = float(yout);
            }
        }
    }
    made.push_back(out);
    return out;
}

AmbisonicBinaural::AmbisonicBinaural(int order, double sampleRate, std::shared_ptr<const Hrirs> hrirs)
    : order_(std::clamp(order, 1, kMaxAmbisonicOrder)), channels_(ambisonicChannels(order_)), sr_(sampleRate > 0 ? sampleRate : 48000) {
    const auto& dirs = binauralDirections();
    if (!hrirs || hrirs->directions.size() != dirs.size() || hrirs->length < 1 || std::fabs(hrirs->sampleRate - sr_) > 0.5)
        hrirs = sphericalHeadHrirs(sr_);
    hrirs_ = std::move(hrirs);
    length_ = hrirs_->length;
    // Each channel's pair of filters: the virtual speakers' responses, each as much as the channel feeds that speaker
    // (a sampling decode, max-rE weighted: order l by (2l + 1) times its weight, at Lebedev's weights).
    filters_.assign(size_t(channels_) * 2 * size_t(length_), 0.0f);
    for (size_t k = 0; k < dirs.size(); ++k) {
        double x, y, z, h[16];
        direction(dirs[k][0], dirs[k][1], x, y, z);
        harmonics(order_, x, y, z, h);
        const float* irs[2] = {&hrirs_->left[k * size_t(length_)], &hrirs_->right[k * size_t(length_)]};
        for (int n = 0; n < channels_; ++n) {
            const int l = degreeOf(n);
            const float g = float(binauralWeight(k) * (2 * l + 1) * maxRe(order_, l) * h[n]);
            for (int e = 0; e < 2; ++e) {
                float* f = &filters_[(size_t(n) * 2 + size_t(e)) * size_t(length_)];
                for (int i = 0; i < length_; ++i) f[length_ - 1 - i] += g * irs[e][i];
            }
        }
    }
    // Every head and order as loud on average (a diffuse field's power kept, half at each ear), so choosing another
    // head or order does not jump in level: the low-order sum of a measured head's responses loses less to their
    // different delays than a model's does.
    double energy = 0;
    std::vector<double> ear(static_cast<size_t>(length_));
    for (size_t k = 0; k < dirs.size(); ++k) {
        double x, y, z, h[16];
        direction(dirs[k][0], dirs[k][1], x, y, z);
        harmonics(order_, x, y, z, h);
        for (int e = 0; e < 2; ++e) {
            std::fill(ear.begin(), ear.end(), 0.0);
            for (int n = 0; n < channels_; ++n) {
                const float* f = &filters_[(size_t(n) * 2 + size_t(e)) * size_t(length_)];
                for (int i = 0; i < length_; ++i) ear[size_t(i)] += h[n] * f[i];
            }
            double sum = 0;
            for (double v : ear) sum += v * v;
            energy += binauralWeight(k) * sum / 2;
        }
    }
    if (energy > 0) {
        const float g = float(std::sqrt(0.5 / energy));
        for (float& f : filters_) f *= g;
    }
    history_.assign(size_t(channels_) * 2 * size_t(length_), 0.0f);
}

void AmbisonicBinaural::reset() {
    std::fill(history_.begin(), history_.end(), 0.0f);
    pos_ = 0;
}

void AmbisonicBinaural::process(const float* in, int frames, float* out) {
    const size_t len = size_t(length_);
    for (int i = 0; i < frames; ++i) {
        // The newest sample goes at pos and pos + length; the window ending at it is then history[pos + 1 ...].
        pos_ = (pos_ + 1) % length_;
        float ears[2] = {0, 0};
        for (int n = 0; n < channels_; ++n) {
            float* h = &history_[size_t(n) * 2 * len];
            const float v = in[size_t(i) * size_t(channels_) + size_t(n)];
            h[size_t(pos_)] = v;
            h[size_t(pos_) + len] = v;
            const float* window = h + size_t(pos_) + 1;  // oldest first
            for (int e = 0; e < 2; ++e) {
                const float* f = &filters_[(size_t(n) * 2 + size_t(e)) * len];
                float s = 0;
                for (size_t k = 0; k < len; ++k) s += window[k] * f[k];
                ears[e] += s;
            }
        }
        out[size_t(i) * 2] = ears[0];
        out[size_t(i) * 2 + 1] = ears[1];
    }
}

}  // namespace montage
