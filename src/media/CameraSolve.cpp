#include "CameraSolve.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <random>
#include <sstream>

#include "Decoder.h"

namespace montage {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kHuber = 2.0;         // pixels: errors beyond count linearly
constexpr double kMinAngle = 0.75;     // degrees between the rays a new point is triangulated from
constexpr int kMaxKeyframes = 120;

using M3 = std::array<double, 9>;

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(double k, Vec3 a) { return {k * a.x, k * a.y, k * a.z}; }
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double norm(Vec3 a) { return std::sqrt(dot(a, a)); }
Vec3 unit(Vec3 a) {
    const double n = norm(a);
    return n > 0 ? (1 / n) * a : a;
}
Vec3 mul(const M3& m, Vec3 v) {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z};
}
Vec3 mulT(const M3& m, Vec3 v) {
    return {m[0] * v.x + m[3] * v.y + m[6] * v.z, m[1] * v.x + m[4] * v.y + m[7] * v.z, m[2] * v.x + m[5] * v.y + m[8] * v.z};
}
M3 mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    return r;
}
M3 transpose(const M3& a) { return {a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8]}; }
M3 fromRows(Vec3 a, Vec3 b, Vec3 c) { return {a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z}; }
double det(const M3& m) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
}

// The rotation by |w| radians about w (Rodrigues).
M3 rotation(Vec3 w) {
    const double t = norm(w);
    const M3 K{0, -w.z, w.y, w.z, 0, -w.x, -w.y, w.x, 0};
    const M3 K2 = mul(K, K);
    const double a = t < 1e-12 ? 1 : std::sin(t) / t, b = t < 1e-6 ? 0.5 : (1 - std::cos(t)) / (t * t);
    M3 R{1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; ++i) R[i] += a * K[i] + b * K2[i];
    return R;
}

// Eigenvalues (ascending) and eigenvectors (column k of `vecs`, row-major n x n) of the symmetric n x n matrix `a`
// (cyclic Jacobi).
void symmetricEigen(std::vector<double> a, int n, std::vector<double>& values, std::vector<double>& vecs) {
    std::vector<double> v(size_t(n) * n, 0);
    for (int i = 0; i < n; ++i) v[size_t(i) * n + i] = 1;
    double total = 0;
    for (double x : a) total += x * x;
    for (int sweep = 0; sweep < 60; ++sweep) {
        double off = 0;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q) off += a[size_t(p) * n + q] * a[size_t(p) * n + q];
        if (off <= 1e-26 * total) break;
        for (int p = 0; p < n; ++p)
            for (int q = p + 1; q < n; ++q) {
                const double apq = a[size_t(p) * n + q];
                if (std::fabs(apq) < 1e-300) continue;
                const double theta = (a[size_t(q) * n + q] - a[size_t(p) * n + p]) / (2 * apq);
                const double t = (theta >= 0 ? 1 : -1) / (std::fabs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < n; ++k) {
                    double& kp = a[size_t(k) * n + p];
                    double& kq = a[size_t(k) * n + q];
                    const double x = kp, y = kq;
                    kp = c * x - s * y;
                    kq = s * x + c * y;
                }
                for (int k = 0; k < n; ++k) {
                    double& pk = a[size_t(p) * n + k];
                    double& qk = a[size_t(q) * n + k];
                    const double x = pk, y = qk;
                    pk = c * x - s * y;
                    qk = s * x + c * y;
                }
                for (int k = 0; k < n; ++k) {
                    double& kp = v[size_t(k) * n + p];
                    double& kq = v[size_t(k) * n + q];
                    const double x = kp, y = kq;
                    kp = c * x - s * y;
                    kq = s * x + c * y;
                }
            }
    }
    std::vector<int> order(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](int i, int j) { return a[size_t(i) * n + i] < a[size_t(j) * n + j]; });
    values.resize(n);
    vecs.assign(size_t(n) * n, 0);
    for (int k = 0; k < n; ++k) {
        values[k] = a[size_t(order[k]) * n + order[k]];
        for (int i = 0; i < n; ++i) vecs[size_t(i) * n + k] = v[size_t(i) * n + order[k]];
    }
}

// Solves a x = b for symmetric positive definite a (Cholesky, in place: `b` gets x). False when a is not.
bool choleskySolve(std::vector<double>& a, int n, std::vector<double>& b) {
    for (int j = 0; j < n; ++j) {
        double d = a[size_t(j) * n + j];
        const double* rj = &a[size_t(j) * n];
        for (int k = 0; k < j; ++k) d -= rj[k] * rj[k];
        if (!(d > 0)) return false;
        d = std::sqrt(d);
        a[size_t(j) * n + j] = d;
        for (int i = j + 1; i < n; ++i) {
            double* ri = &a[size_t(i) * n];
            double s = ri[j];
            for (int k = 0; k < j; ++k) s -= ri[k] * rj[k];
            ri[j] = s / d;
        }
    }
    for (int i = 0; i < n; ++i) {
        double s = b[i];
        for (int k = 0; k < i; ++k) s -= a[size_t(i) * n + k] * b[k];
        b[i] = s / a[size_t(i) * n + i];
    }
    for (int i = n - 1; i >= 0; --i) {
        double s = b[i];
        for (int k = i + 1; k < n; ++k) s -= a[size_t(k) * n + i] * b[k];
        b[i] = s / a[size_t(i) * n + i];
    }
    return true;
}

bool invert3(const M3& m, M3& out) {
    const double d = det(m);
    if (!(std::fabs(d) > 1e-300)) return false;
    const double k = 1 / d;
    out = {k * (m[4] * m[8] - m[5] * m[7]), k * (m[2] * m[7] - m[1] * m[8]), k * (m[1] * m[5] - m[2] * m[4]),
           k * (m[5] * m[6] - m[3] * m[8]), k * (m[0] * m[8] - m[2] * m[6]), k * (m[2] * m[3] - m[0] * m[5]),
           k * (m[3] * m[7] - m[4] * m[6]), k * (m[1] * m[6] - m[0] * m[7]), k * (m[0] * m[4] - m[1] * m[3])};
    return true;
}

double huberWeight(double e) { return e <= kHuber ? 1.0 : kHuber / e; }
double huberCost(double e) { return e <= kHuber ? e * e : 2 * kHuber * e - kHuber * kHuber; }
constexpr double kBehindCost = 2 * kHuber * 100 - kHuber * kHuber;  // a point behind the camera: as if 100 px out

struct Cam {
    M3 R{1, 0, 0, 0, 1, 0, 0, 0, 1};  // world to camera
    Vec3 C;
    bool ok = false;
};

struct Intrinsics {
    double f, cx, cy;
};

