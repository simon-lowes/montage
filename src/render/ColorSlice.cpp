#include "ColorSlice.h"

#include <algorithm>
#include <cmath>

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

// The mean over a (2r+1)-square box, the edges counting only the pixels inside.
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
        std::vector<double> sum(size_t(H) + 1);
        for (int x = x0; x < x1; ++x) {
            sum[0] = 0;
            for (int y = 0; y < H; ++y) sum[size_t(y) + 1] = sum[size_t(y)] + mid[size_t(y) * W + x];
            for (int y = 0; y < H; ++y) {
                const int a = std::max(0, y - r), b = std::min(H - 1, y + r);
                out[size_t(y) * W + x] = float((sum[size_t(b) + 1] - sum[size_t(a)]) / (b - a + 1));
            }
        }
    });
    return out;
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
    bool any = false;
    for (const HueVector& v : colorSliceVectors()) {
        const std::string n = v.name;
        vecs.push_back({float(v.centre), 60, float(e.p(n + "_hue", t)), float(e.p(n + "_saturation", t, 1)), float(e.p(n + "_density", t))});
        any |= vecs.back().hue != 0 || vecs.back().sat != 1 || vecs.back().density != 0;
    }
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
    float lw[3];
    lumaWeights(space, lw);
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
                    c[k] = acesCct(lin[k]);
                }
                const float linMax = std::max({lin[0], lin[1], lin[2]});
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
                if (subtractive && sat != 1)
                    for (float& v : o) v = mx - (mx - v) * sat;
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
    const TransferTables tables(space.transfer);
    const float white = toWhite(space);
    float lw[3];
    lumaWeights(space, lw);
    // Brightness in the log encoding.
    std::vector<float> Y(size_t(W) * size_t(H));
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* p = img.row(y);
            for (int x = 0; x < W; ++x, p += 4) {
                const float a = p[3];
                float yl = 0;
                if (a > 0)
                    for (int k = 0; k < 3; ++k) yl += lw[k] * tables.decode(p[k] / a) * white;
                Y[size_t(y) * W + x] = acesCct(std::max(0.0f, yl));
            }
        }
    });
    std::vector<float> delta(Y.size(), 0.0f);
    if (mid != 0) {
        // Mid-tone detail: brightness less an edge-preserving (guided) smoothing of it, so an edge of a couple of stops
        // or more keeps most of itself and gets no halo.
        const int r = std::max(1, int(std::lround(24 * scale)));
        constexpr float eps = 0.002f;
        std::vector<float> sq(Y.size());
        for (size_t i = 0; i < Y.size(); ++i) sq[i] = Y[i] * Y[i];
        const std::vector<float> mean = boxMean(Y, W, H, r), meanSq = boxMean(sq, W, H, r);
        std::vector<float> A(Y.size()), B(Y.size());
        for (size_t i = 0; i < Y.size(); ++i) {
            const float var = std::max(0.0f, meanSq[i] - mean[i] * mean[i]);
            A[i] = var / (var + eps);
            B[i] = mean[i] - A[i] * mean[i];
        }
        const std::vector<float> mA = boxMean(A, W, H, r), mB = boxMean(B, W, H, r);
        // Strongest round mid grey (ACEScct 0.41), none in deep shadow or well above white.
        for (size_t i = 0; i < Y.size(); ++i) {
            const float d = (Y[i] - 0.43f) / 0.25f, w = d * d >= 1 ? 0.0f : (1 - d * d) * (1 - d * d);
            delta[i] += mid * w * (Y[i] - (mA[i] * Y[i] + mB[i]));
        }
    }
    if (pop != 0) {
        // Texture: brightness less a fine blur of it (three boxes, near a Gaussian of 1.5 px), the smallest differences
        // (noise) left alone.
        const double sigma = 1.5 * scale;
        const int r = std::max(1, int(std::lround((std::sqrt(1 + 4 * sigma * sigma) - 1) / 2)));
        std::vector<float> blur = boxMean(boxMean(boxMean(Y, W, H, r), W, H, r), W, H, r);
        for (size_t i = 0; i < Y.size(); ++i) {
            const float d = Y[i] - blur[i];
            const float keep = floor > 0 ? smoothstep(floor, 2 * floor, std::fabs(d)) : 1.0f;
            delta[i] += pop * keep * d;
        }
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
                for (int k = 0; k < 3; ++k) p[k] = tables.encode(tables.decode(p[k] / a) * white * g / white) * a;
            }
        }
    });
}

}  // namespace montage
