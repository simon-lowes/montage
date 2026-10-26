// Montage — a LUT rebuilt as an editable grade. See LutToGrade.h.
#include "LutToGrade.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numeric>

#include "core/Effects.h"
#include "core/GradeVersions.h"
#include "render/ColorSpace.h"
#include "render/LutExport.h"

namespace montage {

namespace {

constexpr int kFit = 13;       // the lattice the controls are fitted on
constexpr int kCheck = 17;     // and measured on
constexpr int kResidual = 17;  // the residual LUT's size
constexpr int kDense = 33;     // the colours the residual is solved from
constexpr double kHuber = 0.02;

// Every colour of an n³ lattice over 0..1, red varying fastest (as Lut3D's data), one a pixel.
Image latticeOf(int n) {
    Image img(n * n, n);
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                float* px = img.at(r + g * n, b);
                px[0] = float(r) / float(n - 1), px[1] = float(g) / float(n - 1), px[2] = float(b) / float(n - 1), px[3] = 1;
            }
    return img;
}

Image through(const std::function<void(float[3])>& lut, const Image& in) {
    Image out = in;
    for (size_t i = 0; i < out.px.size(); i += 4) lut(&out.px[i]);
    return out;
}

Image runChain(const std::vector<Effect>& fx, const Image& in) {
    Image out = in;
    for (const Effect& e : fx) applyVideoEffect(e, 0, out, 1.0);
    return out;
}

// CIE L*a*b* of a picture in `space`, seen through Rec.709.
std::vector<std::array<double, 3>> labOf(const Image& img, const ColorSpace& space) {
    Image v = img;
    if (space.id != rec709Space().id) convertColor(v, space, rec709Space());
    std::vector<std::array<double, 3>> out(size_t(v.width) * size_t(v.height));
    for (size_t i = 0; i < out.size(); ++i) {
        float c[3] = {v.px[i * 4], v.px[i * 4 + 1], v.px[i * 4 + 2]};
        for (float& x : c) x = std::isfinite(x) ? x : 0.0f;
        double l[3];
        displayLight(rec709Space(), c, l);
        linearToLab(Primaries::Bt709, l, out[i].data());
    }
    return out;
}

// Skin tones (in the LUT's result, through Rec.709) count three times.
std::vector<double> skinWeights(const Image& target, const ColorSpace& space) {
    Image v = target;
    if (space.id != rec709Space().id) convertColor(v, space, rec709Space());
    std::vector<double> w(size_t(v.width) * size_t(v.height), 1.0);
    for (size_t i = 0; i < w.size(); ++i) {
        const float r = v.px[i * 4], g = v.px[i * 4 + 1], b = v.px[i * 4 + 2];
        const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
        if (mx < 0.15f || r != mx || mx - mn < 1e-4f) continue;
        const float s = (mx - mn) / mx, h = (g - b) / (mx - mn) / 6;  // (red the brightest: hue from -1/6 to 1/6)
        if (h > 0.02f && h < 0.14f && s > 0.15f && s < 0.65f) w[i] = 3;
    }
    return w;
}

struct Measure {
    double mean = 0, max = 0;
};
Measure deltaE(const std::vector<std::array<double, 3>>& a, const std::vector<std::array<double, 3>>& b, const std::vector<double>& w) {
    Measure m;
    double sum = 0, total = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double e = deltaE2000(a[i].data(), b[i].data());
        sum += w[i] * e, total += w[i];
        m.max = std::max(m.max, e);
    }
    m.mean = total > 0 ? sum / total : 0;
    return m;
}

// ---- The stages -----------------------------------------------------------------------------------------------------

struct Stage {
    std::string type;
    std::vector<double> init, lo, hi;
    std::function<Effect(const double*)> make;
};

std::string points(const double* y, int n, bool cyclic, bool monotone) {
    std::string s;
    double last = -1e9;
    for (int k = 0; k < n; ++k) {
        const double x = cyclic ? double(k) / n : double(k) / (n - 1);
        double v = y[k];
        if (monotone) v = std::max(v, last), last = v;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s%.6g,%.6g", s.empty() ? "" : " ", x, v);
        s += buf;
    }
    return s;
}

