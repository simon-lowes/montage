#include "StyleFx.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <mutex>

#include "Processing.h"
#include "audio/RoomResponse.h"
#include "core/MaskPath.h"

namespace montage::sfx {

namespace {

constexpr double kDeg = M_PI / 180;

enum class Edge { Transparent, Clamp, Wrap };

// Bilinear sample at (x, y) (pixel centres at +0.5).
void sample(const Image& img, double x, double y, float out[4], Edge edge) {
    x -= 0.5;
    y -= 0.5;
    const int W = img.width, H = img.height;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = float(x - x0), fy = float(y - y0);
    auto px = [&](int xi, int yi, int k) -> float {
        switch (edge) {
            case Edge::Clamp: xi = std::clamp(xi, 0, W - 1), yi = std::clamp(yi, 0, H - 1); break;
            case Edge::Wrap: xi = ((xi % W) + W) % W, yi = ((yi % H) + H) % H; break;
            case Edge::Transparent:
                if (xi < 0 || yi < 0 || xi >= W || yi >= H) return 0.f;
                break;
        }
        return img.at(xi, yi)[k];
    };
    for (int k = 0; k < 4; ++k) {
        const float top = px(x0, y0, k) + (px(x0 + 1, y0, k) - px(x0, y0, k)) * fx;
        const float bottom = px(x0, y0 + 1, k) + (px(x0 + 1, y0 + 1, k) - px(x0, y0 + 1, k)) * fx;
        out[k] = top + (bottom - top) * fy;
    }
}

// Each pixel from wherever `from` says (it returns false to leave the pixel as it was).
template <class F>
void remap(Image& img, Edge edge, F from) {
    const Image src = img;
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                double sx, sy;
                if (from(x + 0.5, y + 0.5, sx, sy)) sample(src, sx, sy, img.at(x, y), edge);
            }
    });
}

float lumaOf(const float* p) { return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; }

uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

double fbm(double x, double y, double z, int octaves, uint32_t seed) {
    double sum = 0, amp = 1, norm = 0, f = 1;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * valueNoise(x * f, y * f, z * f + o * 17.31, seed + uint32_t(o) * 101U);
        norm += amp;
        amp *= 0.5;
        f *= 2;
    }
    return sum / norm;
}

// The effect's centre, in frame pixels.
void centreOf(const Effect& e, FrameTime t, const Image& img, double pixelScale, double& cx, double& cy) {
    cx = img.width / 2.0 + e.p("center_x", t) * pixelScale;
    cy = img.height / 2.0 + e.p("center_y", t) * pixelScale;
}

float smoothstep(float a, float b, float x) {
    const float u = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return u * u * (3 - 2 * u);
}

}  // namespace

double valueNoise(double x, double y, double z, uint32_t seed) {
    const double fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int ix = int(fx), iy = int(fy), iz = int(fz);
    auto q = [](double u) { return u * u * u * (u * (u * 6 - 15) + 10); };
    const double ux = q(x - fx), uy = q(y - fy), uz = q(z - fz);
    auto v = [&](int a, int b, int c) {
        const uint32_t h = hash(uint32_t(a) * 73856093U ^ hash(uint32_t(b) * 19349663U ^ hash(uint32_t(c) * 83492791U ^ seed)));
        return double(h & 0xffffff) / double(0x7fffff) - 1;
    };
    auto lerp = [](double a, double b, double u) { return a + (b - a) * u; };
    const double x00 = lerp(v(ix, iy, iz), v(ix + 1, iy, iz), ux), x10 = lerp(v(ix, iy + 1, iz), v(ix + 1, iy + 1, iz), ux);
    const double x01 = lerp(v(ix, iy, iz + 1), v(ix + 1, iy, iz + 1), ux), x11 = lerp(v(ix, iy + 1, iz + 1), v(ix + 1, iy + 1, iz + 1), ux);
    return lerp(lerp(x00, x10, uy), lerp(x01, x11, uy), uz);
}

// ---- Distort ----------------------------------------------------------------------

void waveWarp(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const double height = e.p("height", t, 10) * pixelScale, width = std::max(1.0, e.p("width", t, 40) * pixelScale);
    if (std::fabs(height) < 1e-3) return;
    const double dir = e.p("direction", t, 0) * kDeg, dx = std::cos(dir), dy = std::sin(dir);
    const double phase = e.p("phase", t, 0) * kDeg + double(t) * e.p("speed", t, 0) * kDeg;
    const int shape = int(std::lround(e.p("shape", t, 0)));
    remap(img, Edge::Clamp, [&](double x, double y, double& sx, double& sy) {
        const double s = 2 * M_PI * (x * dx + y * dy) / width + phase;
        double w = std::sin(s);
        if (shape == 1) w = std::asin(w) * 2 / M_PI;                 // triangle
        else if (shape == 2) w = std::tanh(6 * w) / std::tanh(6.0);  // square, softened
        // Moved across the direction the waves travel in.
        sx = x + dy * height * w;
        sy = y - dx * height * w;
        return true;
    });
}

void twirl(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const double angle = e.p("angle", t, 90) * kDeg;
    if (std::fabs(angle) < 1e-6) return;
    double cx, cy;
    centreOf(e, t, img, pixelScale, cx, cy);
    const double R = std::max(1.0, e.p("radius", t, 50) / 100 * std::hypot(img.width, img.height) / 2);
    remap(img, Edge::Clamp, [&](double x, double y, double& sx, double& sy) {
        const double rx = x - cx, ry = y - cy, r = std::hypot(rx, ry);
        if (r >= R) return false;
        const double a = -angle * (1 - r / R) * (1 - r / R), c = std::cos(a), s = std::sin(a);
        sx = cx + rx * c - ry * s;
        sy = cy + rx * s + ry * c;
        return true;
    });
}

void spherize(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const double amount = std::clamp(e.p("amount", t, 50) / 100, -1.0, 1.0);
    if (std::fabs(amount) < 1e-4) return;
    double cx, cy;
    centreOf(e, t, img, pixelScale, cx, cy);
    const double R = std::max(1.0, e.p("radius", t, 50) / 100 * std::min(img.width, img.height) / 2.0);
    // Out (bulge) samples nearer the middle, so it grows; in (pinch) the other way.
    const double power = amount > 0 ? 1 + amount * 0.9 : 1 / (1 - amount * 0.9);
    remap(img, Edge::Clamp, [&](double x, double y, double& sx, double& sy) {
        const double rx = x - cx, ry = y - cy, r = std::hypot(rx, ry);
        if (r >= R || r < 1e-9) return false;
        const double k = std::pow(r / R, power) * R / r;
        sx = cx + rx * k;
        sy = cy + ry * k;
        return true;
    });
}

