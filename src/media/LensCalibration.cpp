#include "LensCalibration.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <numeric>

#include "core/LinearSolve.h"
#include "media/Decoder.h"
#include "media/Exr.h"

namespace montage {

namespace {

using lens::Distortion;

constexpr double kPi = 3.14159265358979323846;

// ---- Pictures --------------------------------------------------------------------------------------------------

GrayImage blurred(const GrayImage& in, double sigma) {
    if (sigma < 0.3) return in;
    const int r = int(std::ceil(3 * sigma));
    std::vector<float> k(size_t(2 * r + 1));
    float sum = 0;
    for (int i = -r; i <= r; ++i) sum += k[size_t(i + r)] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
    for (float& v : k) v /= sum;
    const int W = in.width, H = in.height;
    GrayImage tmp = in, out = in;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float s = 0;
            for (int i = -r; i <= r; ++i) s += k[size_t(i + r)] * in.at(std::clamp(x + i, 0, W - 1), y);
            tmp.px[size_t(y) * W + x] = s;
        }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float s = 0;
            for (int i = -r; i <= r; ++i) s += k[size_t(i + r)] * tmp.at(x, std::clamp(y + i, 0, H - 1));
            out.px[size_t(y) * W + x] = s;
        }
    return out;
}

// GrayImage samples have their centres at whole numbers; positions here at +0.5.
float sampleAt(const GrayImage& g, double x, double y) { return g.sample(x - 0.5, y - 0.5); }

// The pixel where the gradient is taken (central differences of a smoothed picture).
void gradient(const GrayImage& g, int x, int y, double& gx, double& gy) {
    const int W = g.width, H = g.height;
    const int xl = std::max(0, x - 1), xr = std::min(W - 1, x + 1), yu = std::max(0, y - 1), yd = std::min(H - 1, y + 1);
    gx = (g.at(xr, y) - g.at(xl, y)) * 0.5;
    gy = (g.at(x, yd) - g.at(x, yu)) * 0.5;
}

// ---- Checkerboard corners --------------------------------------------------------------------------------------

struct Candidate {
    Point2 p;
    double response = 0;
    double spacing = 0;  // to the nearest other candidate
};

// Light and dark around a circle about (x, y): a checkerboard's inner corner has four arcs (dark, light, dark,
// light); an outer corner, an edge or a blob has two or none.
bool fourArcs(const GrayImage& g, double x, double y, double radius) {
    constexpr int n = 48;
    float v[n];
    float lo = 2, hi = -1;
    for (int i = 0; i < n; ++i) {
        const double a = 2 * kPi * i / n;
        v[i] = sampleAt(g, x + radius * std::cos(a), y + radius * std::sin(a));
        lo = std::min(lo, v[i]);
        hi = std::max(hi, v[i]);
    }
    if (hi - lo < 0.05f) return false;
    const float mid = (lo + hi) / 2, band = 0.15f * (hi - lo);
    // (With hysteresis: a sample near the middle keeps the side it was on.)
    int state = 0, first = 0, changes = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < n; ++i) {
            const int s = v[i] > mid + band ? 1 : v[i] < mid - band ? -1 : 0;
            if (s == 0) continue;
            if (state == 0) {
                state = first = s;
                continue;
            }
            if (s != state) {
                if (pass == 1) ++changes;
                state = s;
            }
        }
    (void)first;
    return changes == 4;
}

// cornerSubPix: the point every nearby gradient is perpendicular to the way to (on an edge through the corner the
// gradient is across the edge), solved in a Gaussian-weighted window and repeated from where it lands.
bool refineCorner(const GrayImage& g, Point2& p, double half) {
    const Point2 start = p;
    const int r = int(std::ceil(half));
    const double s2 = 2 * (half / 2) * (half / 2);
    for (int it = 0; it < 30; ++it) {
        double a = 0, b = 0, c = 0, bx = 0, by = 0;
        const int x0 = int(std::floor(p.x)), y0 = int(std::floor(p.y));
        for (int y = y0 - r; y <= y0 + r; ++y) {
            if (y < 1 || y >= g.height - 1) continue;
            for (int x = x0 - r; x <= x0 + r; ++x) {
                if (x < 1 || x >= g.width - 1) continue;
                const double qx = x + 0.5, qy = y + 0.5;
                const double d2 = (qx - p.x) * (qx - p.x) + (qy - p.y) * (qy - p.y);
                if (d2 > (half + 0.5) * (half + 0.5)) continue;
                const double w = std::exp(-d2 / s2);
                double gx, gy;
                gradient(g, x, y, gx, gy);
                const double xx = w * gx * gx, xy = w * gx * gy, yy = w * gy * gy;
                a += xx, b += xy, c += yy;
                bx += xx * qx + xy * qy;
                by += xy * qx + yy * qy;
            }
        }
        const double det = a * c - b * b;
        if (!(det > 1e-12 * (a + c) * (a + c))) return false;
        const Point2 next{(c * bx - b * by) / det, (a * by - b * bx) / det};
        const double move = std::hypot(next.x - p.x, next.y - p.y);
        p = next;
        if (std::hypot(p.x - start.x, p.y - start.y) > half) return false;
        if (move < 1e-4) break;
    }
    return true;
}

std::vector<Candidate> saddlePoints(const GrayImage& img) {
    const int W = img.width, H = img.height;
    std::vector<Candidate> out;
    if (W < 16 || H < 16) return out;
    const double sigma = std::max(1.0, std::min(W, H) / 700.0);
    const GrayImage sm = blurred(img, sigma);
    // The saddle response: minus the Hessian's determinant (large where the picture curves up one way and down the
    // other, as at the meeting of two dark and two light squares).
    std::vector<float> s(size_t(W) * H, 0.f);
    const int border = int(std::ceil(3 * sigma)) + 2;
    float best = 0;
    for (int y = border; y < H - border; ++y)
        for (int x = border; x < W - border; ++x) {
            const double c = sm.at(x, y);
            const double fxx = sm.at(x + 1, y) - 2 * c + sm.at(x - 1, y);
            const double fyy = sm.at(x, y + 1) - 2 * c + sm.at(x, y - 1);
            const double fxy = (sm.at(x + 1, y + 1) - sm.at(x + 1, y - 1) - sm.at(x - 1, y + 1) + sm.at(x - 1, y - 1)) / 4;
            const float v = float(fxy * fxy - fxx * fyy);
            s[size_t(y) * W + x] = v;
            best = std::max(best, v);
        }
    if (best <= 0) return out;
    const float threshold = best * 0.01f;
    const int nms = std::max(3, int(std::lround(2 * sigma)));
    for (int y = border; y < H - border; ++y)
        for (int x = border; x < W - border; ++x) {
            const float v = s[size_t(y) * W + x];
            if (v < threshold) continue;
            bool peak = true;
            for (int dy = -nms; dy <= nms && peak; ++dy)
                for (int dx = -nms; dx <= nms; ++dx) {
                    if (!dx && !dy) continue;
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= W || yy >= H) continue;
                    const float o = s[size_t(yy) * W + xx];
                    if (o > v || (o == v && (dy < 0 || (dy == 0 && dx < 0)))) {
                        peak = false;
                        break;
                    }
                }
            if (!peak) continue;
            Candidate c;
            c.p = {x + 0.5, y + 0.5};
            c.response = v;
            out.push_back(c);
        }
    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.response > b.response; });
    if (out.size() > 4000) out.resize(4000);
    // Four arcs around each (just outside where the smoothing reaches), then each refined in a window a third of the
    // way to its nearest neighbour.
    const double radius = std::max(4.0, 3 * sigma);
    std::vector<Candidate> kept;
    for (const Candidate& c : out)
        if (fourArcs(sm, c.p.x, c.p.y, radius)) kept.push_back(c);
    for (Candidate& c : kept) {
        double nearest = 1e30;
        for (const Candidate& o : kept)
            if (&o != &c) nearest = std::min(nearest, std::hypot(o.p.x - c.p.x, o.p.y - c.p.y));
        c.spacing = nearest;
    }
    const GrayImage fine = blurred(img, 0.7);
    std::vector<Candidate> refined;
    for (Candidate c : kept) {
        if (c.spacing < 2 * radius) continue;  // (squares too small to tell apart)
        const double half = std::clamp(0.35 * c.spacing, 3.0, 25.0);
        if (!refineCorner(fine, c.p, half)) continue;
        bool same = false;
        for (const Candidate& o : refined)
            if (std::hypot(o.p.x - c.p.x, o.p.y - c.p.y) < 1.5) same = true;
        if (!same) refined.push_back(c);
    }
    for (Candidate& c : refined) {
        double nearest = 1e30;
        for (const Candidate& o : refined)
            if (&o != &c) nearest = std::min(nearest, std::hypot(o.p.x - c.p.x, o.p.y - c.p.y));
        c.spacing = nearest;
    }
    return refined;
}

}  // namespace

