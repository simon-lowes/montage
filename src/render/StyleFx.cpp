#include "StyleFx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Processing.h"

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

}  // namespace montage::sfx
