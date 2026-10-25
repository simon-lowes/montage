#include "GrainMatch.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <deque>
#include <mutex>
#include <sstream>

#include "audio/RoomResponse.h"
#include "media/MediaPool.h"
#include "media/Tracking.h"
#include "render/ColorSpace.h"
#include "render/Processing.h"

namespace montage {

namespace {

constexpr int kTile = 64;  // the measuring tiles (and their Fourier transforms)
using Cf = std::complex<float>;

uint64_t mix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

// Two independent standard normal numbers from a hash (Box–Muller).
std::pair<float, float> gauss2(uint64_t h) {
    const double u1 = (double((h >> 11) & ((1ULL << 26) - 1)) + 0.5) / double(1ULL << 26);
    const double u2 = double(h & ((1ULL << 26) - 1)) / double(1ULL << 26);
    const double r = std::sqrt(-2 * std::log(u1)), a = 2 * M_PI * u2;
    return {float(r * std::cos(a)), float(r * std::sin(a))};
}

int nextPow2(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

// A 2D transform in place of a w x h grid (powers of two), rows then columns.
void fft2(std::vector<Cf>& a, int w, int h, bool inverse) {
    parallelRows(h, [&](int y0, int y1) {
        std::vector<Cf> row(static_cast<size_t>(w));
        for (int y = y0; y < y1; ++y) {
            std::copy_n(a.begin() + ptrdiff_t(y) * w, w, row.begin());
            fftInPlace(row, inverse);
            std::copy(row.begin(), row.end(), a.begin() + ptrdiff_t(y) * w);
        }
    });
    parallelRows(w, [&](int x0, int x1) {
        std::vector<Cf> col(static_cast<size_t>(h));
        for (int x = x0; x < x1; ++x) {
            for (int y = 0; y < h; ++y) col[size_t(y)] = a[size_t(y) * size_t(w) + size_t(x)];
            fftInPlace(col, inverse);
            for (int y = 0; y < h; ++y) a[size_t(y) * size_t(w) + size_t(x)] = col[size_t(y)];
        }
    });
}

double lumaOf(const double l[3]) { return 0.2126 * l[0] + 0.7152 * l[1] + 0.0722 * l[2]; }

// The band a light falls in (its middle b - 7.5 stops).
int bandOf(double light) { return std::clamp(int(std::floor(std::log2(std::max(light, 1e-9)))) + 8, 0, GrainProfile::kBands - 1); }

// Code values to linear light and back, carried on below 0 (as grain on black takes them) by the curve's mirror where
// the curve itself stops at 0.
double linear(Transfer t, double v) {
    if (v < 0 && (t == Transfer::Bt1886 || t == Transfer::Srgb)) return -toLinear(t, -v);
    return toLinear(t, v);
}
double encoded(Transfer t, double l) {
    if (l < 0 && (t == Transfer::Bt1886 || t == Transfer::Srgb)) return -fromLinear(t, -l);
    return fromLinear(t, l);
}

double spectrumAt(const GrainProfile& g, double f) {  // f in cycles a pixel of the measured frame
    const double x = f * 2 * GrainProfile::kBins - 0.5;
    if (x >= GrainProfile::kBins - 0.5) return 0;
    if (x <= 0) return std::max(0.0, g.spectrum[0]);
    const int i = std::min(int(x), GrainProfile::kBins - 2);
    const double t = std::min(1.0, x - i);
    return std::max(0.0, g.spectrum[i] + (g.spectrum[i + 1] - g.spectrum[i]) * t);
}

// How strong grain `size` times as large is on a picture with `scale` pixels of the frame measured in each of its own,
// as a share of its strength on the frame measured: a smaller picture averages away the grain finer than its pixels.
double resolutionGain(const GrainProfile& g, double scale, double size) {
    if (scale <= 1) return 1;
    auto power = [&](double half) {  // over frequencies to `half` each way, in cycles a pixel of the frame measured
        half = std::min(half, 0.5);  // (the spectrum ends at 0.5)
        constexpr int n = 96;
        double sum = 0;
        for (int v = 0; v < n; ++v)
            for (int u = 0; u < n; ++u) sum += spectrumAt(g, std::hypot((u + 0.5) / n * half, (v + 0.5) / n * half));
        return sum * half * half;
    };
    // (grain `size` times as large has the spectrum at `size` times the frequency, so on the frame measured it is
    // there to 0.5 size, and on this picture to 0.5 size / scale)
    const double all = power(0.5 * size);
    return all > 0 ? std::sqrt(std::clamp(power(0.5 * size / scale) / all, 0.0, 1.0)) : 1;
}

// The lower-triangular L with L Lᵀ = the channels' correlation (shrunk until it has one).
void cholesky(const GrainProfile& g, double saturation, double L[3][3]) {
    for (double shrink = 1; shrink > 0.0; shrink -= 0.05) {
        const double r[3] = {std::clamp(g.correlation[0], -0.999, 0.999), std::clamp(g.correlation[1], -0.999, 0.999),
                             std::clamp(g.correlation[2], -0.999, 0.999)};
        // Less colour: each correlation drawn towards 1.
        const double sat = std::clamp(saturation, 0.0, 2.0);
        auto toward = [&](double v) { return std::clamp(1 - (1 - v) * sat * sat * shrink, -0.999, 0.9995); };
        const double C[3][3] = {{1, toward(r[0]), toward(r[1])}, {toward(r[0]), 1, toward(r[2])}, {toward(r[1]), toward(r[2]), 1}};
        bool ok = true;
        for (int i = 0; i < 3 && ok; ++i)
            for (int j = 0; j <= i; ++j) {
                double sum = C[i][j];
                for (int k = 0; k < j; ++k) sum -= L[i][k] * L[j][k];
                if (i == j) {
                    if (sum <= 1e-9) {
                        ok = false;
                        break;
                    }
                    L[i][i] = std::sqrt(sum);
                } else {
                    L[i][j] = sum / L[j][j];
                }
            }
        if (ok) {
            for (int i = 0; i < 3; ++i)
                for (int j = i + 1; j < 3; ++j) L[i][j] = 0;
            return;
        }
    }
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) L[i][j] = i == j ? 1 : 0;
}

// Three independent fields of unit Gaussian grain with the profile's spectrum, w x h, for frame t: complex white noise
// in the Fourier domain, shaped and transformed back (its real and imaginary parts two fields each). Kept for the
// frames last made, so playing on makes one frame's worth.
struct Fields {
    int w = 0, h = 0;
    std::vector<float> z[3];
};
std::shared_ptr<const Fields> shapedFields(const GrainProfile& g, double scale, uint32_t seed, int64_t t, int w, int h) {
    const std::string key = grainProfileText(g) + '|' + std::to_string(scale) + '|' + std::to_string(seed) + '|' + std::to_string(t) + '|' +
                            std::to_string(w) + 'x' + std::to_string(h);
    static std::mutex m;
    static std::deque<std::pair<std::string, std::shared_ptr<const Fields>>> cache;
    {
        std::lock_guard lock(m);
        for (auto it = cache.begin(); it != cache.end(); ++it)
            if (it->first == key) {
                auto hit = *it;  // (kept as the most recent)
                cache.erase(it);
                cache.push_front(hit);
                return hit.second;
            }
    }
    const int W = nextPow2(w), H = nextPow2(h);
    // The filter: the square root of the spectrum, at frequencies of the measured frame (`scale`: its pixels in one of
    // ours, the grain's size aside, so ours at a frequency are its at that over `scale`).
    std::vector<float> filter(size_t(W) * size_t(H));
    double power = 0;
    for (int v = 0; v < H; ++v)
        for (int u = 0; u < W; ++u) {
            const double fu = double(u < W / 2 ? u : u - W) / W, fv = double(v < H / 2 ? v : v - H) / H;
            const double f = std::hypot(fu, fv) / scale;
            const double s = (u == 0 && v == 0) ? 0.0 : spectrumAt(g, f);
            filter[size_t(v) * size_t(W) + size_t(u)] = float(std::sqrt(s));
            power += s;
        }
    // (unit white noise through it, the inverse unscaled: a variance of the filter's mean power times W H)
    const float norm = power > 0 ? float(1 / std::sqrt(power)) : 0.0f;
    auto made = std::make_shared<Fields>();
    made->w = w, made->h = h;
    std::vector<Cf> a(size_t(W) * size_t(H));
    for (int pair = 0; pair < 2; ++pair) {
        const uint64_t base = mix64(mix64(uint64_t(seed) * 0x100000001b3ULL ^ uint64_t(t)) ^ uint64_t(pair + 1));
        parallelRows(H, [&](int y0, int y1) {
            for (int v = y0; v < y1; ++v)
                for (int u = 0; u < W; ++u) {
                    const size_t i = size_t(v) * size_t(W) + size_t(u);
                    const auto [re, im] = gauss2(mix64(base ^ uint64_t(i)));
                    a[i] = Cf(re, im) * (filter[i] * norm * float(M_SQRT1_2));
                }
        });
        fft2(a, W, H, true);
        for (int k = 0; k < 2 && pair * 2 + k < 3; ++k) {
            std::vector<float>& z = made->z[pair * 2 + k];
            z.resize(size_t(w) * size_t(h));
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const Cf c = a[size_t(y) * size_t(W) + size_t(x)];
                    z[size_t(y) * size_t(w) + size_t(x)] = (k == 0 ? c.real() : c.imag()) * float(M_SQRT2);
                }
        }
    }
    std::lock_guard lock(m);
    cache.emplace_front(key, made);
    if (cache.size() > 4) cache.pop_back();
    return made;
}

// The flow with each point's motion the median of the 5 x 5 points round it: the picture's motion, not the grain's
// (where the picture is flat the flow matches grain to grain by chance, which would make the frames' grain look alike).
FlowField steadyFlow(const FlowField& f) {
    FlowField out = f;
    std::vector<double> xs, ys;
    for (int gy = 0; gy < f.gh; ++gy)
        for (int gx = 0; gx < f.gw; ++gx) {
            xs.clear(), ys.clear();
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const int x = gx + dx, y = gy + dy;
                    if (x < 0 || y < 0 || x >= f.gw || y >= f.gh) continue;
                    xs.push_back(f.v[size_t(y) * size_t(f.gw) + size_t(x)].x), ys.push_back(f.v[size_t(y) * size_t(f.gw) + size_t(x)].y);
                }
            std::nth_element(xs.begin(), xs.begin() + ptrdiff_t(xs.size() / 2), xs.end());
            std::nth_element(ys.begin(), ys.begin() + ptrdiff_t(ys.size() / 2), ys.end());
            out.v[size_t(gy) * size_t(f.gw) + size_t(gx)] = {xs[xs.size() / 2], ys[ys.size() / 2]};
        }
    return out;
}