bool findCheckerboard(const GrayImage& img, int cols, int rows, std::vector<Point2>& corners) {
    corners.clear();
    if (cols < 2 || rows < 2) return false;
    const std::vector<Candidate> cand = saddlePoints(img);
    const int n = int(cand.size());
    if (n < cols * rows) return false;
    // Grown from the candidate nearest the middle of them all, square by square: each neighbour looked for where the
    // steps so far say it is.
    std::vector<double> xs, ys;
    for (const Candidate& c : cand) xs.push_back(c.p.x), ys.push_back(c.p.y);
    std::nth_element(xs.begin(), xs.begin() + n / 2, xs.end());
    std::nth_element(ys.begin(), ys.begin() + n / 2, ys.end());
    const Point2 middle{xs[size_t(n / 2)], ys[size_t(n / 2)]};
    std::vector<int> bySeed(static_cast<size_t>(n));
    std::iota(bySeed.begin(), bySeed.end(), 0);
    std::sort(bySeed.begin(), bySeed.end(), [&](int a, int b) {
        return std::hypot(cand[size_t(a)].p.x - middle.x, cand[size_t(a)].p.y - middle.y) <
               std::hypot(cand[size_t(b)].p.x - middle.x, cand[size_t(b)].p.y - middle.y);
    });
    struct Node {
        int cand = -1;
        Point2 ei, ej;  // the steps to the next corner along the board's rows and columns, here
    };
    const auto key = [](int i, int j) { return (int64_t(i) << 32) ^ int64_t(uint32_t(j)); };
    // (A few seeds tried, in case the middle one is clutter.)
    for (int attempt = 0; attempt < std::min(n, 6); ++attempt) {
        const int seed = bySeed[size_t(attempt)];
        const Point2 sp = cand[size_t(seed)].p;
        std::vector<int> near(static_cast<size_t>(n));
        std::iota(near.begin(), near.end(), 0);
        std::sort(near.begin(), near.end(), [&](int a, int b) {
            return std::hypot(cand[size_t(a)].p.x - sp.x, cand[size_t(a)].p.y - sp.y) <
                   std::hypot(cand[size_t(b)].p.x - sp.x, cand[size_t(b)].p.y - sp.y);
        });
        if (n < 3) break;
        const Point2 e1{cand[size_t(near[1])].p.x - sp.x, cand[size_t(near[1])].p.y - sp.y};
        Point2 e2{0, 0};
        for (size_t k = 2; k < std::min<size_t>(near.size(), 9); ++k) {
            const Point2 d{cand[size_t(near[k])].p.x - sp.x, cand[size_t(near[k])].p.y - sp.y};
            const double cosA = (d.x * e1.x + d.y * e1.y) / (std::hypot(d.x, d.y) * std::hypot(e1.x, e1.y));
            if (std::fabs(cosA) < 0.5) {
                e2 = d;
                break;
            }
        }
        if (e2.x == 0 && e2.y == 0) continue;
        std::map<int64_t, Node> grid;
        std::vector<char> used(size_t(n), 0);
        std::vector<std::pair<int, int>> queue{{0, 0}};
        grid[key(0, 0)] = {seed, e1, e2};
        used[size_t(seed)] = 1;
        bool clash = false;
        for (size_t q = 0; q < queue.size() && !clash; ++q) {
            const auto [i, j] = queue[q];
            const Node node = grid[key(i, j)];
            const Point2 p = cand[size_t(node.cand)].p;
            const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& d : dirs) {
                const int ni = i + d[0], nj = j + d[1];
                if (grid.count(key(ni, nj))) continue;
                const Point2 step{d[0] * node.ei.x + d[1] * node.ej.x, d[0] * node.ei.y + d[1] * node.ej.y};
                const Point2 want{p.x + step.x, p.y + step.y};
                const double reach = 0.35 * std::hypot(step.x, step.y);
                int found = -1;
                double bestD = reach;
                for (int k = 0; k < n; ++k) {
                    const double dd = std::hypot(cand[size_t(k)].p.x - want.x, cand[size_t(k)].p.y - want.y);
                    if (dd < bestD) bestD = dd, found = k;
                }
                if (found < 0) continue;
                if (used[size_t(found)]) {
                    clash = true;  // (one corner two places on the board: not a board)
                    break;
                }
                used[size_t(found)] = 1;
                const Point2 fp = cand[size_t(found)].p;
                Node next{found, node.ei, node.ej};
                const Point2 moved{(fp.x - p.x), (fp.y - p.y)};
                if (d[0]) next.ei = {moved.x * d[0], moved.y * d[0]};
                else next.ej = {moved.x * d[1], moved.y * d[1]};
                grid[key(ni, nj)] = next;
                queue.push_back({ni, nj});
            }
        }
        if (clash) continue;
        int i0 = INT32_MAX, i1 = INT32_MIN, j0 = INT32_MAX, j1 = INT32_MIN;
        for (const auto& [k, node] : grid) {
            const int i = int(k >> 32), j = int(int32_t(uint32_t(k & 0xffffffff)));
            i0 = std::min(i0, i), i1 = std::max(i1, i), j0 = std::min(j0, j), j1 = std::max(j1, j);
        }
        const int ni = i1 - i0 + 1, nj = j1 - j0 + 1;
        if (int(grid.size()) != ni * nj) continue;
        const bool straight = ni == cols && nj == rows, turned = ni == rows && nj == cols;
        if (!straight && !turned) continue;
        // Into board order: rows along i, then the board's normal towards the camera's back (its x by its y
        // pointing into the picture, as the picture's x by y does).
        auto at = [&](int i, int j) {  // (board position to the grid's)
            return turned ? cand[size_t(grid[key(i0 + j, j0 + i)].cand)].p : cand[size_t(grid[key(i0 + i, j0 + j)].cand)].p;
        };
        std::vector<Point2> out(size_t(cols * rows));
        for (int j = 0; j < rows; ++j)
            for (int i = 0; i < cols; ++i) out[size_t(j * cols + i)] = at(i, j);
        double cross = 0;
        for (int j = 0; j + 1 < rows; ++j)
            for (int i = 0; i + 1 < cols; ++i) {
                const Point2 a = out[size_t(j * cols + i)], b = out[size_t(j * cols + i + 1)], c = out[size_t((j + 1) * cols + i)];
                cross += (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            }
        if (cross < 0)
            for (int j = 0; j < rows / 2; ++j)
                for (int i = 0; i < cols; ++i) std::swap(out[size_t(j * cols + i)], out[size_t((rows - 1 - j) * cols + i)]);
        corners = std::move(out);
        return true;
    }
    return false;
}