constexpr int kCurvePoints = 7, kHuePoints = 8;

std::vector<Stage> stages() {
    std::vector<Stage> out;
    {
        Stage cdl{"cdl", {1, 1, 1, 0, 0, 0, 1, 1, 1, 1}, {0.05, 0.05, 0.05, -1, -1, -1, 0.1, 0.1, 0.1, 0}, {4, 4, 4, 1, 1, 1, 4, 4, 4, 4}, {}};
        cdl.make = [](const double* v) {
            Effect e = makeEffect("cdl", 0);
            const char* ch[3] = {"r", "g", "b"};
            for (int i = 0; i < 3; ++i) {
                e.params[std::string("slope.") + ch[i]] = v[i];
                e.params[std::string("offset.") + ch[i]] = v[3 + i];
                e.params[std::string("power.") + ch[i]] = v[6 + i];
            }
            e.params["saturation"] = v[9];
            return e;
        };
        out.push_back(cdl);
    }
    {
        Stage mixer{"rgb_mixer", {1, 0, 0, 0, 1, 0, 0, 0, 1}, std::vector<double>(9, -2), std::vector<double>(9, 3), {}};
        mixer.make = [](const double* v) {
            Effect e = makeEffect("rgb_mixer", 0);
            for (int o = 0; o < 3; ++o)
                for (int i = 0; i < 3; ++i) e.params[std::string(1, "rgb"[o]) + "." + "rgb"[i]] = v[o * 3 + i];
            return e;
        };
        out.push_back(mixer);
    }
    {
        Stage curves{"curves", {}, std::vector<double>(3 * kCurvePoints, 0), std::vector<double>(3 * kCurvePoints, 1), {}};
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < kCurvePoints; ++k) curves.init.push_back(double(k) / (kCurvePoints - 1));
        curves.make = [](const double* v) {
            Effect e = makeEffect("curves", 0);
            e.strings["red"] = points(v, kCurvePoints, false, true);
            e.strings["green"] = points(v + kCurvePoints, kCurvePoints, false, true);
            e.strings["blue"] = points(v + 2 * kCurvePoints, kCurvePoints, false, true);
            return e;
        };
        out.push_back(curves);
    }
    {
        Stage hue{"hue_curves", std::vector<double>(2 * kHuePoints, 0.5), {}, {}, {}};
        for (int k = 0; k < kHuePoints; ++k) hue.lo.push_back(0.35), hue.hi.push_back(0.65);  // (hue turned up to 54°)
        for (int k = 0; k < kHuePoints; ++k) hue.lo.push_back(0.2), hue.hi.push_back(1.0);   // (saturation 0.4 to 2 times)
        hue.make = [](const double* v) {
            Effect e = makeEffect("hue_curves", 0);
            e.strings["hue_hue"] = points(v, kHuePoints, true, false);
            e.strings["hue_sat"] = points(v + kHuePoints, kHuePoints, true, false);
            return e;
        };
        out.push_back(hue);
    }
    return out;
}

// ---- Fitting --------------------------------------------------------------------------------------------------------

// Solves a x = b (n×n, row-major) by Gaussian elimination with partial pivoting; false when singular.
bool solve(std::vector<double> a, std::vector<double> b, int n, std::vector<double>& x) {
    for (int c = 0; c < n; ++c) {
        int piv = c;
        for (int r = c + 1; r < n; ++r)
            if (std::fabs(a[size_t(r * n + c)]) > std::fabs(a[size_t(piv * n + c)])) piv = r;
        if (std::fabs(a[size_t(piv * n + c)]) < 1e-300) return false;
        if (piv != c) {
            for (int k = 0; k < n; ++k) std::swap(a[size_t(c * n + k)], a[size_t(piv * n + k)]);
            std::swap(b[size_t(c)], b[size_t(piv)]);
        }
        for (int r = c + 1; r < n; ++r) {
            const double f = a[size_t(r * n + c)] / a[size_t(c * n + c)];
            if (f == 0) continue;
            for (int k = c; k < n; ++k) a[size_t(r * n + k)] -= f * a[size_t(c * n + k)];
            b[size_t(r)] -= f * b[size_t(c)];
        }
    }
    x.assign(size_t(n), 0);
    for (int r = n - 1; r >= 0; --r) {
        double s = b[size_t(r)];
        for (int k = r + 1; k < n; ++k) s -= a[size_t(r * n + k)] * x[size_t(k)];
        x[size_t(r)] = s / a[size_t(r * n + r)];
    }
    return true;
}

