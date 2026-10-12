#include "ColorSlice.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>

#include "TransferTables.h"

namespace montage {

namespace {

constexpr float kCctToe = 0.0078125f, kCctA = 10.5402377416545f, kCctB = 0.0729055341958355f;
// One stop in ACEScct's log part.
constexpr float kStop = 1.0f / 17.52f;

float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

// HLG decodes to scene light with its peak at 1; its reference white (75%) is brought to 1 like SDR white and PQ's
// 203 nits (as the HDR Palette does).
float toWhite(const ColorSpace& space) { return space.transfer == Transfer::Hlg ? float(1 / toLinear(Transfer::Hlg, 0.75)) : 1.0f; }

void lumaWeights(const ColorSpace& space, float w[3]) {
    double m[9];
    primariesToXyz(space.primaries, m);
    for (int k = 0; k < 3; ++k) w[k] = float(m[3 + k]);
}

// A raised cosine: 1 at no distance, 0 from `width` on.
float cosineWeight(float distance, float width) { return distance >= width ? 0.0f : 0.5f + 0.5f * std::cos(float(M_PI) * distance / width); }

float hueDistance(float a, float b) {
    const float d = std::fabs(std::fmod(a - b + 540.0f, 360.0f) - 180.0f);
    return d;
}

// The mean over a (2r+1)-square box, the edges counting only the pixels inside. Both passes walk the rows in order
// (the vertical one keeps a running sum per column of a band of columns).
std::vector<float> boxMean(const std::vector<float>& in, int W, int H, int r) {
    std::vector<float> mid(in.size()), out(in.size());
    parallelRows(H, [&](int y0, int y1) {
        std::vector<double> sum(size_t(W) + 1);
        for (int y = y0; y < y1; ++y) {
            const float* row = in.data() + size_t(y) * W;
            sum[0] = 0;
            for (int x = 0; x < W; ++x) sum[size_t(x) + 1] = sum[size_t(x)] + row[x];
            for (int x = 0; x < W; ++x) {
                const int a = std::max(0, x - r), b = std::min(W - 1, x + r);
                mid[size_t(y) * W + x] = float((sum[size_t(b) + 1] - sum[size_t(a)]) / (b - a + 1));
            }
        }
    });
    parallelRows(W, [&](int x0, int x1) {
        const int n = x1 - x0;
        std::vector<double> sum(size_t(n), 0.0);
        for (int y = 0; y <= std::min(r, H - 1); ++y)
            for (int i = 0; i < n; ++i) sum[size_t(i)] += mid[size_t(y) * W + x0 + i];
        for (int y = 0; y < H; ++y) {
            const int a = std::max(0, y - r), b = std::min(H - 1, y + r);
            const double inv = 1.0 / (b - a + 1);
            for (int i = 0; i < n; ++i) out[size_t(y) * W + x0 + i] = float(sum[size_t(i)] * inv);
            if (y + r + 1 < H)
                for (int i = 0; i < n; ++i) sum[size_t(i)] += mid[size_t(y + r + 1) * W + x0 + i];
            if (y - r >= 0)
                for (int i = 0; i < n; ++i) sum[size_t(i)] -= mid[size_t(y - r) * W + x0 + i];
        }
    });
    return out;
}

// A Gaussian blur of any (fractional) size, the edges counting only the pixels inside.
std::vector<float> gaussianMean(const std::vector<float>& in, int W, int H, double sigma) {
    sigma = std::max(0.3, sigma);
    const int r = int(std::ceil(3 * sigma));
    std::vector<float> k(size_t(2 * r + 1));
    for (int i = -r; i <= r; ++i) k[size_t(i + r)] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
    std::vector<float> mid(in.size()), out(in.size());
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* row = in.data() + size_t(y) * W;
            for (int x = 0; x < W; ++x) {
                float acc = 0, wsum = 0;
                for (int i = std::max(-r, -x); i <= std::min(r, W - 1 - x); ++i) acc += k[size_t(i + r)] * row[x + i], wsum += k[size_t(i + r)];
                mid[size_t(y) * W + x] = acc / wsum;
            }
        }
    });
    parallelRows(H, [&](int y0, int y1) {
        std::vector<float> acc(static_cast<size_t>(W));
        for (int y = y0; y < y1; ++y) {
            std::fill(acc.begin(), acc.end(), 0.0f);
            float wsum = 0;
            for (int i = std::max(-r, -y); i <= std::min(r, H - 1 - y); ++i) {
                const float w = k[size_t(i + r)];
                const float* row = mid.data() + size_t(y + i) * W;
                for (int x = 0; x < W; ++x) acc[size_t(x)] += w * row[x];
                wsum += w;
            }
            for (int x = 0; x < W; ++x) out[size_t(y) * W + x] = acc[size_t(x)] / wsum;
        }
    });
    return out;
}