// The reprojection residual of X seen at `obs` and its derivatives: Jc by the camera's small rotation (its update is
// R <- rotation(w) R) and position, Jx by the point, Jf by the focal length.
struct Residual {
    bool ok = false;
    double r[2]{};
    double Jc[2][6]{};
    double Jx[2][3]{};
    double Jf[2]{};
    double error() const { return std::hypot(r[0], r[1]); }
};
Residual residual(const Cam& c, Vec3 X, const Intrinsics& k, Point2 obs, bool derivatives) {
    Residual o;
    const Vec3 p = mul(c.R, X - c.C);
    if (!(p.z > 1e-9)) return o;
    o.ok = true;
    const double iz = 1 / p.z, x = p.x * iz, y = p.y * iz;
    o.r[0] = k.f * x + k.cx - obs.x;
    o.r[1] = k.f * y + k.cy - obs.y;
    if (!derivatives) return o;
    const double Jp[2][3] = {{k.f * iz, 0, -k.f * x * iz}, {0, k.f * iz, -k.f * y * iz}};
    // d p / d w = -[p]x; d p / d C = -R; d p / d X = R.
    const double P[3][3] = {{0, p.z, -p.y}, {-p.z, 0, p.x}, {p.y, -p.x, 0}};
    for (int r = 0; r < 2; ++r) {
        for (int j = 0; j < 3; ++j) {
            double w = 0, X3 = 0;
            for (int i = 0; i < 3; ++i) {
                w += Jp[r][i] * P[i][j];
                X3 += Jp[r][i] * c.R[i * 3 + j];
            }
            o.Jc[r][j] = w;
            o.Jc[r][3 + j] = -X3;
            o.Jx[r][j] = X3;
        }
    }
    o.Jf[0] = x;
    o.Jf[1] = y;
    return o;
}

void moveCamera(Cam& c, const double* d) {
    c.R = mul(rotation({d[0], d[1], d[2]}), c.R);
    c.C = c.C + Vec3{d[3], d[4], d[5]};
}

// Fits camera `c` to points `X` seen at `obs` (Levenberg–Marquardt, Huber). Returns how many are within three Huber
// widths; `rms` gets their RMS error.
int refinePose(Cam& c, const std::vector<Vec3>& X, const std::vector<Point2>& obs, const Intrinsics& k, int iterations,
               double* rms = nullptr) {
    auto cost = [&](const Cam& cam) {
        double s = 0;
        for (size_t i = 0; i < X.size(); ++i) {
            const Residual r = residual(cam, X[i], k, obs[i], false);
            s += r.ok ? huberCost(r.error()) : kBehindCost;
        }
        return s;
    };
    double current = cost(c), lambda = 1e-3;
    for (int it = 0; it < iterations; ++it) {
        std::vector<double> H(36, 0), g(6, 0);
        for (size_t i = 0; i < X.size(); ++i) {
            const Residual r = residual(c, X[i], k, obs[i], true);
            if (!r.ok) continue;
            const double w = huberWeight(r.error());
            for (int a = 0; a < 6; ++a) {
                for (int b = 0; b < 6; ++b) H[a * 6 + b] += w * (r.Jc[0][a] * r.Jc[0][b] + r.Jc[1][a] * r.Jc[1][b]);
                g[a] += w * (r.Jc[0][a] * r.r[0] + r.Jc[1][a] * r.r[1]);
            }
        }
        bool moved = false;
        for (int attempt = 0; attempt < 10 && !moved; ++attempt) {
            std::vector<double> A = H, d(6);
            for (int a = 0; a < 6; ++a) {
                A[a * 6 + a] += lambda * H[a * 6 + a] + 1e-12;
                d[a] = -g[a];
            }
            if (choleskySolve(A, 6, d)) {
                Cam n = c;
                moveCamera(n, d.data());
                const double nc = cost(n);
                if (nc < current) {
                    const double gain = (current - nc) / std::max(current, 1e-30);
                    c = n;
                    current = nc;
                    lambda = std::max(lambda / 3, 1e-9);
                    moved = gain > 1e-10;
                    if (!moved) it = iterations;
                    break;
                }
            }
            lambda *= 10;
        }
        if (!moved) break;
    }
    int inliers = 0;
    double sum = 0;
    for (size_t i = 0; i < X.size(); ++i) {
        const Residual r = residual(c, X[i], k, obs[i], false);
        if (r.ok && r.error() < 3 * kHuber) {
            ++inliers;
            sum += r.error() * r.error();
        }
    }
    if (rms) *rms = inliers ? std::sqrt(sum / inliers) : 0;
    return inliers;
}

// The point seen along normalised rays (x, y, 1) of the cameras (linear, least squares). False when it is at infinity.
bool triangulate(const std::vector<const Cam*>& cams, const std::vector<Point2>& rays, Vec3& X) {
    std::vector<double> A(16, 0);
    for (size_t v = 0; v < cams.size(); ++v) {
        const Cam& c = *cams[v];
        const Vec3 t = -1.0 * mul(c.R, c.C);
        const double P[3][4] = {{c.R[0], c.R[1], c.R[2], t.x}, {c.R[3], c.R[4], c.R[5], t.y}, {c.R[6], c.R[7], c.R[8], t.z}};
        double rows[2][4];
        for (int j = 0; j < 4; ++j) {
            rows[0][j] = rays[v].x * P[2][j] - P[0][j];
            rows[1][j] = rays[v].y * P[2][j] - P[1][j];
        }
        for (auto& r : rows) {
            double n = 0;
            for (double x : r) n += x * x;
            n = n > 0 ? 1 / std::sqrt(n) : 0;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) A[i * 4 + j] += r[i] * r[j] * n * n;
        }
    }
    std::vector<double> values, vecs;
    symmetricEigen(A, 4, values, vecs);
    const double w = vecs[12];
    if (std::fabs(w) < 1e-12) return false;
    X = {vecs[0] / w, vecs[4] / w, vecs[8] / w};
    return std::isfinite(X.x) && std::isfinite(X.y) && std::isfinite(X.z);
}

double rayAngle(const Cam& a, Point2 ra, const Cam& b, Point2 rb) {
    const Vec3 da = unit(mulT(a.R, {ra.x, ra.y, 1})), db = unit(mulT(b.R, {rb.x, rb.y, 1}));
    return std::acos(std::clamp(dot(da, db), -1.0, 1.0)) * 180 / kPi;
}

// ---- Two views ----------------------------------------------------------------------------------------------------

// The essential matrix x2' E x1 = 0 of normalised pairs (the eight-point algorithm, least squares over `idx`, then
// made essential: two equal singular values and a zero).
bool essential(const std::vector<Point2>& a, const std::vector<Point2>& b, const std::vector<int>& idx, M3& E) {
    std::vector<double> A(81, 0);
    for (int i : idx) {
        const double r[9] = {b[i].x * a[i].x, b[i].x * a[i].y, b[i].x, b[i].y * a[i].x, b[i].y * a[i].y, b[i].y, a[i].x, a[i].y, 1};
        for (int p = 0; p < 9; ++p)
            for (int q = 0; q < 9; ++q) A[p * 9 + q] += r[p] * r[q];
    }
    std::vector<double> values, vecs;
    symmetricEigen(A, 9, values, vecs);
    M3 F;
    for (int i = 0; i < 9; ++i) F[i] = vecs[size_t(i) * 9];
    // Its singular vectors: V from F'F, U from F V.
    const M3 FtF = mul(transpose(F), F);
    std::vector<double> sv, V;
    symmetricEigen(std::vector<double>(FtF.begin(), FtF.end()), 3, sv, V);
    const Vec3 v0{V[2], V[5], V[8]}, v1{V[1], V[4], V[7]};
    const Vec3 u0 = unit(mul(F, v0));
    Vec3 u1 = mul(F, v1);
    u1 = unit(u1 - dot(u1, u0) * u0);
    if (norm(u0) < 0.5 || norm(u1) < 0.5) return false;
    const Vec3 u2 = cross(u0, u1), v2 = cross(v0, v1);
    const M3 U = transpose(fromRows(u0, u1, u2)), Vt = fromRows(v0, v1, v2);
    const M3 S{1, 0, 0, 0, 1, 0, 0, 0, 0};
    E = mul(mul(U, S), Vt);
    return true;
}