void ripple(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const double amp = e.p("amplitude", t, 8) * pixelScale, lambda = std::max(1.0, e.p("wavelength", t, 40) * pixelScale);
    if (std::fabs(amp) < 1e-3) return;
    double cx, cy;
    centreOf(e, t, img, pixelScale, cx, cy);
    const double R = std::max(1.0, e.p("radius", t, 100) / 100 * std::hypot(img.width, img.height) / 2);
    const double phase = double(t) * e.p("speed", t, 20) * kDeg;
    remap(img, Edge::Clamp, [&](double x, double y, double& sx, double& sy) {
        const double rx = x - cx, ry = y - cy, r = std::hypot(rx, ry);
        if (r >= R || r < 1e-9) return false;
        // Rings moving outwards, dying away towards the edge of the radius.
        const double d = amp * std::sin(2 * M_PI * r / lambda - phase) * (1 - r / R);
        sx = cx + rx / r * (r + d);
        sy = cy + ry / r * (r + d);
        return true;
    });
}

void turbulentDisplace(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const double amount = e.p("amount", t, 20) * pixelScale, size = std::max(1.0, e.p("size", t, 100) * pixelScale);
    if (std::fabs(amount) < 1e-3) return;
    const int octaves = std::clamp(int(std::lround(e.p("complexity", t, 3))), 1, 8);
    const double z = (e.p("evolution", t, 0) + double(t) * e.p("speed", t, 0)) / 360;
    const uint32_t seed = uint32_t(std::lround(e.p("seed", t, 0)));
    remap(img, Edge::Clamp, [&](double x, double y, double& sx, double& sy) {
        // In source pixels, so the pattern is the same at any preview size.
        const double u = x / size, v = y / size;
        sx = x + amount * fbm(u, v, z, octaves, seed * 2 + 1);
        sy = y + amount * fbm(u, v, z, octaves, seed * 2 + 2);
        return true;
    });
}

void motionTile(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const double tw = std::clamp(e.p("tile_width", t, 100), 1.0, 100.0) / 100, th = std::clamp(e.p("tile_height", t, 100), 1.0, 100.0) / 100;
    const double ox = e.p("offset_x", t) * pixelScale, oy = e.p("offset_y", t) * pixelScale;
    const bool mirror = e.p("mirror", t) > 0.5;
    if (tw == 1 && th == 1 && ox == 0 && oy == 0) return;
    const double W = img.width, H = img.height, cw = W * tw, ch = H * th;
    remap(img, Edge::Wrap, [&](double x, double y, double& sx, double& sy) {
        // Tiles laid out from the middle, each a smaller copy of the whole frame.
        const double u = (x - ox - (W - cw) / 2) / cw, v = (y - oy - (H - ch) / 2) / ch;
        double fu = u - std::floor(u), fv = v - std::floor(v);
        if (mirror && (int64_t(std::floor(u)) & 1)) fu = 1 - fu;
        if (mirror && (int64_t(std::floor(v)) & 1)) fv = 1 - fv;
        sx = fu * W;
        sy = fv * H;
        return true;
    });
}

// ---- Stylize ----------------------------------------------------------------------

void findEdges(const Effect& e, FrameTime t, Image& img) {
    const bool invert = e.p("invert", t, 1) > 0.5;
    const float blend = float(std::clamp(e.p("blend", t, 0) / 100, 0.0, 1.0));
    const Image src = img;
    const int W = img.width, H = img.height;
    auto at = [&](int x, int y, int c) {
        const float* p = src.at(std::clamp(x, 0, W - 1), std::clamp(y, 0, H - 1));
        return p[3] > 0 ? p[c] / p[3] : 0.0f;
    };
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                float* p = img.at(x, y);
                const float a = p[3];
                if (a <= 0) continue;
                for (int c = 0; c < 3; ++c) {
                    // Sobel, each colour on its own; a full step reads as 1.
                    const float gx = at(x + 1, y - 1, c) + 2 * at(x + 1, y, c) + at(x + 1, y + 1, c) - at(x - 1, y - 1, c) -
                                     2 * at(x - 1, y, c) - at(x - 1, y + 1, c);
                    const float gy = at(x - 1, y + 1, c) + 2 * at(x, y + 1, c) + at(x + 1, y + 1, c) - at(x - 1, y - 1, c) -
                                     2 * at(x, y - 1, c) - at(x + 1, y - 1, c);
                    float edge = std::min(1.0f, std::hypot(gx, gy) / 4);
                    if (invert) edge = 1 - edge;
                    const float orig = p[c] / a;
                    p[c] = (edge + (orig - edge) * blend) * a;
                }
            }
    });
}

void emboss(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const double dir = e.p("direction", t, 135) * kDeg, relief = std::max(0.25, e.p("relief", t, 2) * pixelScale);
    const float contrast = float(e.p("contrast", t, 100) / 100) * 2;
    const float blend = float(std::clamp(e.p("blend", t, 0) / 100, 0.0, 1.0));
    const Image src = img;
    const double dx = std::cos(dir) * relief, dy = -std::sin(dir) * relief;
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                float* p = img.at(x, y);
                const float a = p[3];
                if (a <= 0) continue;
                float lit[4], shade[4];
                sample(src, x + 0.5 + dx, y + 0.5 + dy, lit, Edge::Clamp);
                sample(src, x + 0.5 - dx, y + 0.5 - dy, shade, Edge::Clamp);
                // Grey, raised where the picture brightens towards the light.
                const float v = std::clamp(0.5f + contrast * (lumaOf(lit) - lumaOf(shade)), 0.0f, 1.0f);
                for (int c = 0; c < 3; ++c) p[c] = (v + (p[c] / a - v) * blend) * a;
            }
    });
}