// A blur of `plane` weighted by `weight` (the pixels' alpha), so transparent pixels round a matte do not count; no
// weight: a plain one.
template <typename Blur>
std::vector<float> weightedBlur(const std::vector<float>& plane, const std::vector<float>& weight, Blur blur) {
    if (weight.empty()) return blur(plane);
    std::vector<float> pw(plane.size());
    for (size_t i = 0; i < plane.size(); ++i) pw[i] = plane[i] * weight[i];
    std::vector<float> num = blur(pw);
    const std::vector<float> den = blur(weight);
    for (size_t i = 0; i < num.size(); ++i) num[i] = den[i] > 1e-6f ? num[i] / den[i] : plane[i];
    return num;
}

}  // namespace

float acesCct(float l) { return l <= kCctToe ? kCctA * l + kCctB : (std::log2(l) + 9.72f) / 17.52f; }
float acesCctToLinear(float c) { return c <= kCctA * kCctToe + kCctB ? (c - kCctB) / kCctA : std::exp2(c * 17.52f - 9.72f); }

const std::vector<HueVector>& colorSliceVectors() {
    static const std::vector<HueVector> v{{"red", "Red", 0},    {"yellow", "Yellow", 60}, {"green", "Green", 120},
                                          {"cyan", "Cyan", 180}, {"blue", "Blue", 240},     {"magenta", "Magenta", 300}};
    return v;
}