struct Problem {
    const Image* in = nullptr;
    std::vector<float> target;  // 3 a colour
    std::vector<double> weight;
    std::function<std::vector<Effect>(const std::vector<double>&)> make;
    const std::atomic<bool>* cancel = nullptr;

    void residuals(const std::vector<double>& x, std::vector<double>& r) const {
        const Image out = runChain(make(x), *in);
        r.resize(target.size());
        for (size_t i = 0; i < target.size(); ++i) {
            const float v = out.px[i / 3 * 4 + i % 3];
            r[i] = std::isfinite(v) ? double(v) - target[i] : 1.0;
        }
    }
    double cost(const std::vector<double>& r) const {
        double c = 0;
        for (size_t i = 0; i < r.size(); ++i) {
            const double a = std::fabs(r[i]);
            c += weight[i / 3] * (a <= kHuber ? 0.5 * a * a : kHuber * (a - 0.5 * kHuber));
        }
        return c;
    }
};

// Levenberg–Marquardt with Huber weights (iteratively reweighted) over the parameters marked free, kept in bounds.
double levenbergMarquardt(const Problem& pb, std::vector<double>& x, const std::vector<double>& lo, const std::vector<double>& hi,
                          const std::vector<int>& free, int iterations) {
    std::vector<double> r, rn, rj;
    pb.residuals(x, r);
    double cost = pb.cost(r), mu = 1e-3;
    const int n = int(free.size());
    const size_t m = r.size();
    std::vector<double> jac(m * size_t(n));
    for (int it = 0; it < iterations; ++it) {
        if (pb.cancel && pb.cancel->load()) break;
        for (int j = 0; j < n; ++j) {
            const size_t k = size_t(free[size_t(j)]);
            std::vector<double> xs = x;
            double h = 1e-4 * std::max(1.0, std::fabs(x[k]));
            if (xs[k] + h > hi[k]) h = -h;
            xs[k] += h;
            pb.residuals(xs, rj);
            for (size_t i = 0; i < m; ++i) jac[i * size_t(n) + size_t(j)] = (rj[i] - r[i]) / h;
        }
        std::vector<double> a(size_t(n) * size_t(n), 0), g(size_t(n), 0);
        for (size_t i = 0; i < m; ++i) {
            const double ar = std::fabs(r[i]);
            const double w = pb.weight[i / 3] * (ar <= kHuber ? 1.0 : kHuber / ar);
            const double* ji = &jac[i * size_t(n)];
            for (int p = 0; p < n; ++p) {
                if (ji[p] == 0) continue;
                g[size_t(p)] += w * ji[p] * r[i];
                for (int q = p; q < n; ++q) a[size_t(p * n + q)] += w * ji[p] * ji[q];
            }
        }
        for (int p = 0; p < n; ++p)
            for (int q = 0; q < p; ++q) a[size_t(p * n + q)] = a[size_t(q * n + p)];
        bool improved = false;
        double next = cost;
        for (int tries = 0; tries < 8 && !improved; ++tries) {
            std::vector<double> damped = a, step, neg(static_cast<size_t>(n));
            for (int p = 0; p < n; ++p) damped[size_t(p * n + p)] += mu * std::max(a[size_t(p * n + p)], 1e-9) + 1e-12;
            for (int p = 0; p < n; ++p) neg[size_t(p)] = -g[size_t(p)];
            if (!solve(damped, neg, n, step)) {
                mu *= 10;
                continue;
            }
            std::vector<double> xn = x;
            for (int p = 0; p < n; ++p) {
                const size_t k = size_t(free[size_t(p)]);
                xn[k] = std::clamp(x[k] + step[size_t(p)], lo[k], hi[k]);
            }
            pb.residuals(xn, rn);
            next = pb.cost(rn);
            if (next < cost) {
                improved = true;
                x = xn;
                r = rn;
                mu = std::max(mu / 3, 1e-9);
            } else {
                mu *= 5;
            }
        }
        if (!improved) break;
        const double gain = cost - next;
        cost = next;
        if (gain < 1e-7 * std::max(cost, 1e-12)) break;
    }
    return cost;
}