void halftone(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const double cell = std::max(2.0, e.p("dot_size", t, 8) * pixelScale);
    const double angle = e.p("angle", t, 45) * kDeg;
    const bool colour = e.p("mode", t, 0) > 0.5;
    const Image src = img;
    // Dots on a grid turned by `angle`; each dot's area is the ink the cell's tone needs.
    auto dot = [&](double x, double y, double ang, int channel) {
        const double c = std::cos(ang), s = std::sin(ang);
        const double u = (x * c + y * s) / cell, v = (-x * s + y * c) / cell;
        const double cu = std::floor(u) + 0.5, cv = std::floor(v) + 0.5;
        const double px = (cu * c - cv * s) * cell, py = (cu * s + cv * c) * cell;
        float at[4];
        sample(src, px, py, at, Edge::Clamp);
        const float a = at[3] > 0 ? at[3] : 1;
        const float tone = std::clamp(channel < 0 ? lumaOf(at) / a : at[channel] / a, 0.0f, 1.0f);
        const double r = cell * std::sqrt((1 - tone) / M_PI);
        const double d = std::hypot(u - cu, v - cv) * cell;
        return smoothstep(float(r + 0.5), float(r - 0.5), float(d));  // 1 inside the dot
    };
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                float* p = img.at(x, y);
                const float a = p[3];
                if (a <= 0) continue;
                if (colour) {
                    static const double angles[3] = {15 * kDeg, 75 * kDeg, 0};
                    for (int c = 0; c < 3; ++c) p[c] = (1 - dot(x + 0.5, y + 0.5, angle + angles[c], c)) * a;
                } else {
                    const float v = 1 - dot(x + 0.5, y + 0.5, angle, -1);
                    p[0] = p[1] = p[2] = v * a;
                }
            }
    });
}

void duotone(const Effect& e, FrameTime t, Image& img) {
    const float lo[3] = {float(e.p("shadows.r", t, 0.08)), float(e.p("shadows.g", t, 0.1)), float(e.p("shadows.b", t, 0.35))};
    const float hi[3] = {float(e.p("highlights.r", t, 1)), float(e.p("highlights.g", t, 0.85)), float(e.p("highlights.b", t, 0.6))};
    const float mix = float(std::clamp(e.p("mix", t, 100) / 100, 0.0, 1.0));
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                float* p = img.at(x, y);
                const float a = p[3];
                if (a <= 0) continue;
                const float l = std::clamp(lumaOf(p) / a, 0.0f, 1.0f);
                for (int c = 0; c < 3; ++c) {
                    const float v = lo[c] + (hi[c] - lo[c]) * l;
                    p[c] += (v * a - p[c]) * mix;
                }
            }
    });
}

void vhs(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const float amount = float(std::clamp(e.p("amount", t, 100) / 100, 0.0, 1.0));
    if (amount <= 0) return;
    const double bleed = e.p("bleed", t, 4) * pixelScale * amount;
    const float noise = float(e.p("noise", t, 30) / 100) * amount, lines = float(e.p("scanlines", t, 30) / 100) * amount;
    const double tracking = e.p("tracking", t, 30) / 100 * amount;
    const int W = img.width, H = img.height;
    const Image src = img;
    const uint32_t seed = hash(uint32_t(t) * 2654435761U + 12345U);
    // Each line wobbles a little; a band of bad tracking rolls up the frame; and the bottom
    // few lines tear sideways, where the heads switch.
    const double bandY = std::fmod(double(t) * 3.7 * pixelScale + double(seed % 997), double(H) * 1.5) - double(H) * 0.25;
    const double bandH = std::max(4.0, 0.06 * H), switchY = H * (1 - 0.025);
    std::vector<double> shift(static_cast<size_t>(H));
    std::vector<float> torn(static_cast<size_t>(H), 0.0f);
    for (int y = 0; y < H; ++y) {
        const double wobble = (double(hash(seed ^ uint32_t(y / std::max(1, int(2 * pixelScale)))) & 0xffff) / 65535.0 - 0.5) * 1.5 * pixelScale;
        const double inBand = std::max(0.0, 1 - std::fabs(y - bandY) / bandH);
        const double inSwitch = y >= switchY ? (y - switchY) / (H - switchY) : 0;
        shift[size_t(y)] = (wobble + inBand * inBand * 25 * pixelScale + inSwitch * 18 * pixelScale) * tracking;
        torn[size_t(y)] = float(std::max(inBand * inBand, inSwitch) * tracking);
    }
    // Tape's narrow bandwidth: soft brightness, and colour smeared and lagging behind it.
    const double soft = 1.2 * pixelScale * amount;
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                float* p = img.at(x, y);
                const double sx = x + 0.5 - shift[size_t(y)];
                float here[4], l0[4], l1[4];
                sample(src, sx, y + 0.5, here, Edge::Clamp);
                const float a = here[3];
                if (a <= 0) {
                    std::copy_n(here, 4, p);
                    continue;
                }
                sample(src, sx - soft, y + 0.5, l0, Edge::Clamp);
                sample(src, sx + soft, y + 0.5, l1, Edge::Clamp);
                float l = (lumaOf(here) * 2 + lumaOf(l0) + lumaOf(l1)) / 4 / a;
                float cb = 0, cr = 0, wsum = 0;
                const int taps = std::max(1, int(std::ceil(bleed)));
                for (int k = 0; k <= 2 * taps; ++k) {
                    float q[4];
                    sample(src, sx - bleed * double(k) / taps, y + 0.5, q, Edge::Clamp);
                    const float qa = q[3] > 0 ? q[3] : 1, ql = lumaOf(q) / qa;
                    cb += q[2] / qa - ql, cr += q[0] / qa - ql, wsum += 1;
                }
                cb /= wsum, cr /= wsum;
                // Streaky noise (a few pixels long), stronger where the tracking is bad.
                const uint32_t h = hash(seed ^ (uint32_t(y) * 2246822519U + uint32_t(x / std::max(1, int(3 * pixelScale)))));
                const float n = float(h & 0xffff) / 65535.0f - 0.5f;
                l += n * noise * (0.35f + torn[size_t(y)] * 1.5f);
                if (torn[size_t(y)] > 0.3f && (h >> 16) % 7 == 0) l = std::max(l, 0.85f);  // dropouts
                // Lifted blacks, a little less contrast and colour.
                l = 0.04f * amount + l * (1 - 0.1f * amount);
                const float row = (int(y / std::max(1.0, pixelScale)) & 1) ? 1 - lines * 0.5f : 1.0f;
                const float sat = 1 - 0.2f * amount;
                const float r = l + cr * sat, b = l + cb * sat;
                const float g = (l - 0.2126f * r - 0.0722f * b) / 0.7152f;
                p[0] = std::max(0.0f, r * row) * a;
                p[1] = std::max(0.0f, g * row) * a;
                p[2] = std::max(0.0f, b * row) * a;
                p[3] = a;
            }
    });
}