double sampson(const M3& E, Point2 a, Point2 b) {
    const Vec3 x1{a.x, a.y, 1}, x2{b.x, b.y, 1};
    const Vec3 l2 = mul(E, x1), l1 = mulT(E, x2);
    const double e = dot(x2, l2);
    const double d = l2.x * l2.x + l2.y * l2.y + l1.x * l1.x + l1.y * l1.y;
    return d > 0 ? e * e / d : 1e30;
}

struct TwoView {
    Cam second;            // the first is at the origin, unturned
    std::vector<int> pairs;  // indices of the pairs in front of both
    double angle = 0;      // median angle between their rays, degrees
};

bool twoView(const std::vector<Point2>& a, const std::vector<Point2>& b, double f, std::mt19937& rng, TwoView& out) {
    const int n = int(a.size());
    if (n < 16) return false;
    const double threshold = (1.5 / f) * (1.5 / f);
    std::uniform_int_distribution<int> pick(0, n - 1);
    std::vector<int> best;
    for (int it = 0; it < 256; ++it) {
        std::vector<int> s;
        while (s.size() < 8) {
            const int i = pick(rng);
            if (std::find(s.begin(), s.end(), i) == s.end()) s.push_back(i);
        }
        M3 E;
        if (!essential(a, b, s, E)) continue;
        std::vector<int> in;
        for (int i = 0; i < n; ++i)
            if (sampson(E, a[i], b[i]) < threshold) in.push_back(i);
        if (in.size() > best.size()) best.swap(in);
    }
    M3 E;
    if (best.size() < 12 || !essential(a, b, best, E)) return false;
    std::vector<int> in;
    for (int i = 0; i < n; ++i)
        if (sampson(E, a[i], b[i]) < threshold) in.push_back(i);
    if (in.size() < 12 || !essential(a, b, in, E)) return false;
    // E = U diag(1, 1, 0) V': the four (R, t) it allows, and the one with the points in front of both cameras.
    const M3 EtE = mul(transpose(E), E);
    std::vector<double> sv, V;
    symmetricEigen(std::vector<double>(EtE.begin(), EtE.end()), 3, sv, V);
    const Vec3 v0{V[2], V[5], V[8]}, v1{V[1], V[4], V[7]};
    const Vec3 u0 = unit(mul(E, v0));
    Vec3 u1 = mul(E, v1);
    u1 = unit(u1 - dot(u1, u0) * u0);
    const Vec3 u2 = cross(u0, u1), v2 = cross(v0, v1);
    const M3 U = transpose(fromRows(u0, u1, u2)), Vt = fromRows(v0, v1, v2);
    const M3 W{0, -1, 0, 1, 0, 0, 0, 0, 1};
    const M3 Rs[2] = {mul(mul(U, W), Vt), mul(mul(U, transpose(W)), Vt)};
    const Cam first{{1, 0, 0, 0, 1, 0, 0, 0, 1}, {}, true};
    size_t most = 0;
    for (const M3& R : Rs)
        for (double sign : {1.0, -1.0}) {
            Cam c;
            c.R = R;
            c.C = -1.0 * mulT(R, sign * u2);
            c.ok = true;
            std::vector<int> front;
            std::vector<double> angles;
            for (int i : in) {
                Vec3 X;
                if (!triangulate({&first, &c}, {a[i], b[i]}, X)) continue;
                if (X.z > 0 && mul(c.R, X - c.C).z > 0) {
                    front.push_back(i);
                    angles.push_back(rayAngle(first, a[i], c, b[i]));
                }
            }
            if (front.size() > most) {
                most = front.size();
                out.second = c;
                out.pairs = front;
                std::nth_element(angles.begin(), angles.begin() + angles.size() / 2, angles.end());
                out.angle = angles[angles.size() / 2];
            }
        }
    return most >= 12;
}

// ---- The solve ----------------------------------------------------------------------------------------------------

struct Problem {
    std::vector<const FeatureTrack*> tracks;
    std::vector<int> keys;               // keyframes' frames
    std::vector<std::vector<int>> seen;  // per keyframe: the tracks in it
    int frames = 0;
    double cx = 0, cy = 0;
    bool covers(int t, int frame) const { return frame >= tracks[t]->first && frame <= tracks[t]->last(); }
    Point2 at(int t, int frame) const { return tracks[t]->at[size_t(frame - tracks[t]->first)]; }
};

struct State {
    double f = 0;
    std::vector<Cam> cams;  // per keyframe
    std::vector<Vec3> X;    // per track
    std::vector<char> has;  // per track
    Intrinsics k(const Problem& p) const { return {f, p.cx, p.cy}; }
};

Point2 normalised(const Problem& p, double f, Point2 px) { return {(px.x - p.cx) / f, (px.y - p.cy) / f}; }

// Keyframes: the first frame, then each frame where the features have moved `step` pixels (median) since the last
// keyframe, or half of them are gone; and the last.
std::vector<int> chooseKeyframes(const Problem& p, double step) {
    std::vector<int> keys{0};
    int key = 0;
    std::vector<int> atKey;
    auto alive = [&](int frame) {
        std::vector<int> out;
        for (int t = 0; t < int(p.tracks.size()); ++t)
            if (p.covers(t, frame)) out.push_back(t);
        return out;
    };
    atKey = alive(0);
    for (int i = 1; i < p.frames;) {
        std::vector<double> moved;
        for (int t : atKey)
            if (p.covers(t, i)) {
                const Point2 a = p.at(t, key), b = p.at(t, i);
                moved.push_back(std::hypot(b.x - a.x, b.y - a.y));
            }
        const bool few = moved.size() < std::max<size_t>(20, atKey.size() / 2);
        bool far = false;
        if (!moved.empty()) {
            std::nth_element(moved.begin(), moved.begin() + moved.size() / 2, moved.end());
            far = moved[moved.size() / 2] >= step;
        }
        if (few || far) {
            const int k = few && i - 1 > key ? i - 1 : i;
            keys.push_back(k);
            key = k;
            atKey = alive(k);
            i = k + 1;
            continue;
        }
        ++i;
    }
    if (keys.back() != p.frames - 1) keys.push_back(p.frames - 1);
    return keys;
}

struct Observation {
    int cam;  // slot in the adjusted cameras
    int track;
    Point2 at;
};

