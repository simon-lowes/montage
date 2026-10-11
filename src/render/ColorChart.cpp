#include "ColorChart.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/EditOps.h"
#include "core/Effects.h"
#include "render/Compositor.h"

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

Homography quadHomography(const TrackQuad& q) {
    Homography h;
    fitHomography({{0, 0}, {1, 0}, {1, 1}, {0, 1}}, {q.p[0], q.p[1], q.p[2], q.p[3]}, h);
    return h;
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

ChartColors sampleChart(const Image& img, const TrackQuad& quad) {
    ChartColors out{};
    if (img.empty()) return out;
    const Homography h = quadHomography(quad);
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
                    for (int k = 0; k < 3; ++k) sum[k] += px[k];
                    ++n;
                }
            ChartColor& o = out[size_t(r * 6 + c)];
            for (int k = 0; k < 3; ++k) o[size_t(k)] = n ? sum[k] / n : 0.0;
        }
    return out;
}

bool detectChart(const Image& img, TrackQuad& out, double* residual) {
    if (img.width < 24 || img.height < 16) return false;
    // A smaller copy (at most 360 wide) and its summed-area tables, so any box's mean is four look-ups.
    const int sw = std::min(360, img.width), sh = std::max(1, int(std::lround(double(img.height) * sw / img.width)));
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
    auto score = [&](int turn, double x, double y, double w, double h) {
        const double cw = (turn < 2 ? w / 6 : w / 4), ch = (turn < 2 ? h / 4 : h / 6);  // a patch's size
        if (cw < 3 || ch < 3) return 1e9;
        double lum[24];
        auto sample = [&](int i) {
            const int r = i / 6, c = i % 6;
            const Point2 p = place(turn, x, y, w, h, (c + 0.5) / 6, (r + 0.5) / 4);
            ChartColor& m = measured[size_t(i)];
            if (!boxMean(p.x - cw / 4, p.y - ch / 4, p.x + cw / 4, p.y + ch / 4, m)) return false;
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
    for (int turn = 0; turn < 4; ++turn)
        for (double aspect : {1.3, 1.45, 1.6, 1.75}) {
            const double ar = turn < 2 ? aspect : 1 / aspect;  // width over height on the picture
            for (double w = 0.12 * sw; w <= sw; w *= 1.08) {
                const double h = w / ar;
                if (h > sh) break;
                const double step = std::max(1.0, std::min(w, h) / 24);
                for (double y = 0; y + h <= sh; y += step)
                    for (double x = 0; x + w <= sw; x += step) {
                        const double s = score(turn, x, y, w, h);
                        if (s < best.score) best = {s, turn, x, y, w, h};
                    }
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

Effect chartMatchEffect(const ChartFit& f, Id id) {
    Effect e = makeEffect("chart_match", id);
    static const char* const m[3][3] = {{"rr", "rg", "rb"}, {"gr", "gg", "gb"}, {"br", "bg", "bb"}};
    static const char* const o[3] = {"offset_r", "offset_g", "offset_b"};
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 3; ++k) e.params[m[c][k]] = f.m[c][k];
        e.params[o[c]] = f.offset[c];
    }
    return e;
}

void applyChartMatch(const Effect& e, FrameTime t, Image& img) {
    const double amount = std::clamp(e.p("amount", t, 100) / 100, 0.0, 1.0);
    if (amount <= 0) return;
    static const char* const names[3][3] = {{"rr", "rg", "rb"}, {"gr", "gg", "gb"}, {"br", "bg", "bb"}};
    float m[3][3], off[3];
    for (int c = 0; c < 3; ++c) {
        for (int k = 0; k < 3; ++k) m[c][k] = float(e.p(names[c][k], t, c == k ? 1 : 0));
        off[c] = float(e.p(c == 0 ? "offset_r" : c == 1 ? "offset_g" : "offset_b", t, 0));
    }
    // Tables for the gamma both ways (code values 0..1 in, linear 0..2 out), straight maths beyond them.
    static const auto tables = [] {
        std::pair<std::vector<float>, std::vector<float>> t;
        t.first.resize(4097);
        t.second.resize(8193);
        for (int i = 0; i <= 4096; ++i) t.first[size_t(i)] = float(toLin(i / 4096.0));
        for (int i = 0; i <= 8192; ++i) t.second[size_t(i)] = float(toCode(2.0 * i / 8192));
        return t;
    }();
    auto lin = [&](float v) {
        if (v <= 0) return 0.0f;
        if (v >= 1) return float(toLin(v));
        const float x = v * 4096, f = x - std::floor(x);
        const size_t i = size_t(x);
        return tables.first[i] + (tables.first[std::min<size_t>(i + 1, 4096)] - tables.first[i]) * f;
    };
    auto code = [&](float v) {
        if (v <= 0) return 0.0f;
        if (v >= 2) return float(toCode(v));
        const float x = v * 4096, f = x - std::floor(x);
        const size_t i = size_t(x);
        return tables.second[i] + (tables.second[std::min<size_t>(i + 1, 8192)] - tables.second[i]) * f;
    };
    const float a = float(amount);
    const size_t n = size_t(img.width) * size_t(img.height);
#pragma omp parallel for if (n > 65536)
    for (long long i = 0; i < (long long)n; ++i) {
        float* px = img.px.data() + size_t(i) * 4;
        const float l[3] = {lin(px[0]), lin(px[1]), lin(px[2])};
        for (int c = 0; c < 3; ++c) {
            const float v = code(m[c][0] * l[0] + m[c][1] * l[1] + m[c][2] * l[2] + off[c]);
            px[c] += (v - px[c]) * a;
        }
    }
}

Image chartFrame(const Project& p, const Sequence& s, const Clip& c, FrameTime at) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image) || !m->hasVideo) return {};
    const FrameTime t = c.contains(at) ? at : c.start + c.duration / 2;
    const double sec = m->kind == MediaKind::Video ? std::max(0.0, c.sourceFrameAt(t) / s.fpsValue()) : 0.0;
    const int mw = std::max(1, m->width), mh = std::max(1, m->height);
    const double scale = std::min(1.0, 1280.0 / mw);
    return renderMediaFrame(p, *m, sec, std::max(1, int(std::lround(mw * scale))), std::max(1, int(std::lround(mh * scale))));
}

int applyChartMatchToClips(Project& p, Sequence& s, const std::vector<Id>& clips, const ChartFit& fit) {
    int n = 0;
    for (Id id : clips) {
        Clip* c = edit::clipById(s, id);
        if (!c || !c->mediaId) continue;
        Effect e = chartMatchEffect(fit, p.newId());
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