void tiltShift(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const double radius = e.p("blur", t, 12) * pixelScale;
    const double focus = e.p("focus", t, 50) / 100, width = std::max(0.0, e.p("width", t, 20) / 100);
    const double angle = e.p("angle", t, 0) * kDeg;
    const float sat = float(e.p("saturation", t, 120) / 100);
    if (radius >= 0.5) {
        Image blurred = img;
        gaussianBlur(blurred, radius);
        const double W = img.width, H = img.height, nx = -std::sin(angle), ny = std::cos(angle);
        const double cx = W / 2, cy = focus * H;
        parallelRows(img.height, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < img.width; ++x) {
                    // Sharp in the band, going soft over as far again either side.
                    const double d = std::fabs((x + 0.5 - cx) * nx + (y + 0.5 - cy) * ny) / H;
                    const float k = smoothstep(float(width / 2), float(width / 2 + std::max(0.02, width)), float(d));
                    float* p = img.at(x, y);
                    const float* b = blurred.at(x, y);
                    for (int c = 0; c < 4; ++c) p[c] += (b[c] - p[c]) * k;
                }
        });
    }
    if (std::fabs(sat - 1) > 1e-4f)
        parallelRows(img.height, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < img.width; ++x) {
                    float* p = img.at(x, y);
                    const float l = lumaOf(p);
                    for (int c = 0; c < 3; ++c) p[c] = std::max(0.0f, l + (p[c] - l) * sat);
                }
        });
}

void cameraShake(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const double amp = e.p("amplitude", t, 10) * pixelScale, rot = e.p("rotation", t, 1) * kDeg;
    if (std::fabs(amp) < 1e-3 && std::fabs(rot) < 1e-6) return;
    const double speed = std::max(0.0, e.p("speed", t, 50)) / 100 * 0.35;  // noise cycles per frame
    const uint32_t seed = uint32_t(std::lround(e.p("seed", t, 0)));
    const double s = double(t) * speed;
    const double ox = amp * fbm(s, 0.5, 0, 3, seed * 3 + 1), oy = amp * fbm(s, 1.5, 0, 3, seed * 3 + 2);
    const double a = rot * fbm(s, 2.5, 0, 3, seed * 3 + 3);
    const double W = img.width, H = img.height, cx = W / 2, cy = H / 2;
    // Zoomed in just enough that the frame's edges never show.
    double zoom = 1;
    if (e.p("zoom", t, 1) > 0.5) zoom = 1 + 2 * std::fabs(amp) / std::min(W, H) + std::sin(std::fabs(rot)) * (W + H) / std::min(W, H);
    const double c = std::cos(-a), sn = std::sin(-a);
    remap(img, Edge::Clamp, [&](double x, double y, double& sx, double& sy) {
        const double rx = (x - cx - ox) / zoom, ry = (y - cy - oy) / zoom;
        sx = cx + rx * c - ry * sn;
        sy = cy + rx * sn + ry * c;
        return true;
    });
}

void lensFlare(const Effect& e, FrameTime t, Image& img) {
    const int W = img.width, H = img.height;
    if (W <= 0 || H <= 0) return;
    const double px = e.p("pos_x", t, 0.3) * W, py = e.p("pos_y", t, 0.3) * H;
    const double bright = std::max(0.0, e.p("brightness", t, 100) / 100), ghostAmt = std::max(0.0, e.p("ghosts", t, 100) / 100);
    const int lens = std::clamp(int(e.p("lens", t, 0)), 0, 3);  // 50-300mm zoom, 35mm prime, 105mm prime, anamorphic
    const float tint[3] = {float(e.p("color.r", t, 1.0)), float(e.p("color.g", t, 0.9)), float(e.p("color.b", t, 0.75))};
    if (bright <= 0) return;
    const double R = std::hypot(W, H) / 2, cx = W / 2.0, cy = H / 2.0;
    // Ghosts: discs along the line from the light through the centre, at these fractions of the way.
    struct Ghost {
        double at, radius, strength;
        float rgb[3];
    };
    static const Ghost kZoom[] = {{0.45, 0.035, 0.10, {0.6f, 1.0f, 0.6f}}, {0.75, 0.09, 0.06, {0.7f, 0.5f, 1.0f}}, {1.25, 0.025, 0.14, {1.0f, 0.8f, 0.4f}},
                                  {1.55, 0.13, 0.05, {0.5f, 0.8f, 1.0f}}, {1.9, 0.06, 0.08, {1.0f, 0.5f, 0.6f}},  {2.3, 0.17, 0.04, {0.6f, 1.0f, 0.8f}}};
    static const Ghost kPrime[] = {{0.6, 0.05, 0.08, {0.8f, 0.9f, 1.0f}}, {1.3, 0.08, 0.06, {1.0f, 0.85f, 0.6f}}, {2.0, 0.12, 0.05, {0.7f, 1.0f, 0.8f}}};
    static const Ghost kAnamorphic[] = {{0.5, 0.05, 0.08, {0.4f, 0.6f, 1.0f}}, {1.4, 0.09, 0.06, {0.5f, 0.7f, 1.0f}}, {2.1, 0.14, 0.04, {0.4f, 0.5f, 1.0f}}};
    const Ghost* ghosts = lens == 0 ? kZoom : lens == 3 ? kAnamorphic : kPrime;
    const int nGhosts = lens == 0 ? 6 : 3;
    const int spokes = lens == 1 ? 6 : lens == 2 ? 8 : 4;
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < W; ++x, p += 4) {
                const double dx = x + 0.5 - px, dy = y + 0.5 - py, d = std::hypot(dx, dy) / R;
                double light[3] = {0, 0, 0};
                // The core and its halo.
                const double core = 1.6 * std::exp(-(d / 0.025) * (d / 0.025)) + 0.35 * std::exp(-d / 0.1);
                for (int c = 0; c < 3; ++c) light[c] += core * (c == 0 ? 1.0 : 0.5 + 0.5 * tint[c] / std::max(0.01f, tint[0]));
                if (lens == 3) {
                    // A long horizontal streak, blue.
                    const double streak = std::exp(-(dy / (0.006 * R)) * (dy / (0.006 * R))) * std::exp(-std::fabs(dx) / (0.8 * R));
                    light[0] += 0.25 * streak, light[1] += 0.45 * streak, light[2] += 0.9 * streak;
                } else {
                    // A star of rays, fading with distance.
                    const double a = std::atan2(dy, dx);
                    const double ray = std::pow(std::fabs(std::cos(spokes * 0.5 * a)), lens == 0 ? 60.0 : 120.0) * std::exp(-d / (lens == 0 ? 0.25 : 0.35));
                    for (int c = 0; c < 3; ++c) light[c] += 0.5 * ray;
                }
                if (lens == 0) {
                    // The zoom's rainbow ring: each colour at a slightly different radius.
                    for (int c = 0; c < 3; ++c) {
                        const double ring = (d - (0.33 + 0.012 * c)) / 0.012;
                        light[c] += 0.07 * std::exp(-ring * ring);
                    }
                }
                for (int k = 0; k < nGhosts; ++k) {
                    const Ghost& g = ghosts[k];
                    const double gx = px + (cx - px) * g.at, gy = py + (cy - py) * g.at;  // 1 = the centre, 2 = opposite the light
                    double gd = std::hypot((x + 0.5 - gx) / (lens == 3 ? 1.6 : 1.0), y + 0.5 - gy) / R / g.radius;
                    if (gd >= 1.15) continue;
                    // A soft-edged disc, a little brighter at its rim.
                    const double disc = (1 - std::clamp((gd - 0.85) / 0.3, 0.0, 1.0)) * (0.7 + 0.3 * std::min(1.0, gd));
                    for (int c = 0; c < 3; ++c) light[c] += ghostAmt * g.strength * disc * g.rgb[c];
                }
                double lum = 0;
                for (int c = 0; c < 3; ++c) {
                    const float add = float(bright * light[c] * tint[c]);
                    p[c] += add;
                    lum = std::max(lum, double(add));
                }
                p[3] = std::min(1.0f, p[3] + float(lum));  // light shows on transparent areas too
            }
        }
    });
}

