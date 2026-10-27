#include "ColorChart.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/ColorGroups.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "media/MediaPool.h"
#include "render/Processing.h"

namespace montage {

namespace {

constexpr double kGamma = 2.4;

const char* const kNames[24] = {"Dark Skin",   "Light Skin", "Blue Sky", "Foliage",       "Blue Flower",  "Bluish Green",
                                "Orange",      "Purplish Blue", "Moderate Red", "Purple", "Yellow Green", "Orange Yellow",
                                "Blue",        "Green",      "Red",      "Yellow",        "Magenta",      "Cyan",
                                "White",       "Neutral 8",  "Neutral 6.5", "Neutral 5",  "Neutral 3.5",  "Black"};

double toLin(double v) { return v > 0 ? std::pow(v, kGamma) : 0.0; }
double toCode(double v) { return v > 0 ? std::pow(v, 1 / kGamma) : 0.0; }

// sRGB (0..1) to CIE L*a*b* (D65).
std::array<double, 3> lab(const ChartColor& c) {
    auto dec = [](double v) {
        v = std::clamp(v, 0.0, 1.0);
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    const double r = dec(c[0]), g = dec(c[1]), b = dec(c[2]);
    const double X = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047, Y = 0.2126 * r + 0.7152 * g + 0.0722 * b,
                 Z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883;
    auto f = [](double t) { return t > 0.008856 ? std::cbrt(t) : 7.787 * t + 16.0 / 116; };
    return {116 * f(Y) - 16, 500 * (f(X) - f(Y)), 200 * (f(Y) - f(Z))};
}

// Solves the 4 x 4 system a x = b (Gaussian elimination with pivoting); false if singular.
bool solve4(double a[4][4], double b[4], double x[4]) {
    double m[4][5];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) m[i][j] = a[i][j];
        m[i][4] = b[i];
    }
    for (int c = 0; c < 4; ++c) {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(m[r][c]) > std::fabs(m[p][c])) p = r;
        if (std::fabs(m[p][c]) < 1e-12) return false;
        std::swap(m[c], m[p]);
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double k = m[r][c] / m[c][c];
            for (int j = c; j < 5; ++j) m[r][j] -= k * m[c][j];
        }
    }
    for (int i = 0; i < 4; ++i) x[i] = m[i][4] / m[i][i];
    return true;
}

// Least squares out ≈ A [in, 1] for each channel, held towards `prior` by `ridge`; the RMS residual.
double fitAffine(const std::vector<ChartColor>& in, const std::vector<ChartColor>& out, double A[3][4], double ridge,
                 const double prior[3][4]) {
    double ata[4][4] = {};
    double atb[3][4] = {};
    for (size_t k = 0; k < in.size(); ++k) {
        const double v[4] = {in[k][0], in[k][1], in[k][2], 1};
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) ata[i][j] += v[i] * v[j];
            for (int c = 0; c < 3; ++c) atb[c][i] += v[i] * out[k][c];
        }
    }
    for (int i = 0; i < 4; ++i) ata[i][i] += ridge;
    for (int c = 0; c < 3; ++c) {
        double a[4][4], b[4], x[4];
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) a[i][j] = ata[i][j];
            b[i] = atb[c][i] + ridge * prior[c][i];
        }
        if (!solve4(a, b, x)) return 1e9;
        for (int i = 0; i < 4; ++i) A[c][i] = x[i];
    }
    double err = 0;
    for (size_t k = 0; k < in.size(); ++k)
        for (int c = 0; c < 3; ++c) {
            const double e = A[c][0] * in[k][0] + A[c][1] * in[k][1] + A[c][2] * in[k][2] + A[c][3] - out[k][c];
            err += e * e;
        }
    return std::sqrt(err / (3.0 * std::max<size_t>(1, in.size())));
}

// Chart (u, v in 0..1, u across its 6 columns) to the picture, for an outline.
Point2 onQuad(const Homography& h, double u, double v) { return h.apply({u, v}); }