// ---- The camera model -------------------------------------------------------------------------------------------

namespace {

struct Vec3d {
    double x = 0, y = 0, z = 0;
};

using M3 = std::array<double, 9>;

M3 rotation(const double w[3]) {
    const double th = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (th < 1e-12) return {1, -w[2], w[1], w[2], 1, -w[0], -w[1], w[0], 1};
    const double kx = w[0] / th, ky = w[1] / th, kz = w[2] / th, c = std::cos(th), s = std::sin(th), v = 1 - c;
    return {c + kx * kx * v,      kx * ky * v - kz * s, kx * kz * v + ky * s,
            ky * kx * v + kz * s, c + ky * ky * v,      ky * kz * v - kx * s,
            kz * kx * v - ky * s, kz * ky * v + kx * s, c + kz * kz * v};
}

void rotationVector(const M3& R, double w[3]) {
    const double c = std::clamp((R[0] + R[4] + R[8] - 1) / 2, -1.0, 1.0), th = std::acos(c);
    if (th < 1e-9) {
        w[0] = (R[7] - R[5]) / 2, w[1] = (R[2] - R[6]) / 2, w[2] = (R[3] - R[1]) / 2;
        return;
    }
    if (kPi - th < 1e-6) {
        // (A half turn: the axis from the matrix's diagonal.)
        const double x = std::sqrt(std::max(0.0, (R[0] + 1) / 2)), y = std::sqrt(std::max(0.0, (R[4] + 1) / 2)),
                     z = std::sqrt(std::max(0.0, (R[8] + 1) / 2));
        w[0] = th * x;
        w[1] = th * (R[1] >= 0 ? y : -y);
        w[2] = th * (R[2] >= 0 ? z : -z);
        return;
    }
    const double k = th / (2 * std::sin(th));
    w[0] = k * (R[7] - R[5]), w[1] = k * (R[2] - R[6]), w[2] = k * (R[3] - R[1]);
}

int termCount(Distortion::Model m) { return m == Distortion::Poly3 ? 1 : m == Distortion::Poly5 ? 2 : m == Distortion::PtLens ? 3 : 0; }

// rd / ru (lensfun's polynomials over ru), and rd itself and its slope.
double ratio(Distortion::Model m, const double* k, double ru) {
    const double r2 = ru * ru;
    switch (m) {
        case Distortion::Poly3: return 1 - k[0] + k[0] * r2;
        case Distortion::Poly5: return 1 + k[0] * r2 + k[1] * r2 * r2;
        case Distortion::PtLens: return k[0] * r2 * ru + k[1] * r2 + k[2] * ru + 1 - k[0] - k[1] - k[2];
        case Distortion::None: break;
    }
    return 1;
}
double slope(Distortion::Model m, const double* k, double ru) {
    const double r2 = ru * ru;
    switch (m) {
        case Distortion::Poly3: return 1 - k[0] + 3 * k[0] * r2;
        case Distortion::Poly5: return 1 + 3 * k[0] * r2 + 5 * k[1] * r2 * r2;
        case Distortion::PtLens: return 4 * k[0] * r2 * ru + 3 * k[1] * r2 + 2 * k[2] * ru + 1 - k[0] - k[1] - k[2];
        case Distortion::None: break;
    }
    return 1;
}
// The undistorted radius for a recorded one (Newton from rd); false where the polynomial has turned back.
bool undistortRadius(Distortion::Model m, const double* k, double rd, double& ru) {
    ru = rd;
    for (int i = 0; i < 50; ++i) {
        const double e = ru * ratio(m, k, ru) - rd, d = slope(m, k, ru);
        if (!(d > 1e-6)) return false;
        const double next = ru - e / d;
        if (!(next >= 0) || !std::isfinite(next)) return false;
        const bool done = std::fabs(next - ru) < 1e-15 * std::max(1.0, ru);
        ru = next;
        if (done) return true;
    }
    return std::fabs(ru * ratio(m, k, ru) - rd) < 1e-10;
}

// The intrinsics in one place: the frame's size, and what is (or may be) fitted.
struct Intrinsics {
    int width = 0, height = 0;
    double unit = 1;  // calibration units per pixel (down; across times the squeeze)
    double F = 1, cxo = 0, cyo = 0, squeeze = 1;
    Distortion::Model model = Distortion::Poly5;
    double k[3] = {0, 0, 0};
    double cx() const { return width / 2.0 + cxo / unit; }
    double cy() const { return height / 2.0 + cyo / unit; }
};

double frameUnit(int width, int height) { return 2 / std::max(1.0, double(std::min(width - 1, height - 1))); }

// Which of the intrinsics move, as a parameter vector's first entries.
struct Layout {
    bool F = true, centre = false, squeeze = false;
    int terms = 0;
    int count() const { return int(F) + (centre ? 2 : 0) + int(squeeze) + terms; }
    void get(const Intrinsics& in, double* p) const {
        int i = 0;
        if (F) p[i++] = in.F;
        if (centre) p[i++] = in.cxo, p[i++] = in.cyo;
        if (squeeze) p[i++] = in.squeeze;
        for (int t = 0; t < terms; ++t) p[i++] = in.k[t];
    }
    void set(Intrinsics& in, const double* p) const {
        int i = 0;
        if (F) in.F = p[i++];
        if (centre) in.cxo = p[i++], in.cyo = p[i++];
        if (squeeze) in.squeeze = p[i++];
        for (int t = 0; t < terms; ++t) in.k[t] = p[i++];
    }
};

Point2 project(const Intrinsics& in, const M3& R, const double* t, double bx, double by) {
    const double X = R[0] * bx + R[1] * by + t[0], Y = R[3] * bx + R[4] * by + t[1], Z = R[6] * bx + R[7] * by + t[2];
    if (!(Z > 1e-9)) return {1e9, 1e9};
    const double a = X / Z, b = Y / Z;
    const double ru = in.F * std::hypot(a, b), f = ratio(in.model, in.k, ru);
    return {in.cx() + in.F * a * f / (in.unit * in.squeeze), in.cy() + in.F * b * f / in.unit};
}

// Levenberg–Marquardt over a problem of shared parameters (`shared`) and blocks of its own for each group of residuals:
// `residuals(g, shared, own, out)` fills group g's. Numeric derivatives (central differences).
struct Blocks {
    int shared = 0, own = 0, groups = 0;
    std::function<void(int, const double*, const double*, std::vector<double>&)> residuals;
};

double sumSquares(const Blocks& b, const std::vector<double>& p) {
    double s = 0;
    std::vector<double> r;
    for (int g = 0; g < b.groups; ++g) {
        b.residuals(g, p.data(), p.data() + b.shared + g * b.own, r);
        for (double v : r) s += v * v;
    }
    return s;
}

bool levenbergMarquardt(const Blocks& b, std::vector<double>& p, int iterations = 200) {
    const int n = b.shared + b.own * b.groups;
    double cost = sumSquares(b, p);
    if (!std::isfinite(cost)) return false;
    double lambda = 1e-3;
    std::vector<double> r, rp, rm;
    for (int it = 0; it < iterations; ++it) {
        std::vector<double> A(size_t(n) * n, 0.0), g(size_t(n), 0.0);
        for (int grp = 0; grp < b.groups; ++grp) {
            const int ownAt = b.shared + grp * b.own;
            b.residuals(grp, p.data(), p.data() + ownAt, r);
            const size_t m = r.size();
            // This group's columns: the shared, then its own.
            std::vector<int> cols;
            for (int k = 0; k < b.shared; ++k) cols.push_back(k);
            for (int k = 0; k < b.own; ++k) cols.push_back(ownAt + k);
            std::vector<std::vector<double>> J(cols.size(), std::vector<double>(m));
            for (size_t c = 0; c < cols.size(); ++c) {
                const int k = cols[c];
                const double h = 1e-6 * std::max(1.0, std::fabs(p[size_t(k)])), keep = p[size_t(k)];
                p[size_t(k)] = keep + h;
                b.residuals(grp, p.data(), p.data() + ownAt, rp);
                p[size_t(k)] = keep - h;
                b.residuals(grp, p.data(), p.data() + ownAt, rm);
                p[size_t(k)] = keep;
                for (size_t i = 0; i < m; ++i) J[c][i] = (rp[i] - rm[i]) / (2 * h);
            }
            for (size_t c = 0; c < cols.size(); ++c) {
                double gi = 0;
                for (size_t i = 0; i < m; ++i) gi += J[c][i] * r[i];
                g[size_t(cols[c])] += gi;
                for (size_t d = c; d < cols.size(); ++d) {
                    double s = 0;
                    for (size_t i = 0; i < m; ++i) s += J[c][i] * J[d][i];
                    A[size_t(cols[c]) * n + size_t(cols[d])] += s;
                    if (d != c) A[size_t(cols[d]) * n + size_t(cols[c])] += s;
                }
            }
        }
        bool improved = false;
        for (int tries = 0; tries < 12 && !improved; ++tries) {
            std::vector<double> M = A, step(static_cast<size_t>(n));
            for (int k = 0; k < n; ++k) {
                M[size_t(k) * n + size_t(k)] += lambda * std::max(A[size_t(k) * n + size_t(k)], 1e-12);
                step[size_t(k)] = -g[size_t(k)];
            }
            if (!choleskySolve(M, n, step)) {
                lambda *= 10;
                continue;
            }
            std::vector<double> q = p;
            for (int k = 0; k < n; ++k) q[size_t(k)] += step[size_t(k)];
            const double c2 = sumSquares(b, q);
            if (std::isfinite(c2) && c2 < cost) {
                const double drop = cost - c2;
                p = q;
                improved = true;
                lambda = std::max(lambda / 4, 1e-12);
                const bool done = drop < 1e-15 * std::max(cost, 1e-30) || c2 < 1e-28;
                cost = c2;
                if (done) return true;
            } else {
                lambda *= 8;
            }
        }
        if (!improved) break;
    }
    return std::isfinite(cost);
}

// A homography (row-major 3 x 3) from `from` to `to` by the normalised DLT, every point counted.
bool homography(const std::vector<Point2>& from, const std::vector<Point2>& to, double H[9]) {
    const size_t n = from.size();
    if (n < 4 || to.size() != n) return false;
    auto normaliser = [](const std::vector<Point2>& v, double T[3]) {
        double mx = 0, my = 0;
        for (const Point2& p : v) mx += p.x, my += p.y;
        mx /= double(v.size()), my /= double(v.size());
        double d = 0;
        for (const Point2& p : v) d += std::hypot(p.x - mx, p.y - my);
        d /= double(v.size());
        const double s = d > 0 ? std::sqrt(2.0) / d : 1;
        T[0] = s, T[1] = -s * mx, T[2] = -s * my;
    };
    double Ta[3], Tb[3];
    normaliser(from, Ta);
    normaliser(to, Tb);
    std::vector<double> AtA(81, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const double x = Ta[0] * from[i].x + Ta[1], y = Ta[0] * from[i].y + Ta[2];
        const double u = Tb[0] * to[i].x + Tb[1], v = Tb[0] * to[i].y + Tb[2];
        const double r1[9] = {-x, -y, -1, 0, 0, 0, u * x, u * y, u};
        const double r2[9] = {0, 0, 0, -x, -y, -1, v * x, v * y, v};
        for (int a = 0; a < 9; ++a)
            for (int c = 0; c < 9; ++c) AtA[size_t(a * 9 + c)] += r1[a] * r1[c] + r2[a] * r2[c];
    }
    std::vector<double> values, vecs;
    symmetricEigen(AtA, 9, values, vecs);
    double h[9];
    for (int k = 0; k < 9; ++k) h[k] = vecs[size_t(k * 9)];  // (column 0: the smallest)
    // Undo the normalisations: H = Tb^-1 h Ta.
    const double Ta3[9] = {Ta[0], 0, Ta[1], 0, Ta[0], Ta[2], 0, 0, 1};
    const double Tbi[9] = {1 / Tb[0], 0, -Tb[1] / Tb[0], 0, 1 / Tb[0], -Tb[2] / Tb[0], 0, 0, 1};
    double t1[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) t1[r * 3 + c] = h[r * 3] * Ta3[c] + h[r * 3 + 1] * Ta3[3 + c] + h[r * 3 + 2] * Ta3[6 + c];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) H[r * 3 + c] = Tbi[r * 3] * t1[c] + Tbi[r * 3 + 1] * t1[3 + c] + Tbi[r * 3 + 2] * t1[6 + c];
    double norm = 0;
    for (int k = 0; k < 9; ++k) norm += H[k] * H[k];
    norm = std::sqrt(norm);
    if (!(norm > 0)) return false;
    for (int k = 0; k < 9; ++k) H[k] /= norm;
    return true;
}

}  // namespace