// Bundle adjustment: the keyframe cameras `cams` (indices into st.cams), the points they see and (`focal`) the focal
// length, refined together (Levenberg–Marquardt with the Schur complement, Huber); camera `fixed` stays put. Returns
// the RMS error.
double bundleAdjust(const Problem& p, State& st, const std::vector<int>& cams, int fixed, bool focal, int iterations) {
    std::vector<Observation> obs;
    std::vector<int> start{0}, points;
    for (int t = 0; t < int(p.tracks.size()); ++t) {
        if (!st.has[t]) continue;
        const size_t before = obs.size();
        for (int s = 0; s < int(cams.size()); ++s)
            if (p.covers(t, p.keys[cams[s]])) obs.push_back({s, t, p.at(t, p.keys[cams[s]])});
        if (obs.size() > before) {
            points.push_back(t);
            start.push_back(int(obs.size()));
        }
    }
    std::vector<int> offset(cams.size(), -1);
    int nc = 0;
    for (size_t s = 0; s < cams.size(); ++s)
        if (cams[s] != fixed) {
            offset[s] = nc;
            nc += 6;
        }
    const int fOffset = focal ? nc++ : -1;
    const int np = int(points.size());

    auto cost = [&](const State& s) {
        double c = 0;
        for (const Observation& o : obs) {
            const Residual r = residual(s.cams[cams[o.cam]], s.X[o.track], s.k(p), o.at, false);
            c += r.ok ? huberCost(r.error()) : kBehindCost;
        }
        return c;
    };
    double current = cost(st), lambda = 1e-4;
    struct Row {
        int index;
        double w[3];
    };
    for (int it = 0; it < iterations; ++it) {
        std::vector<double> U(size_t(nc) * nc, 0), gc(nc, 0);
        std::vector<M3> V(np, M3{});
        std::vector<Vec3> gp(np);
        std::vector<Row> rows;
        std::vector<int> rowStart{0};
        for (int j = 0; j < np; ++j) {
            double fRow[3] = {0, 0, 0};
            for (int k = start[j]; k < start[j + 1]; ++k) {
                const Observation& o = obs[k];
                const Residual r = residual(st.cams[cams[o.cam]], st.X[o.track], st.k(p), o.at, true);
                if (!r.ok) continue;
                const double w = huberWeight(r.error());
                for (int a = 0; a < 3; ++a) {
                    for (int b = 0; b < 3; ++b) V[j][a * 3 + b] += w * (r.Jx[0][a] * r.Jx[0][b] + r.Jx[1][a] * r.Jx[1][b]);
                }
                gp[j].x += w * (r.Jx[0][0] * r.r[0] + r.Jx[1][0] * r.r[1]);
                gp[j].y += w * (r.Jx[0][1] * r.r[0] + r.Jx[1][1] * r.r[1]);
                gp[j].z += w * (r.Jx[0][2] * r.r[0] + r.Jx[1][2] * r.r[1]);
                const int o6 = offset[o.cam];
                if (o6 >= 0) {
                    for (int a = 0; a < 6; ++a) {
                        for (int b = 0; b < 6; ++b)
                            U[size_t(o6 + a) * nc + o6 + b] += w * (r.Jc[0][a] * r.Jc[0][b] + r.Jc[1][a] * r.Jc[1][b]);
                        gc[o6 + a] += w * (r.Jc[0][a] * r.r[0] + r.Jc[1][a] * r.r[1]);
                        Row row{o6 + a, {}};
                        for (int b = 0; b < 3; ++b) row.w[b] = w * (r.Jc[0][a] * r.Jx[0][b] + r.Jc[1][a] * r.Jx[1][b]);
                        rows.push_back(row);
                        if (fOffset >= 0) {
                            const double cf = w * (r.Jc[0][a] * r.Jf[0] + r.Jc[1][a] * r.Jf[1]);
                            U[size_t(o6 + a) * nc + fOffset] += cf;
                            U[size_t(fOffset) * nc + o6 + a] += cf;
                        }
                    }
                }
                if (fOffset >= 0) {
                    U[size_t(fOffset) * nc + fOffset] += w * (r.Jf[0] * r.Jf[0] + r.Jf[1] * r.Jf[1]);
                    gc[fOffset] += w * (r.Jf[0] * r.r[0] + r.Jf[1] * r.r[1]);
                    for (int b = 0; b < 3; ++b) fRow[b] += w * (r.Jf[0] * r.Jx[0][b] + r.Jf[1] * r.Jx[1][b]);
                }
            }
            if (fOffset >= 0) rows.push_back({fOffset, {fRow[0], fRow[1], fRow[2]}});
            rowStart.push_back(int(rows.size()));
        }
        bool moved = false;
        for (int attempt = 0; attempt < 10 && !moved; ++attempt) {
            std::vector<double> S = U, d(nc);
            for (int a = 0; a < nc; ++a) {
                S[size_t(a) * nc + a] += lambda * U[size_t(a) * nc + a] + 1e-9;
                d[a] = -gc[a];
            }
            std::vector<M3> Vinv(np);
            bool ok = true;
            for (int j = 0; j < np && ok; ++j) {
                M3 Vd = V[j];
                for (int a = 0; a < 3; ++a) Vd[a * 4] += lambda * V[j][a * 4] + 1e-9;
                ok = invert3(Vd, Vinv[j]);
                if (!ok) break;
                const M3& Vi = Vinv[j];
                for (int a = rowStart[j]; a < rowStart[j + 1]; ++a) {
                    const Row& ra = rows[a];
                    const Vec3 y = mul(Vi, Vec3{ra.w[0], ra.w[1], ra.w[2]});
                    d[ra.index] += dot(y, gp[j]);
                    for (int b = rowStart[j]; b < rowStart[j + 1]; ++b) {
                        const Row& rb = rows[b];
                        S[size_t(ra.index) * nc + rb.index] -= y.x * rb.w[0] + y.y * rb.w[1] + y.z * rb.w[2];
                    }
                }
            }
            if (ok && (nc == 0 || choleskySolve(S, nc, d))) {
                State n = st;
                for (size_t s = 0; s < cams.size(); ++s)
                    if (offset[s] >= 0) moveCamera(n.cams[cams[s]], &d[offset[s]]);
                if (fOffset >= 0) n.f += d[fOffset];
                for (int j = 0; j < np; ++j) {
                    Vec3 b = -1.0 * gp[j];
                    for (int a = rowStart[j]; a < rowStart[j + 1]; ++a) {
                        const Row& ra = rows[a];
                        b = b - d[ra.index] * Vec3{ra.w[0], ra.w[1], ra.w[2]};
                    }
                    n.X[points[j]] = n.X[points[j]] + mul(Vinv[j], b);
                }
                const double nc2 = n.f > 0 ? cost(n) : 1e300;
                if (nc2 < current) {
                    const double gain = (current - nc2) / std::max(current, 1e-30);
                    st = std::move(n);
                    current = nc2;
                    lambda = std::max(lambda / 3, 1e-9);
                    moved = true;
                    if (gain < 1e-7) it = iterations;
                    break;
                }
            }
            lambda *= 10;
        }
        if (!moved) break;
    }
    double sum = 0;
    int n = 0;
    for (const Observation& o : obs) {
        const Residual r = residual(st.cams[cams[o.cam]], st.X[o.track], st.k(p), o.at, false);
        if (!r.ok) continue;
        sum += r.error() * r.error();
        ++n;
    }
    return n ? std::sqrt(sum / n) : 0;
}

std::vector<int> registered(const State& st) {
    std::vector<int> out;
    for (int k = 0; k < int(st.cams.size()); ++k)
        if (st.cams[k].ok) out.push_back(k);
    return out;
}

// Drops the points whose RMS error over the registered keyframes is over `limit` pixels (or that are behind one).
void prune(const Problem& p, State& st, double limit) {
    for (int t = 0; t < int(p.tracks.size()); ++t) {
        if (!st.has[t]) continue;
        double sum = 0;
        int n = 0;
        bool behind = false;
        for (int k = 0; k < int(p.keys.size()); ++k) {
            if (!st.cams[k].ok || !p.covers(t, p.keys[k])) continue;
            const Residual r = residual(st.cams[k], st.X[t], st.k(p), p.at(t, p.keys[k]), false);
            if (!r.ok) {
                behind = true;
                break;
            }
            sum += r.error() * r.error();
            ++n;
        }
        if (behind || n < 2 || std::sqrt(sum / n) > limit) st.has[t] = 0;
    }
}