// The outline's homography; false when it is no outline (corners together, in a line, or crossed).
bool quadHomography(const TrackQuad& q, Homography& h) {
    double area = 0;
    int sign = 0;
    for (int i = 0; i < 4; ++i) {
        const Point2 &a = q.p[i], &b = q.p[(i + 1) % 4], &c = q.p[(i + 2) % 4];
        if (!std::isfinite(a.x) || !std::isfinite(a.y)) return false;
        area += a.x * b.y - b.x * a.y;
        const double turn = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
        const int sg = turn > 0 ? 1 : turn < 0 ? -1 : 0;
        if (sg == 0 || (sign && sg != sign)) return false;
        sign = sg;
    }
    if (std::fabs(area) / 2 < 1e-4) return false;
    return fitHomography({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, {q.p[0], q.p[1], q.p[2], q.p[3]}, h);
}

// HLG's OOTF as convertColor applies it: a 1000-nit display, system gamma 1.2, SDR white at 203 nits.
constexpr double kHlgPeakNits = 1000, kHlgSystemGamma = 1.2, kSdrWhiteNits = 203;
template <typename T>
T luma2020(const T c[3]) {
    return T(0.2627) * c[0] + T(0.6780) * c[1] + T(0.0593) * c[2];
}

// How a colour space's code values become linear light with SDR white at 1, and back: each channel through the
// transfer curve (negatives mirrored for the display curves, so out-of-gamut colour survives), then for HLG the
// OOTF taking scene light to display light (and its inverse), which works on the three channels together.
struct LinearLight {
    Transfer tr;
    bool hlg = false, mirror = true;
    explicit LinearLight(const ColorSpace& cs) : tr(cs.transfer) {
        hlg = tr == Transfer::Hlg;
        mirror = tr == Transfer::Bt1886 || tr == Transfer::Srgb || tr == Transfer::Pq || hlg;
    }
    double lin(double v) const {
        if (!std::isfinite(v)) return 0;
        return v < 0 && mirror ? -toLinear(tr, -v) : toLinear(tr, v);
    }
    double code(double l) const {
        if (!std::isfinite(l)) return 0;
        return l < 0 && mirror ? -fromLinear(tr, -l) : fromLinear(tr, l);
    }
    // The gain taking a pixel's channel values (as lin gives them) to display light, and display light back.
    template <typename T>
    T toDisplay(const T c[3]) const {
        if (!hlg) return T(1);
        const T ys = std::max(T(0), luma2020(c));
        return T(kHlgPeakNits / kSdrWhiteNits) * std::pow(ys, T(kHlgSystemGamma - 1));
    }
    template <typename T>
    T toScene(const T l[3]) const {
        if (!hlg) return T(1);
        const T yd = std::max(T(1e-12), luma2020(l) * T(kSdrWhiteNits / kHlgPeakNits));
        return std::pow(yd, T((1 - kHlgSystemGamma) / kHlgSystemGamma)) * T(kSdrWhiteNits / kHlgPeakNits);
    }
    void decode(double c[3]) const {
        for (int k = 0; k < 3; ++k) c[k] = lin(c[k]);
        const double g = toDisplay(c);
        for (int k = 0; k < 3; ++k) c[k] *= g;
    }
};

void mul3(const double a[9], const double b[9], double out[9]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}

}  // namespace

const ChartColors& colorChecker24() {
    // X-Rite's published sRGB values for the ColorChecker Classic (D65).
    static const ChartColors kRef = [] {
        static const int v[24][3] = {{115, 82, 68},   {194, 150, 130}, {98, 122, 157},  {87, 108, 67},   {133, 128, 177}, {103, 189, 170},
                                     {214, 126, 44},  {80, 91, 166},   {193, 90, 99},   {94, 60, 108},   {157, 188, 64},  {224, 163, 46},
                                     {56, 61, 150},   {70, 148, 73},   {175, 54, 60},   {231, 199, 31},  {187, 86, 149},  {8, 133, 161},
                                     {243, 243, 242}, {200, 200, 200}, {160, 160, 160}, {122, 122, 121}, {85, 85, 85},    {52, 52, 52}};
        ChartColors c;
        for (int i = 0; i < 24; ++i)
            for (int k = 0; k < 3; ++k) c[size_t(i)][size_t(k)] = v[i][k] / 255.0;
        return c;
    }();
    return kRef;
}