bool calibrateFromBoards(const std::vector<std::vector<Point2>>& views, int cols, int rows, int width, int height,
                         const LensFitOptions& o, LensFit& out, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    const size_t N = size_t(cols) * size_t(rows);
    std::vector<const std::vector<Point2>*> used;
    for (const auto& v : views)
        if (v.size() == N) used.push_back(&v);
    const bool fixedIntrinsics = !o.fitCenter && !o.fitSqueeze;
    if (used.size() < (fixedIntrinsics ? 2u : 3u) || N < 4 || width < 2 || height < 2)
        return fail(fixedIntrinsics ? "Calibration needs the board seen in two frames or more"
                                    : "Calibration needs the board seen in three frames or more");
    Intrinsics in;
    in.width = width, in.height = height;
    in.unit = frameUnit(width, height);
    in.model = o.model == Distortion::None ? Distortion::Poly5 : o.model;
    in.squeeze = o.squeeze > 0 ? o.squeeze : 1;
    in.cxo = o.fitCenter ? 0 : o.centerX;
    in.cyo = o.fitCenter ? 0 : o.centerY;
    std::vector<Point2> board(N);
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i) board[size_t(j * cols + i)] = {double(i), double(j)};

    // Zhang's closed form with the centre at the frame's middle (or where given) and no skew: in calibration units
    // the projection is diag(F/s, F, 1); with the squeeze held, x times s makes it diag(F, F, 1).
    std::vector<std::array<double, 9>> Hs;
    for (const auto* v : used) {
        std::vector<Point2> img(N);
        for (size_t k = 0; k < N; ++k)
            img[k] = {((*v)[k].x - in.cx()) * in.unit * in.squeeze, ((*v)[k].y - in.cy()) * in.unit};
        std::array<double, 9> H;
        if (!homography(board, img, H.data())) return fail("The board's corners could not be fitted");
        Hs.push_back(H);
    }
    double F = 0, sx = 1;
    {
        // Rows of h1' B h2 = 0 and h1' B h1 = h2' B h2 in (B11, B22, B33).
        std::vector<double> M(9, 0.0);
        auto add = [&](const double v[3]) {
            for (int a = 0; a < 3; ++a)
                for (int c = 0; c < 3; ++c) M[size_t(a * 3 + c)] += v[a] * v[c];
        };
        for (const auto& H : Hs) {
            const double h1[3] = {H[0], H[3], H[6]}, h2[3] = {H[1], H[4], H[7]};
            const double a[3] = {h1[0] * h2[0], h1[1] * h2[1], h1[2] * h2[2]};
            const double b[3] = {h1[0] * h1[0] - h2[0] * h2[0], h1[1] * h1[1] - h2[1] * h2[1], h1[2] * h1[2] - h2[2] * h2[2]};
            add(a);
            add(b);
        }
        std::vector<double> values, vecs;
        if (o.fitSqueeze) {
            symmetricEigen(M, 3, values, vecs);
            const double B11 = vecs[0], B22 = vecs[3], B33 = vecs[6];
            // (B = diag(1/fx², 1/fy², 1) up to scale: fy = F, fx = F / sx with sx the squeeze still to add.)
            if (B11 * B33 > 0 && B22 * B33 > 0) {
                F = std::sqrt(B33 / B22);
                sx = std::sqrt(B11 / B22);
            }
        }
        if (!(F > 0)) {
            // B11 = B22 (no further squeeze).
            std::vector<double> M2 = {M[0] + 2 * M[1] + M[4], M[2] + M[5], M[2] + M[5], M[8]};
            symmetricEigen(M2, 2, values, vecs);
            const double B = vecs[0], B33 = vecs[2];
            if (B * B33 > 0) F = std::sqrt(B33 / B);
            sx = 1;
        }
        if (!(F > 0) || !std::isfinite(F)) return fail("The board needs to be seen at an angle in some frames");
    }
    in.F = F;
    in.squeeze *= sx;
    // Each view's pose from its homography.
    std::vector<double> poses;
    for (auto H : Hs) {
        // (The columns in a frame where the projection is diag(F, F, 1): the squeeze's change taken out of x.)
        for (int c = 0; c < 3; ++c) H[size_t(c)] *= sx;
        Vec3d r1{H[0] / F, H[3] / F, H[6]}, r2{H[1] / F, H[4] / F, H[7]}, t{H[2] / F, H[5] / F, H[8]};
        const double n1 = std::sqrt(r1.x * r1.x + r1.y * r1.y + r1.z * r1.z), n2 = std::sqrt(r2.x * r2.x + r2.y * r2.y + r2.z * r2.z);
        double lam = 2 / (n1 + n2);
        if (t.z * lam < 0) lam = -lam;
        r1 = {r1.x * lam, r1.y * lam, r1.z * lam};
        r2 = {r2.x * lam, r2.y * lam, r2.z * lam};
        t = {t.x * lam, t.y * lam, t.z * lam};
        // Made a rotation (Gram–Schmidt, the two columns shared alike).
        auto unitv = [](Vec3d v) {
            const double l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            return Vec3d{v.x / l, v.y / l, v.z / l};
        };
        const Vec3d a = unitv(r1), b = unitv(r2);
        const Vec3d s = unitv({a.x + b.x, a.y + b.y, a.z + b.z}), d = unitv({a.x - b.x, a.y - b.y, a.z - b.z});
        const double h = std::sqrt(0.5);
        const Vec3d c1{h * (s.x + d.x), h * (s.y + d.y), h * (s.z + d.z)}, c2{h * (s.x - d.x), h * (s.y - d.y), h * (s.z - d.z)};
        const Vec3d c3{c1.y * c2.z - c1.z * c2.y, c1.z * c2.x - c1.x * c2.z, c1.x * c2.y - c1.y * c2.x};
        const M3 R = {c1.x, c2.x, c3.x, c1.y, c2.y, c3.y, c1.z, c2.z, c3.z};
        double w[3];
        rotationVector(R, w);
        poses.insert(poses.end(), {w[0], w[1], w[2], t.x, t.y, t.z});
    }

    // Levenberg–Marquardt: first the focal length and the poses with no distortion, then everything.
    auto fit = [&](const Layout& lay) {
        Blocks b;
        b.shared = lay.count();
        b.own = 6;
        b.groups = int(used.size());
        b.residuals = [&, lay](int g, const double* shared, const double* own, std::vector<double>& r) {
            Intrinsics k = in;
            lay.set(k, shared);
            const M3 R = rotation(own);
            const auto& obs = *used[size_t(g)];
            r.resize(2 * N);
            for (size_t i = 0; i < N; ++i) {
                const Point2 p = project(k, R, own + 3, board[i].x, board[i].y);
                r[2 * i] = obs[i].x - p.x;
                r[2 * i + 1] = obs[i].y - p.y;
            }
        };
        std::vector<double> p(size_t(b.shared));
        lay.get(in, p.data());
        p.insert(p.end(), poses.begin(), poses.end());
        if (!levenbergMarquardt(b, p)) return false;
        lay.set(in, p.data());
        std::copy(p.begin() + b.shared, p.end(), poses.begin());
        return true;
    };
    Layout first;
    if (!fit(first)) return fail("The fit did not converge");
    Layout all;
    all.centre = o.fitCenter;
    all.squeeze = o.fitSqueeze;
    all.terms = o.model == Distortion::None ? 0 : termCount(in.model);
    if (all.terms > 0 || all.centre || all.squeeze)
        if (!fit(all)) return fail("The fit did not converge");
    if (!(in.F > 0) || !(in.squeeze > 0)) return fail("The fit did not converge");

    out = LensFit();
    out.model = o.model == Distortion::None ? Distortion::None : in.model;
    std::copy(in.k, in.k + 3, out.terms);
    out.focal = in.F;
    out.centerX = in.cxo, out.centerY = in.cyo;
    out.squeeze = in.squeeze;
    out.views = int(used.size());
    out.points = int(N * used.size());
    double total = 0;
    for (size_t g = 0; g < used.size(); ++g) {
        const double* own = poses.data() + 6 * g;
        const M3 R = rotation(own);
        double s = 0;
        for (size_t i = 0; i < N; ++i) {
            const Point2 p = project(in, R, own + 3, board[i].x, board[i].y);
            const double e2 = std::pow((*used[g])[i].x - p.x, 2) + std::pow((*used[g])[i].y - p.y, 2);
            s += e2;
            out.worst = std::max(out.worst, std::sqrt(e2));
        }
        total += s;
        out.viewRms.push_back(std::sqrt(s / double(N)));
    }
    out.rms = std::sqrt(total / double(out.points));
    return true;
}