void colorSlice(const Effect& e, FrameTime t, const ColorSpace& space, Image& img) {
    struct Vec {
        float centre, width, hue, sat, density;
    };
    std::vector<Vec> vecs;
    for (const HueVector& v : colorSliceVectors()) {
        const std::string n = v.name;
        const Vec vec{float(v.centre), 60, float(e.p(n + "_hue", t)), float(e.p(n + "_saturation", t, 1)), float(e.p(n + "_density", t))};
        // (A vector left as it is adds nothing to any colour's change.)
        if (vec.hue != 0 || vec.sat != 1 || vec.density != 0) vecs.push_back(vec);
    }
    bool any = !vecs.empty();
    const Vec skin{float(e.p("skin_centre", t, 25)), std::max(1.0f, float(e.p("skin_width", t, 25))), float(e.p("skin_hue", t)),
                   float(e.p("skin_saturation", t, 1)), float(e.p("skin_density", t))};
    const float globalSat = float(e.p("saturation", t, 1)), globalDensity = float(e.p("density", t));
    const float depth = float(e.p("depth", t, 100)) / 100, protect = float(e.p("protect_highlights", t, 50)) / 100;
    const float mix = float(std::clamp(e.p("mix", t, 100) / 100, 0.0, 1.0));
    const bool subtractive = e.p("subtractive", t, 1) >= 0.5;
    any |= skin.hue != 0 || skin.sat != 1 || skin.density != 0 || globalSat != 1 || globalDensity != 0;
    if (!any || mix <= 0) return;

    const TransferTables tables(space.transfer);
    const float white = toWhite(space);
    // Hues are those of Rec.709's primaries whatever the working space, so a grade lands alike in any of them (colours
    // outside Rec.709 have negative channels there, which ACEScct's straight toe takes).
    const bool ref709 = space.primaries != Primaries::Bt709;
    double toRef[9], fromRef[9];
    primariesMatrix(space.primaries, Primaries::Bt709, toRef);
    primariesMatrix(Primaries::Bt709, space.primaries, fromRef);
    auto mul = [](const double m[9], float v[3]) {
        const float a = v[0], b = v[1], c = v[2];
        for (int r = 0; r < 3; ++r) v[r] = float(m[r * 3] * a + m[r * 3 + 1] * b + m[r * 3 + 2] * c);
    };
    const float lw[3] = {0.2126f, 0.7152f, 0.0722f};
    // (Chroma in the log encoding at which a colour counts as fully saturated: four stops between its brightest and
    // darkest channels.)
    constexpr float kFull = 4 * kStop;
    // Darkening at full density and saturation, in stops.
    constexpr float kDensityStops = 1.0f;

    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                const float a = p[3];
                if (a <= 0) continue;
                float code[3], lin[3], c[3];
                for (int k = 0; k < 3; ++k) {
                    code[k] = p[k] / a;
                    lin[k] = tables.decode(code[k]) * white;
                }
                const float linMax = std::max({lin[0], lin[1], lin[2]});
                if (ref709) mul(toRef, lin);
                for (int k = 0; k < 3; ++k) c[k] = acesCct(lin[k]);
                const float mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]}), ch = mx - mn;
                if (ch <= 1e-6f) continue;  // (a neutral colour has no hue, saturation or density to change)
                float h = 0;
                if (ch > 1e-6f) {
                    if (mx == c[0]) h = std::fmod((c[1] - c[2]) / ch + 6.0f, 6.0f);
                    else if (mx == c[1]) h = (c[2] - c[0]) / ch + 2;
                    else h = (c[0] - c[1]) / ch + 4;
                    h *= 60;
                }
                // Each vector's share of this hue: the six share it all between them (each to its neighbours' centres),
                // skin takes its part first.
                const float ws = cosineWeight(hueDistance(h, skin.centre), skin.width);
                float hueShift = ws * skin.hue, sat = 1 + ws * (skin.sat - 1), density = ws * skin.density;
                for (const Vec& v : vecs) {
                    const float w = (1 - ws) * cosineWeight(hueDistance(h, v.centre), v.width);
                    if (w <= 0) continue;
                    hueShift += w * v.hue;
                    sat += w * (v.sat - 1);
                    density += w * v.density;
                }
                sat = std::max(0.0f, sat * globalSat);
                density += globalDensity;
                if (hueShift == 0 && sat == 1 && density == 0) continue;

                float o[3] = {c[0], c[1], c[2]};
                if (hueShift != 0 && ch > 1e-6f) {
                    // The same brightest channel and spread, at the new hue.
                    const float nh = std::fmod(h + hueShift + 720.0f, 360.0f) / 60;
                    const float xx = ch * (1 - std::fabs(std::fmod(nh, 2.0f) - 1)), m = mx - ch;
                    float rr, gg, bb;
                    switch (int(nh) % 6) {
                        case 0: rr = ch, gg = xx, bb = 0; break;
                        case 1: rr = xx, gg = ch, bb = 0; break;
                        case 2: rr = 0, gg = ch, bb = xx; break;
                        case 3: rr = 0, gg = xx, bb = ch; break;
                        case 4: rr = xx, gg = 0, bb = ch; break;
                        default: rr = ch, gg = 0, bb = xx; break;
                    }
                    o[0] = rr + m, o[1] = gg + m, o[2] = bb + m;
                }
                // Subtractive: the brightest channel stays and the others move away from it (or towards it) in stops, as
                // dyes do.
                // (Never below no light, unless the colour was already: ACEScct's toe is straight, not stops.)
                if (subtractive && sat != 1)
                    for (int k = 0; k < 3; ++k) o[k] = std::max(mx - (mx - o[k]) * sat, std::min(o[k], kCctB));
                float l[3];
                for (int k = 0; k < 3; ++k) l[k] = acesCctToLinear(o[k]);
                if (!subtractive && sat != 1) {
                    // Additive: round the brightness, which stays.
                    const float yy = lw[0] * l[0] + lw[1] * l[1] + lw[2] * l[2];
                    for (float& v : l) v = yy + (v - yy) * sat;
                }
                // Density: richer colours darker, by how saturated they are now.
                const float s = std::min(1.0f, ch * sat / kFull);
                if (density != 0 && s > 0) {
                    const float g = std::exp2(-kDensityStops * depth * density * s);
                    for (float& v : l) v *= g;
                }
                if (ref709) mul(fromRef, l);
                const float keep = 1 - protect * smoothstep(0.5f, 1.0f, linMax);
                const float amount = mix * keep;
                for (int k = 0; k < 3; ++k) {
                    const float out = tables.encode(l[k] / white);
                    p[k] = (code[k] + (out - code[k]) * amount) * a;
                }
            }
        }
    });
}