const char* colorCheckerName(int patch) { return patch >= 0 && patch < 24 ? kNames[patch] : ""; }

ChartColors sampleChart(const Image& img, const TrackQuad& quad, const ColorSpace& space, bool* complete) {
    ChartColors out{};
    if (complete) *complete = false;
    Homography h;
    if (img.empty() || !quadHomography(quad, h)) return out;
    bool all = true;
    constexpr int kSteps = 9;  // a 9 x 9 grid of samples across each patch's middle half
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 6; ++c) {
            double sum[3] = {0, 0, 0};
            int n = 0;
            for (int j = 0; j < kSteps; ++j)
                for (int i = 0; i < kSteps; ++i) {
                    const double u = (c + 0.25 + 0.5 * (i + 0.5) / kSteps) / 6, v = (r + 0.25 + 0.5 * (j + 0.5) / kSteps) / 4;
                    const Point2 p = onQuad(h, u, v);
                    const int x = int(std::floor(p.x * img.width)), y = int(std::floor(p.y * img.height));
                    if (x < 0 || y < 0 || x >= img.width || y >= img.height) continue;
                    const float* px = img.at(x, y);
                    if (px[3] <= 0) continue;
                    for (int k = 0; k < 3; ++k) sum[k] += px[k] / px[3];
                    ++n;
                }
            ChartColor& o = out[size_t(r * 6 + c)];
            for (int k = 0; k < 3; ++k) o[size_t(k)] = n ? sum[k] / n : 0.0;
            all = all && n >= kSteps * kSteps / 2;
        }
    // Into Rec.709 code values: the picture's linear light in Rec.709 primaries, with the chart's 2.4 gamma.
    if (space.id != rec709Space().id) {
        const LinearLight from(space);
        double m[9];
        primariesMatrix(space.primaries, Primaries::Bt709, m);
        for (ChartColor& o : out) {
            double l[3] = {o[0], o[1], o[2]};
            from.decode(l);
            for (int k = 0; k < 3; ++k) {
                const double v = m[k * 3] * l[0] + m[k * 3 + 1] * l[1] + m[k * 3 + 2] * l[2];
                o[size_t(k)] = v < 0 ? -toCode(-v) : toCode(v);
            }
        }
    }
    if (complete) *complete = all;
    return out;
}