// ---- The residual -----------------------------------------------------------------------------------------------------

// A colour's place in an n³ LUT as Lut3D::apply interpolates it (tetrahedrally): four lattice points and their weights.
void tetrahedron(const float v[3], int n, size_t idx[4], double w[4]) {
    double u[3];
    for (int i = 0; i < 3; ++i) u[i] = std::clamp(double(v[i]), 0.0, 1.0) * (n - 1);
    const int x0 = std::min(n - 2, int(u[0])), y0 = std::min(n - 2, int(u[1])), z0 = std::min(n - 2, int(u[2]));
    const double fx = u[0] - x0, fy = u[1] - y0, fz = u[2] - z0;
    auto at = [&](int x, int y, int z) { return (size_t(z) * size_t(n) + size_t(y)) * size_t(n) + size_t(x); };
    idx[0] = at(x0, y0, z0);
    idx[3] = at(x0 + 1, y0 + 1, z0 + 1);
    if (fx > fy) {
        if (fy > fz) idx[1] = at(x0 + 1, y0, z0), idx[2] = at(x0 + 1, y0 + 1, z0), w[0] = 1 - fx, w[1] = fx - fy, w[2] = fy - fz, w[3] = fz;
        else if (fx > fz) idx[1] = at(x0 + 1, y0, z0), idx[2] = at(x0 + 1, y0, z0 + 1), w[0] = 1 - fx, w[1] = fx - fz, w[2] = fz - fy, w[3] = fy;
        else idx[1] = at(x0, y0, z0 + 1), idx[2] = at(x0 + 1, y0, z0 + 1), w[0] = 1 - fz, w[1] = fz - fx, w[2] = fx - fy, w[3] = fy;
    } else {
        if (fz > fy) idx[1] = at(x0, y0, z0 + 1), idx[2] = at(x0, y0 + 1, z0 + 1), w[0] = 1 - fz, w[1] = fz - fy, w[2] = fy - fx, w[3] = fx;
        else if (fz > fx) idx[1] = at(x0, y0 + 1, z0), idx[2] = at(x0, y0 + 1, z0 + 1), w[0] = 1 - fy, w[1] = fy - fz, w[2] = fz - fx, w[3] = fx;
        else idx[1] = at(x0, y0 + 1, z0), idx[2] = at(x0 + 1, y0 + 1, z0), w[0] = 1 - fy, w[1] = fy - fx, w[2] = fx - fz, w[3] = fz;
    }
}