// The linear light of a premultiplied pixel in `space` (false where it is not opaque).
bool linearOf(const ColorSpace& space, const float* p, double l[3]) {
    if (p[3] < 0.999f) return false;
    for (int c = 0; c < 3; ++c) l[c] = linear(space.transfer, p[c] / p[3]);
    return true;
}

}  // namespace

bool GrainProfile::valid() const {
    double s = 0, a = 0;
    for (double v : spectrum) s += v;
    for (const auto& ch : amplitude)
        for (double v : ch) a += v;
    return s > 0 && a > 0 && width > 0;
}

std::string grainProfileText(const GrainProfile& g) {
    std::ostringstream o;
    o.precision(6);
    o << "grain1 width=" << g.width << " temporal=" << g.temporal << " correlation=" << g.correlation[0] << ',' << g.correlation[1] << ','
      << g.correlation[2];
    const char* names[3] = {"red", "green", "blue"};
    for (int c = 0; c < 3; ++c) {
        o << ' ' << names[c] << '=';
        for (int b = 0; b < GrainProfile::kBands; ++b) o << (b ? "," : "") << g.amplitude[c][b];
    }
    o << " spectrum=";
    for (int i = 0; i < GrainProfile::kBins; ++i) o << (i ? "," : "") << g.spectrum[i];
    return o.str();
}