bool detectChart(const Image& img, TrackQuad& out, double* residual) {
    if (img.width < 24 || img.height < 16) return false;
    // A smaller copy (its longest side at most 360) and its summed-area tables, so any box's mean is four look-ups.
    const double shrink = std::min(1.0, 360.0 / std::max(img.width, img.height));
    const int sw = std::max(1, int(std::lround(img.width * shrink))), sh = std::max(1, int(std::lround(img.height * shrink)));
    std::vector<double> sat[3];
    for (auto& s : sat) s.assign(size_t(sw + 1) * size_t(sh + 1), 0.0);
    for (int y = 0; y < sh; ++y) {
        const int sy = std::min(img.height - 1, int((y + 0.5) * img.height / sh));
        double row[3] = {0, 0, 0};
        for (int x = 0; x < sw; ++x) {
            const int sx = std::min(img.width - 1, int((x + 0.5) * img.width / sw));
            const float* p = img.at(sx, sy);
            for (int k = 0; k < 3; ++k) {
                row[k] += std::clamp(double(p[k]), 0.0, 1.5);
                sat[k][size_t(y + 1) * size_t(sw + 1) + size_t(x + 1)] = sat[k][size_t(y) * size_t(sw + 1) + size_t(x + 1)] + row[k];
            }
        }
    }
    auto boxMean = [&](double x0, double y0, double x1, double y1, ChartColor& c) {
        const int a = std::clamp(int(x0), 0, sw), b = std::clamp(int(y0), 0, sh), e = std::clamp(int(std::ceil(x1)), 0, sw),
                  f = std::clamp(int(std::ceil(y1)), 0, sh);
        if (e <= a || f <= b) return false;
        const double n = double(e - a) * (f - b);
        for (int k = 0; k < 3; ++k) {
            const auto& s = sat[k];
            const auto at = [&](int x, int y) { return s[size_t(y) * size_t(sw + 1) + size_t(x)]; };
            c[size_t(k)] = (at(e, f) - at(a, f) - at(e, b) + at(a, b)) / n;
        }
        return true;
    };
    const ChartColors& ref = colorChecker24();
    std::vector<ChartColor> target(24), measured(24);
    for (int i = 0; i < 24; ++i) target[size_t(i)] = ref[size_t(i)];
    static const double kIdentity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    // The chart (u across its columns, v down its rows) on a rectangle, in each of four turns.
    auto place = [](int turn, double x, double y, double w, double h, double u, double v) -> Point2 {
        switch (turn) {
            case 1: return {x + (1 - u) * w, y + (1 - v) * h};  // upside down
            case 2: return {x + (1 - v) * w, y + u * h};        // on its side, top to the right
            case 3: return {x + v * w, y + (1 - u) * h};        // on its side, top to the left
            default: return {x + u * w, y + v * h};
        }
    };
    struct Best {
        double score = 1e9;
        int turn = 0;
        double x = 0, y = 0, w = 0, h = 0;
    } best;
    // `box`: the part of each patch sampled about its middle (a quarter each way, or an eighth in the coarse search,
    // whose rectangles are further apart).
    auto score = [&](int turn, double x, double y, double w, double h, double box = 0.25) {
        const double cw = (turn < 2 ? w / 6 : w / 4), ch = (turn < 2 ? h / 4 : h / 6);  // a patch's size
        if (cw < 3 || ch < 3) return 1e9;
        double lum[24];
        auto sample = [&](int i) {
            const int r = i / 6, c = i % 6;
            const Point2 p = place(turn, x, y, w, h, (c + 0.5) / 6, (r + 0.5) / 4);
            ChartColor& m = measured[size_t(i)];
            if (!boxMean(p.x - cw * box, p.y - ch * box, p.x + cw * box, p.y + ch * box, m)) return false;
            lum[i] = 0.2126 * m[0] + 0.7152 * m[1] + 0.0722 * m[2];
            return true;
        };
        // The grey row first: it runs from white down to black, and the patches differ (not a plain wall).
        for (int i = 18; i < 24; ++i)
            if (!sample(i)) return 1e9;
        for (int i = 18; i < 23; ++i)
            if (lum[i] <= lum[i + 1]) return 1e9;
        if (lum[18] - lum[23] < 0.15) return 1e9;
        for (int i = 0; i < 18; ++i)
            if (!sample(i)) return 1e9;
        double A[3][4];
        return fitAffine(measured, target, A, 1e-4, kIdentity);
    };
    // Coarse to fine: rectangles half a patch apart, each patch sampled close to its middle so a rectangle that far off
    // still reads the right patches; the best few places (apart from each other) then searched a sixth of a patch apart.
    std::vector<Best> coarse;
    for (int turn = 0; turn < 4; ++turn)
        for (double aspect : {1.3, 1.45, 1.6, 1.75}) {
            const double ar = turn < 2 ? aspect : 1 / aspect;  // width over height on the picture
            for (double w = 0.12 * sw; w <= sw; w *= 1.08) {
                const double h = w / ar;
                if (h > sh) break;
                const double step = std::max(1.0, std::min(w, h) / 8);
                for (double y = 0; y + h <= sh; y += step)
                    for (double x = 0; x + w <= sw; x += step) {
                        const double s = score(turn, x, y, w, h, 0.125);
                        if (s < 0.15) coarse.push_back({s, turn, x, y, w, h});
                    }
            }
        }
    std::sort(coarse.begin(), coarse.end(), [](const Best& a, const Best& b) { return a.score < b.score; });
    std::vector<Best> places;
    for (const Best& c : coarse) {
        if (places.size() >= 16) break;
        const bool near = std::any_of(places.begin(), places.end(), [&](const Best& o) {
            const double ix = std::max(0.0, std::min(c.x + c.w, o.x + o.w) - std::max(c.x, o.x));
            const double iy = std::max(0.0, std::min(c.y + c.h, o.y + o.h) - std::max(c.y, o.y));
            return ix * iy > 0.5 * std::min(c.w * c.h, o.w * o.h);
        });
        if (!near) places.push_back(c);
    }
    for (const Best& c : places)
        for (double k : {1 / 1.08, 1.0, 1.08}) {
            const double w = c.w * k, h = c.h * k, reach = std::max(1.0, std::min(c.w, c.h) / 8);
            const double step = std::max(0.5, std::min(w, h) / 24);
            for (double dy = -reach; dy <= reach + 1e-9; dy += step)
                for (double dx = -reach; dx <= reach + 1e-9; dx += step) {
                    const double x = c.x + (c.w - w) / 2 + dx, y = c.y + (c.h - h) / 2 + dy;
                    if (x < 0 || y < 0 || x + w > sw || y + h > sh) continue;
                    const double s = score(c.turn, x, y, w, h);
                    if (s < best.score) best = {s, c.turn, x, y, w, h};
                }
        }
    if (best.score > 0.08) return false;
    // Refined: each edge nudged while that fits better, in ever smaller steps.
    for (double step = std::max(best.w, best.h) / 48; step > 0.25; step /= 2)
        for (bool moved = true; moved;) {
            moved = false;
            for (int k = 0; k < 4; ++k)
                for (double d : {-step, step}) {
                    Best t = best;
                    if (k == 0) t.x += d, t.w -= d;
                    else if (k == 1) t.w += d;
                    else if (k == 2) t.y += d, t.h -= d;
                    else t.h += d;
                    if (t.w < 8 || t.h < 8 || t.x < 0 || t.y < 0 || t.x + t.w > sw || t.y + t.h > sh) continue;
                    t.score = score(t.turn, t.x, t.y, t.w, t.h);
                    if (t.score < best.score - 1e-9) best = t, moved = true;
                }
        }
    const double corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 0; i < 4; ++i) {
        const Point2 p = place(best.turn, best.x, best.y, best.w, best.h, corners[i][0], corners[i][1]);
        out.p[i] = {p.x / sw, p.y / sh};
    }
    if (residual) *residual = best.score;
    return true;
}