// ---- Light Rays ----------------------------------------------------------------------

void lightRays(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    (void)pixelScale;  // (the rays' reach is a share of the way to the centre, the same at any size)
    const int W = img.width, H = img.height;
    if (W <= 0 || H <= 0) return;
    const bool follow = e.p("follow", t, 0) > 0.5;
    double cx = e.p("pos_x", t, 0.5) * W, cy = e.p("pos_y", t, 0.25) * H;
    if (follow) {
        const MaskBox box = maskBox(e, t);
        cx = box.x * W, cy = box.y * H;
    }
    cx = std::clamp(std::isfinite(cx) ? cx : W / 2.0, -4.0 * W, 5.0 * W), cy = std::clamp(std::isfinite(cy) ? cy : H / 2.0, -4.0 * H, 5.0 * H);
    const double threshold = std::max(0.0, e.p("threshold", t, 0.8)), intensity = std::max(0.0, e.p("intensity", t, 1.0));
    const double length = std::clamp(e.p("length", t, 60) / 100, 0.0, 1.0), decay = std::clamp(e.p("decay", t, 50) / 100, 0.0, 1.0);
    const double block = std::clamp(e.p("block", t, 0) / 100, 0.0, 1.0);
    const float tint[3] = {float(e.p("color.r", t, 1.0)), float(e.p("color.g", t, 0.95)), float(e.p("color.b", t, 0.85))};
    if (intensity <= 0 || length <= 0) return;
    // The light: what is above the threshold, tinted.
    Image rays(W, H);
    bool any = false;
    for (int y = 0; y < H; ++y) {
        const float* p = img.row(y);
        float* q = rays.row(y);
        for (int x = 0; x < W; ++x, p += 4, q += 4) {
            const float a = p[3];
            const double l = a > 0 ? lumaOf(p) / a : 0.0;
            const double k = std::isfinite(l) && l > threshold ? (l - threshold) / l : 0.0;
            for (int c = 0; c < 3; ++c) q[c] = k > 0 ? float(p[c] * k * tint[c]) : 0.0f;
            q[3] = 0;
            any = any || k > 0;
        }
    }
    if (!any) return;
    // Three passes of eight samples along the line to the centre, each pass's spacing eight times the last, so
    // together they reach `length` of the way; nearer samples count more as `decay` rises.
    constexpr int kSamples = 8, kPasses = 3;
    const double first = length / (std::pow(double(kSamples), kPasses) - 1);
    const double rate = decay * 6 / length;  // (at full decay, light from the far end is e^-6 of light from beside)
    for (int pass = 0; pass < kPasses; ++pass) {
        const double step = first * std::pow(double(kSamples), pass);
        double w[kSamples], sum = 0;
        for (int j = 0; j < kSamples; ++j) sum += w[j] = std::exp(-rate * j * step);
        for (double& v : w) v /= sum;
        const Image from = rays;
        parallelRows(H, [&](int y0, int y1) {
            float s[4];
            for (int y = y0; y < y1; ++y) {
                float* q = rays.row(y);
                for (int x = 0; x < W; ++x, q += 4) {
                    double acc[3] = {0, 0, 0};
                    const double px = x + 0.5, py = y + 0.5;
                    for (int j = 0; j < kSamples; ++j) {
                        const double f = j * step;
                        sample(from, px + (cx - px) * f, py + (cy - py) * f, s, Edge::Transparent);
                        for (int c = 0; c < 3; ++c) acc[c] += w[j] * s[c];
                    }
                    for (int c = 0; c < 3; ++c) q[c] = float(acc[c]);
                }
            }
        });
    }
    // Shadows Block Rays: how much light gets through from the centre's side, the darker parts on the way taking it
    // (each 1/160 of the frame's longer side of a thing below the threshold lets through 1 - block of it).
    std::vector<float> through;
    int tw = 0, th = 0;
    if (block > 0) {
        // (worked out on a small copy: it changes slowly)
        const double shrink = std::min(1.0, 320.0 / std::max(W, H));
        tw = std::max(1, int(std::lround(W * shrink))), th = std::max(1, int(std::lround(H * shrink)));
        std::vector<float> pass(size_t(tw) * size_t(th));
        parallelRows(th, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < tw; ++x) {
                    // (the average of the pixels it covers, so a thin dark thing is not missed)
                    const int ax = std::min(W - 1, int(x / shrink)), bx = std::max(ax + 1, std::min(W, int((x + 1) / shrink)));
                    const int ay = std::min(H - 1, int(y / shrink)), by = std::max(ay + 1, std::min(H, int((y + 1) / shrink)));
                    double sum = 0;
                    for (int sy = ay; sy < by; ++sy)
                        for (int sx = ax; sx < bx; ++sx) {
                            const float* p = img.at(sx, sy);
                            const double l = p[3] > 0 ? lumaOf(p) / p[3] : 0.0;
                            sum += std::isfinite(l) ? std::clamp(l / std::max(threshold, 1e-3), 0.0, 1.0) : 1.0;
                        }
                    pass[size_t(y) * size_t(tw) + size_t(x)] = float(1 - block * (1 - sum / ((bx - ax) * (by - ay))));
                }
        });
        through.assign(pass.size(), 1.0f);
        const double scx = cx * shrink, scy = cy * shrink;
        parallelRows(th, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < tw; ++x) {
                    const double px = x + 0.5, py = y + 0.5, reach = std::hypot(scx - px, scy - py) * length;
                    const int steps = std::max(1, int(std::ceil(reach)));
                    double logT = 0;
                    for (int j = 1; j <= steps; ++j) {  // (from beside the pixel: the near side of a dark thing is lit)
                        const double f = length * j / steps;
                        const int sx = int(std::floor(px + (scx - px) * f)), sy = int(std::floor(py + (scy - py) * f));
                        if (sx < 0 || sy < 0 || sx >= tw || sy >= th) break;
                        // (each 1/160 of the frame's longer side taking its share, at any size)
                        logT += std::log(std::max(1e-4f, pass[size_t(sy) * size_t(tw) + size_t(sx)])) * (reach / steps) / std::max(tw, th) * 160;
                    }
                    through[size_t(y) * size_t(tw) + size_t(x)] = float(std::exp(logT));
                }
        });
    }
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            const float* q = rays.row(y);
            for (int x = 0; x < W; ++x, p += 4, q += 4) {
                float k = float(intensity);
                if (!through.empty()) {
                    // (bilinear from the small copy)
                    const double fx = std::clamp((x + 0.5) * tw / W - 0.5, 0.0, tw - 1.0), fy = std::clamp((y + 0.5) * th / H - 0.5, 0.0, th - 1.0);
                    const int x0 = int(fx), y0 = int(fy), x1 = std::min(tw - 1, x0 + 1), y1 = std::min(th - 1, y0 + 1);
                    const double ax = fx - x0, ay = fy - y0;
                    auto at = [&](int xi, int yi) { return double(through[size_t(yi) * size_t(tw) + size_t(xi)]); };
                    k *= float((at(x0, y0) * (1 - ax) + at(x1, y0) * ax) * (1 - ay) + (at(x0, y1) * (1 - ax) + at(x1, y1) * ax) * ay);
                }
                float most = 0;
                for (int c = 0; c < 3; ++c) {
                    const float add = q[c] * k;
                    p[c] += add;
                    most = std::max(most, add);
                }
                p[3] = std::min(1.0f, p[3] + most);  // light shows on transparent areas too
            }
        }
    });
}