bool parseGrainProfile(const std::string& text, GrainProfile& out) {
    std::istringstream in(text);
    std::string word;
    if (!(in >> word) || word != "grain1") return false;
    GrainProfile g;
    auto list = [](const std::string& v, double* to, int n) {
        std::istringstream s(v);
        std::string item;
        int i = 0;
        while (std::getline(s, item, ',')) {
            if (i >= n) return false;
            try {
                to[i] = std::stod(item);
            } catch (...) {
                return false;
            }
            if (!std::isfinite(to[i++])) return false;
        }
        return i == n;
    };
    bool seen[7] = {};
    while (in >> word) {
        const size_t eq = word.find('=');
        if (eq == std::string::npos) return false;
        const std::string k = word.substr(0, eq), v = word.substr(eq + 1);
        double one = 0;
        if (k == "width" && list(v, &one, 1) && one >= 1 && one <= 65536) g.width = int(one), seen[0] = true;
        else if (k == "temporal" && list(v, &one, 1)) g.temporal = std::clamp(one, 0.0, 0.45), seen[1] = true;
        else if (k == "correlation" && list(v, g.correlation, 3)) seen[2] = true;
        else if (k == "red" && list(v, g.amplitude[0], GrainProfile::kBands)) seen[3] = true;
        else if (k == "green" && list(v, g.amplitude[1], GrainProfile::kBands)) seen[4] = true;
        else if (k == "blue" && list(v, g.amplitude[2], GrainProfile::kBands)) seen[5] = true;
        else if (k == "spectrum" && list(v, g.spectrum, GrainProfile::kBins)) seen[6] = true;
        else return false;
    }
    if (!std::all_of(std::begin(seen), std::end(seen), [](bool b) { return b; }) || g.width <= 0 || !g.valid()) return false;
    auto atLeast0 = [](const double* v, int n) { return std::all_of(v, v + n, [](double x) { return x >= 0; }); };
    if (!atLeast0(g.spectrum, GrainProfile::kBins) || !atLeast0(g.amplitude[0], 3 * GrainProfile::kBands)) return false;
    for (double& r : g.correlation) r = std::clamp(r, -1.0, 1.0);
    out = g;
    return true;
}

GrainProfile stockGrain(int gauge) {
    // Film Look's grain sizes (a thousandth of the frame's width each), on a 1920-pixel frame; stronger in the
    // smaller gauges, rising as the square root of the light, mostly shared across the colours as dye clouds are.
    const double size[4] = {0.8, 1.0, 1.5, 2.2}, strength[4] = {0.55, 1.0, 1.7, 2.6};
    const int k = std::clamp(gauge, 0, 3);
    GrainProfile g;
    g.width = 1920;
    const double sigma = size[k] * 1.92 * 1.2 / 2;  // the blur of a grain, in pixels
    for (int i = 0; i < GrainProfile::kBins; ++i) {
        const double f = (i + 0.5) / (2.0 * GrainProfile::kBins);
        g.spectrum[i] = std::exp(-4 * M_PI * M_PI * sigma * sigma * f * f);
    }
    for (int b = 0; b < GrainProfile::kBands; ++b) {
        const double light = std::exp2(b - 7.5);
        for (int c = 0; c < 3; ++c) g.amplitude[c][b] = 0.018 * strength[k] * std::sqrt(light) * (c == 2 ? 1.15 : 1.0);
    }
    g.correlation[0] = g.correlation[1] = g.correlation[2] = 0.85;
    return g;
}

