#include "Crosstalk.h"

#include <algorithm>
#include <cmath>
#include <sstream>

#include "core/EditOps.h"
#include "core/Effects.h"
#include "render/Processing.h"

namespace montage {

namespace {

constexpr double kIdentity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
const char* const kPrimaryNames[3] = {"red", "green", "blue"};

void mul3(const double a[9], const double b[9], double out[9]) {
    double r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    std::copy(r, r + 9, out);
}
void apply3(const double m[9], const double in[3], double out[3]) {
    const double r = m[0] * in[0] + m[1] * in[1] + m[2] * in[2], g = m[3] * in[0] + m[4] * in[1] + m[5] * in[2],
                 b = m[6] * in[0] + m[7] * in[1] + m[8] * in[2];
    out[0] = r, out[1] = g, out[2] = b;
}
double det3(const double m[9]) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
}
// Each row divided by its sum (where that is too small, by a floor), so (1, 1, 1) goes to itself.
void rowsToOne(double m[9]) {
    for (int r = 0; r < 3; ++r) {
        const double sum = m[r * 3] + m[r * 3 + 1] + m[r * 3 + 2];
        const double d = std::fabs(sum) < 0.05 ? (sum < 0 ? -0.05 : 0.05) : sum;
        for (int c = 0; c < 3; ++c) m[r * 3 + c] /= d;
    }
}

// The chart's linear light (a 2.4 gamma, as the Chart Match fit takes it; negatives mirrored).
double chartLinear(double v) { return v < 0 ? -std::pow(-v, 2.4) : std::pow(v, 2.4); }
double chartCode(double l) { return l < 0 ? -std::pow(-l, 1 / 2.4) : std::pow(l, 1 / 2.4); }

// A pixel's channels to linear light and back through the curves themselves (tables are not close enough for a matrix
// and its inverse to come back: after a matrix a dark channel can be the small difference of bright ones), the display
// curves mirrored below 0 and camera logs carried on as they are.
struct Linearise {
    bool linear, mirror;
    Transfer tf;
    explicit Linearise(const ColorSpace& s)
        : linear(s.transfer == Transfer::Linear),
          mirror(s.transfer == Transfer::Bt1886 || s.transfer == Transfer::Srgb || s.transfer == Transfer::Pq || s.transfer == Transfer::Hlg),
          tf(s.transfer) {}
    double lin(float v) const {
        if (linear) return v;
        return v < 0 && mirror ? -toLinear(tf, -double(v)) : toLinear(tf, double(v));
    }
    float code(double l) const {
        if (linear) return float(l);
        return float(l < 0 && mirror ? -fromLinear(tf, -l) : fromLinear(tf, l));
    }
};

double smoothstep(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// The compression of the way to the boundary: as it is up to `start`, then easing towards 1 without reaching it (any
// distance beyond, however far, kept apart from the others).
double compress(double u, double start) { return u <= start ? u : start + (1 - start) * std::tanh((u - start) / (1 - start)); }

}  // namespace

// ---- Crosstalk ----

void crosstalkHandles(const double hue[3], const double strength[3], double m[9]) {
    // Each primary's image (the matrix's column): moved along the gamut's edge towards a neighbour (by a share giving
    // that hue on the hexagonal hue circle, so 60° reaches the secondary between them), then towards grey or past
    // itself by its strength. The columns are then scaled (as a gamut's primaries are, to make its white) so the rows
    // sum to 1, which keeps each primary's hue and colour exactly where its handles put it. Where the three no longer
    // make white with positive amounts (two pulled onto each other, or one made grey), the rows are scaled instead,
    // eased in over the way there, so the matrix stays well made.
    double col[3][3] = {};
    for (int i = 0; i < 3; ++i) {
        const double h = std::clamp(hue[i], -60.0, 60.0);
        const double a = std::fabs(h) / (60 + std::fabs(h));
        const int toward = h >= 0 ? (i + 1) % 3 : (i + 2) % 3;
        col[i][i] = 1 - a;
        col[i][toward] += a;
        const double s = strength[i], mean = (col[i][0] + col[i][1] + col[i][2]) / 3;
        for (int k = 0; k < 3; ++k) col[i][k] = s * col[i][k] + (1 - s) * mean;
    }
    double T[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) T[r * 3 + c] = col[c][r];
    double byRows[9];
    std::copy(T, T + 9, byRows);
    rowsToOne(byRows);
    double k[3] = {0, 0, 0};
    if (std::fabs(det3(T)) > 1e-9) {
        double inv[9];
        invertMatrix3(T, inv);
        const double one[3] = {1, 1, 1};
        apply3(inv, one, k);
    }
    const double least = std::min({k[0], k[1], k[2]});
    const double rows = 1 - smoothstep(0.1, 0.3, least);  // (how much the rows' scaling is used)
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            const double byColumns = rows < 1 ? T[r * 3 + c] * k[c] : 0;
            m[r * 3 + c] = rows * byRows[r * 3 + c] + (1 - rows) * byColumns;
        }
}