// Points for the tracks in keyframe k that have none yet, from the registered keyframes they are in.
void addPoints(const Problem& p, State& st, int k) {
    for (int t : p.seen[k]) {
        if (st.has[t]) continue;
        std::vector<const Cam*> views;
        std::vector<Point2> rays, pixels;
        double widest = 0;
        const Point2 here = normalised(p, st.f, p.at(t, p.keys[k]));
        for (int m = 0; m < int(p.keys.size()); ++m) {
            if (!st.cams[m].ok || !p.covers(t, p.keys[m])) continue;
            views.push_back(&st.cams[m]);
            pixels.push_back(p.at(t, p.keys[m]));
            rays.push_back(normalised(p, st.f, pixels.back()));
            if (m != k) widest = std::max(widest, rayAngle(st.cams[k], here, st.cams[m], rays.back()));
        }
        if (views.size() < 2 || widest < kMinAngle) continue;
        Vec3 X;
        if (!triangulate(views, rays, X)) continue;
        bool good = true;
        for (size_t v = 0; v < views.size() && good; ++v) {
            const Residual r = residual(*views[v], X, st.k(p), pixels[v], false);
            good = r.ok && r.error() < 3;
        }
        if (good) {
            st.X[t] = X;
            st.has[t] = 1;
        }
    }
}

// Registers keyframe k: its camera fitted to the points it sees, starting from `from`'s. False when too few fit.
bool registerKeyframe(const Problem& p, State& st, int k, int from) {
    std::vector<Vec3> X;
    std::vector<Point2> obs;
    for (int t : p.seen[k])
        if (st.has[t]) {
            X.push_back(st.X[t]);
            obs.push_back(p.at(t, p.keys[k]));
        }
    if (X.size() < 12) return false;
    Cam c = st.cams[from];
    double rms = 0;
    const int inliers = refinePose(c, X, obs, st.k(p), 30, &rms);
    if (inliers < std::max<int>(10, int(X.size()) / 2) || rms > 3) return false;
    c.ok = true;
    st.cams[k] = c;
    return true;
}

// The keyframe cameras and points from focal length f0 (`fixed`: kept). `rms` gets the final error.
bool solveKeyframes(const Problem& p, double f0, bool fixed, State& st, double& rms, std::string& why,
                    const std::atomic<bool>* cancel) {
    std::mt19937 rng(7);
    const int n = int(p.keys.size());
    // The first two cameras: the pair of keyframes that sees the scene from the most different places.
    int bestA = -1, bestB = -1;
    double bestScore = 0;
    TwoView best;
    const int stride = std::max(1, n / 12);
    for (int a = 0; a + 1 < n; a += stride)
        for (int d : {1, 2, 3, 5, 8}) {
            const int b = a + d;
            if (b >= n) break;
            std::vector<int> shared;
            for (int t : p.seen[a])
                if (p.covers(t, p.keys[b])) shared.push_back(t);
            if (shared.size() < 30) continue;
            std::vector<Point2> xa, xb, pa, pb;
            for (int t : shared) {
                pa.push_back(p.at(t, p.keys[a]));
                pb.push_back(p.at(t, p.keys[b]));
                xa.push_back(normalised(p, f0, pa.back()));
                xb.push_back(normalised(p, f0, pb.back()));
            }
            TwoView tv;
            if (!twoView(xa, xb, f0, rng, tv)) continue;
            // When one homography moves (nearly) all of them, the camera only turned or the scene is a plane: the
            // essential matrix is then any of many, its depths meaningless.
            Homography h;
            int flat = 0;
            if (fitHomography(pa, pb, h, &flat) && flat >= int(0.85 * tv.pairs.size())) continue;
            if (tv.angle < 1.0) continue;
            const double score = double(tv.pairs.size()) * std::min(tv.angle, 4.0);
            if (score > bestScore) {
                bestScore = score;
                bestA = a;
                bestB = b;
                best = tv;
            }
        }
    if (bestA < 0) {
        why = "The camera moves too little for the scene's depth to be seen (it only turns, or the scene is flat)";
        return false;
    }
    st.f = f0;
    st.cams.assign(n, Cam{});
    st.X.assign(p.tracks.size(), Vec3{});
    st.has.assign(p.tracks.size(), 0);
    st.cams[bestA].ok = true;
    st.cams[bestB] = best.second;
    addPoints(p, st, bestB);
    bundleAdjust(p, st, {bestA, bestB}, bestA, false, 10);
    prune(p, st, 4);
    // Then the others, outwards from the pair.
    std::vector<std::pair<int, int>> order;  // (keyframe, -1 inside / +1 after / -2 before)
    for (int k = bestA + 1; k < bestB; ++k) order.push_back({k, -1});
    for (int k = bestB + 1; k < n; ++k) order.push_back({k, 1});
    for (int k = bestA - 1; k >= 0; --k) order.push_back({k, -2});
    int inside = bestA, after = bestB, before = bestA, added = 0;
    int afterMissed = 0, beforeMissed = 0;  // keyframes in a row that did not fit (a blurred one is passed over)
    for (const auto& [k, way] : order) {
        if (cancel && cancel->load()) {
            why = "Cancelled";
            return false;
        }
        if ((way == 1 && afterMissed > 3) || (way == -2 && beforeMissed > 3)) continue;
        int& from = way == -1 ? inside : way == 1 ? after : before;
        if (!registerKeyframe(p, st, k, from)) {
            if (way == 1) ++afterMissed;
            if (way == -2) ++beforeMissed;
            continue;
        }
        if (way == 1) afterMissed = 0;
        if (way == -2) beforeMissed = 0;
        from = k;
        addPoints(p, st, k);
        if (++added % 5 == 0) {
            bundleAdjust(p, st, registered(st), bestA, false, 8);
            prune(p, st, 4);
        }
    }
    const std::vector<int> cams = registered(st);
    bundleAdjust(p, st, cams, bestA, !fixed, 40);
    prune(p, st, 2.5);
    rms = bundleAdjust(p, st, cams, bestA, !fixed, 20);
    int points = 0;
    for (char h : st.has) points += h;
    if (points < 12 || !(st.f > 0)) {
        why = "Too few features fit one camera move";
        return false;
    }
    return true;
}

}  // namespace

double CameraSolve::fieldOfView() const { return focal > 0 ? 2 * std::atan(width / 2.0 / focal) * 180 / kPi : 0; }

bool CameraSolve::project(int frame, const Vec3& X, Point2& out) const {
    if (frame < 0 || frame >= int(cameras.size()) || !cameras[frame].valid) return false;
    const SolvedCamera& c = cameras[frame];
    const Vec3 p = mul(c.R, X - c.C);
    if (!(p.z > 1e-9)) return false;
    out = {focal * p.x / p.z + width / 2.0, focal * p.y / p.z + height / 2.0};
    return true;
}