double grainSize(const GrainProfile& g) {
    double peak = 0;
    for (double v : g.spectrum) peak = std::max(peak, v);
    if (peak <= 0) return 0;
    for (int i = 1; i < GrainProfile::kBins; ++i)
        if (g.spectrum[i] < peak / 2) {
            // Where it falls to half, between the bins; a Gaussian grain of blur σ falls to half at √ln2 / (2πσ).
            const double a = g.spectrum[i - 1], b = g.spectrum[i];
            const double at = (i - 1 + (a - peak / 2) / std::max(1e-12, a - b) + 0.5) / (2.0 * GrainProfile::kBins);
            return 2 * std::sqrt(std::log(2.0)) / (2 * M_PI * std::max(at, 1e-6));
        }
    return 2 * std::sqrt(std::log(2.0)) / (2 * M_PI * 0.5);
}

double grainAmplitude(const GrainProfile& g, int c, double light) {
    const double x = std::log2(std::max(light, 1e-9)) + 7.5;  // in bands, 0 the first's middle
    if (x <= 0) return g.amplitude[c][0];
    if (x >= GrainProfile::kBands - 1) return g.amplitude[c][GrainProfile::kBands - 1];
    const int i = int(x);
    return g.amplitude[c][i] + (g.amplitude[c][i + 1] - g.amplitude[c][i]) * (x - i);
}