namespace {

// A line's points' distances from the straight line through them (total least squares), in calibration units.
void straightness(const std::vector<Point2>& pts, std::vector<double>& out) {
    double mx = 0, my = 0;
    for (const Point2& p : pts) mx += p.x, my += p.y;
    mx /= double(pts.size()), my /= double(pts.size());
    double sxx = 0, sxy = 0, syy = 0;
    for (const Point2& p : pts) {
        const double dx = p.x - mx, dy = p.y - my;
        sxx += dx * dx, sxy += dx * dy, syy += dy * dy;
    }
    // The normal: the covariance's smaller eigenvector.
    const double tr = sxx + syy, det = sxx * syy - sxy * sxy;
    const double small = tr / 2 - std::sqrt(std::max(0.0, tr * tr / 4 - det));
    double nx = sxy, ny = small - sxx;
    if (std::hypot(nx, ny) < 1e-300) nx = small - syy, ny = sxy;
    const double l = std::hypot(nx, ny);
    if (l < 1e-300) nx = 0, ny = 1;
    else nx /= l, ny /= l;
    for (const Point2& p : pts) out.push_back((p.x - mx) * nx + (p.y - my) * ny);
}

// A recorded point to the corrected picture (calibration units from the lens's centre, the squeeze undone).
bool undistortPoint(const Intrinsics& in, Point2 p, Point2& u) {
    const double dx = (p.x - in.cx()) * in.unit * in.squeeze, dy = (p.y - in.cy()) * in.unit;
    const double rd = std::hypot(dx, dy);
    if (rd < 1e-12) {
        u = {dx, dy};
        return true;
    }
    double ru;
    if (!undistortRadius(in.model, in.k, rd, ru)) return false;
    u = {dx * ru / rd, dy * ru / rd};
    return true;
}

}  // namespace