bool solveCamera(const std::vector<FeatureTrack>& tracks, int frames, int width, int height, const CameraSolveOptions& options,
                 CameraSolve& out, const SolveProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    out = CameraSolve{};
    out.width = width;
    out.height = height;
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (frames < 2 || width < 16 || height < 16) return fail("The shot is too short to solve its camera");
    Problem p;
    p.frames = frames;
    p.cx = width / 2.0;
    p.cy = height / 2.0;
    const size_t minLength = size_t(std::max(2, options.minTrackLength));
    for (const FeatureTrack& t : tracks)
        if (t.first >= 0 && t.at.size() >= minLength && t.last() < frames) p.tracks.push_back(&t);
    if (p.tracks.size() < 30) return fail("Too few features could be followed through the shot to solve its camera");
    // Fewer keyframes for a long shot: farther apart, and (when features come and go so fast that it is not enough)
    // every so many of them.
    double step = 0.03 * width;
    for (int tries = 0; tries < 12; ++tries) {
        if (cancel && cancel->load()) return fail("Cancelled");
        p.keys = chooseKeyframes(p, step);
        if (int(p.keys.size()) <= kMaxKeyframes) break;
        step *= 1.4;
    }
    if (int(p.keys.size()) > kMaxKeyframes) {
        std::vector<int> fewer;
        for (int i = 0; i < kMaxKeyframes; ++i) fewer.push_back(p.keys[size_t(std::llround(double(i) * (p.keys.size() - 1) / (kMaxKeyframes - 1)))]);
        p.keys = fewer;
    }
    p.seen.assign(p.keys.size(), {});
    for (int t = 0; t < int(p.tracks.size()); ++t)
        for (int k = 0; k < int(p.keys.size()); ++k)
            if (p.covers(t, p.keys[k])) p.seen[k].push_back(t);

    // Several fields of view to start from (the solve's focal length moves from there), the best kept.
    std::vector<double> starts;
    double known = options.focal;
    if (!(known > 0) && options.fieldOfView > 0 && options.fieldOfView < 180)
        known = width / 2.0 / std::tan(options.fieldOfView / 2 * kPi / 180);
    if (known > 0)
        starts.push_back(known);
    else
        for (double fov : {30.0, 45.0, 60.0, 80.0}) starts.push_back(width / 2.0 / std::tan(fov / 2 * kPi / 180));
    State best;
    int bestCount = 0;
    double bestRms = 0;
    std::string why;
    for (size_t i = 0; i < starts.size(); ++i) {
        if (progress) progress(0.8 * i / starts.size(), "Solving the camera");
        State st;
        double rms = 0;
        if (!solveKeyframes(p, starts[i], known > 0, st, rms, why, cancel)) {
            if (cancel && cancel->load()) return fail("Cancelled");
            continue;
        }
        if (!(known > 0) && (st.f < 0.15 * width || st.f > 20.0 * width)) continue;
        const int count = int(registered(st).size());
        // More keyframes registered wins; then the smaller error.
        if (bestCount == 0 || count > bestCount + int(p.keys.size()) / 20 ||
            (count + int(p.keys.size()) / 20 >= bestCount && rms < bestRms)) {
            best = st;
            bestCount = count;
            bestRms = rms;
        }
    }
    if (bestCount == 0) return fail(why.empty() ? "The camera could not be solved" : why);

    // Every frame's camera, from the points.
    if (progress) progress(0.8, "Solving every frame");
    const Intrinsics k = best.k(p);
    std::vector<Cam> cams(frames);
    std::vector<int> keyOf(frames, -1);
    for (int i = 0; i < int(p.keys.size()); ++i)
        if (best.cams[i].ok) keyOf[p.keys[i]] = i;
    auto solveFrame = [&](int i, const Cam* previous) {
        if (keyOf[i] >= 0) {
            cams[i] = best.cams[keyOf[i]];
            return;
        }
        Cam c;
        if (previous && previous->ok) {
            c = *previous;
        } else {
            int nearest = -1;
            for (int j = 0; j < int(p.keys.size()); ++j)
                if (best.cams[j].ok && (nearest < 0 || std::abs(p.keys[j] - i) < std::abs(p.keys[nearest] - i))) nearest = j;
            c = best.cams[nearest];
        }
        std::vector<Vec3> X;
        std::vector<Point2> obs;
        for (int t = 0; t < int(p.tracks.size()); ++t)
            if (best.has[t] && p.covers(t, i)) {
                X.push_back(best.X[t]);
                obs.push_back(p.at(t, i));
            }
        if (X.size() < 8) return;
        double rms = 0;
        const int inliers = refinePose(c, X, obs, k, 20, &rms);
        if (inliers >= std::max<int>(6, int(X.size()) / 2) && rms < 4) {
            c.ok = true;
            cams[i] = c;
        }
    };
    int firstKey = frames;
    for (int i = 0; i < frames; ++i)
        if (keyOf[i] >= 0) {
            firstKey = i;
            break;
        }
    // Twice when the first pass shows points that only fit the keyframes (something moving on its own, seen in too
    // few of them to tell): those are dropped and the frames solved again without them.
    for (int pass = 0; pass < 2; ++pass) {
        cams.assign(frames, Cam{});
        for (int i = firstKey; i < frames; ++i) {
            if (cancel && cancel->load()) return fail("Cancelled");
            solveFrame(i, i > firstKey ? &cams[i - 1] : nullptr);
            if (progress && i % 10 == 0) progress(0.8 + 0.1 * (pass + double(i + 1) / frames), "Solving every frame");
        }
        for (int i = firstKey - 1; i >= 0; --i) solveFrame(i, &cams[i + 1]);
        bool dropped = false;
        for (int t = 0; t < int(p.tracks.size()); ++t) {
            if (!best.has[t]) continue;
            double sum = 0;
            int n = 0;
            for (int i = p.tracks[t]->first; i <= p.tracks[t]->last(); ++i) {
                if (!cams[i].ok) continue;
                const Residual r = residual(cams[i], best.X[t], k, p.at(t, i), false);
                sum += r.ok ? r.error() * r.error() : 1e4;
                ++n;
            }
            if (n && std::sqrt(sum / n) > 3) {
                best.has[t] = 0;
                dropped = true;
            }
        }
        if (!dropped) break;
    }

    // The result, with each frame's and each point's error.
    out.focal = best.f;
    out.keyframes = bestCount;
    out.cameras.resize(frames);
    std::vector<int> pointOf(p.tracks.size(), -1);
    for (int t = 0; t < int(p.tracks.size()); ++t)
        if (best.has[t]) {
            pointOf[t] = int(out.points.size());
            SolvedPoint sp;
            sp.X = best.X[t];
            sp.track = int(p.tracks[t] - tracks.data());
            sp.first = p.tracks[t]->first;
            sp.last = p.tracks[t]->last();
            out.points.push_back(sp);
        }
    std::vector<double> pointSum(out.points.size(), 0);
    double total = 0;
    int totalN = 0;
    for (int i = 0; i < frames; ++i) {
        if (!cams[i].ok) continue;
        SolvedCamera& sc = out.cameras[i];
        sc.valid = true;
        sc.R = cams[i].R;
        sc.C = cams[i].C;
        double sum = 0;
        int n = 0;
        for (int t = 0; t < int(p.tracks.size()); ++t) {
            if (pointOf[t] < 0 || !p.covers(t, i)) continue;
            const Residual r = residual(cams[i], best.X[t], k, p.at(t, i), false);
            if (!r.ok) continue;
            const double e2 = r.error() * r.error();
            sum += e2;
            ++n;
            pointSum[pointOf[t]] += e2;
            ++out.points[pointOf[t]].seen;
        }
        sc.error = n ? std::sqrt(sum / n) : 0;
        total += sum;
        totalN += n;
    }
    for (size_t j = 0; j < out.points.size(); ++j)
        out.points[j].error = out.points[j].seen ? std::sqrt(pointSum[j] / out.points[j].seen) : 0;
    out.error = totalN ? std::sqrt(total / totalN) : 0;
    if (progress) progress(1, "Solving every frame");
    return true;
}