ChartFit fitChart(const ChartColors& measured, const ChartColors& reference) {
    ChartFit f;
    std::vector<ChartColor> in(24), out(24);
    for (size_t i = 0; i < 24; ++i)
        for (size_t k = 0; k < 3; ++k) {
            in[i][k] = toLin(measured[i][k]);
            out[i][k] = toLin(reference[i][k]);
        }
    static const double kIdentity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    double A[3][4];
    if (fitAffine(in, out, A, 1e-3, kIdentity) >= 1e8) return f;
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 3; ++k) f.m[c][k] = A[c][k];
        f.offset[c] = A[c][3];
    }
    f.ok = true;
    for (size_t i = 0; i < 24; ++i) {
        const double before = deltaE76(measured[i], reference[i]), after = deltaE76(applyChartFit(f, measured[i]), reference[i]);
        f.deltaEBefore += before / 24;
        f.deltaEAfter += after / 24;
        f.deltaEMax = std::max(f.deltaEMax, after);
    }
    return f;
}

ChartColor applyChartFit(const ChartFit& f, const ChartColor& rgb) {
    const double l[3] = {toLin(rgb[0]), toLin(rgb[1]), toLin(rgb[2])};
    ChartColor o;
    for (int c = 0; c < 3; ++c) o[size_t(c)] = toCode(f.m[c][0] * l[0] + f.m[c][1] * l[1] + f.m[c][2] * l[2] + f.offset[c]);
    return o;
}