// The LUT that takes the stages' results `got` to the wanted `want` (both 3 a colour): its offsets from the identity at
// the lattice points, by least squares (conjugate gradients), kept smooth where no colour lands.
Lut3D solveResidual(const std::vector<float>& got, const std::vector<float>& want, int n) {
    const size_t nodes = size_t(n) * size_t(n) * size_t(n), samples = got.size() / 3;
    std::vector<std::array<size_t, 4>> idx(samples);
    std::vector<std::array<double, 4>> w(samples);
    for (size_t i = 0; i < samples; ++i) tetrahedron(&got[i * 3], n, idx[i].data(), w[i].data());
    constexpr double kSmooth = 1e-3, kSmall = 1e-6;
    Lut3D lut;
    lut.size = n;
    lut.data.resize(nodes * 3);
    auto neighbours = [&](size_t k, auto&& fn) {
        const int x = int(k % size_t(n)), y = int(k / size_t(n) % size_t(n)), z = int(k / (size_t(n) * size_t(n)));
        if (x > 0) fn(k - 1);
        if (x + 1 < n) fn(k + 1);
        if (y > 0) fn(k - size_t(n));
        if (y + 1 < n) fn(k + size_t(n));
        if (z > 0) fn(k - size_t(n) * size_t(n));
        if (z + 1 < n) fn(k + size_t(n) * size_t(n));
    };
    auto multiply = [&](const std::vector<double>& d, std::vector<double>& out) {
        out.assign(nodes, 0);
        for (size_t i = 0; i < samples; ++i) {
            double v = 0;
            for (int c = 0; c < 4; ++c) v += w[i][size_t(c)] * d[idx[i][size_t(c)]];
            for (int c = 0; c < 4; ++c) out[idx[i][size_t(c)]] += w[i][size_t(c)] * v;
        }
        for (size_t k = 0; k < nodes; ++k) {
            double lap = 0;
            neighbours(k, [&](size_t j) { lap += d[k] - d[j]; });
            out[k] += kSmooth * lap + kSmall * d[k];
        }
    };
    for (int ch = 0; ch < 3; ++ch) {
        std::vector<double> b(nodes, 0);
        for (size_t i = 0; i < samples; ++i) {
            const double d = double(want[i * 3 + size_t(ch)]) - std::clamp(double(got[i * 3 + size_t(ch)]), 0.0, 1.0);
            for (int c = 0; c < 4; ++c) b[idx[i][size_t(c)]] += w[i][size_t(c)] * d;
        }
        std::vector<double> x(nodes, 0), r = b, p = b, ap;
        double rr = std::inner_product(r.begin(), r.end(), r.begin(), 0.0);
        const double stop = 1e-20 * std::max(rr, 1e-30);
        for (int it = 0; it < 500 && rr > stop; ++it) {
            multiply(p, ap);
            const double pap = std::inner_product(p.begin(), p.end(), ap.begin(), 0.0);
            if (pap <= 0) break;
            const double alpha = rr / pap;
            for (size_t k = 0; k < nodes; ++k) x[k] += alpha * p[k], r[k] -= alpha * ap[k];
            const double next = std::inner_product(r.begin(), r.end(), r.begin(), 0.0);
            for (size_t k = 0; k < nodes; ++k) p[k] = r[k] + next / rr * p[k];
            rr = next;
        }
        for (size_t k = 0; k < nodes; ++k) {
            const int at = ch == 0 ? int(k % size_t(n)) : ch == 1 ? int(k / size_t(n) % size_t(n)) : int(k / (size_t(n) * size_t(n)));
            lut.data[k * 3 + size_t(ch)] = float(double(at) / (n - 1) + x[k]);
        }
    }
    return lut;
}

// Where the LUT holds a range flat at its top or bottom, along the grey axis and each primary: the input it starts at.
std::string findClip(const std::function<void(float[3])>& lut) {
    constexpr int kSteps = 64;
    const char* axes[4] = {"grey", "pure red", "pure green", "pure blue"};
    std::string note;
    for (int a = 0; a < 4; ++a) {
        std::vector<std::array<float, 3>> out(kSteps + 1);
        for (int k = 0; k <= kSteps; ++k) {
            const float x = float(k) / kSteps;
            float v[3] = {a == 0 || a == 1 ? x : 0.0f, a == 0 || a == 2 ? x : 0.0f, a == 0 || a == 3 ? x : 0.0f};
            lut(v);
            out[size_t(k)] = {v[0], v[1], v[2]};
        }
        // The top of a channel: flat at its maximum, at 0.999 or more, from well before the end.
        std::vector<std::string> flat;
        int first = kSteps;
        for (int c = 0; c < 3; ++c) {
            int from = kSteps;
            while (from > 0 && std::fabs(out[size_t(from - 1)][size_t(c)] - out[size_t(kSteps)][size_t(c)]) < 1e-5f) --from;
            if (out[size_t(kSteps)][size_t(c)] >= 0.999f && from <= kSteps * 9 / 10) {
                flat.push_back(c == 0 ? "red" : c == 1 ? "green" : "blue");
                first = std::min(first, from);
            }
        }
        if (!flat.empty()) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%s%s from %d%% %s", flat.size() == 3 ? "every channel clips" : flat[0].c_str(),
                          flat.size() == 3 ? "" : flat.size() == 2 ? (" and " + flat[1] + " clip").c_str() : " clips",
                          int(std::lround(100.0 * first / kSteps)), axes[a]);
            note = buf;
        }
        if (!note.empty()) break;
    }
    return note;
}

}  // namespace