std::string crosstalkMatrixText(const double m[9]) {
    std::ostringstream o;
    o.precision(9);
    for (int i = 0; i < 9; ++i) o << (i ? " " : "") << m[i];
    return o.str();
}

bool parseCrosstalkMatrix(const std::string& text, double m[9]) {
    std::istringstream in(text);
    double v[9];
    for (double& x : v)
        if (!(in >> x) || !std::isfinite(x)) return false;
    std::string more;
    if (in >> more) return false;
    std::copy(v, v + 9, m);
    return true;
}

bool crosstalkMatrix(const Effect& e, FrameTime t, const ColorSpace& space, double m[9]) {
    std::copy(kIdentity, kIdentity + 9, m);
    double hue[3], strength[3];
    bool handles = false;
    for (int i = 0; i < 3; ++i) {
        hue[i] = e.p(std::string(kPrimaryNames[i]) + "_hue", t, 0);
        strength[i] = e.p(std::string(kPrimaryNames[i]) + "_strength", t, 100) / 100;
        handles = handles || hue[i] != 0 || strength[i] != 1;
    }
    double fitted[9];
    const bool hasFit = parseCrosstalkMatrix(e.s("matrix"), fitted) && !std::equal(fitted, fitted + 9, kIdentity);
    if (!handles && !hasFit) return false;
    if (handles) crosstalkHandles(hue, strength, m);
    if (hasFit) {
        // (kept in linear Rec.709, as fitted: into the working space's primaries and back round it, the rows put to 1
        // again for the whites' adaptation)
        double to709[9], from709[9];
        primariesMatrix(space.primaries, Primaries::Bt709, to709);
        primariesMatrix(Primaries::Bt709, space.primaries, from709);
        mul3(fitted, to709, fitted);
        mul3(from709, fitted, fitted);
        rowsToOne(fitted);
        mul3(m, fitted, m);
    }
    if (e.p("invert", t, 0) > 0.5) {
        if (std::fabs(det3(m)) < 1e-9) {
            std::copy(kIdentity, kIdentity + 9, m);
            return false;
        }
        double inv[9];
        invertMatrix3(m, inv);
        std::copy(inv, inv + 9, m);
    }
    return true;
}

void crosstalk(const Effect& e, FrameTime t, const ColorSpace& space, Image& img) {
    double m[9];
    if (img.empty() || !crosstalkMatrix(e, t, space, m)) return;
    const Linearise L(space);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                const float a = p[3];
                if (a <= 0) continue;
                const double r = L.lin(p[0] / a), g = L.lin(p[1] / a), b = L.lin(p[2] / a);
                p[0] = L.code(m[0] * r + m[1] * g + m[2] * b) * a;
                p[1] = L.code(m[3] * r + m[4] * g + m[5] * b) * a;
                p[2] = L.code(m[6] * r + m[7] * g + m[8] * b) * a;
            }
        }
    });
}