// ---- Aperture Glare ----------------------------------------------------------------------

namespace {

using Cf = std::complex<float>;

// The aperture's transform: kApN square, the aperture kApR pixels in radius (its corners'), centred on the origin.
constexpr int kApN = 1024;
constexpr double kApR = 32;
constexpr double kBinsPerLambdaOverD = kApN / (2 * kApR);

int nextPow2(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

// A 2D transform in place of an n x n grid, rows then columns (unscaled both ways).
void fft2(std::vector<Cf>& a, int n, bool inverse) {
    parallelRows(n, [&](int y0, int y1) {
        std::vector<Cf> row(static_cast<size_t>(n));
        for (int y = y0; y < y1; ++y) {
            std::copy_n(a.begin() + std::ptrdiff_t(y) * n, n, row.begin());
            fftInPlace(row, inverse);
            std::copy(row.begin(), row.end(), a.begin() + std::ptrdiff_t(y) * n);
        }
    });
    parallelRows(n, [&](int x0, int x1) {
        std::vector<Cf> col(static_cast<size_t>(n));
        for (int x = x0; x < x1; ++x) {
            for (int y = 0; y < n; ++y) col[size_t(y)] = a[size_t(y) * size_t(n) + size_t(x)];
            fftInPlace(col, inverse);
            for (int y = 0; y < n; ++y) a[size_t(y) * size_t(n) + size_t(x)] = col[size_t(y)];
        }
    });
}

// Whether a point (in units of the corners' radius) is inside the aperture.
bool insideAperture(const ApertureShape& a, double x, double y) {
    const double r = std::hypot(x, y);
    if (r > 1) return false;
    const int n = std::max(3, a.blades);
    const double sector = 2 * M_PI / n;
    double angle = std::fmod(std::atan2(y, x) + 2 * M_PI, sector);
    const double polygon = std::cos(M_PI / n) / std::cos(angle - M_PI / n);  // the straight blade's distance
    return r <= polygon + std::clamp(a.rounding, 0.0, 1.0) * (1 - polygon);
}

// The aperture's Fourier transform (kApN square, bins wrapped), for its blades and rounding (the rotation turns the
// pattern, so it is left to sampling). Kept for the last few shapes.
std::shared_ptr<const std::vector<Cf>> apertureTransform(const ApertureShape& shape) {
    ApertureShape key = shape;
    key.rotation = 0;
    key.blades = std::max(3, key.blades);
    key.rounding = std::round(std::clamp(key.rounding, 0.0, 1.0) * 1000) / 1000;
    static std::mutex lock, building;
    static std::vector<std::pair<ApertureShape, std::shared_ptr<const std::vector<Cf>>>> kept;
    auto find = [&]() -> std::shared_ptr<const std::vector<Cf>> {
        const std::lock_guard<std::mutex> g(lock);
        for (const auto& [k, v] : kept)
            if (k == key) return v;
        return nullptr;
    };
    if (auto found = find()) return found;
    // (one built at a time, so threads wanting the same one wait for it rather than each making it)
    const std::lock_guard<std::mutex> one(building);
    if (auto found = find()) return found;
    auto grid = std::make_shared<std::vector<Cf>>(size_t(kApN) * kApN, Cf(0, 0));
    // The opening, 4 x 4 samples a pixel at its edge.
    const int reach = int(std::ceil(kApR)) + 1;
    for (int y = -reach; y <= reach; ++y)
        for (int x = -reach; x <= reach; ++x) {
            int in = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                    in += insideAperture(key, (x - 0.375 + 0.25 * sx) / kApR, (y - 0.375 + 0.25 * sy) / kApR);
            if (in) (*grid)[size_t((y + kApN) % kApN) * kApN + size_t((x + kApN) % kApN)] = Cf(float(in / 16.0), 0);
        }
    fft2(*grid, kApN, false);
    const std::lock_guard<std::mutex> g(lock);
    kept.insert(kept.begin(), {key, grid});
    if (kept.size() > 4) kept.pop_back();
    return grid;
}

// The transform at a fractional bin (bilinear on the complex amplitude: its zeros fall between bins), 0 beyond.
Cf transformAt(const std::vector<Cf>& f, double u, double v) {
    const double edge = kApN / 2.0 - 2;
    if (std::fabs(u) >= edge || std::fabs(v) >= edge) return 0;
    const int u0 = int(std::floor(u)), v0 = int(std::floor(v));
    const float au = float(u - u0), av = float(v - v0);
    auto at = [&](int i, int j) { return f[size_t((j + kApN) % kApN) * kApN + size_t((i + kApN) % kApN)]; };
    return (at(u0, v0) * (1 - au) + at(u0 + 1, v0) * au) * (1 - av) + (at(u0, v0 + 1) * (1 - au) + at(u0 + 1, v0 + 1) * au) * av;
}

// CIE 1931 colour matching functions (the multi-lobe Gaussian fit of Wyman, Sloan and Shirley, 2013).
void cieXyz(double nm, double xyz[3]) {
    auto g = [nm](double mu, double s1, double s2) {
        const double d = (nm - mu) / (nm < mu ? s1 : s2);
        return std::exp(-0.5 * d * d);
    };
    xyz[0] = 1.056 * g(599.8, 37.9, 31.0) + 0.362 * g(442.0, 16.0, 26.7) - 0.065 * g(501.1, 20.4, 26.2);
    xyz[1] = 0.821 * g(568.8, 46.9, 40.5) + 0.286 * g(530.9, 16.3, 31.1);
    xyz[2] = 1.217 * g(437.0, 11.8, 36.0) + 0.681 * g(459.0, 26.0, 13.8);
}

struct GlareKernel {
    ApertureShape shape;
    int radius = 0, fftSize = 0;
    std::vector<Cf> spectrum[3];  // each channel's kernel transformed, fftSize square
};

// The kernel transformed for convolving at `fftSize`, kept for the last few.
std::shared_ptr<const GlareKernel> glareKernel(const ApertureShape& shape, int radius, int fftSize) {
    static std::mutex lock, building;
    static std::vector<std::shared_ptr<const GlareKernel>> kept;
    auto find = [&]() -> std::shared_ptr<const GlareKernel> {
        const std::lock_guard<std::mutex> g(lock);
        for (const auto& k : kept)
            if (k->shape == shape && k->radius == radius && k->fftSize == fftSize) return k;
        return nullptr;
    };
    if (auto found = find()) return found;
    const std::lock_guard<std::mutex> one(building);
    if (auto found = find()) return found;
    auto k = std::make_shared<GlareKernel>();
    k->shape = shape, k->radius = radius, k->fftSize = fftSize;
    const std::vector<float> rgb = apertureKernel(shape, radius);
    const int size = 2 * radius + 1;
    for (int c = 0; c < 3; ++c) {
        std::vector<Cf>& s = k->spectrum[c];
        s.assign(size_t(fftSize) * fftSize, Cf(0, 0));
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                s[size_t((y - radius + fftSize) % fftSize) * fftSize + size_t((x - radius + fftSize) % fftSize)] =
                    Cf(rgb[(size_t(y) * size + size_t(x)) * 3 + size_t(c)], 0);
        fft2(s, fftSize, false);
    }
    const std::lock_guard<std::mutex> g(lock);
    kept.insert(kept.begin(), k);
    // (kept to about 256 MB)
    size_t bytes = 0;
    for (size_t i = 0; i < kept.size(); ++i) {
        bytes += 3 * size_t(kept[i]->fftSize) * size_t(kept[i]->fftSize) * sizeof(Cf);
        if (i > 0 && bytes > (size_t(256) << 20)) {
            kept.resize(i);
            break;
        }
    }
    return k;
}

}  // namespace

std::vector<double> apertureDiffraction(const ApertureShape& a, double nm, int size, double perLambdaOverD) {
    size = std::max(1, size | 1);
    std::vector<double> out(size_t(size) * size, 0.0);
    const auto f = apertureTransform(a);
    const double scale = kBinsPerLambdaOverD / std::max(1e-6, perLambdaOverD) * (550.0 / nm);  // bins a pixel
    const int ss = std::clamp(int(std::ceil(scale)), 1, 4);  // (samples a pixel each way, where bins are finer than pixels)
    const double cs = std::cos(-a.rotation * kDeg), sn = std::sin(-a.rotation * kDeg), half = size / 2;
    parallelRows(size, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < size; ++x) {
                double sum = 0;
                for (int sy = 0; sy < ss; ++sy)
                    for (int sx = 0; sx < ss; ++sx) {
                        const double dx = x - half + (sx + 0.5) / ss - 0.5, dy = y - half + (sy + 0.5) / ss - 0.5;
                        sum += std::norm(transformAt(*f, (dx * cs - dy * sn) * scale, (dx * sn + dy * cs) * scale));
                    }
                out[size_t(y) * size + size_t(x)] = sum;
            }
    });
    double total = 0;
    for (double v : out) total += v;
    if (total > 0)
        for (double& v : out) v /= total;
    return out;
}