bool fitLutGrade(const std::function<void(float[3])>& lut, const ColorSpace& space, LutGrade& out, std::string* error,
                 const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    out = LutGrade{};
    const Image fitIn = latticeOf(kFit), checkIn = latticeOf(kCheck), denseIn = latticeOf(kDense);
    const Image fitWant = through(lut, fitIn), checkWant = through(lut, checkIn), denseWant = through(lut, denseIn);
    const auto checkLab = labOf(checkWant, space);
    const std::vector<double> checkWeight = skinWeights(checkWant, space);
    Problem pb;
    pb.in = &fitIn;
    pb.cancel = cancel;
    pb.weight = skinWeights(fitWant, space);
    pb.target.reserve(size_t(fitWant.width) * size_t(fitWant.height) * 3);
    for (size_t i = 0; i < fitWant.px.size(); i += 4)
        for (int c = 0; c < 3; ++c) pb.target.push_back(std::isfinite(fitWant.px[i + size_t(c)]) ? fitWant.px[i + size_t(c)] : 0.0f);

    const std::vector<Stage> all = stages();
    // The parameters of every stage in one vector; a stage's slice is [offset[i], offset[i] + size).
    std::vector<size_t> offset;
    std::vector<double> x, lo, hi;
    for (const Stage& st : all) {
        offset.push_back(x.size());
        x.insert(x.end(), st.init.begin(), st.init.end());
        lo.insert(lo.end(), st.lo.begin(), st.lo.end());
        hi.insert(hi.end(), st.hi.begin(), st.hi.end());
    }
    std::vector<int> kept;
    auto chain = [&](const std::vector<int>& which, const std::vector<double>& v) {
        std::vector<Effect> fx;
        for (int i : which) fx.push_back(all[size_t(i)].make(&v[offset[size_t(i)]]));
        return fx;
    };
    Measure best = deltaE(labOf(checkIn, space), checkLab, checkWeight);
    for (size_t i = 0; i < all.size(); ++i) {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return false;
        }
        std::vector<int> trial = kept;
        trial.push_back(int(i));
        pb.make = [&](const std::vector<double>& v) { return chain(trial, v); };
        std::vector<double> tx = x;
        // The stage alone first, the others as they are; then all of them together.
        std::vector<int> own, every;
        for (size_t k = 0; k < all[i].init.size(); ++k) own.push_back(int(offset[i] + k));
        for (int s : trial)
            for (size_t k = 0; k < all[size_t(s)].init.size(); ++k) every.push_back(int(offset[size_t(s)] + k));
        levenbergMarquardt(pb, tx, lo, hi, own, 30);
        if (trial.size() > 1) levenbergMarquardt(pb, tx, lo, hi, every, 15);
        const Measure m = deltaE(labOf(runChain(chain(trial, tx), checkIn), space), checkLab, checkWeight);
        if (m.mean < best.mean - std::max(0.02, 0.03 * best.mean)) {
            kept = trial;
            x = tx;
            best = m;
        }
        if (progress) progress(double(i + 1) / double(all.size() + 1));
    }
    out.effects = chain(kept, x);
    for (int i : kept) out.stages.push_back(all[size_t(i)].type);
    out.meanDeltaE = best.mean;
    out.maxDeltaE = best.max;

    // The residual: from what the stages make of every colour to what the LUT does.
    const Image denseGot = runChain(out.effects, denseIn);
    std::vector<float> got, want;
    for (size_t i = 0; i < denseGot.px.size(); i += 4)
        for (int c = 0; c < 3; ++c) {
            const float g = denseGot.px[i + size_t(c)], wv = denseWant.px[i + size_t(c)];
            got.push_back(std::isfinite(g) ? g : 0.0f);
            want.push_back(std::isfinite(wv) ? wv : 0.0f);
        }
    out.residual = solveResidual(got, want, kResidual);
    Image checked = runChain(out.effects, checkIn);
    for (size_t i = 0; i < checked.px.size(); i += 4) out.residual.apply(checked.px[i], checked.px[i + 1], checked.px[i + 2]);
    const Measure after = deltaE(labOf(checked, space), checkLab, checkWeight);
    out.residualMeanDeltaE = after.mean;
    out.residualMaxDeltaE = after.max;
    out.clipNote = findClip(lut);
    out.clips = !out.clipNote.empty();
    if (progress) progress(1);
    return true;
}