bool measureGrain(const std::vector<Image>& frames, const ColorSpace& space, const GrainRegion& region, GrainProfile& out,
                  std::string* error, const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    const int n = int(frames.size());
    if (n < 7) return fail("Measuring grain needs at least seven frames in a row");
    const int W = frames[0].width, H = frames[0].height;
    for (const Image& f : frames)
        if (f.width != W || f.height != H) return fail("The frames differ in size");
    const int rx = std::clamp(region.x, 0, W), ry = std::clamp(region.y, 0, H);
    const int rw = region.w > 0 ? std::min(region.w, W - rx) : W - rx, rh = region.h > 0 ? std::min(region.h, H - ry) : H - ry;
    const int tilesX = rw / kTile, tilesY = rh / kTile;
    if (tilesX * tilesY < 1) return fail("The region is too small to measure grain in (it needs 64 pixels each way)");

    // What each tile gives: its texture (how much the picture changes in it), its band, its sums.
    struct TileSums {
        double texture = 0;
        int band = 0;
        double sq[3] = {}, cross[3] = {}, next[3] = {};  // r², r_a r_b, r_t r_{t+1}
        double bandSq[GrainProfile::kBands][3] = {};
        double bandCount[GrainProfile::kBands] = {};
        double count = 0;
        double spectrum[GrainProfile::kBins] = {};
        bool ok = false;
    };
    // Measured frames: those with neighbours 2 and 3 away each side and the next one, up to 8 spread over the run.
    std::vector<int> centres;
    for (int i = 3; i + 3 < n; ++i) centres.push_back(i);
    if (centres.size() > 8) {
        std::vector<int> some;
        for (int k = 0; k < 8; ++k) some.push_back(centres[size_t(k) * (centres.size() - 1) / 7]);
        centres = some;
    }
    const int grayW = std::min(W, 960);
    const double up = double(W) / grayW;
    std::vector<TileSums> all;
    std::vector<double> hann(kTile);
    for (int i = 0; i < kTile; ++i) hann[size_t(i)] = 0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / kTile);
    for (size_t ci = 0; ci < centres.size(); ++ci) {
        if (cancel && cancel->load()) return fail("Cancelled");
        const int i = centres[ci];
        const GrayImage here = toGray(frames[size_t(i)], grayW);
        const int offsets[5] = {-3, -2, 2, 3, 1};
        FlowField flow[5];
        for (int k = 0; k < 5; ++k) flow[k] = steadyFlow(denseFlow(here, toGray(frames[size_t(i + offsets[k])], grayW), 8));
        // A neighbour moved onto this frame, a tile at a time, to whole pixels: by the median of the flow over the tile,
        // or near it, when that matches the picture in the tile clearly better (in 4 x 4 averages, so the grain's own
        // likeness does not count) than not moving it; in a flat or striped tile the flow follows the grain, and moving
        // a neighbour to match the grain would hide some of it.
        auto tileShift = [&](int k, int x0, int y0, int& sx, int& sy) {
            std::vector<double> dx, dy;
            for (int y = 4; y < kTile; y += 8)
                for (int x = 4; x < kTile; x += 8) {
                    const Point2 d = flow[k].at((x0 + x) / up, (y0 + y) / up);
                    dx.push_back(d.x * up), dy.push_back(d.y * up);
                }
            auto median = [](std::vector<double>& v) {
                std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
                return v[v.size() / 2];
            };
            const int mx = int(std::lround(median(dx))), my = int(std::lround(median(dy)));
            const Image& a = frames[size_t(i)];
            const Image& b = frames[size_t(i + offsets[k])];
            auto mismatch = [&](int ox, int oy) {
                double sum = 0;
                for (int by = 0; by < kTile; by += 4)
                    for (int bx = 0; bx < kTile; bx += 4) {
                        double d = 0;
                        for (int y = by; y < by + 4; ++y)
                            for (int x = bx; x < bx + 4; ++x)
                                d += double(a.at(x0 + x, y0 + y)[1]) - b.at(std::clamp(x0 + x + ox, 0, W - 1), std::clamp(y0 + y + oy, 0, H - 1))[1];
                        sum += d * d;
                    }
                return sum;
            };
            const int candidates[5][2] = {{mx, my}, {mx - 1, my}, {mx + 1, my}, {mx, my - 1}, {mx, my + 1}};
            const double still = mismatch(0, 0);
            double best = still;
            sx = 0, sy = 0;
            for (const auto& c : candidates) {
                if (c[0] == 0 && c[1] == 0) continue;
                const double m = mismatch(c[0], c[1]);
                if (m < best && m < 0.7 * still) best = m, sx = c[0], sy = c[1];
            }
        };
        auto movedLinear = [&](int k, const int (&shift)[5][2], int x, int y, double l[3]) {
            const int sx = std::clamp(x + shift[k][0], 0, W - 1), sy = std::clamp(y + shift[k][1], 0, H - 1);
            return linearOf(space, frames[size_t(i + offsets[k])].at(sx, sy), l);
        };
        std::vector<TileSums> tiles(size_t(tilesX) * size_t(tilesY));
        parallelRows(tilesY, [&](int ty0, int ty1) {
            std::vector<double> scene(size_t(kTile) * kTile), res(size_t(kTile) * kTile * 3), nxt(size_t(kTile) * kTile * 3);
            std::vector<Cf> row(kTile), grid(size_t(kTile) * kTile);
            for (int ty = ty0; ty < ty1; ++ty)
                for (int tx = 0; tx < tilesX; ++tx) {
                    TileSums& t = tiles[size_t(ty) * size_t(tilesX) + size_t(tx)];
                    const int x0 = rx + tx * kTile, y0 = ry + ty * kTile;
                    int shift[5][2];
                    for (int k = 0; k < 5; ++k) tileShift(k, x0, y0, shift[k][0], shift[k][1]);
                    bool opaque = true;
                    double lightSum = 0;
                    for (int y = 0; y < kTile && opaque; ++y)
                        for (int x = 0; x < kTile && opaque; ++x) {
                            double l[3], m[3] = {0, 0, 0}, nb[3];
                            if (!linearOf(space, frames[size_t(i)].at(x0 + x, y0 + y), l)) opaque = false;
                            for (int k = 0; k < 4 && opaque; ++k) {
                                if (!movedLinear(k, shift, x0 + x, y0 + y, nb)) opaque = false;
                                for (int c = 0; c < 3; ++c) m[c] += nb[c] / 4;
                            }
                            double nl[3];
                            if (opaque && !movedLinear(4, shift, x0 + x, y0 + y, nl)) opaque = false;
                            if (!opaque) break;
                            const size_t at = size_t(y) * kTile + size_t(x);
                            scene[at] = lumaOf(m);
                            lightSum += scene[at];
                            for (int c = 0; c < 3; ++c) res[at * 3 + size_t(c)] = l[c] - m[c], nxt[at * 3 + size_t(c)] = nl[c] - m[c];
                        }
                    if (!opaque) continue;
                    // Texture: the mean step of the picture (its 3 x 3 average) next to the grain's own size there (in a
                    // flat tile, the steps are the averaged grain's alone).
                    double texture = 0;
                    auto smooth = [&](int x, int y) {
                        double s = 0;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                                s += scene[size_t(std::clamp(y + dy, 0, kTile - 1)) * kTile + size_t(std::clamp(x + dx, 0, kTile - 1))];
                        return s / 9;
                    };
                    for (int y = 2; y < kTile - 2; y += 2)
                        for (int x = 2; x < kTile - 2; x += 2)
                            texture += std::hypot(smooth(x + 1, y) - smooth(x - 1, y), smooth(x, y + 1) - smooth(x, y - 1));
                    const double mean = lightSum / (kTile * kTile);
                    double grain = 0;
                    for (size_t at = 0; at < scene.size(); ++at) grain += lumaOf(&res[at * 3]) * lumaOf(&res[at * 3]);
                    grain = std::sqrt(grain / double(scene.size()));
                    t.texture = texture / double((kTile / 2 - 2) * (kTile / 2 - 2)) / std::max(grain, 1e-6);
                    t.band = bandOf(mean);
                    t.ok = true;
                    for (size_t at = 0; at < scene.size(); ++at) {
                        const double* r = &res[at * 3];
                        const int b = bandOf(scene[at]);
                        for (int c = 0; c < 3; ++c) {
                            t.sq[c] += r[c] * r[c];
                            t.next[c] += r[c] * nxt[at * 3 + size_t(c)];
                            t.bandSq[b][c] += r[c] * r[c];
                        }
                        t.bandCount[b] += 1;
                        t.cross[0] += r[0] * r[1], t.cross[1] += r[0] * r[2], t.cross[2] += r[1] * r[2];
                    }
                    t.count = double(scene.size());
                    // The spectrum of its luma grain, windowed, as a share of its power.
                    double mean3 = 0;
                    for (size_t at = 0; at < scene.size(); ++at) mean3 += lumaOf(&res[at * 3]);
                    mean3 /= double(scene.size());
                    for (int y = 0; y < kTile; ++y)
                        for (int x = 0; x < kTile; ++x) {
                            const size_t at = size_t(y) * kTile + size_t(x);
                            grid[at] = Cf(float((lumaOf(&res[at * 3]) - mean3) * hann[size_t(x)] * hann[size_t(y)]), 0.0f);
                        }
                    for (int y = 0; y < kTile; ++y) {
                        std::copy_n(grid.begin() + y * kTile, kTile, row.begin());
                        fftInPlace(row, false);
                        std::copy(row.begin(), row.end(), grid.begin() + y * kTile);
                    }
                    for (int x = 0; x < kTile; ++x) {
                        for (int y = 0; y < kTile; ++y) row[size_t(y)] = grid[size_t(y) * kTile + size_t(x)];
                        fftInPlace(row, false);
                        for (int y = 0; y < kTile; ++y) grid[size_t(y) * kTile + size_t(x)] = row[size_t(y)];
                    }
                    double count[GrainProfile::kBins] = {}, total = 0;
                    for (int v = 0; v < kTile; ++v)
                        for (int u = 0; u < kTile; ++u) {
                            const int fu = u < kTile / 2 ? u : u - kTile, fv = v < kTile / 2 ? v : v - kTile;
                            const int bin = int(std::hypot(fu, fv) * GrainProfile::kBins * 2 / kTile);
                            if (bin >= GrainProfile::kBins || (fu == 0 && fv == 0)) continue;
                            const double pw = std::norm(grid[size_t(v) * kTile + size_t(u)]);
                            t.spectrum[bin] += pw, count[bin] += 1, total += pw;
                        }
                    for (int b = 0; b < GrainProfile::kBins; ++b) t.spectrum[b] = count[b] > 0 && total > 0 ? t.spectrum[b] / count[b] / total : 0;
                }
        });
        for (const TileSums& t : tiles)
            if (t.ok) all.push_back(t);
        if (progress) progress(double(ci + 1) / double(centres.size()));
    }
    if (all.empty()) return fail("Nowhere in the frames is opaque and still enough to measure grain");
    // The flattest tiles: no more textured than half again the flattest fifth, and of a grain no more than three
    // times the usual in their band (where the picture moved in ways the flow missed).
    std::vector<double> textures;
    for (const TileSums& t : all) textures.push_back(t.texture);
    std::nth_element(textures.begin(), textures.begin() + ptrdiff_t(textures.size() / 5), textures.end());
    const double limit = textures[textures.size() / 5] * 1.5 + 1e-9;
    std::vector<const TileSums*> flat;
    for (const TileSums& t : all)
        if (t.texture <= limit) flat.push_back(&t);
    for (int b = 0; b < GrainProfile::kBands; ++b) {
        std::vector<double> vars;
        for (const TileSums* t : flat)
            if (t->band == b) vars.push_back((t->sq[0] + t->sq[1] + t->sq[2]) / t->count);
        if (vars.empty()) continue;
        std::nth_element(vars.begin(), vars.begin() + ptrdiff_t(vars.size() / 2), vars.end());
        const double median = vars[vars.size() / 2];
        std::erase_if(flat, [&](const TileSums* t) { return t->band == b && (t->sq[0] + t->sq[1] + t->sq[2]) / t->count > 3 * median; });
    }
    if (flat.empty()) return fail("Nowhere in the frames is flat enough to measure grain");
    GrainProfile g;
    g.width = W;
    double sq[3] = {}, cross[3] = {}, next[3] = {}, count = 0, bandSq[GrainProfile::kBands][3] = {}, bandCount[GrainProfile::kBands] = {};
    for (const TileSums* t : flat) {
        for (int c = 0; c < 3; ++c) sq[c] += t->sq[c], cross[c] += t->cross[c], next[c] += t->next[c];
        for (int b = 0; b < GrainProfile::kBands; ++b) {
            for (int c = 0; c < 3; ++c) bandSq[b][c] += t->bandSq[b][c];
            bandCount[b] += t->bandCount[b];
        }
        for (int b = 0; b < GrainProfile::kBins; ++b) g.spectrum[b] += t->spectrum[b] / double(flat.size());
        count += t->count;
    }
    // What is left holds a quarter of the neighbours' grain too: its variance is σ²(1.25 + ρ/4), and the next frame's
    // leftover shares σ²(ρ + 1/4) with it, ρ the grain's correlation from frame to frame.
    const double V = (sq[0] + sq[1] + sq[2]) / (3 * count), C = (next[0] + next[1] + next[2]) / (3 * count);
    const double sigma2 = std::max(1e-12, (V - 0.25 * C) / 1.1875);
    g.temporal = std::clamp(C / sigma2 - 0.25, 0.0, 0.45);
    const double share = 1 / (1.25 + 0.25 * g.temporal);
    for (int k = 0; k < 3; ++k) {
        const int a = k < 2 ? 0 : 1, b = k == 0 ? 1 : 2;
        g.correlation[k] = std::clamp(cross[k] / std::sqrt(std::max(1e-30, sq[a] * sq[b])), -0.999, 0.999);
    }
    // Each band's amplitude; a band without enough flat area to measure takes its nearest measured neighbour's,
    // scaled as the square root of the light (as grain and photon noise go).
    bool measured[GrainProfile::kBands] = {};
    for (int b = 0; b < GrainProfile::kBands; ++b)
        if (bandCount[b] >= kTile * kTile / 4) {
            measured[b] = true;
            for (int c = 0; c < 3; ++c) g.amplitude[c][b] = std::sqrt(bandSq[b][c] / bandCount[b] * share);
        }
    for (int b = 0; b < GrainProfile::kBands; ++b) {
        if (measured[b]) continue;
        int best = -1;
        for (int o = 0; o < GrainProfile::kBands; ++o)
            if (measured[o] && (best < 0 || std::abs(o - b) < std::abs(best - b))) best = o;
        if (best < 0) return fail("Too little of the frames is flat to measure grain");
        for (int c = 0; c < 3; ++c) g.amplitude[c][b] = g.amplitude[c][best] * std::exp2(0.5 * (b - best));
    }
    g.spectrum[0] = g.spectrum[1];  // (bin 0 holds only the mean, taken out)
    const double peak = *std::max_element(std::begin(g.spectrum), std::end(g.spectrum));
    if (peak <= 0) return fail("No grain was found");
    for (double& v : g.spectrum) v /= peak;
    out = g;
    return true;
}

