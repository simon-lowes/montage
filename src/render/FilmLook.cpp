#include "FilmLook.h"

#include <algorithm>
#include <cmath>

#include "Processing.h"

namespace montage {

namespace {

uint32_t mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// Smooth noise in time, -1..1: sines at unrelated rates, so it never visibly repeats.
double wander(double t, double seed) {
    return (std::sin(t * 0.37 + seed) * 0.5 + std::sin(t * 0.91 + seed * 2.3) * 0.3 + std::sin(t * 2.13 + seed * 4.1) * 0.2);
}

struct Gauge {
    double grain, grainSize, weave, softness;
};
Gauge gaugeOf(int g) {
    switch (std::clamp(g, 0, 3)) {
        case 0: return {0.55, 0.8, 0.3, 0.5};  // 65mm
        case 2: return {1.7, 1.5, 1.8, 1.6};   // 16mm
        case 3: return {2.6, 2.2, 3.0, 2.4};   // Super 8
        default: return {1.0, 1.0, 1.0, 1.0};  // 35mm
    }
}

float luma(const float* p) { return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; }

// What is brighter than `threshold` (softly), blurred by `radius` px.
Image highlights(const Image& img, float threshold, double radius) {
    Image h = img;
    for (size_t i = 0; i < h.px.size(); i += 4) {
        float* p = &h.px[i];
        const float a = p[3];
        const float l = a > 0 ? luma(p) / a : 0;
        const float k = std::clamp((l - threshold) / std::max(1e-3f, 1 - threshold), 0.0f, 1.0f);
        for (int c = 0; c < 4; ++c) p[c] *= k * k;
    }
    if (radius >= 0.5) gaussianBlur(h, radius);
    return h;
}

}  // namespace

FilmLookSettings filmLookSettings(const Effect& e, FrameTime t) {
    FilmLookSettings s;
    s.gauge = int(std::lround(e.p("gauge", t, 1)));
    s.halation = e.p("halation", t, 30) / 100;
    s.bloom = e.p("bloom", t, 20) / 100;
    s.grain = e.p("grain", t, 30) / 100;
    s.weave = e.p("weave", t, 20) / 100;
    s.vignette = e.p("vignette", t, 25) / 100;
    s.softness = e.p("softness", t, 10) / 100;
    s.flicker = e.p("flicker", t) / 100;
    s.aberration = e.p("aberration", t) / 100;
    s.fade = e.p("fade", t, 10) / 100;
    return s;
}

FilmLookPlan filmLookPlan(int W, int H, const FilmLookSettings& s, FrameTime t) {
    FilmLookPlan plan;
    if (W <= 0 || H <= 0) return plan;
    const Gauge g = gaugeOf(s.gauge);
    const double unit = W / 1000.0;  // a thousandth of the frame's width
    // Gate weave and aberration: the frame moved, its red and blue a little apart.
    plan.wx = wander(double(t) * 0.25, 1.7) * s.weave * g.weave * unit * 1.2;
    plan.wy = wander(double(t) * 0.21, 5.3) * s.weave * g.weave * unit * 1.6;
    plan.aberration = s.aberration * unit * 3;
    plan.moved = std::fabs(plan.wx) > 1e-3 || std::fabs(plan.wy) > 1e-3 || plan.aberration > 1e-3;
    if (s.softness > 0) {
        plan.softRadius = std::max(0.5, s.softness * g.softness * unit * 6);
        plan.softMix = float(std::min(1.0, s.softness * 1.5));
    }
    plan.halation = s.halation, plan.bloom = s.bloom;
    plan.haloRadius = unit * 16, plan.bloomRadius = unit * 60;
    plan.flicker = 1 + float(s.flicker * 0.08 * std::sin(double(mix(uint32_t(t) * 2654435761U) & 0xffff) / 65535.0 * 6.2832));
    plan.lift = float(s.fade * 0.08);
    plan.vignette = s.vignette;
    if (s.grain > 0) {
        // Grain: noise on a grid the grain's size, new each frame.
        plan.grainSize = std::max(0.6, g.grainSize * unit * 1.2);
        plan.gridW = std::max(1, int(std::ceil(W / plan.grainSize))) + 1, plan.gridH = std::max(1, int(std::ceil(H / plan.grainSize))) + 1;
        plan.noise.resize(size_t(plan.gridW) * size_t(plan.gridH) * 3);
        const uint32_t seed = mix(uint32_t(t) * 2654435761U + 977U);
        for (int y = 0; y < plan.gridH; ++y)
            for (int x = 0; x < plan.gridW; ++x)
                for (int c = 0; c < 3; ++c) {
                    // Mostly shared across the colours, as film's dye clouds are.
                    const uint32_t shared = mix(seed ^ mix(uint32_t(x) * 73856093U ^ uint32_t(y) * 19349663U));
                    const uint32_t own = mix(shared ^ uint32_t(c + 1) * 83492791U);
                    plan.noise[(size_t(y) * size_t(plan.gridW) + size_t(x)) * 3 + size_t(c)] =
                        (float(shared & 0xffff) / 65535.0f - 0.5f) * 0.8f + (float(own & 0xffff) / 65535.0f - 0.5f) * 0.2f;
                }
        plan.grain = float(s.grain * g.grain * 0.09);
    }
    return plan;
}

void filmLook(Image& img, const FilmLookSettings& s, FrameTime t) {
    const int W = img.width, H = img.height;
    if (img.empty()) return;
    const FilmLookPlan plan = filmLookPlan(W, H, s, t);
    const double wx = plan.wx, wy = plan.wy, ab = plan.aberration;
    if (plan.moved) {
        const Image src = img;
        const double cx = W / 2.0, cy = H / 2.0, diag = std::hypot(cx, cy);
        auto sampleAt = [&](double x, double y, int c) {
            x = std::clamp(x, 0.0, double(W - 1)), y = std::clamp(y, 0.0, double(H - 1));
            const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
            const float fx = float(x - x0), fy = float(y - y0);
            const float a = src.at(x0, y0)[c] + (src.at(x1, y0)[c] - src.at(x0, y0)[c]) * fx;
            const float b = src.at(x0, y1)[c] + (src.at(x1, y1)[c] - src.at(x0, y1)[c]) * fx;
            return a + (b - a) * fy;
        };
        parallelRows(H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < W; ++x) {
                    float* p = img.at(x, y);
                    const double sx = x - wx, sy = y - wy;
                    const double rx = (x - cx) / diag, ry = (y - cy) / diag;
                    p[0] = sampleAt(sx + rx * ab, sy + ry * ab, 0);
                    p[1] = sampleAt(sx, sy, 1);
                    p[2] = sampleAt(sx - rx * ab, sy - ry * ab, 2);
                    p[3] = sampleAt(sx, sy, 3);
                }
        });
    }
    // Softness: a little of the picture's sharpness gone.
    if (plan.softRadius > 0) {
        Image soft = img;
        gaussianBlur(soft, plan.softRadius);
        const float k = plan.softMix;
        for (size_t i = 0; i < img.px.size(); ++i) img.px[i] += (soft.px[i] - img.px[i]) * k;
    }
    // Halation (red-orange, close round highlights) and bloom (neutral, wider).
    if (plan.halation > 0 || plan.bloom > 0) {
        const Image halo = plan.halation > 0 ? highlights(img, 0.6f, plan.haloRadius) : Image();
        const Image bloom = plan.bloom > 0 ? highlights(img, 0.75f, plan.bloomRadius) : Image();
        const float tint[3] = {1.0f, 0.28f, 0.06f};
        for (size_t i = 0; i < img.px.size(); i += 4) {
            float* p = &img.px[i];
            const float hl = halo.empty() ? 0.0f : luma(&halo.px[i]);
            for (int c = 0; c < 3; ++c) {
                if (!halo.empty()) p[c] += float(plan.halation * 1.6) * tint[c] * hl;
                if (!bloom.empty()) p[c] += float(plan.bloom * 0.8) * bloom.px[i + size_t(c)];
            }
            const float extra = std::max({halo.empty() ? 0.0f : float(plan.halation) * halo.px[i + 3], bloom.empty() ? 0.0f : float(plan.bloom) * bloom.px[i + 3]});
            p[3] = std::min(1.0f, p[3] + extra);
        }
    }
    // Flicker, lifted blacks and the vignette, per pixel.
    const float flick = plan.flicker, lift = plan.lift;
    const double cx = W / 2.0, cy = H / 2.0, corner = std::hypot(cx, cy);
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                float* p = img.at(x, y);
                const float a = p[3];
                if (a <= 0) continue;
                const double r = std::hypot(x + 0.5 - cx, y + 0.5 - cy) / corner;
                const double v = 1 - plan.vignette * 0.65 * std::clamp((r - 0.35) / 0.65, 0.0, 1.0) * std::clamp((r - 0.35) / 0.65, 0.0, 1.0);
                for (int c = 0; c < 3; ++c) p[c] = (lift * a + p[c] * (1 - lift)) * flick * float(v);
            }
    });
    // Grain, strongest in the mid-tones.
    if (plan.grain > 0) {
        const double size = plan.grainSize;
        const int gw = plan.gridW, gh = plan.gridH;
        const std::vector<float>& noise = plan.noise;
        const float amount = plan.grain;
        parallelRows(H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < W; ++x) {
                    float* p = img.at(x, y);
                    const float a = p[3];
                    if (a <= 0) continue;
                    const double gx = x / size, gy = y / size;
                    const int ix = std::min(gw - 2, int(gx)), iy = std::min(gh - 2, int(gy));
                    const float fx = float(gx - ix), fy = float(gy - iy);
                    const float l = std::clamp(luma(p) / a, 0.0f, 1.0f);
                    const float k = amount * (0.35f + 2.6f * l * (1 - l)) * a;
                    for (int c = 0; c < 3; ++c) {
                        auto n = [&](int xx, int yy) { return noise[(size_t(yy) * size_t(gw) + size_t(xx)) * 3 + size_t(c)]; };
                        const float top = n(ix, iy) + (n(ix + 1, iy) - n(ix, iy)) * fx, bottom = n(ix, iy + 1) + (n(ix + 1, iy + 1) - n(ix, iy + 1)) * fx;
                        p[c] = std::max(0.0f, p[c] + (top + (bottom - top) * fy) * k);
                    }
                }
        });
    }
}

}  // namespace montage