namespace edit {

namespace {
// The clip's LUT effect to rebuild, refused when it cannot be.
const Effect* lutEffectOf(const Clip& c, Id lutEffect, std::string* error) {
    auto it = std::find_if(c.effects.begin(), c.effects.end(), [&](const Effect& e) { return e.type == "lut" && (lutEffect == 0 || e.id == lutEffect); });
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return nullptr;
    };
    if (it == c.effects.end()) return fail(lutEffect ? "No such LUT effect on the clip" : "The clip has no LUT effect");
    if (!it->enabled) return fail("The LUT effect is switched off");
    if (hasMask(*it, 0)) return fail("The LUT effect is masked (the rebuilt grade would not be)");
    if (it->p("strength", 0, 100) < 99.95) return fail("Set the LUT's strength to 100% first (a part-strength LUT is a mix)");
    if (it->params.count("strength") && it->params.at("strength").varies()) return fail("The LUT's strength is animated");
    if (it->s("path").empty()) return fail("The LUT effect has no file");
    return &*it;
}
}  // namespace

bool lutGradeSource(const Sequence& s, Id clipId, Id lutEffect, std::function<void(float[3])>& lut, const ColorSpace*& space, std::string* error,
                    Id* resolved) {
    const WorkingSpaceScope scope(&sequenceColorSpace(s));  // (what "Apply In" names is told apart from it)
    const Clip* c = clipById(s, clipId);
    if (!c) {
        if (error) *error = "No such clip";
        return false;
    }
    const Effect* e = lutEffectOf(*c, lutEffect, error);
    if (!e) return false;
    lut = lutTransform(e->s("path"), error);
    if (!lut) return false;
    const ColorSpace* other = effectApplySpace(*e);
    space = other ? other : &sequenceColorSpace(s);
    if (resolved) *resolved = e->id;
    return true;
}

Result lutToGrade(Project& p, Sequence& s, Id clipId, Id lutEffect, const std::string& residualPath, LutGrade* fitOut,
                  const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    std::function<void(float[3])> fn;
    const ColorSpace* space = nullptr;
    std::string err;
    Id resolved = 0;
    if (!lutGradeSource(s, clipId, lutEffect, fn, space, &err, &resolved)) return Result::fail(err);
    const WorkingSpaceScope scope(&sequenceColorSpace(s));
    LutGrade fit;
    if (!fitLutGrade(fn, *space, fit, &err, progress, cancel)) return Result::fail(err);
    Result r = applyLutGrade(p, s, clipId, resolved, residualPath, fit);
    if (r.ok && fitOut) *fitOut = std::move(fit);
    return r;
}