std::vector<FeatureTrack> trackFeatures(const std::string& path, double from, double to, int maxWidth, int maxFeatures,
                                        int& width, int& height, double& fps, const SolveProgress& progress,
                                        const std::atomic<bool>* cancel, std::string* error) {
    std::vector<FeatureTrack> out;
    VideoDecoder dec;
    if (!dec.open(path, error)) return out;
    fps = dec.fps() > 0 ? dec.fps() : 25;
    const int mw = std::max(1, dec.displayWidth()), mh = std::max(1, dec.displayHeight());
    width = std::min(std::max(64, maxWidth), mw);
    height = std::max(16, int(std::lround(double(width) * mh / mw)));
    const int frames = std::max(1, int(std::floor((to - from) * fps + 1e-6)));
    const double spacing = std::max(4.0, width / 50.0);
    auto read = [&](int i, GrayImage& g) {
        Frame16Ptr f = dec.frameAt(from + i / fps, width, height);
        if (!f) return false;
        g = toGray(*f);
        return true;
    };
    GrayImage cur, next;
    if (!read(0, cur)) {
        if (error) *error = "Cannot read the clip's frames";
        return out;
    }
    std::vector<int> active;  // tracks still followed
    auto topUp = [&](const GrayImage& img, int frame) {
        if (int(active.size()) >= maxFeatures * 4 / 5) return;
        const int gw = int(width / spacing) + 1, gh = int(height / spacing) + 1;
        std::vector<char> taken(size_t(gw) * gh, 0);
        auto cell = [&](Point2 q) { return std::pair<int, int>{int(q.x / spacing), int(q.y / spacing)}; };
        for (int t : active) {
            const auto [x, y] = cell(out[t].at.back());
            if (x >= 0 && y >= 0 && x < gw && y < gh) taken[size_t(y) * gw + x] = 1;
        }
        for (const Point2& q : goodFeatures(img, maxFeatures, spacing)) {
            if (int(active.size()) >= maxFeatures) break;
            const auto [x, y] = cell(q);
            bool near = false;
            for (int dy = -1; dy <= 1 && !near; ++dy)
                for (int dx = -1; dx <= 1 && !near; ++dx) {
                    const int cx = x + dx, cy = y + dy;
                    near = cx >= 0 && cy >= 0 && cx < gw && cy < gh && taken[size_t(cy) * gw + cx];
                }
            if (near) continue;
            if (x >= 0 && y >= 0 && x < gw && y < gh) taken[size_t(y) * gw + x] = 1;
            active.push_back(int(out.size()));
            out.push_back({frame, {q}});
        }
    };
    topUp(cur, 0);
    for (int i = 1; i < frames; ++i) {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return {};
        }
        if (!read(i, next)) break;
        std::vector<Point2> from_, to_;
        std::vector<bool> ok;
        for (int t : active) from_.push_back(out[t].at.back());
        trackPoints(cur, next, from_, to_, ok);
        std::vector<int> still;
        for (size_t j = 0; j < active.size(); ++j) {
            const Point2 q = to_[j];
            if (!ok[j] || q.x < 1 || q.y < 1 || q.x > width - 2 || q.y > height - 2) continue;
            out[active[j]].at.push_back(q);
            still.push_back(active[j]);
        }
        active.swap(still);
        topUp(next, i);
        std::swap(cur, next);
        if (progress) progress(double(i) / frames, "Following features");
    }
    return out;
}

bool solveMediaCamera(const std::string& path, double from, double to, const CameraSolveOptions& options, CameraSolve& out,
                      const SolveProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    int width = 0, height = 0;
    double fps = 0;
    const auto tracks = trackFeatures(path, from, to, 960, options.maxFeatures, width, height, fps,
                                      [&](double f, const char* stage) {
                                          if (progress) progress(0.5 * f, stage);
                                      },
                                      cancel, error);
    if (tracks.empty()) {
        if (error && error->empty()) *error = "No features to follow in the shot";
        return false;
    }
    int frames = 0;
    for (const FeatureTrack& t : tracks) frames = std::max(frames, t.last() + 1);
    if (!solveCamera(tracks, frames, width, height, options, out,
                     [&](double f, const char* stage) {
                         if (progress) progress(0.5 + 0.5 * f, stage);
                     },
                     cancel, error))
        return false;
    out.fps = fps;
    out.start = from;
    return true;
}

bool orientScene(CameraSolve& s, double height) {
    const SolvedCamera* first = nullptr;
    for (const SolvedCamera& c : s.cameras)
        if (c.valid) {
            first = &c;
            break;
        }
    const int n = int(s.points.size());
    if (!first || n < 12 || !(height > 0)) return false;
    std::vector<double> dist;
    for (const SolvedPoint& p : s.points) dist.push_back(norm(p.X - first->C));
    std::nth_element(dist.begin(), dist.begin() + n / 2, dist.end());
    const double tolerance = 0.02 * dist[n / 2];
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> pick(0, n - 1);
    // A floor is level enough for a camera tilted up to 60 degrees: the plane with the most points among those, else
    // (a camera looking straight down) among any.
    const Vec3 cameraUp = mulT(first->R, {0, -1, 0});
    std::vector<int> best, bestLevel;
    for (int it = 0; it < 800; ++it) {
        const Vec3 a = s.points[pick(rng)].X, b = s.points[pick(rng)].X, c = s.points[pick(rng)].X;
        const Vec3 normal = cross(b - a, c - a);
        if (norm(normal) < 1e-12) continue;
        const Vec3 u = unit(normal);
        std::vector<int> in;
        for (int i = 0; i < n; ++i)
            if (std::fabs(dot(s.points[i].X - a, u)) < tolerance) in.push_back(i);
        if (std::fabs(dot(u, cameraUp)) >= 0.5 && in.size() > bestLevel.size()) bestLevel = in;
        if (in.size() > best.size()) best.swap(in);
    }
    const int enough = std::max(12, n * 3 / 20);
    if (int(bestLevel.size()) >= enough) best.swap(bestLevel);
    if (int(best.size()) < enough) return false;
    // Least squares on the inliers (the plane through their middle, normal to their least spread), then again on
    // those within three robust deviations of it: points just off the floor (the foot of a wall) leave it.
    Vec3 mid, up;
    auto fit = [&](const std::vector<int>& idx) {
        mid = {};
        for (int i : idx) mid = mid + s.points[i].X;
        mid = (1.0 / idx.size()) * mid;
        std::vector<double> cov(9, 0);
        for (int i : idx) {
            const Vec3 d = s.points[i].X - mid;
            const double v[3] = {d.x, d.y, d.z};
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b) cov[a * 3 + b] += v[a] * v[b];
        }
        std::vector<double> values, vecs;
        symmetricEigen(cov, 3, values, vecs);
        up = unit({vecs[0], vecs[3], vecs[6]});
    };
    fit(best);
    for (int round = 0; round < 3; ++round) {
        std::vector<double> off;
        for (int i : best) off.push_back(std::fabs(dot(s.points[i].X - mid, up)));
        std::nth_element(off.begin(), off.begin() + off.size() / 2, off.end());
        const double within = std::min(tolerance, 3 * 1.4826 * off[off.size() / 2]);
        std::vector<int> in;
        for (int i = 0; i < n; ++i)
            if (std::fabs(dot(s.points[i].X - mid, up)) <= within) in.push_back(i);
        if (int(in.size()) < 12) break;
        best.swap(in);
        fit(best);
    }
    double above = dot(first->C - mid, up);
    if (above < 0) {
        up = -1.0 * up;
        above = -above;
    }
    if (above < 1e-9) return false;
    // The first camera looks along -z (as Nuke's and most 3D programs' cameras do).
    Vec3 forward = mulT(first->R, {0, 0, 1});
    forward = forward - dot(forward, up) * up;
    if (norm(forward) < 1e-6) forward = mulT(first->R, {0, -1, 0}) - dot(mulT(first->R, {0, -1, 0}), up) * up;
    const Vec3 z = -1.0 * unit(forward), x = cross(up, z);
    const M3 M = fromRows(x, up, z);
    const double k = height / above;
    auto place = [&](Vec3 X) { return k * mul(M, X - mid); };
    for (SolvedPoint& p : s.points) p.X = place(p.X);
    const M3 Mt = transpose(M);
    for (SolvedCamera& c : s.cameras) {
        if (!c.valid) continue;
        c.C = place(c.C);
        c.R = mul(c.R, Mt);
    }
    s.oriented = true;
    return true;
}