bool measureClipGrain(const Project& p, const Sequence& s, const Clip& c, const GrainRegion& region, GrainProfile& out, std::string* error,
                      const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    const MediaItem* m = p.findMedia(c.mediaId);
    if (!m || m->kind != MediaKind::Video || !m->fps.valid()) {
        if (error) *error = "Grain is measured on video clips";
        return false;
    }
    const double mf = m->fps.toDouble(), fps = s.fpsValue();
    const int64_t first = int64_t(std::floor(c.sourceFrameAt(c.start) / fps * mf + 1e-6));
    const int64_t last = int64_t(std::floor(c.sourceFrameAt(c.end() - 1) / fps * mf + 1e-6));
    const int64_t mediaFrames = int64_t(std::floor(m->duration * mf));
    const int64_t lo = std::max<int64_t>(0, std::min(first, last)), hi = std::min(std::max(first, last), mediaFrames - 1);
    if (hi - lo + 1 < 7) {
        if (error) *error = "The clip is too short to measure grain in (it needs seven frames)";
        return false;
    }
    // Up to four runs of seven frames, spread over the clip.
    const int runs = int(std::clamp<int64_t>((hi - lo + 1) / 7, 1, 4));
    GrainProfile sum;
    double weight = 0;
    std::string err;
    for (int r = 0; r < runs; ++r) {
        const int64_t from = runs == 1 ? lo : lo + (hi - 6 - lo) * r / (runs - 1);
        std::vector<Image> frames;
        for (int64_t k = from; k < from + 7; ++k) {
            if (cancel && cancel->load()) {
                if (error) *error = "Cancelled";
                return false;
            }
            Frame16Ptr f = MediaPool::instance().videoFrame(m->path, (double(k) + 0.25) / mf, 0, 0, true);
            if (!f) break;
            Image img = toImage(*f);
            convertColor(img, mediaColorSpace(*m), sequenceColorSpace(s));
            frames.push_back(std::move(img));
        }
        GrainProfile one;
        if (frames.size() < 7 ||
            !measureGrain(frames, sequenceColorSpace(s), region, one, &err,
                          [&](double f) {
                              if (progress) progress((r + f) / runs);
                          },
                          cancel))
            continue;
        // Averaged: amplitudes as variances, the rest as they are.
        for (int ch = 0; ch < 3; ++ch)
            for (int b = 0; b < GrainProfile::kBands; ++b) sum.amplitude[ch][b] += one.amplitude[ch][b] * one.amplitude[ch][b];
        for (int b = 0; b < GrainProfile::kBins; ++b) sum.spectrum[b] += one.spectrum[b];
        for (int k = 0; k < 3; ++k) sum.correlation[k] = weight == 0 ? one.correlation[k] : sum.correlation[k] + one.correlation[k];
        sum.temporal += one.temporal;
        sum.width = one.width;
        weight += 1;
    }
    if (weight == 0) {
        if (error) *error = err.empty() ? std::string("The clip's frames could not be read") : err;
        return false;
    }
    for (int ch = 0; ch < 3; ++ch)
        for (int b = 0; b < GrainProfile::kBands; ++b) sum.amplitude[ch][b] = std::sqrt(sum.amplitude[ch][b] / weight);
    for (double& v : sum.spectrum) v /= weight;
    for (double& v : sum.correlation) v /= weight;
    sum.temporal /= weight;
    out = sum;
    return true;
}