std::vector<float> apertureKernel(const ApertureShape& a, int radius) {
    radius = std::max(1, radius);
    const int size = 2 * radius + 1;
    std::vector<double> rgb(size_t(size) * size * 3, 0.0);
    // Eight wavelengths of equal energy, each into linear Rec.709 (its negative parts dropped).
    for (int i = 0; i < 8; ++i) {
        const double nm = 400 + 300.0 * i / 7;
        double xyz[3];
        cieXyz(nm, xyz);
        const double w[3] = {std::max(0.0, 3.2406 * xyz[0] - 1.5372 * xyz[1] - 0.4986 * xyz[2]),
                             std::max(0.0, -0.9689 * xyz[0] + 1.8758 * xyz[1] + 0.0415 * xyz[2]),
                             std::max(0.0, 0.0557 * xyz[0] - 0.2040 * xyz[1] + 1.0570 * xyz[2])};
        const std::vector<double> pattern = apertureDiffraction(a, nm, size, radius / 20.0);
        for (size_t k = 0; k < pattern.size(); ++k)
            for (int c = 0; c < 3; ++c) rgb[k * 3 + size_t(c)] += w[c] * pattern[k];
    }
    // (round: the transform reaches every wavelength's pattern out to the radius, not into the square's corners)
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (std::hypot(x - radius, y - radius) > radius + 0.5)
                for (int c = 0; c < 3; ++c) rgb[(size_t(y) * size + size_t(x)) * 3 + size_t(c)] = 0;
    std::vector<float> out(rgb.size());
    for (int c = 0; c < 3; ++c) {
        double total = 0;
        for (size_t k = size_t(c); k < rgb.size(); k += 3) total += rgb[k];
        for (size_t k = size_t(c); k < rgb.size(); k += 3) out[k] = float(total > 0 ? rgb[k] / total : 0);
        // (what float rounding left over goes to the centre, so each channel sums to 1)
        double sum = 0;
        for (size_t k = size_t(c); k < out.size(); k += 3) sum += out[k];
        out[(size_t(radius) * size + size_t(radius)) * 3 + size_t(c)] += float(1 - sum);
    }
    return out;
}