double deltaE76(const ChartColor& a, const ChartColor& b) {
    const auto x = lab(a), y = lab(b);
    return std::sqrt((x[0] - y[0]) * (x[0] - y[0]) + (x[1] - y[1]) * (x[1] - y[1]) + (x[2] - y[2]) * (x[2] - y[2]));
}

Effect chartMatchEffect(const ChartFit& f, Id id, const std::string& space) {
    Effect e = makeEffect("chart_match", id);
    static const char* const m[3][3] = {{"rr", "rg", "rb"}, {"gr", "gg", "gb"}, {"br", "bg", "bb"}};
    static const char* const o[3] = {"offset_r", "offset_g", "offset_b"};
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 3; ++k) e.params[m[c][k]] = f.m[c][k];
        e.params[o[c]] = f.offset[c];
    }
    e.strings["space"] = space;
    return e;
}

void applyChartMatch(const Effect& e, FrameTime t, Image& img) {
    const float a = float(std::clamp(e.p("amount", t, 100) / 100, 0.0, 1.0));
    if (!(a > 0) || img.empty()) return;
    const ColorSpace* found = findColorSpace(e.s("space"));
    const ColorSpace& space = found ? *found : rec709Space();
    const LinearLight light(space);
    // The fit (made in linear Rec.709) in the picture's own primaries: to Rec.709, the fit, back again.
    static const char* const names[3][3] = {{"rr", "rg", "rb"}, {"gr", "gg", "gb"}, {"br", "bg", "bb"}};
    double fit[9], off[3];
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 3; ++k) fit[c * 3 + k] = e.p(names[c][k], t, c == k ? 1 : 0);
        off[c] = e.p(c == 0 ? "offset_r" : c == 1 ? "offset_g" : "offset_b", t, 0);
    }
    double to709[9], from709[9], tmp[9], whole[9];
    primariesMatrix(space.primaries, Primaries::Bt709, to709);
    primariesMatrix(Primaries::Bt709, space.primaries, from709);
    mul3(fit, to709, tmp);
    mul3(from709, tmp, whole);
    float m[9], o[3];
    for (int i = 0; i < 9; ++i) m[i] = float(whole[i]);
    for (int c = 0; c < 3; ++c) o[c] = float(from709[c * 3] * off[0] + from709[c * 3 + 1] * off[1] + from709[c * 3 + 2] * off[2]);
    // Tables both ways: code values 0..1 to linear light (smooth there), and linear light up to code value 1's to code
    // values, looked up by the fourth root of the light so the curve's steep start by black is followed closely. Exact
    // maths outside them; negatives mirrored; anything not a number becomes black. (HLG's OOTF on top, per pixel.)
    constexpr int kN = 4096;
    const double top = std::max(1e-6, light.lin(1.0)), rootTop = std::sqrt(std::sqrt(top));
    std::vector<float> dec(kN + 2), enc(kN + 2);
    for (int i = 0; i <= kN + 1; ++i) {
        dec[size_t(i)] = float(light.lin(std::min(1.0, double(i) / kN)));
        const double w = rootTop * std::min(1.0, double(i) / kN);
        enc[size_t(i)] = float(light.code(w * w * w * w));
    }
    auto lin = [&](float v) -> float {
        if (v >= 0.0f && v <= 1.0f) {
            const float x = v * kN;
            const int i = int(x);
            return dec[size_t(i)] + (dec[size_t(i) + 1] - dec[size_t(i)]) * (x - float(i));
        }
        return float(light.lin(v));
    };
    const float scale = float(kN / rootTop);
    auto code = [&](float l) -> float {
        if (l >= 0.0f && l <= float(top)) {
            const float x = std::sqrt(std::sqrt(l)) * scale;
            const int i = std::min(kN, int(x));
            return enc[size_t(i)] + (enc[size_t(i) + 1] - enc[size_t(i)]) * (x - float(i));
        }
        return float(light.code(l));
    };
    // Straight colour (the picture is premultiplied), so edges and empty pixels stay as they are.
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* px = img.row(y);
            for (int x = 0; x < img.width; ++x, px += 4) {
                const float alpha = px[3];
                if (!(alpha > 0)) continue;
                const float inv = 1 / alpha;
                float v[3] = {px[0] * inv, px[1] * inv, px[2] * inv};
                for (float& x : v)
                    if (!std::isfinite(x)) x = 0;
                float l[3] = {lin(v[0]), lin(v[1]), lin(v[2])};
                if (light.hlg) {
                    const float g = light.toDisplay(l);
                    for (float& x : l) x *= g;
                }
                float r[3];
                for (int c = 0; c < 3; ++c) r[c] = m[c * 3] * l[0] + m[c * 3 + 1] * l[1] + m[c * 3 + 2] * l[2] + o[c];
                if (light.hlg) {
                    const float g = light.toScene(r);
                    for (float& x : r) x *= g;
                }
                for (int c = 0; c < 3; ++c) px[c] = (v[c] + (code(r[c]) - v[c]) * a) * alpha;
            }
        }
    });
}