void addGrain(Image& img, const ColorSpace& space, const GrainProfile& g, const GrainSettings& s, int64_t t) {
    if (img.empty() || s.amount <= 0 || !g.valid()) return;
    // Our pixels per pixel of the frame measured, the grain's size aside: the grain scales with the picture.
    const double scale = double(g.width) / img.width / std::clamp(s.size, 0.05, 20.0);
    const float amount = float(s.amount * resolutionGain(g, double(g.width) / img.width, std::clamp(s.size, 0.05, 20.0)));
    const auto now = shapedFields(g, scale, s.seed, t, img.width, img.height);
    const double rho = std::clamp(g.temporal, 0.0, 0.45);
    // Carried over as a moving average of this frame's fields and the last's: a ρ correlation from frame to frame.
    const double alpha = std::sqrt((1 + std::sqrt(1 - 4 * rho * rho)) / 2), beta = rho / alpha;
    const auto before = beta > 1e-4 ? shapedFields(g, scale, s.seed, t - 1, img.width, img.height) : nullptr;
    double L[3][3];
    cholesky(g, s.saturation, L);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                const float a = p[3];
                if (a <= 0) continue;
                const size_t i = size_t(y) * size_t(img.width) + size_t(x);
                double z[3];
                for (int k = 0; k < 3; ++k) z[k] = alpha * now->z[k][i] + (before ? beta * before->z[k][i] : 0.0);
                double l[3];
                for (int c = 0; c < 3; ++c) l[c] = linear(space.transfer, p[c] / a);
                const double light = lumaOf(l);
                for (int c = 0; c < 3; ++c) {
                    const double gn = L[c][0] * z[0] + L[c][1] * z[1] + L[c][2] * z[2];
                    const double v = l[c] + amount * grainAmplitude(g, c, light) * gn;
                    p[c] = float(encoded(space.transfer, v)) * a;
                }
            }
        }
    });
}