void textureDetail(const Effect& e, FrameTime t, const ColorSpace& space, Image& img, double pixelScale) {
    const float mid = float(e.p("midtone_detail", t)) / 100, pop = float(e.p("texture_pop", t)) / 100;
    const double scale = std::max(0.05, e.p("scale", t, 1) * pixelScale);
    const float floor = float(e.p("noise_floor", t, 10)) / 100 * 0.35f * kStop;
    if ((mid == 0 && pop == 0) || img.width <= 0 || img.height <= 0) return;
    const int W = img.width, H = img.height;
    const size_t N = size_t(W) * size_t(H);
    const TransferTables tables(space.transfer);
    const float white = toWhite(space);
    float lw[3];
    lumaWeights(space, lw);
    // Brightness in the log encoding, and alpha where any pixel is not opaque (so a matte's transparent surround is not
    // taken for black next to it).
    std::vector<float> Y(N), alpha(N);
    std::atomic<bool> opaque = true;
    parallelRows(H, [&](int y0, int y1) {
        bool rowsOpaque = true;
        for (int y = y0; y < y1; ++y) {
            const float* p = img.row(y);
            for (int x = 0; x < W; ++x, p += 4) {
                const float a = p[3];
                float yl = 0;
                if (a > 0)
                    for (int k = 0; k < 3; ++k) yl += lw[k] * tables.decode(p[k] / a) * white;
                Y[size_t(y) * W + x] = acesCct(std::max(0.0f, yl));
                alpha[size_t(y) * W + x] = std::clamp(a, 0.0f, 1.0f);
                rowsOpaque &= a >= 1;
            }
        }
        if (!rowsOpaque) opaque = false;
    });
    if (opaque) std::vector<float>().swap(alpha);
    auto rows = [&](const std::function<void(size_t, size_t)>& fn) {
        parallelRows(H, [&](int y0, int y1) { fn(size_t(y0) * W, size_t(y1) * W); });
    };
    std::vector<float> delta(N, 0.0f);
    if (mid != 0) {
        // Mid-tone detail: brightness less an edge-preserving (self-guided) smoothing of it, so an edge of a couple of
        // stops or more keeps most of itself and gets no halo.
        const int r = std::max(1, int(std::lround(24 * scale)));
        auto box = [&](const std::vector<float>& v) { return boxMean(v, W, H, r); };
        constexpr float eps = 0.002f;
        std::vector<float> A(N);
        rows([&](size_t i0, size_t i1) {
            for (size_t i = i0; i < i1; ++i) A[i] = Y[i] * Y[i];
        });
        std::vector<float> B = weightedBlur(Y, alpha, box);  // the mean, then B
        A = weightedBlur(A, alpha, box);                     // the mean square, then A
        rows([&](size_t i0, size_t i1) {
            for (size_t i = i0; i < i1; ++i) {
                const float mean = B[i], var = std::max(0.0f, A[i] - mean * mean);
                A[i] = var / (var + eps);
                B[i] = mean - A[i] * mean;
            }
        });
        A = weightedBlur(A, alpha, box);
        B = weightedBlur(B, alpha, box);
        // Strongest at mid grey (ACEScct 0.41), none in deep shadow or well above white.
        rows([&](size_t i0, size_t i1) {
            for (size_t i = i0; i < i1; ++i) {
                const float d = (Y[i] - 0.41f) / 0.25f, w = d * d >= 1 ? 0.0f : (1 - d * d) * (1 - d * d);
                delta[i] += mid * w * (Y[i] - (A[i] * Y[i] + B[i]));
            }
        });
    }
    if (pop != 0) {
        // Texture: brightness less a fine Gaussian blur of it (1.5 px), the smallest differences (noise) left alone.
        const std::vector<float> blur = weightedBlur(Y, alpha, [&](const std::vector<float>& v) { return gaussianMean(v, W, H, 1.5 * scale); });
        rows([&](size_t i0, size_t i1) {
            for (size_t i = i0; i < i1; ++i) {
                const float d = Y[i] - blur[i];
                const float keep = floor > 0 ? smoothstep(floor, 2 * floor, std::fabs(d)) : 1.0f;
                delta[i] += pop * keep * d;
            }
        });
    }
    // The change in brightness as a gain on every channel alike, so colours keep their balance.
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < W; ++x, p += 4) {
                const size_t i = size_t(y) * W + x;
                const float a = p[3];
                if (a <= 0 || delta[i] == 0) continue;
                const float before = acesCctToLinear(Y[i]), after = acesCctToLinear(Y[i] + delta[i]);
                if (before <= 1e-7f) continue;
                const float g = std::clamp(std::max(0.0f, after) / before, 1.0f / 16, 16.0f);
                for (int k = 0; k < 3; ++k) p[k] = tables.encode(tables.decode(p[k] / a) * g) * a;
            }
        }
    });
}

}  // namespace montage