std::array<Vec3, 4> sceneRectangle(const CameraSolve& s, const Vec3& centre, double width, double height, bool upright, double turn) {
    const SolvedCamera* first = nullptr;
    for (const SolvedCamera& c : s.cameras)
        if (c.valid) {
            first = &c;
            break;
        }
    const M3 R = first ? first->R : M3{1, 0, 0, 0, 1, 0, 0, 0, 1};
    const Vec3 C = first ? first->C : Vec3{};
    const Vec3 up = s.oriented ? Vec3{0, 1, 0} : unit(mulT(R, {0, -1, 0}));
    Vec3 away = centre - C;
    away = away - dot(away, up) * up;
    if (norm(away) < 1e-9) away = mulT(R, {0, 0, 1}) - dot(mulT(R, {0, 0, 1}), up) * up;
    away = unit(away);
    // Across and along the rectangle, before its turn about its normal.
    Vec3 across = cross(away, up), along = upright ? up : away;
    const Vec3 normal = upright ? -1.0 * away : up;
    const double a = turn * kPi / 180;
    const M3 T = rotation(a * normal);
    across = mul(T, across);
    along = mul(T, along);
    const Vec3 w = (width / 2) * across, h = (height / 2) * along;
    return {centre + h - w, centre + h + w, centre - h + w, centre - h - w};
}

bool groundPoint(const CameraSolve& s, int frame, Point2 pixel, Vec3& out) {
    if (!s.oriented || frame < 0 || frame >= int(s.cameras.size()) || !s.cameras[frame].valid || !(s.focal > 0)) return false;
    const SolvedCamera& c = s.cameras[frame];
    const Vec3 d = mulT(c.R, {(pixel.x - s.width / 2.0) / s.focal, (pixel.y - s.height / 2.0) / s.focal, 1});
    if (!(std::fabs(d.y) > 1e-12)) return false;
    const double t = -c.C.y / d.y;
    if (!(t > 0)) return false;
    out = c.C + t * d;
    return true;
}

std::string cameraSolveToString(const CameraSolve& s) {
    std::ostringstream o;
    o.precision(10);
    o << "c1 " << s.width << ' ' << s.height << ' ' << s.focal << ' ' << s.fps << ' ' << s.start << ' ' << s.error << ' '
      << s.keyframes << ' ' << (s.oriented ? 1 : 0) << ' ' << s.cameras.size();
    for (const SolvedCamera& c : s.cameras) {
        o << ' ' << (c.valid ? 1 : 0);
        if (!c.valid) continue;
        for (double r : c.R) o << ' ' << r;
        o << ' ' << c.C.x << ' ' << c.C.y << ' ' << c.C.z << ' ' << c.error;
    }
    o << ' ' << s.points.size();
    for (const SolvedPoint& p : s.points)
        o << ' ' << p.X.x << ' ' << p.X.y << ' ' << p.X.z << ' ' << p.track << ' ' << p.error << ' ' << p.seen << ' ' << p.first << ' '
          << p.last;
    return o.str();
}

bool cameraSolveFromString(const std::string& text, CameraSolve& s) {
    std::istringstream in(text);
    std::string tag;
    CameraSolve r;
    int oriented = 0;
    size_t n = 0;
    if (!(in >> tag >> r.width >> r.height >> r.focal >> r.fps >> r.start >> r.error >> r.keyframes >> oriented >> n) ||
        tag != "c1" || r.width <= 0 || r.height <= 0 || !(r.focal > 0) || n > 10000000)
        return false;
    r.oriented = oriented != 0;
    for (size_t i = 0; i < n; ++i) {
        SolvedCamera& c = r.cameras.emplace_back();
        int valid = 0;
        if (!(in >> valid)) return false;
        c.valid = valid != 0;
        if (!c.valid) continue;
        for (double& x : c.R)
            if (!(in >> x)) return false;
        if (!(in >> c.C.x >> c.C.y >> c.C.z >> c.error)) return false;
    }
    if (!(in >> n) || n > 10000000) return false;
    for (size_t i = 0; i < n; ++i) {
        SolvedPoint& p = r.points.emplace_back();
        if (!(in >> p.X.x >> p.X.y >> p.X.z >> p.track >> p.error >> p.seen >> p.first >> p.last)) return false;
    }
    s = std::move(r);
    return true;
}

bool writeNukeChan(const CameraSolve& s, const std::string& path, int firstFrame, std::string* error) {
    std::ofstream f(path);
    if (!f) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    f.setf(std::ios::fixed);
    f.precision(6);
    const double vfov = 2 * std::atan(s.height / 2.0 / s.focal) * 180 / kPi;
    for (size_t i = 0; i < s.cameras.size(); ++i) {
        const SolvedCamera& c = s.cameras[i];
        if (!c.valid) continue;
        // Nuke's camera: x right, y up, looking along -z; its rotation, camera to world, is Ry Rx Rz (ZXY). A scene
        // with no ground is turned to match: y up and -z forward for the first camera.
        const M3 flip{1, 0, 0, 0, -1, 0, 0, 0, -1};
        const M3 R = s.oriented ? c.R : mul(c.R, flip);
        const Vec3 C = s.oriented ? c.C : Vec3{c.C.x, -c.C.y, -c.C.z};
        const M3 M = mul(transpose(R), flip);
        const double rx = std::asin(std::clamp(-M[5], -1.0, 1.0));
        const double ry = std::atan2(M[2], M[8]);
        const double rz = std::atan2(M[3], M[4]);
        const double deg = 180 / kPi;
        f << (firstFrame + int(i)) << ' ' << C.x << ' ' << C.y << ' ' << C.z << ' ' << rx * deg << ' ' << ry * deg << ' '
          << rz * deg << ' ' << vfov << '\n';
    }
    if (!f) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    return true;
}

bool writePointsPly(const CameraSolve& s, const std::string& path, std::string* error) {
    std::ofstream f(path);
    if (!f) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    f << "ply\nformat ascii 1.0\ncomment Montage camera solve\nelement vertex " << s.points.size()
      << "\nproperty float x\nproperty float y\nproperty float z\nend_header\n";
    f.precision(8);
    const double k = s.oriented ? 1 : -1;  // (as writeNukeChan turns a scene with no ground)
    for (const SolvedPoint& p : s.points) f << p.X.x << ' ' << k * p.X.y << ' ' << k * p.X.z << '\n';
    if (!f) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    return true;
}

}  // namespace montage