std::array<std::array<Xy, 2>, 3> crosstalkPrimaries(const Effect& e, FrameTime t, const ColorSpace& space) {
    double m[9], xyz[9];
    crosstalkMatrix(e, t, space, m);
    primariesToXyz(space.primaries, xyz);
    auto xyOf = [&](const double rgb[3]) {
        double X[3];
        apply3(xyz, rgb, X);
        const double sum = X[0] + X[1] + X[2];
        return std::fabs(sum) < 1e-12 ? Xy{0.3127, 0.3290} : Xy{X[0] / sum, X[1] / sum};
    };
    std::array<std::array<Xy, 2>, 3> out;
    for (int i = 0; i < 3; ++i) {
        const double e3[3] = {double(i == 0), double(i == 1), double(i == 2)};
        double moved[3];
        apply3(m, e3, moved);
        out[size_t(i)] = {xyOf(e3), xyOf(moved)};
    }
    return out;
}

CrosstalkFit fitCrosstalk(const ChartColors& measured, const ChartColors& reference) {
    CrosstalkFit f;
    // Row by row in linear Rec.709: the least squares with the row summing to 1 (Lagrange), held a little towards no
    // change so patches that leave a direction open do not swing it.
    double A[3][3] = {}, b[3][3] = {}, trace = 0;
    for (size_t i = 0; i < 24; ++i) {
        double in[3], out[3];
        for (int k = 0; k < 3; ++k) in[k] = chartLinear(measured[i][size_t(k)]), out[k] = chartLinear(reference[i][size_t(k)]);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) A[r][c] += in[r] * in[c];
            for (int c = 0; c < 3; ++c) b[c][r] += out[c] * in[r];
        }
    }
    for (int k = 0; k < 3; ++k) trace += A[k][k];
    if (!(trace > 1e-9)) return f;
    const double lambda = 1e-3 * trace / 3;
    double m709[9];
    for (int row = 0; row < 3; ++row) {
        // [A + λI  1; 1ᵀ 0] [f; μ] = [b + λ e_row; 1]
        double K[4][5] = {};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) K[r][c] = A[r][c] + (r == c ? lambda : 0);
            K[r][3] = 1;
            K[r][4] = b[row][r] + (r == row ? lambda : 0);
        }
        K[3][0] = K[3][1] = K[3][2] = 1, K[3][3] = 0, K[3][4] = 1;
        for (int c = 0; c < 4; ++c) {
            int piv = c;
            for (int r = c + 1; r < 4; ++r)
                if (std::fabs(K[r][c]) > std::fabs(K[piv][c])) piv = r;
            if (std::fabs(K[piv][c]) < 1e-15) return f;
            std::swap(K[c], K[piv]);
            for (int r = 0; r < 4; ++r) {
                if (r == c) continue;
                const double k = K[r][c] / K[c][c];
                for (int j = c; j < 5; ++j) K[r][j] -= k * K[c][j];
            }
        }
        for (int c = 0; c < 3; ++c) m709[row * 3 + c] = K[c][4] / K[c][c];
    }
    for (size_t i = 0; i < 24; ++i) {
        double in[3], out[3];
        for (int k = 0; k < 3; ++k) in[k] = chartLinear(measured[i][size_t(k)]);
        apply3(m709, in, out);
        const ChartColor after{chartCode(out[0]), chartCode(out[1]), chartCode(out[2])};
        const double before = deltaE76(measured[i], reference[i]), now = deltaE76(after, reference[i]);
        f.deltaEBefore += before / 24, f.deltaEAfter += now / 24, f.deltaEMax = std::max(f.deltaEMax, now);
    }
    std::copy(m709, m709 + 9, f.m);
    f.ok = std::all_of(f.m, f.m + 9, [](double v) { return std::isfinite(v); });
    return f;
}

Effect crosstalkEffect(const CrosstalkFit& f, Id id) {
    Effect e = makeEffect("crosstalk", id);
    e.strings["matrix"] = crosstalkMatrixText(f.m);
    return e;
}

int applyCrosstalkToClips(Project& p, Sequence& s, const std::vector<Id>& clips, const CrosstalkFit& fit) {
    int n = 0;
    for (Id id : clips) {
        const auto loc = edit::locate(s, id);
        Clip* c = loc && loc->track.kind == TrackKind::Video ? edit::clipById(s, id) : nullptr;
        if (!c || !c->mediaId) continue;
        auto it = std::find_if(c->effects.begin(), c->effects.end(), [](const Effect& x) { return x.type == "crosstalk"; });
        if (it != c->effects.end()) {
            it->strings["matrix"] = crosstalkMatrixText(fit.m);  // (its handles kept, on the new fit)
        } else {
            c->effects.insert(c->effects.begin(), crosstalkEffect(fit, p.newId()));  // (first: the camera's colour before any grade)
        }
        ++n;
    }
    return n;
}