bool calibrateFromLines(const std::vector<std::vector<Point2>>& lines, int width, int height, const LensFitOptions& o,
                        LensFit& out, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    std::vector<std::vector<Point2>> used;
    for (const auto& l : lines)
        if (l.size() >= 3) used.push_back(l);
    if (used.empty() || o.model == Distortion::None) return fail("Draw at least one line that should be straight");
    Intrinsics in;
    in.width = width, in.height = height;
    in.unit = frameUnit(width, height);
    in.model = o.model;
    in.squeeze = o.squeeze > 0 ? o.squeeze : 1;
    in.cxo = o.centerX, in.cyo = o.centerY;
    Layout lay;
    lay.F = false;
    lay.terms = termCount(in.model);
    lay.centre = o.fitCenter && used.size() >= 4;
    lay.squeeze = o.fitSqueeze && used.size() >= 4;
    // (One line: one term.)
    if (used.size() == 1) lay.terms = std::min(lay.terms, 1);
    Blocks b;
    b.shared = lay.count();
    b.own = 0;
    b.groups = int(used.size());
    b.residuals = [&](int g, const double* shared, const double*, std::vector<double>& r) {
        Intrinsics k = in;
        lay.set(k, shared);
        const auto& line = used[size_t(g)];
        std::vector<Point2> u(line.size());
        r.clear();
        // (A centre far outside the frame, or a squeeze past any lens's, is no answer.)
        bool bad = std::fabs(k.cxo) > 0.5 || std::fabs(k.cyo) > 0.5 || !(k.squeeze >= 0.25 && k.squeeze <= 4);
        for (size_t i = 0; i < line.size() && !bad; ++i) bad = !undistortPoint(k, line[i], u[i]);
        const double recordedLength = std::hypot((line.back().x - line.front().x) * k.unit * k.squeeze, (line.back().y - line.front().y) * k.unit);
        const double correctedLength = std::hypot(u.back().x - u.front().x, u.back().y - u.front().y);
        if (bad || !(correctedLength > 1e-9)) {
            r.assign(line.size(), 1e3);
            return;
        }
        straightness(u, r);
        // In pixels, at the line's own scale as recorded (so the fit cannot straighten lines by shrinking them).
        for (double& v : r) v *= recordedLength / correctedLength / k.unit;
    };
    std::vector<double> p(size_t(b.shared));
    auto solve = [&]() {
        b.groups = int(used.size());
        // The terms first (with no distortion the centre and squeeze change nothing, so they cannot start the fit),
        // then everything.
        if (lay.centre || lay.squeeze) {
            Layout terms = lay;
            terms.centre = terms.squeeze = false;
            Blocks first = b;
            first.shared = terms.count();
            first.residuals = [&, terms, full = b.residuals, lay](int g, const double* shared, const double* own, std::vector<double>& r) {
                Intrinsics k = in;
                terms.set(k, shared);
                std::vector<double> all(size_t(lay.count()));
                lay.get(k, all.data());
                full(g, all.data(), own, r);
            };
            std::vector<double> q(size_t(first.shared));
            terms.get(in, q.data());
            if (!levenbergMarquardt(first, q)) return false;
            terms.set(in, q.data());
        }
        lay.get(in, p.data());
        if (!levenbergMarquardt(b, p)) return false;
        lay.set(in, p.data());
        return true;
    };
    if (!solve()) return fail("The fit did not converge");
    {
        // Points well off their line (snapped onto something else, as where lines cross) left out, and fitted again.
        std::vector<std::vector<double>> res(used.size());
        double sum = 0;
        size_t count = 0;
        for (size_t g = 0; g < used.size(); ++g) {
            b.residuals(int(g), p.data(), nullptr, res[g]);
            for (double v : res[g]) sum += v * v, ++count;
        }
        const double limit = std::max(0.5, 3 * std::sqrt(sum / std::max<size_t>(1, count)));
        std::vector<std::vector<Point2>> kept;
        bool dropped = false;
        for (size_t g = 0; g < used.size(); ++g) {
            std::vector<Point2> line;
            for (size_t i = 0; i < used[g].size(); ++i)
                if (std::fabs(res[g][i]) <= limit) line.push_back(used[g][i]);
            dropped |= line.size() != used[g].size();
            if (line.size() >= 3) kept.push_back(std::move(line));
        }
        if (dropped && !kept.empty()) {
            used = std::move(kept);
            if (!solve()) return fail("The fit did not converge");
        }
    }
    out = LensFit();
    out.model = in.model;
    std::copy(in.k, in.k + 3, out.terms);
    out.focal = 0;
    out.centerX = in.cxo, out.centerY = in.cyo;
    out.squeeze = in.squeeze;
    out.views = int(used.size());
    double total = 0;
    std::vector<double> r;
    for (int g = 0; g < b.groups; ++g) {
        b.residuals(g, p.data(), nullptr, r);
        double s = 0;
        for (double v : r) s += v * v, out.worst = std::max(out.worst, std::fabs(v));
        total += s;
        out.points += int(r.size());
        out.viewRms.push_back(std::sqrt(s / std::max<double>(1, r.size())));
    }
    out.rms = std::sqrt(total / std::max(1, out.points));
    return true;
}