Image chartFrame(const Project& p, const Sequence& s, const Clip& c, FrameTime at) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image) || !m->hasVideo) return {};
    const FrameTime t = c.contains(at) ? at : c.start + c.duration / 2;
    const FrameTime lt = t - c.start;
    const double sec = m->kind == MediaKind::Video ? std::max(0.0, c.sourceFrameAt(t) / s.fpsValue()) : 0.0;
    const int mw = std::max(1, m->width), mh = std::max(1, m->height);
    const double scale = std::min(1.0, 1280.0 / std::max(mw, mh));
    Frame16Ptr f = MediaPool::instance().videoFrame(m->path, sec, std::max(1, int(std::lround(mw * scale))), std::max(1, int(std::lround(mh * scale))));
    if (!f) return {};
    Image img = toImage(*f);
    // As the compositor takes it to the clip's effects: an on-set CDL in the camera's own space, the input transform,
    // then the colour group's pre-clip grade.
    const ColorSpace& source = mediaColorSpace(*m);
    for (const Effect& e : c.effects)
        if (e.enabled && e.type == "cdl" && e.s("space") == source.label) {
            applyCdlValues(e, lt, img);
            break;
        }
    convertColor(img, source, sequenceColorSpace(s), s.hdrPeakNits);
    for (const ColorGroup* g : colorGroupsOf(s, c))
        for (const Effect& e : g->pre)
            if (e.enabled) applyVideoEffect(e, lt, img, double(img.width) / mw, sec);
    return img;
}

Image chartView(const Image& frame, const Sequence& s) {
    Image v = frame;
    const ColorSpace& space = sequenceColorSpace(s);
    if (space.id != rec709Space().id) convertColor(v, space, rec709Space(), s.hdrPeakNits);
    return v;
}

int applyChartMatchToClips(Project& p, Sequence& s, const std::vector<Id>& clips, const ChartFit& fit) {
    int n = 0;
    const std::string space = sequenceColorSpace(s).id;
    for (Id id : clips) {
        const auto loc = edit::locate(s, id);
        Clip* c = loc && loc->track.kind == TrackKind::Video ? edit::clipById(s, id) : nullptr;
        if (!c || !c->mediaId) continue;
        Effect e = chartMatchEffect(fit, p.newId(), space);
        auto it = std::find_if(c->effects.begin(), c->effects.end(), [](const Effect& x) { return x.type == "chart_match"; });
        if (it != c->effects.end()) {
            e.id = it->id;
            e.enabled = it->enabled;
            *it = e;
        } else {
            c->effects.insert(c->effects.begin(), e);  // (first: the camera's colour put right before any grade)
        }
        ++n;
    }
    return n;
}

std::vector<Id> clipsOfSameMedia(const Sequence& s, Id clip) {
    const Clip* c = edit::clipById(s, clip);
    if (!c || !c->mediaId) return {};
    std::vector<Id> out;
    for (const Track& t : s.videoTracks)
        for (const Clip& x : t.clips)
            if (x.mediaId == c->mediaId) out.push_back(x.id);
    return out;
}

}  // namespace montage