// ---- Colour Correlation ----

CorrelationSettings correlationSettings(const Effect& e, FrameTime t, const ColorSpace& space) {
    CorrelationSettings s;
    s.saturation = std::max(0.0, e.p("saturation", t, 100) / 100);
    s.start = std::clamp(e.p("compress_start", t, 80) / 100, 0.5, 0.99);
    s.low = e.p("exposure_low", t, -12), s.high = e.p("exposure_high", t, 12), s.soft = std::max(0.0, e.p("exposure_soft", t, 1));
    double boundaryXyz[9], workingXyz[9], toXyzInv[9];
    const int which = int(std::lround(e.p("boundary", t, 1)));
    if (which == 3) {
        const double xy[8] = {e.p("custom_rx", t, 0.64), e.p("custom_ry", t, 0.33), e.p("custom_gx", t, 0.30), e.p("custom_gy", t, 0.60),
                              e.p("custom_bx", t, 0.15), e.p("custom_by", t, 0.06), 0.3127, 0.3290};
        chromaticitiesToXyz(xy, boundaryXyz);
        if (std::fabs(det3(boundaryXyz)) < 1e-9) primariesToXyz(Primaries::Bt709, boundaryXyz);
    } else {
        primariesToXyz(which == 0 ? Primaries::Bt709 : which == 2 ? Primaries::Bt2020 : Primaries::P3D65, boundaryXyz);
    }
    primariesToXyz(space.primaries, workingXyz);
    invertMatrix3(boundaryXyz, toXyzInv);
    mul3(toXyzInv, workingXyz, s.toBoundary);
    invertMatrix3(s.toBoundary, s.fromBoundary);
    double sum = boundaryXyz[3] + boundaryXyz[4] + boundaryXyz[5];
    if (!(std::fabs(sum) > 1e-9)) sum = 1;
    for (int k = 0; k < 3; ++k) s.luma[k] = boundaryXyz[3 + k] / sum;
    return s;
}

void correlatePixel(const CorrelationSettings& s, double rgb[3]) {
    double b[3];
    apply3(s.toBoundary, rgb, b);
    const double Y = s.luma[0] * b[0] + s.luma[1] * b[1] + s.luma[2] * b[2];
    if (!(Y > 1e-12)) return;  // (no light, or less than none: the noise below black is left as it is)
    // How far the colour is towards the boundary: 1 where its smallest channel reaches 0.
    const double f = (Y - std::min({b[0], b[1], b[2]})) / Y;
    if (f <= 0) return;
    double sat = s.saturation;
    if (sat != 1) {
        const double stops = std::log2(Y / 0.18);
        double w = 1;
        if (stops < s.low) w = s.soft > 0 ? 1 - smoothstep(0, s.soft, s.low - stops) : 0;
        else if (stops > s.high) w = s.soft > 0 ? 1 - smoothstep(0, s.soft, stops - s.high) : 0;
        sat = 1 + (sat - 1) * w;
    }
    const double target = compress(sat * f, s.start);
    const double k = target / f;
    for (double& v : b) v = Y + (v - Y) * k;
    apply3(s.fromBoundary, b, rgb);
}

void colourCorrelation(const Effect& e, FrameTime t, const ColorSpace& space, Image& img) {
    if (img.empty()) return;
    const CorrelationSettings s = correlationSettings(e, t, space);
    const Linearise L(space);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                const float a = p[3];
                if (a <= 0) continue;
                double rgb[3] = {L.lin(p[0] / a), L.lin(p[1] / a), L.lin(p[2] / a)};
                correlatePixel(s, rgb);
                for (int c = 0; c < 3; ++c) p[c] = L.code(rgb[c]) * a;
            }
        }
    });
}

}  // namespace montage