std::vector<Point2> snapLineToEdge(const GrayImage& img, const std::vector<Point2>& drawn, double reach, double step) {
    std::vector<Point2> out;
    if (drawn.size() < 2 || img.width < 3) return out;
    const GrayImage g = blurred(img, 1.0);
    step = std::max(1.0, step);
    std::vector<double> contrast;
    for (size_t s = 0; s + 1 < drawn.size(); ++s) {
        const Point2 a = drawn[s], b = drawn[s + 1];
        const double len = std::hypot(b.x - a.x, b.y - a.y);
        if (len < 1e-6) continue;
        const double tx = (b.x - a.x) / len, ty = (b.y - a.y) / len, nx = -ty, ny = tx;
        const int count = std::max(1, int(len / step));
        for (int k = 0; k <= count; ++k) {
            if (k == count && s + 2 < drawn.size()) continue;  // (the next segment starts here)
            const double d = len * k / count;
            const Point2 c{a.x + tx * d, a.y + ty * d};
            auto along = [&](double o) { return double(sampleAt(g, c.x + o * nx, c.y + o * ny)); };
            // The picture's change across the line every half pixel: the strongest is the edge.
            const int m = int(std::ceil(reach * 2));
            std::vector<double> diff(size_t(2 * m + 1));
            double strongest = 0, typical = 0;
            int at = 0;
            for (int i = -m; i <= m; ++i) {
                const double v = diff[size_t(i + m)] = along(i * 0.5 + 0.5) - along(i * 0.5 - 0.5);
                typical += std::fabs(v);
                if (std::fabs(v) > strongest) strongest = std::fabs(v), at = i;
            }
            typical /= 2 * m + 1;
            if (std::abs(at) == m || strongest < 0.02 || strongest < 2.5 * typical) continue;
            // (A peak's offset to a fraction of a step, from the parabola through it and its neighbours.)
            auto peak = [&](int i) {
                const double l = std::fabs(diff[size_t(i + m - 1)]), mid = std::fabs(diff[size_t(i + m)]), r = std::fabs(diff[size_t(i + m + 1)]);
                const double denom = l - 2 * mid + r;
                return (i + (denom < 0 ? std::clamp(0.5 * (l - r) / denom, -0.5, 0.5) : 0.0)) * 0.5;
            };
            // A thin line (a wire, a seam) has its other edge close by, the change the other way: the middle of the two.
            int other = 0;
            for (int i = std::max(-m + 1, at - 10); i <= std::min(m - 1, at + 10); ++i)
                if (i != at && std::fabs(i - at) >= 2 && diff[size_t(i + m)] * diff[size_t(at + m)] < 0 &&
                    std::fabs(diff[size_t(i + m)]) > 0.5 * strongest &&
                    std::fabs(diff[size_t(i + m)]) >= std::fabs(diff[size_t(i + m - 1)]) &&
                    std::fabs(diff[size_t(i + m)]) >= std::fabs(diff[size_t(i + m + 1)]) &&
                    (!other || std::fabs(diff[size_t(i + m)]) > std::fabs(diff[size_t(other + m)])))
                    other = i;
            if (other && std::abs(at) < m - 1 && std::abs(other) < m) {
                const double found = (peak(at) + peak(other)) / 2;
                out.push_back({c.x + found * nx, c.y + found * ny});
                contrast.push_back(strongest);
                continue;
            }
            // An edge: placed where the picture is half way between its two sides (a blurred edge's middle, unbiased
            // by how the samples fall).
            // (A step: its two sides differ by about what the change through it adds up to; not a thin line's
            // other edge missed, where the two sides are alike.)
            const double o0 = at * 0.5, lo = along(o0 - 3), hi = along(o0 + 3), half = (lo + hi) / 2;
            if (std::fabs(hi - lo) < 0.04 || std::fabs(hi - lo) < 1.5 * strongest) continue;
            double found = 1e9;
            for (int i = -15; i < 15 && found > 1e8; ++i) {
                const double p0 = o0 + i * 0.1, v0 = along(p0) - half, v1 = along(p0 + 0.1) - half;
                if ((v0 <= 0) != (v1 <= 0)) found = p0 + 0.1 * v0 / (v0 - v1);
            }
            if (found > 1e8) continue;
            out.push_back({c.x + found * nx, c.y + found * ny});
            contrast.push_back(strongest);
        }
    }
    // (Where the edge fades, as where something crosses it, its middle is not where the line is: points with much
    // less contrast than the line's usual left out.)
    if (contrast.size() >= 3) {
        std::vector<double> sorted = contrast;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const double median = sorted[sorted.size() / 2];
        std::vector<Point2> kept;
        for (size_t i = 0; i < out.size(); ++i)
            if (contrast[i] >= 0.75 * median) kept.push_back(out[i]);
        out.swap(kept);
    }
    return out;
}

namespace {

// The profile's units: halves of the camera's sensor's shorter side (taken as 3:2, as lensfun takes it, with the
// camera's crop factor), the recording as large as fits on it, as Lens Correction places a recording on a sensor.
// How many of them a half of the frame's shorter side is (the fit's units).
double sensorScale(int width, int height, double crop) {
    lens::Lens probe;
    probe.cropFactor = crop > 0 ? crop : 1;
    probe.aspectRatio = 1.5;
    lens::Settings s;
    s.distortion = s.tca = s.vignetting = false;
    s.cameraCrop = probe.cropFactor;
    const lens::Correction c(probe, s, width, height);
    return c.unit() / frameUnit(width, height);
}

}  // namespace

double fitFocalMm(const LensFit& fit, int width, int height, double crop) {
    if (!(fit.focal > 0)) return 0;
    // (Half of a 3:2 sensor's shorter side is 12 mm over its crop factor.)
    return fit.focal * sensorScale(width, height, crop) * 12 / (crop > 0 ? crop : 1);
}

lens::Lens calibratedLens(const LensFit& fit, int width, int height, const LensIdentity& id) {
    lens::Lens l;
    l.maker = id.maker.empty() ? "Calibrated" : id.maker;
    l.model = id.model.empty() ? "Lens" : id.model;
    l.names = {l.maker, l.model};
    if (!id.mount.empty()) l.mounts = {id.mount};
    l.type = lens::Projection::Rectilinear;
    // (On the camera's sensor, so its other recordings, of other shapes, are corrected with it too.)
    l.cropFactor = id.cropFactor > 0 ? id.cropFactor : 1;
    l.aspectRatio = 1.5;
    const double real = fitFocalMm(fit, width, height, l.cropFactor);
    const double focal = id.focalMm > 0 ? id.focalMm : real > 0 ? std::round(real * 10) / 10 : 50;
    l.minFocal = l.maxFocal = focal;
    l.centerX = fit.centerX, l.centerY = fit.centerY;
    l.squeeze = fit.squeeze;
    lens::Calibration c;
    c.cropFactor = l.cropFactor;
    c.aspectRatio = l.aspectRatio;
    lens::Distortion d;
    d.model = fit.model == Distortion::None ? Distortion::Poly5 : fit.model;
    d.focal = focal;
    if (real > 0) d.realFocal = real, d.realFocalMeasured = true;
    // The terms in the sensor's units (radii k times the fit's). Poly5's carry over exactly; poly3 and ptlens keep
    // a radius of 1 where it was, so theirs carry over up to a scale (the corrected picture's size, which a fit sets).
    const double k = sensorScale(width, height, l.cropFactor), *t = fit.terms;
    switch (d.model) {
        case Distortion::Poly5:
            d.terms[0] = t[0] / (k * k), d.terms[1] = t[1] / (k * k * k * k);
            break;
        case Distortion::Poly3: {
            const double a = t[0] / (k * k), scale = 1 - t[0] + a;
            d.terms[0] = scale != 0 ? a / scale : t[0];
            break;
        }
        case Distortion::PtLens: {
            const double a = t[0] / (k * k * k), b = t[1] / (k * k), cc = t[2] / k, scale = a + b + cc + 1 - t[0] - t[1] - t[2];
            if (scale != 0) d.terms[0] = a / scale, d.terms[1] = b / scale, d.terms[2] = cc / scale;
            else std::copy(t, t + 3, d.terms);
            break;
        }
        case Distortion::None: break;
    }
    c.distortion.push_back(d);
    l.calibrations.push_back(c);
    return l;
}