namespace {

// Code values to where the grain is kStab strong at every light, per channel, and back: tabulated over code values
// -0.25 .. 1.25 (straight on past them).
constexpr double kStab = 0.02;
struct Stabilizer {
    static constexpr int kSteps = 6000;
    static constexpr double kLow = -0.25, kHigh = 1.25;
    std::vector<double> to[3];  // at code kLow + i (kHigh - kLow) / kSteps
    Stabilizer(const ColorSpace& space, const GrainProfile& g) {
        const double step = (kHigh - kLow) / kSteps;
        for (int c = 0; c < 3; ++c) {
            to[c].resize(kSteps + 1);
            double t = 0;
            to[c][0] = 0;
            for (int i = 0; i < kSteps; ++i) {
                const double a = linear(space.transfer, kLow + i * step), b = linear(space.transfer, kLow + (i + 1) * step);
                const double amp = std::max(1e-6, grainAmplitude(g, c, std::max(1e-9, (a + b) / 2)));
                t += std::max(0.0, b - a) / amp * kStab;
                to[c][size_t(i) + 1] = t;
            }
        }
    }
    double forward(int c, double v) const {
        const double x = (v - kLow) / (kHigh - kLow) * kSteps;
        if (x <= 0) return to[c][0] + x * (to[c][1] - to[c][0]);
        if (x >= kSteps) return to[c][kSteps] + (x - kSteps) * (to[c][kSteps] - to[c][kSteps - 1]);
        const int i = int(x);
        return to[c][size_t(i)] + (to[c][size_t(i) + 1] - to[c][size_t(i)]) * (x - i);
    }
    double back(int c, double t) const {
        const std::vector<double>& v = to[c];
        const double step = (kHigh - kLow) / kSteps;
        if (t <= v[0]) return kLow + (t - v[0]) / std::max(1e-12, v[1] - v[0]) * step;
        if (t >= v[kSteps]) return kHigh + (t - v[kSteps]) / std::max(1e-12, v[kSteps] - v[kSteps - 1]) * step;
        const size_t i = size_t(std::upper_bound(v.begin(), v.end(), t) - v.begin()) - 1;
        const double span = v[i + 1] - v[i];
        return kLow + (double(i) + (span > 0 ? (t - v[i]) / span : 0)) * step;
    }
    Image apply(const Image& img, bool inverse) const {
        Image out = img;
        parallelRows(out.height, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float* p = out.row(y);
                for (int x = 0; x < out.width; ++x, p += 4) {
                    const float a = p[3];
                    if (a <= 0) continue;
                    for (int c = 0; c < 3; ++c) p[c] = float((inverse ? back(c, p[c] / a) : forward(c, p[c] / a)) * a);
                }
            }
        });
        return out;
    }
};

}  // namespace

Image removeGrain(const Image& current, const std::vector<const Image*>& neighbours, const std::vector<MotionFn>& motion,
                  const ColorSpace& space, const GrainProfile& g) {
    if (!g.valid()) return current;
    const Stabilizer stab(space, g);
    const Image here = stab.apply(current, false);
    std::vector<Image> others;
    std::vector<const Image*> around;
    others.reserve(neighbours.size());
    for (const Image* n : neighbours) others.push_back(stab.apply(*n, false));
    for (const Image& n : others) around.push_back(&n);
    DenoiseSettings ds;
    ds.noise = kStab * resolutionGain(g, double(g.width) / current.width, 1);
    return stab.apply(denoiseFrame(here, around, ds, motion), true);
}

GrainProfile effectGrainProfile(const Effect& e) {
    GrainProfile g;
    if (int(std::lround(e.p("source", 0, 0))) == 0 && parseGrainProfile(e.s("profile"), g)) return g;
    const int gauge = int(std::lround(e.p("source", 0, 0))) - 1;
    return stockGrain(gauge < 0 ? 1 : gauge);
}

void grainMatch(const Effect& e, FrameTime t, Image& img) {
    // (Replace Grain's taking the plate's grain out first is done on the source frames, with their neighbours; the grain
    // put back allows for what that leaves.)
    GrainSettings s;
    s.amount = e.p("amount", t, 100) / 100;
    if (std::lround(e.p("mode", 0, 0)) == 1) s.amount *= std::sqrt(1 - 1.0 / (2 * kReplaceGrainFrames + 1));
    s.size = e.p("size", t, 100) / 100;
    s.saturation = e.p("saturation", t, 100) / 100;
    s.seed = uint32_t(std::max(0.0, e.p("seed", t, 0)));
    addGrain(img, currentWorkingSpace(), effectGrainProfile(e), s, t);
}

}  // namespace montage