Result applyLutGrade(Project& p, Sequence& s, Id clipId, Id lutEffect, const std::string& residualPath, const LutGrade& fit) {
    Clip* c = clipById(s, clipId);
    if (!c) return Result::fail("No such clip");
    std::string err;
    const Effect* found = lutEffectOf(*c, lutEffect, &err);
    if (!found) return Result::fail(err);
    const auto it = c->effects.begin() + (found - c->effects.data());
    const std::string path = found->s("path");
    const ColorSpace& working = sequenceColorSpace(s);
    const WorkingSpaceScope scope(&working);
    const ColorSpace* other = effectApplySpace(*found);
    // The residual is a file of its own: never the LUT's, nor one another effect of the project reads.
    if (!residualPath.empty()) {
        const auto same = [&](const std::string& a) {
            std::error_code ec;
            return a == residualPath || std::filesystem::equivalent(a, residualPath, ec);
        };
        bool used = false;
        auto check = [&](const Sequence& q) {
            for (const auto* tracks : {&q.videoTracks, &q.audioTracks})
                for (const Track& t : *tracks)
                    for (const Clip& k : t.clips) {
                        auto look = [&](const std::vector<Effect>& fx) {
                            for (const Effect& e : fx) used = used || (!e.s("path").empty() && same(e.s("path")));
                        };
                        look(k.effects);
                        for (const GradeVersion& v : k.gradeVersions) look(v.effects);
                    }
        };
        for (const Sequence& q : p.sequences) check(q);
        if (&s < p.sequences.data() || &s >= p.sequences.data() + p.sequences.size()) check(s);
        if (used || same(path)) return Result::fail("Choose another file for the residual LUT: that one is in use");
    }
    // In its place: the stages (and the residual) between transforms into and out of the LUT's own space.
    std::vector<Effect> fx;
    auto transform = [&](const ColorSpace& from, const ColorSpace& to) {
        Effect e = makeEffect("color_space_transform", 0);
        e.strings["from"] = from.label;
        e.strings["to"] = to.label;
        e.strings["rendering"] = "None";
        return e;
    };
    if (other) fx.push_back(transform(working, *other));
    fx.insert(fx.end(), fit.effects.begin(), fit.effects.end());
    float most = 0;
    for (size_t k = 0; k < fit.residual.data.size(); ++k) {
        const size_t node = k / 3, n = size_t(fit.residual.size);
        const size_t at = k % 3 == 0 ? node % n : k % 3 == 1 ? node / n % n : node / (n * n);
        most = std::max(most, std::fabs(fit.residual.data[k] - float(at) / float(n - 1)));
    }
    const bool residual = !residualPath.empty() && most > 5e-4f;  // (written once the rest of the edit has been made)
    if (residual) {
        Effect r = makeEffect("lut", 0);
        r.strings["path"] = residualPath;
        fx.push_back(r);
    }
    if (other) fx.push_back(transform(*other, working));
    // The grade as it was kept as a version; the new one in its place.
    int ordinal = 0;
    for (auto e = c->effects.begin(); e != it; ++e) ordinal += e->type == "lut" && e->s("path") == path;
    const bool first = c->gradeVersions.empty();
    Result r = addGradeVersion(p, s, clipId, "Rebuilt from LUT");
    if (!r.ok) return r;
    c = clipById(s, clipId);
    if (first) renameGradeVersion(s, clipId, 0, "With LUT");
    int seen = 0;
    auto copy = c->effects.end();
    for (auto e = c->effects.begin(); e != c->effects.end(); ++e)
        if (e->type == "lut" && e->s("path") == path && seen++ == ordinal) {
            copy = e;
            break;
        }
    if (copy == c->effects.end()) return Result::fail("The LUT effect was not found in the new version");
    const int node = copy->node;
    for (Effect& e : fx) e.id = p.newId(), e.node = node;
    const auto pos = c->effects.erase(copy);
    c->effects.insert(pos, fx.begin(), fx.end());
    if (residual) {
        if (!writeCubeLut(fit.residual, residualPath, "Residual of " + std::filesystem::path(path).filename().string(), &err)) return Result::fail(err);
        forgetCubeLut(residualPath);
    }
    return {};
}

}  // namespace edit

}  // namespace montage