bool calibrateClip(const std::string& path, double from, double to, int cols, int rows, const LensFitOptions& options,
                   ClipCalibration& out, std::string* error, const std::function<void(double)>& progress,
                   const std::atomic<bool>* cancel, int maxViews) {
    out = ClipCalibration();
    VideoDecoder dec;
    if (!dec.open(path, error)) return false;
    const int W = dec.displayWidth(), H = dec.displayHeight();
    if (W < 16 || H < 16) {
        if (error) *error = "The clip has no picture";
        return false;
    }
    from = std::max(0.0, from);
    if (!(to > from)) to = dec.duration();
    if (dec.isStill()) to = from;
    const double fps = dec.fps() > 0 ? dec.fps() : 25;
    // Frames across the range (at most four times the views wanted, at most every frame).
    const int frames = dec.isStill() ? 1 : std::max(1, std::min(int((to - from) * fps), std::max(1, maxViews) * 4));
    std::vector<std::vector<Point2>> views;
    for (int f = 0; f < frames; ++f) {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return false;
        }
        const double t = frames == 1 ? from : from + (to - from) * (f + 0.5) / frames;
        if (progress) progress(0.8 * f / frames);
        Frame16Ptr fr = dec.frameAt(t, W, H, true);
        if (!fr) continue;
        ++out.framesRead;
        std::vector<Point2> corners;
        if (!findCheckerboard(toGray(*fr), cols, rows, corners)) continue;
        ++out.boardsFound;
        // (A board that has hardly moved since the last kept adds nothing.)
        if (!views.empty()) {
            double moved = 0;
            for (size_t k = 0; k < corners.size(); ++k)
                moved = std::max(moved, std::hypot(corners[k].x - views.back()[k].x, corners[k].y - views.back()[k].y));
            if (moved < 0.02 * std::min(W, H)) continue;
        }
        views.push_back(std::move(corners));
        out.times.push_back(t);
        if (int(views.size()) >= maxViews) break;
    }
    out.width = W, out.height = H;
    if (cancel && cancel->load()) {
        if (error) *error = "Cancelled";
        return false;
    }
    if (progress) progress(0.85);
    const size_t needed = options.fitCenter || options.fitSqueeze ? 3 : 2;
    if (views.size() < needed) {
        if (error)
            *error = "The " + std::to_string(cols) + " x " + std::to_string(rows) + " board's corners were found in " +
                     std::to_string(views.size()) + " frame" + (views.size() == 1 ? "" : "s") + " (" +
                     (needed == 3 ? "three" : "two") + " or more needed)";
        return false;
    }
    if (!calibrateFromBoards(views, cols, rows, W, H, options, out.fit, error)) return false;
    if (progress) progress(1);
    return true;
}

bool makeStMaps(const lens::Lens& lens, const lens::Settings& settings, int width, int height, StMaps& out,
                std::string* error) {
    out = StMaps();
    if (width < 2 || height < 2) {
        if (error) *error = "No picture size";
        return false;
    }
    lens::Settings s = settings;
    s.fit = lens::Settings::None;
    s.scale = 1;
    s.tca = s.vignetting = false;
    const lens::Correction c(lens, s, width, height);
    // The overscan: what the corrected picture of the whole recorded frame reaches (the frame's edge, corrected).
    double x0 = 0, y0 = 0, x1 = width, y1 = height;
    const int steps = 512;
    for (int i = 0; i <= steps; ++i)
        for (int side = 0; side < 4; ++side) {
            const double f = double(i) / steps;
            const double x = side == 0 ? f * width : side == 1 ? f * width : side == 2 ? 0 : width;
            const double y = side == 0 ? 0 : side == 1 ? height : f * height;
            double cx, cy;
            if (!c.corrected(x, y, cx, cy)) continue;
            x0 = std::min(x0, cx), y0 = std::min(y0, cy), x1 = std::max(x1, cx), y1 = std::max(y1, cy);
        }
    // (Equal on both sides, so the recorded frame stays in the middle; at most half as large again each way.)
    const int ox = std::min(width / 4, int(std::ceil(std::max(-x0, x1 - width) - 1e-6))),
              oy = std::min(height / 4, int(std::ceil(std::max(-y0, y1 - height) - 1e-6)));
    out.width = width, out.height = height;
    out.overscanWidth = width + 2 * std::max(0, ox);
    out.overscanHeight = height + 2 * std::max(0, oy);
    const int OW = out.overscanWidth, OH = out.overscanHeight, px = (OW - width) / 2, py = (OH - height) / 2;
    out.undistort.assign(size_t(OW) * OH * 3, 0.f);
    out.redistort.assign(size_t(width) * height * 3, 0.f);
    for (int y = 0; y < OH; ++y)
        for (int x = 0; x < OW; ++x) {
            double at[6];
            c.source(x + 0.5 - px, y + 0.5 - py, at);
            float* p = &out.undistort[(size_t(y) * OW + x) * 3];
            p[0] = float(at[2] / width);
            p[1] = float(1 - at[3] / height);
        }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            double cx = x + 0.5, cy = y + 0.5;
            if (!c.corrected(x + 0.5, y + 0.5, cx, cy)) cx = x + 0.5, cy = y + 0.5;
            float* p = &out.redistort[(size_t(y) * width + x) * 3];
            p[0] = float((cx + px) / OW);
            p[1] = float(1 - (cy + py) / OH);
        }
    return true;
}

bool writeStMaps(const StMaps& m, const std::string& undistortFile, const std::string& redistortFile, std::string* error) {
    if (!exrAvailable()) {
        if (error) *error = "EXR writing is not available in this build";
        return false;
    }
    const std::map<std::string, std::string> attrs = {
        {"montage:overscan", std::to_string(m.overscanWidth) + "x" + std::to_string(m.overscanHeight)},
        {"montage:format", std::to_string(m.width) + "x" + std::to_string(m.height)}};
    auto layer = [](const std::vector<float>& v) {
        ExrOutLayer l;
        l.channels = {"R", "G", "B"};
        l.data = v;
        l.fullFloat = true;
        return std::vector<ExrOutLayer>{l};
    };
    if (!undistortFile.empty() && !writeExr(undistortFile, m.overscanWidth, m.overscanHeight, layer(m.undistort), "zip", attrs, error))
        return false;
    if (!redistortFile.empty() && !writeExr(redistortFile, m.width, m.height, layer(m.redistort), "zip", attrs, error))
        return false;
    return true;
}

bool stMapLookup(const std::vector<float>& map, int width, int height, double x, double y, int sourceWidth,
                 int sourceHeight, double& sx, double& sy) {
    if (width < 1 || height < 1 || map.size() < size_t(width) * height * 3) return false;
    const double fx = std::clamp(x - 0.5, 0.0, double(width - 1)), fy = std::clamp(y - 0.5, 0.0, double(height - 1));
    const int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    const double ax = fx - x0, ay = fy - y0;
    auto at = [&](int xx, int yy, int c) { return double(map[(size_t(yy) * width + xx) * 3 + size_t(c)]); };
    double v[2];
    for (int c = 0; c < 2; ++c)
        v[c] = (at(x0, y0, c) * (1 - ax) + at(x1, y0, c) * ax) * (1 - ay) + (at(x0, y1, c) * (1 - ax) + at(x1, y1, c) * ax) * ay;
    sx = v[0] * sourceWidth;
    sy = (1 - v[1]) * sourceHeight;
    return true;
}

}  // namespace montage