void apertureGlare(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const int W = img.width, H = img.height;
    if (W <= 0 || H <= 0) return;
    const double threshold = std::max(0.0, e.p("threshold", t, 1.0)), intensity = std::max(0.0, e.p("intensity", t, 1.0));
    ApertureShape shape;
    shape.blades = std::clamp(int(std::lround(e.p("blades", t, 6))), 3, 16);
    shape.rounding = std::round(std::clamp(e.p("rounding", t, 0) / 100, 0.0, 1.0) * 100) / 100;
    // (the pattern repeats every 360 / blades degrees, so a turn by a whole step makes the same kernel)
    const double step = shape.rounding >= 1 ? 360.0 : 360.0 / shape.blades;
    shape.rotation = std::round(std::fmod(std::fmod(e.p("rotation", t, 0), step) + step, step) * 10) / 10;
    const int radius = std::max(2, int(std::lround(std::clamp(e.p("size", t, 200), 10.0, 600.0) * std::min(pixelScale, 2.0))));
    if (intensity <= 0) return;
    // The highlights: the light above the threshold.
    std::vector<float> bright(size_t(W) * H * 3, 0.0f);
    bool any = false;
    for (int y = 0; y < H; ++y) {
        const float* p = img.row(y);
        for (int x = 0; x < W; ++x, p += 4) {
            const float a = p[3];
            const double l = a > 0 ? lumaOf(p) / a : 0.0;
            if (!std::isfinite(l) || !(l > threshold)) continue;
            const double k = (l - threshold) / l;
            for (int c = 0; c < 3; ++c) bright[(size_t(y) * W + size_t(x)) * 3 + size_t(c)] = float(p[c] * k);
            any = true;
        }
    }
    if (!any) return;
    // Convolved by FFT in tiles (overlap-add): red and green as one complex picture, blue as another.
    const int K = 2 * radius + 1, M = nextPow2(K + std::max(K / 2, 128)), T = M - K + 1;
    const auto kernel = glareKernel(shape, radius, M);
    std::vector<float> glare(size_t(W) * H * 3, 0.0f);
    std::vector<Cf> rg(size_t(M) * M), b(size_t(M) * M), out(size_t(M) * M);
    for (int ty = 0; ty < H; ty += T)
        for (int tx = 0; tx < W; tx += T) {
            const int tw = std::min(T, W - tx), th = std::min(T, H - ty);
            bool lit = false;
            std::fill(rg.begin(), rg.end(), Cf(0, 0));
            std::fill(b.begin(), b.end(), Cf(0, 0));
            for (int y = 0; y < th; ++y)
                for (int x = 0; x < tw; ++x) {
                    const float* s = &bright[(size_t(ty + y) * W + size_t(tx + x)) * 3];
                    rg[size_t(y) * M + size_t(x)] = Cf(s[0], s[1]);
                    b[size_t(y) * M + size_t(x)] = Cf(s[2], 0);
                    lit = lit || s[0] > 0 || s[1] > 0 || s[2] > 0;
                }
            if (!lit) continue;
            fft2(rg, M, false);
            fft2(b, M, false);
            // Each half of rg's transform from its symmetry, through its own channel's kernel.
            for (int v = 0; v < M; ++v)
                for (int u = 0; u < M; ++u) {
                    const size_t i = size_t(v) * M + size_t(u), j = size_t((M - v) % M) * M + size_t((M - u) % M);
                    const Cf r = (rg[i] + std::conj(rg[j])) * 0.5f, g = (rg[i] - std::conj(rg[j])) * Cf(0, -0.5f);
                    out[i] = r * kernel->spectrum[0][i] + Cf(0, 1) * g * kernel->spectrum[1][i];
                    b[i] *= kernel->spectrum[2][i];
                }
            fft2(out, M, true);
            fft2(b, M, true);
            const float norm = 1.0f / (float(M) * float(M));
            for (int y = -radius; y < th + radius; ++y) {
                const int oy = ty + y;
                if (oy < 0 || oy >= H) continue;
                for (int x = -radius; x < tw + radius; ++x) {
                    const int ox = tx + x;
                    if (ox < 0 || ox >= W) continue;
                    const size_t i = size_t((y + M) % M) * M + size_t((x + M) % M);
                    float* g = &glare[(size_t(oy) * W + size_t(ox)) * 3];
                    g[0] += out[i].real() * norm, g[1] += out[i].imag() * norm, g[2] += b[i].real() * norm;
                }
            }
        }
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < W; ++x, p += 4) {
                const float* g = &glare[(size_t(y) * W + size_t(x)) * 3];
                float most = 0;
                for (int c = 0; c < 3; ++c) {
                    const float add = std::max(0.0f, g[c]) * float(intensity);  // (FFT rounding can dip below 0)
                    p[c] += add;
                    most = std::max(most, add);
                }
                p[3] = std::min(1.0f, p[3] + most);
            }
        }
    });
}

}  // namespace montage::sfx
