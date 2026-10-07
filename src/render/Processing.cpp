#include "Processing.h"

#include "core/Effects.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace montage {

namespace {

constexpr float kLumaR = 0.2126f, kLumaG = 0.7152f, kLumaB = 0.0722f;

inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float smoothstep(float e0, float e1, float x) {
    if (e1 == e0) return x < e0 ? 0.0f : 1.0f;
    float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}
inline float luma(float r, float g, float b) { return kLumaR * r + kLumaG * g + kLumaB * b; }

// Runs fn(r,g,b,a) on unpremultiplied colour for every pixel, re-premultiplying after.
template <typename F>
void perPixel(Image& img, F fn) {
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                float a = p[3];
                if (a <= 0.0f) continue;
                float inv = 1.0f / a;
                float r = p[0] * inv, g = p[1] * inv, b = p[2] * inv;
                fn(r, g, b, a);
                p[0] = r * a;
                p[1] = g * a;
                p[2] = b * a;
                p[3] = a;
            }
        }
    });
}

void colorCorrect(const Effect& e, FrameTime t, Image& img) {
    const float exposure = std::pow(2.0f, float(e.p("exposure", t)));
    const float contrast = float(e.p("contrast", t, 1));
    const float pivot = float(e.p("pivot", t, 0.435));
    const float sat = float(e.p("saturation", t, 1));
    const float temp = float(e.p("temperature", t)) / 100.0f;
    const float tint = float(e.p("tint", t)) / 100.0f;
    const float offset = float(e.p("offset", t));
    const float liftM = float(e.p("lift", t)), gammaM = float(e.p("gamma", t)), gainM = float(e.p("gain", t, 1));
    const float lift[3] = {liftM + float(e.p("lift_r", t)), liftM + float(e.p("lift_g", t)), liftM + float(e.p("lift_b", t))};
    const float gam[3] = {std::pow(2.0f, gammaM + float(e.p("gamma_r", t))),
                          std::pow(2.0f, gammaM + float(e.p("gamma_g", t))),
                          std::pow(2.0f, gammaM + float(e.p("gamma_b", t)))};
    const float gain[3] = {gainM * float(e.p("gain_r", t, 1)), gainM * float(e.p("gain_g", t, 1)),
                           gainM * float(e.p("gain_b", t, 1))};
    // White balance as channel multipliers (normalised to keep luma roughly constant).
    float wb[3] = {1 + 0.3f * temp + 0.1f * tint, 1 - 0.25f * tint, 1 - 0.3f * temp + 0.1f * tint};
    float wl = luma(wb[0], wb[1], wb[2]);
    for (float& w : wb) w /= wl;
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        float c[3] = {r, g, b};
        for (int i = 0; i < 3; ++i) {
            float v = c[i] * exposure * wb[i];
            v = v * gain[i] + offset;
            v = v + lift[i] * (1.0f - v);
            v = v > 0 ? std::pow(v, 1.0f / gam[i]) : v;
            v = (v - pivot) * contrast + pivot;
            c[i] = v;
        }
        float l = luma(c[0], c[1], c[2]);
        r = l + (c[0] - l) * sat;
        g = l + (c[1] - l) * sat;
        b = l + (c[2] - l) * sat;
    });
}

void curves(const Effect& e, FrameTime t, Image& img) {
    // Cached by the curve strings (rebuilding 4 x 1024 entries is cheap anyway).
    std::vector<float> m = buildCurve(e.s("master", "0,0 1,1"));
    std::vector<float> cr = buildCurve(e.s("red", "0,0 1,1"));
    std::vector<float> cg = buildCurve(e.s("green", "0,0 1,1"));
    std::vector<float> cb = buildCurve(e.s("blue", "0,0 1,1"));
    float mix = float(e.p("mix", t, 100)) / 100.0f;
    const int n = int(m.size()) - 1;
    auto look = [n](const std::vector<float>& lut, float v) {
        float x = clamp01(v) * float(n);
        int i = std::min(n - 1, int(x));
        float f = x - float(i);
        return lut[size_t(i)] * (1 - f) + lut[size_t(i) + 1] * f;
    };
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        float nr = look(cr, look(m, r)), ng = look(cg, look(m, g)), nb = look(cb, look(m, b));
        r += (nr - r) * mix;
        g += (ng - g) * mix;
        b += (nb - b) * mix;
    });
}

void hueSat(const Effect& e, FrameTime t, Image& img) {
    float hue = float(e.p("hue", t)) * float(M_PI) / 180.0f;
    float sat = float(e.p("saturation", t, 1));
    float light = float(e.p("lightness", t));
    float vib = float(e.p("vibrance", t));
    float ch = std::cos(hue), sh = std::sin(hue);
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        // Hue rotation in YIQ space.
        float y = 0.299f * r + 0.587f * g + 0.114f * b;
        float i = 0.596f * r - 0.274f * g - 0.322f * b;
        float q = 0.211f * r - 0.523f * g + 0.312f * b;
        float i2 = i * ch - q * sh, q2 = i * sh + q * ch;
        float s = sat;
        if (vib != 0) {
            float curSat = std::sqrt(i2 * i2 + q2 * q2) * 1.6f;
            s *= 1.0f + vib * (1.0f - clamp01(curSat));
        }
        i2 *= s;
        q2 *= s;
        y += light * (light > 0 ? (1 - y) : y);
        r = y + 0.956f * i2 + 0.621f * q2;
        g = y - 0.272f * i2 - 0.647f * q2;
        b = y - 1.106f * i2 + 1.703f * q2;
    });
}

void applyLut(const Effect& e, FrameTime t, Image& img) {
    std::string path = e.s("path");
    if (path.empty()) return;
    auto lut = loadCubeLut(path);
    if (!lut) return;
    float strength = float(e.p("strength", t, 100)) / 100.0f;
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        float nr = r, ng = g, nb = b;
        lut->apply(nr, ng, nb);
        r += (nr - r) * strength;
        g += (ng - g) * strength;
        b += (nb - b) * strength;
    });
}

void chromaKey(const Effect& e, FrameTime t, Image& img) {
    float kr = float(e.p("key.r", t, 0)), kg = float(e.p("key.g", t, 1)), kb = float(e.p("key.b", t, 0));
    float tol = float(e.p("tolerance", t, 0.25));
    float soft = float(e.p("softness", t, 0.1));
    float spill = float(e.p("spill", t, 0.6));
    float choke = float(e.p("choke", t, 0));
    bool showMatte = e.p("show_matte", t) > 0.5;
    auto cbcr = [](float r, float g, float b, float& cb, float& cr) {
        float y = luma(r, g, b);
        cb = (b - y) / 1.8556f;
        cr = (r - y) / 1.5748f;
    };
    float kcb, kcr;
    cbcr(kr, kg, kb, kcb, kcr);
    float klen = std::sqrt(kcb * kcb + kcr * kcr);
    int dom = (kg >= kr && kg >= kb) ? 1 : (kb >= kr ? 2 : 0);
    tol = std::max(0.0f, tol + choke * 0.2f);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                float a = p[3];
                if (a <= 0) continue;
                float inv = 1.0f / a;
                float r = p[0] * inv, g = p[1] * inv, b = p[2] * inv;
                float cb, cr;
                cbcr(r, g, b, cb, cr);
                float d = std::sqrt((cb - kcb) * (cb - kcb) + (cr - kcr) * (cr - kcr));
                // Normalise distance by key saturation so tolerance behaves for any key colour.
                float nd = d / std::max(0.05f, klen * 2.0f);
                float matte = smoothstep(tol, tol + std::max(0.001f, soft), nd);
                if (spill > 0) {
                    float c[3] = {r, g, b};
                    float others = 0.5f * (c[(dom + 1) % 3] + c[(dom + 2) % 3]);
                    if (c[dom] > others) c[dom] -= (c[dom] - others) * spill;
                    r = c[0];
                    g = c[1];
                    b = c[2];
                }
                float na = a * matte;
                if (showMatte) {
                    p[0] = p[1] = p[2] = na;
                    p[3] = 1;
                } else {
                    p[0] = r * na;
                    p[1] = g * na;
                    p[2] = b * na;
                    p[3] = na;
                }
            }
        }
    });
}

void lumaKey(const Effect& e, FrameTime t, Image& img) {
    float th = float(e.p("threshold", t, 0.1)), soft = float(e.p("softness", t, 0.05));
    bool invert = e.p("invert", t) > 0.5;
    perPixel(img, [&](float& r, float& g, float& b, float& a) {
        float l = luma(r, g, b);
        float m = smoothstep(th - soft, th + soft, l);
        if (invert) m = 1 - m;
        a *= m;
    });
}

void blackWhite(const Effect& e, FrameTime t, Image& img) {
    float amt = float(e.p("amount", t, 100)) / 100.0f;
    float tr = float(e.p("tint.r", t, 1)), tg = float(e.p("tint.g", t, 1)), tb = float(e.p("tint.b", t, 1));
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        float l = luma(r, g, b);
        r += (l * tr - r) * amt;
        g += (l * tg - g) * amt;
        b += (l * tb - b) * amt;
    });
}

void invert(const Effect& e, FrameTime t, Image& img) {
    float amt = float(e.p("amount", t, 100)) / 100.0f;
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        r += (1 - 2 * r) * amt;
        g += (1 - 2 * g) * amt;
        b += (1 - 2 * b) * amt;
    });
}

void vignette(const Effect& e, FrameTime t, Image& img) {
    float amount = float(e.p("amount", t, 0.5)), size = float(e.p("size", t, 0.75)),
          soft = float(e.p("softness", t, 0.5));
    float cx = img.width * 0.5f, cy = img.height * 0.5f;
    float norm = std::sqrt(cx * cx + cy * cy);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                float dx = (x + 0.5f - cx), dy = (y + 0.5f - cy);
                float d = std::sqrt(dx * dx + dy * dy) / norm;
                float k = 1.0f - amount * smoothstep(size - soft * 0.5f, size + soft * 0.5f, d);
                p[0] *= k;
                p[1] *= k;
                p[2] *= k;
            }
        }
    });
}

void mosaic(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    int s = std::max(1, int(std::lround(e.p("size", t, 16) * pixelScale)));
    if (s <= 1) return;
    for (int by = 0; by < img.height; by += s)
        for (int bx = 0; bx < img.width; bx += s) {
            float acc[4] = {0, 0, 0, 0};
            int n = 0;
            int ey = std::min(img.height, by + s), ex = std::min(img.width, bx + s);
            for (int y = by; y < ey; ++y)
                for (int x = bx; x < ex; ++x) {
                    const float* p = img.at(x, y);
                    for (int c = 0; c < 4; ++c) acc[c] += p[c];
                    ++n;
                }
            for (float& v : acc) v /= float(n);
            for (int y = by; y < ey; ++y)
                for (int x = bx; x < ex; ++x) std::copy(acc, acc + 4, img.at(x, y));
        }
}

void mirror(const Effect& e, FrameTime t, Image& img) {
    int mode = int(e.p("mode", t));
    int w = img.width, h = img.height;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int sx = x, sy = y;
            bool write = false;
            switch (mode) {
                case 0: write = x >= w / 2; sx = w - 1 - x; break;
                case 1: write = x < w / 2; sx = w - 1 - x; break;
                case 2: write = y >= h / 2; sy = h - 1 - y; break;
                default: write = y < h / 2; sy = h - 1 - y; break;
            }
            if (write) std::copy(img.at(sx, sy), img.at(sx, sy) + 4, img.at(x, y));
        }
}

void dropShadow(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    double dist = e.p("distance", t, 10) * pixelScale;
    double ang = e.p("angle", t, 135) * M_PI / 180.0;
    double soft = e.p("softness", t, 8) * pixelScale;
    float op = float(e.p("opacity", t, 60)) / 100.0f;
    float cr = float(e.p("color.r", t)), cg = float(e.p("color.g", t)), cb = float(e.p("color.b", t));
    int dx = int(std::lround(std::cos(ang) * dist)), dy = int(std::lround(std::sin(ang) * dist));
    Image sh(img.width, img.height);
    for (int y = 0; y < img.height; ++y) {
        int sy = y - dy;
        if (sy < 0 || sy >= img.height) continue;
        for (int x = 0; x < img.width; ++x) {
            int sx = x - dx;
            if (sx < 0 || sx >= img.width) continue;
            float a = img.at(sx, sy)[3] * op;
            float* d = sh.at(x, y);
            d[0] = cr * a;
            d[1] = cg * a;
            d[2] = cb * a;
            d[3] = a;
        }
    }
    if (soft > 0.5) gaussianBlur(sh, soft);
    // Layer goes over its shadow.
    blendOnto(sh, img, "normal", 1.0f);
    img = std::move(sh);
}

void boxBlurH(const Image& src, Image& dst, int r) {
    int w = src.width;
    parallelRows(src.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* s = src.row(y);
            float* d = dst.row(y);
            float acc[4] = {0, 0, 0, 0};
            float norm = 1.0f / float(2 * r + 1);
            for (int i = -r; i <= r; ++i) {
                int xi = std::clamp(i, 0, w - 1);
                for (int c = 0; c < 4; ++c) acc[c] += s[xi * 4 + c];
            }
            for (int x = 0; x < w; ++x) {
                for (int c = 0; c < 4; ++c) d[x * 4 + c] = acc[c] * norm;
                int xo = std::clamp(x - r, 0, w - 1), xn = std::clamp(x + r + 1, 0, w - 1);
                for (int c = 0; c < 4; ++c) acc[c] += s[xn * 4 + c] - s[xo * 4 + c];
            }
        }
    });
}

void transpose(const Image& src, Image& dst) {
    dst.width = src.height;
    dst.height = src.width;
    dst.px.resize(src.px.size());
    parallelRows(src.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < src.width; ++x) std::copy(src.at(x, y), src.at(x, y) + 4, dst.at(y, x));
    });
}

}  // namespace

void gaussianBlur(Image& img, double radius, bool horizontal, bool vertical) {
    if (radius < 0.5 || img.empty()) return;
    // Three box passes approximate a Gaussian with sigma ~ radius / 2.
    double sigma = radius / 2.0;
    int n = 3;
    double wIdeal = std::sqrt(12.0 * sigma * sigma / n + 1.0);
    int wl = int(std::floor(wIdeal));
    if (wl % 2 == 0) --wl;
    int wu = wl + 2;
    double mIdeal = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0);
    int m = int(std::lround(mIdeal));
    Image tmp(img.width, img.height);
    auto passes = [&](Image& a) {
        Image t2(a.width, a.height);
        for (int i = 0; i < n; ++i) {
            int r = ((i < m ? wl : wu) - 1) / 2;
            if (r <= 0) continue;
            boxBlurH(a, t2, r);
            std::swap(a.px, t2.px);
        }
    };
    if (horizontal) passes(img);
    if (vertical) {
        transpose(img, tmp);
        passes(tmp);
        transpose(tmp, img);
    }
}

Effect autoColorCorrection(const Image& img, Id effectId) {
    Effect e = makeEffect("color_correct", effectId);
    if (img.empty()) return e;
    // Histograms of unpremultiplied channels and luma.
    constexpr int kN = 1024;
    std::vector<double> hist[4];
    for (auto& h : hist) h.assign(kN, 0.0);
    double sum[3] = {0, 0, 0}, count = 0;
    for (size_t i = 0; i < img.px.size(); i += 4) {
        float a = img.px[i + 3];
        if (a < 0.5f) continue;
        float c[3] = {img.px[i] / a, img.px[i + 1] / a, img.px[i + 2] / a};
        for (int k = 0; k < 3; ++k) {
            hist[k][size_t(std::clamp(int(c[k] * (kN - 1)), 0, kN - 1))] += 1;
            sum[k] += c[k];
        }
        hist[3][size_t(std::clamp(int(luma(c[0], c[1], c[2]) * (kN - 1)), 0, kN - 1))] += 1;
        count += 1;
    }
    if (count < 16) return e;
    auto percentile = [&](const std::vector<double>& h, double q) {
        double target = q * count, acc = 0;
        for (int i = 0; i < kN; ++i) {
            acc += h[size_t(i)];
            if (acc >= target) return double(i) / (kN - 1);
        }
        return 1.0;
    };
    // Grey world: scale channels so their means match the luma-weighted mean.
    double mean[3] = {sum[0] / count, sum[1] / count, sum[2] / count};
    double grey = kLumaR * mean[0] + kLumaG * mean[1] + kLumaB * mean[2];
    const char* gains[3] = {"gain_r", "gain_g", "gain_b"};
    for (int k = 0; k < 3; ++k)
        if (mean[k] > 1e-4) e.params[gains[k]] = std::clamp(grey / mean[k], 0.5, 2.0);
    // Levels from the luma distribution: black point to 0, white point to 1.
    double lo = percentile(hist[3], 0.005), hi = percentile(hist[3], 0.995);
    if (hi - lo > 0.05) {
        double gain = std::clamp(1.0 / (hi - lo), 0.5, 3.0);
        e.params["gain"] = gain;
        e.params["offset"] = std::clamp(-lo * gain, -0.5, 0.5);
    }
    return e;
}

void flattenOver(Image& img, float r, float g, float b) {
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                float k = 1.0f - p[3];
                p[0] += r * k;
                p[1] += g * k;
                p[2] += b * k;
                p[3] = 1.0f;
            }
        }
    });
}

std::vector<float> effectMatte(const Effect& e, FrameTime t, const Image& img, double pixelScale) {
    std::vector<float> matte;
    if (img.empty() || !hasMask(e, t)) return matte;
    const int W = img.width, H = img.height;
    matte.assign(size_t(W) * size_t(H), 1.0f);
    const int shape = int(std::lround(e.p("mask.shape", t)));
    if (shape == 1 || shape == 2) {
        // Image pixels, centred on the mask and rotated into its axes.
        const double cx = e.p("mask.x", t, 0.5) * W, cy = e.p("mask.y", t, 0.5) * H;
        const double a = std::max(0.5, e.p("mask.w", t, 0.4) * W / 2), b = std::max(0.5, e.p("mask.h", t, 0.4) * H / 2);
        const double rot = e.p("mask.rotation", t) * M_PI / 180.0, cr = std::cos(rot), sr = std::sin(rot);
        const double feather = std::max(1.0, e.p("mask.feather", t, 20) * pixelScale);
        const double expand = e.p("mask.expansion", t) * pixelScale;
        parallelRows(H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < W; ++x) {
                    const double px = x + 0.5 - cx, py = y + 0.5 - cy;
                    const double u = px * cr + py * sr, v = -px * sr + py * cr;
                    double d;  // signed distance to the edge, negative inside
                    if (shape == 1) {
                        const double f = (u * u) / (a * a) + (v * v) / (b * b) - 1;
                        const double gx = 2 * u / (a * a), gy = 2 * v / (b * b);
                        d = f / std::max(1e-9, std::sqrt(gx * gx + gy * gy));
                    } else {
                        const double qx = std::fabs(u) - a, qy = std::fabs(v) - b;
                        d = std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) + std::min(std::max(qx, qy), 0.0);
                    }
                    d -= expand;
                    const double k = std::clamp(0.5 - d / feather, 0.0, 1.0);
                    matte[size_t(y) * size_t(W) + size_t(x)] = float(k * k * (3 - 2 * k));  // smoothstep
                }
        });
    }
    if (e.p("mask.qualify", t) > 0.5) {
        const double hc = e.p("mask.hue", t), hw = e.p("mask.hue_width", t, 60) / 2;
        const double soft = std::clamp(e.p("mask.softness", t, 20) / 100.0, 0.0, 1.0);
        const double sl = e.p("mask.sat_low", t, 15) / 100, sh = e.p("mask.sat_high", t, 100) / 100;
        const double ll = e.p("mask.lum_low", t, 5) / 100, lh = e.p("mask.lum_high", t, 100) / 100;
        // 1 inside [lo, hi], falling to 0 over `ramp` outside it.
        auto band = [](double v, double lo, double hi, double ramp) {
            if (v >= lo && v <= hi) return 1.0;
            const double out = v < lo ? lo - v : v - hi;
            return ramp <= 0 ? 0.0 : std::max(0.0, 1.0 - out / ramp);
        };
        parallelRows(H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < W; ++x) {
                    const float* p = img.at(x, y);
                    const float al = p[3];
                    if (al <= 1e-6f) {
                        matte[size_t(y) * size_t(W) + size_t(x)] = 0;
                        continue;
                    }
                    const double r = p[0] / al, g = p[1] / al, bl = p[2] / al;
                    const double mx = std::max({r, g, bl}), mn = std::min({r, g, bl}), c = mx - mn;
                    const double sat = mx > 1e-6 ? c / mx : 0;
                    const double luma = 0.2126 * r + 0.7152 * g + 0.0722 * bl;
                    double hue = 0;
                    if (c > 1e-6) {
                        if (mx == r) hue = 60 * std::fmod((g - bl) / c + 6, 6.0);
                        else if (mx == g) hue = 60 * ((bl - r) / c + 2);
                        else hue = 60 * ((r - g) / c + 4);
                    }
                    double dh = std::fabs(hue - hc);
                    dh = std::min(dh, 360 - dh);
                    const double mh = hw >= 180 ? 1.0 : band(dh, 0, hw, 60 * soft + 1e-9);
                    const double ms = band(sat, sl, sh, 0.25 * soft);
                    const double ml = band(luma, ll, lh, 0.25 * soft);
                    matte[size_t(y) * size_t(W) + size_t(x)] *= float(mh * ms * ml);
                }
        });
    }
    const bool invert = e.p("mask.invert", t) > 0.5;
    const float opacity = float(std::clamp(e.p("mask.opacity", t, 100) / 100.0, 0.0, 1.0));
    for (float& m : matte) m = (invert ? 1 - m : m) * opacity;
    return matte;
}

namespace {
void applyEffectUnmasked(const Effect& e, FrameTime t, Image& img, double pixelScale);
}

void applyVideoEffect(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    if (!e.enabled || img.empty()) return;
    if (!hasMask(e, t)) {
        applyEffectUnmasked(e, t, img, pixelScale);
        return;
    }
    const std::vector<float> matte = effectMatte(e, t, img, pixelScale);
    if (e.p("mask.show", t) > 0.5) {
        // The mask itself, as grey over the clip's shape.
        for (size_t i = 0; i < matte.size(); ++i) {
            float* p = &img.px[i * 4];
            p[0] = p[1] = p[2] = matte[i] * p[3];
        }
        return;
    }
    Image original = img;
    applyEffectUnmasked(e, t, img, pixelScale);
    for (size_t i = 0; i < matte.size(); ++i) {
        const float m = matte[i];
        float* p = &img.px[i * 4];
        const float* o = &original.px[i * 4];
        for (int c = 0; c < 4; ++c) p[c] = o[c] + (p[c] - o[c]) * m;
    }
}

namespace {
void applyEffectUnmasked(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const std::string& ty = e.type;
    if (ty == "color_correct") colorCorrect(e, t, img);
    else if (ty == "curves") curves(e, t, img);
    else if (ty == "hue_sat") hueSat(e, t, img);
    else if (ty == "lut") applyLut(e, t, img);
    else if (ty == "chroma_key") chromaKey(e, t, img);
    else if (ty == "luma_key") lumaKey(e, t, img);
    else if (ty == "black_white") blackWhite(e, t, img);
    else if (ty == "invert") invert(e, t, img);
    else if (ty == "vignette") vignette(e, t, img);
    else if (ty == "mosaic") mosaic(e, t, img, pixelScale);
    else if (ty == "mirror") mirror(e, t, img);
    else if (ty == "drop_shadow") dropShadow(e, t, img, pixelScale);
    else if (ty == "gaussian_blur") {
        int dir = int(e.p("direction", t));
        gaussianBlur(img, e.p("radius", t, 10) * pixelScale, dir != 2, dir != 1);
    } else if (ty == "sharpen") {
        Image blurred = img;
        gaussianBlur(blurred, e.p("radius", t, 1.5) * pixelScale);
        float amt = float(e.p("amount", t, 1));
        for (size_t i = 0; i < img.px.size(); i += 4) {
            float a = img.px[i + 3];
            for (int c = 0; c < 3; ++c) {
                float v = img.px[i + c] + amt * (img.px[i + c] - blurred.px[i + c]);
                img.px[i + c] = std::clamp(v, 0.0f, std::max(a, 1e-6f) * 4.0f);
            }
        }
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// LUTs

void Lut3D::apply(float& r, float& g, float& b) const {
    if (size < 2) return;
    float in[3] = {r, g, b};
    float u[3];
    for (int i = 0; i < 3; ++i) {
        float span = domainMax[i] - domainMin[i];
        u[i] = clamp01(span > 0 ? (in[i] - domainMin[i]) / span : in[i]) * float(size - 1);
    }
    if (is1D) {
        float out[3];
        for (int c = 0; c < 3; ++c) {
            int i0 = std::min(size - 2, int(u[c]));
            float f = u[c] - float(i0);
            out[c] = data[size_t(i0) * 3 + size_t(c)] * (1 - f) + data[size_t(i0 + 1) * 3 + size_t(c)] * f;
        }
        r = out[0];
        g = out[1];
        b = out[2];
        return;
    }
    int x0 = std::min(size - 2, int(u[0])), y0 = std::min(size - 2, int(u[1])), z0 = std::min(size - 2, int(u[2]));
    float fx = u[0] - float(x0), fy = u[1] - float(y0), fz = u[2] - float(z0);
    auto at = [&](int x, int y, int z, int c) {
        return data[((size_t(z) * size_t(size) + size_t(y)) * size_t(size) + size_t(x)) * 3 + size_t(c)];
    };
    float out[3];
    for (int c = 0; c < 3; ++c) {
        float c00 = at(x0, y0, z0, c) * (1 - fx) + at(x0 + 1, y0, z0, c) * fx;
        float c10 = at(x0, y0 + 1, z0, c) * (1 - fx) + at(x0 + 1, y0 + 1, z0, c) * fx;
        float c01 = at(x0, y0, z0 + 1, c) * (1 - fx) + at(x0 + 1, y0, z0 + 1, c) * fx;
        float c11 = at(x0, y0 + 1, z0 + 1, c) * (1 - fx) + at(x0 + 1, y0 + 1, z0 + 1, c) * fx;
        float c0 = c00 * (1 - fy) + c10 * fy;
        float c1 = c01 * (1 - fy) + c11 * fy;
        out[c] = c0 * (1 - fz) + c1 * fz;
    }
    r = out[0];
    g = out[1];
    b = out[2];
}

std::shared_ptr<const Lut3D> loadCubeLut(const std::string& path, std::string* error) {
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<const Lut3D>> cache;
    {
        std::lock_guard lock(m);
        auto it = cache.find(path);
        if (it != cache.end()) return it->second;
    }
    // Paths are UTF-8; a u8string path opens non-ASCII names on Windows too.
    std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())));
    if (!in) {
        if (error) *error = "Cannot open LUT " + path;
        return nullptr;
    }
    auto lut = std::make_shared<Lut3D>();
    std::string line;
    size_t expected = 0;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        std::string first;
        if (!(ls >> first)) continue;
        if (first == "TITLE") continue;
        if (first == "LUT_3D_SIZE") {
            ls >> lut->size;
            lut->is1D = false;
            expected = size_t(lut->size) * size_t(lut->size) * size_t(lut->size) * 3;
        } else if (first == "LUT_1D_SIZE") {
            ls >> lut->size;
            lut->is1D = true;
            expected = size_t(lut->size) * 3;
        } else if (first == "DOMAIN_MIN") {
            ls >> lut->domainMin[0] >> lut->domainMin[1] >> lut->domainMin[2];
        } else if (first == "DOMAIN_MAX") {
            ls >> lut->domainMax[0] >> lut->domainMax[1] >> lut->domainMax[2];
        } else if (std::isdigit(static_cast<unsigned char>(first[0])) || first[0] == '-' || first[0] == '.') {
            float r = std::stof(first), g = 0, b = 0;
            ls >> g >> b;
            lut->data.push_back(r);
            lut->data.push_back(g);
            lut->data.push_back(b);
        }
    }
    if (lut->size < 2 || lut->data.size() != expected) {
        if (error) *error = "Malformed .cube LUT";
        return nullptr;
    }
    std::lock_guard lock(m);
    cache[path] = lut;
    return lut;
}

std::vector<float> buildCurve(const std::string& pointsStr, int n) {
    std::vector<std::pair<double, double>> pts;
    std::istringstream is(pointsStr);
    std::string tok;
    while (is >> tok) {
        auto comma = tok.find(',');
        if (comma == std::string::npos) continue;
        try {
            pts.push_back({std::clamp(std::stod(tok.substr(0, comma)), 0.0, 1.0), std::stod(tok.substr(comma + 1))});
        } catch (...) {
        }
    }
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end(), [](auto& a, auto& b) { return std::fabs(a.first - b.first) < 1e-9; }),
              pts.end());
    std::vector<float> lut(size_t(n) + 1);
    if (pts.size() < 2) {
        for (int i = 0; i <= n; ++i) lut[size_t(i)] = float(i) / float(n);
        return lut;
    }
    // Fritsch–Carlson monotone cubic interpolation.
    size_t k = pts.size();
    std::vector<double> d(k - 1), m(k);
    for (size_t i = 0; i + 1 < k; ++i) d[i] = (pts[i + 1].second - pts[i].second) / (pts[i + 1].first - pts[i].first);
    m[0] = d[0];
    m[k - 1] = d[k - 2];
    for (size_t i = 1; i + 1 < k; ++i) m[i] = (d[i - 1] * d[i] <= 0) ? 0 : (d[i - 1] + d[i]) / 2;
    for (size_t i = 0; i + 1 < k; ++i) {
        if (d[i] == 0) {
            m[i] = m[i + 1] = 0;
            continue;
        }
        double a = m[i] / d[i], b = m[i + 1] / d[i];
        double s = a * a + b * b;
        if (s > 9) {
            double tau = 3 / std::sqrt(s);
            m[i] = tau * a * d[i];
            m[i + 1] = tau * b * d[i];
        }
    }
    for (int j = 0; j <= n; ++j) {
        double x = double(j) / n;
        double y;
        if (x <= pts.front().first) y = pts.front().second;
        else if (x >= pts.back().first) y = pts.back().second;
        else {
            size_t i = 0;
            while (i + 2 < k && x > pts[i + 1].first) ++i;
            double h = pts[i + 1].first - pts[i].first;
            double tt = (x - pts[i].first) / h;
            double t2 = tt * tt, t3 = t2 * tt;
            y = (2 * t3 - 3 * t2 + 1) * pts[i].second + (t3 - 2 * t2 + tt) * h * m[i] + (-2 * t3 + 3 * t2) * pts[i + 1].second +
                (t3 - t2) * h * m[i + 1];
        }
        lut[size_t(j)] = float(y);
    }
    return lut;
}

// ---------------------------------------------------------------------------
// Blending

namespace {

inline float blendChannel(int mode, float cb, float cs) {
    switch (mode) {
        case 1: return cb + cs;                                      // add
        case 2: return cb * cs;                                      // multiply
        case 3: return cb + cs - cb * cs;                            // screen
        case 4: return cb <= 0.5f ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);  // overlay
        case 5: return std::min(cb, cs);                             // darken
        case 6: return std::max(cb, cs);                             // lighten
        case 7: return std::fabs(cb - cs);                           // difference
        case 8: {                                                    // soft light (W3C)
            if (cs <= 0.5f) return cb - (1 - 2 * cs) * cb * (1 - cb);
            float dcb = cb <= 0.25f ? ((16 * cb - 12) * cb + 4) * cb : std::sqrt(std::max(cb, 0.0f));
            return cb + (2 * cs - 1) * (dcb - cb);
        }
        case 9: return cs <= 0.5f ? 2 * cb * cs : 1 - 2 * (1 - cb) * (1 - cs);  // hard light
        case 10: return cs >= 1 ? 1.0f : std::min(1.0f, cb / (1 - cs));         // colour dodge
        case 11: return cs <= 0 ? 0.0f : 1 - std::min(1.0f, (1 - cb) / cs);     // colour burn
        case 12: return std::max(0.0f, cb - cs);                               // subtract
        default: return cs;
    }
}

int modeIndex(const std::string& mode) {
    static const std::map<std::string, int> idx = {
        {"normal", 0},   {"add", 1},         {"multiply", 2},    {"screen", 3},      {"overlay", 4},
        {"darken", 5},   {"lighten", 6},     {"difference", 7},  {"soft_light", 8},  {"hard_light", 9},
        {"color_dodge", 10}, {"color_burn", 11}, {"subtract", 12}};
    auto it = idx.find(mode);
    return it == idx.end() ? 0 : it->second;
}

}  // namespace

void blendOnto(Image& dst, const Image& src, const std::string& mode, float opacity) {
    if (src.width != dst.width || src.height != dst.height || opacity <= 0) return;
    int m = modeIndex(mode);
    parallelRows(dst.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* d = dst.row(y);
            const float* s = src.row(y);
            for (int x = 0; x < dst.width; ++x, d += 4, s += 4) {
                float as = s[3] * opacity;
                if (as <= 0) continue;
                if (m == 0) {
                    float k = 1 - as;
                    d[0] = s[0] * opacity + d[0] * k;
                    d[1] = s[1] * opacity + d[1] * k;
                    d[2] = s[2] * opacity + d[2] * k;
                    d[3] = as + d[3] * k;
                    continue;
                }
                float ab = d[3];
                float invS = 1.0f / s[3];
                float invB = ab > 0 ? 1.0f / ab : 0.0f;
                for (int c = 0; c < 3; ++c) {
                    float cs = s[c] * invS;
                    float cb = d[c] * invB;
                    float bl = blendChannel(m, cb, cs);
                    d[c] = cs * as * (1 - ab) + d[c] * (1 - as) + as * ab * bl;
                }
                d[3] = as + ab * (1 - as);
            }
        }
    });
}

// ---------------------------------------------------------------------------
// Transitions

namespace {

// Bilinear sample with transparent outside.
inline void sample(const Image& img, float x, float y, float out[4]) {
    x -= 0.5f;
    y -= 0.5f;
    int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    float fx = x - float(x0), fy = y - float(y0);
    out[0] = out[1] = out[2] = out[3] = 0;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int xx = x0 + i, yy = y0 + j;
            if (xx < 0 || yy < 0 || xx >= img.width || yy >= img.height) continue;
            float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            const float* p = img.at(xx, yy);
            for (int c = 0; c < 4; ++c) out[c] += p[c] * w;
        }
}

}  // namespace

Image transitionMix(const std::string& type, const Effect& params, const Image& aIn, const Image& bIn, double u, int w,
                    int h) {
    Image out(w, h);
    const Image empty;
    const Image& a = aIn.empty() ? empty : aIn;
    const Image& b = bIn.empty() ? empty : bIn;
    auto px = [&](const Image& img, int x, int y, float o[4]) {
        if (img.empty() || x < 0 || y < 0 || x >= img.width || y >= img.height) {
            o[0] = o[1] = o[2] = o[3] = 0;
            return;
        }
        const float* p = img.at(x, y);
        std::copy(p, p + 4, o);
    };
    const float uf = float(std::clamp(u, 0.0, 1.0));
    const float soft = float(params.p("softness", 0, 0.05));
    const int dir = int(params.p("direction", 0, 0));
    const float ang = float(params.p("angle", 0, 0)) * float(M_PI) / 180.0f;
    const float strength = float(params.p("strength", 0, 1));
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                float pa[4], pb[4];
                float* o = out.at(x, y);
                if (type == "dip_to_black" || type == "dip_to_white") {
                    float col = type == "dip_to_white" ? 1.0f : 0.0f;
                    if (uf < 0.5f) {
                        px(a, x, y, pa);
                        float k = uf * 2;
                        float mixA = pa[3] > 0 || !a.empty() ? 1.0f : 0.0f;
                        (void)mixA;
                        for (int c = 0; c < 3; ++c) o[c] = pa[c] * (1 - k) + col * k;
                        o[3] = pa[3] * (1 - k) + k;
                    } else {
                        px(b, x, y, pb);
                        float k = (uf - 0.5f) * 2;
                        for (int c = 0; c < 3; ++c) o[c] = col * (1 - k) + pb[c] * k;
                        o[3] = (1 - k) + pb[3] * k;
                    }
                } else if (type == "wipe") {
                    float nx = (x + 0.5f) / float(w) - 0.5f, ny = (y + 0.5f) / float(h) - 0.5f;
                    float proj = nx * std::cos(ang) + ny * std::sin(ang);
                    float ext = 0.5f * (std::fabs(std::cos(ang)) + std::fabs(std::sin(ang))) + soft;
                    float edge = -ext + 2 * ext * uf;
                    float mb = 1 - smoothstep(edge - soft, edge + soft, proj);
                    px(a, x, y, pa);
                    px(b, x, y, pb);
                    for (int c = 0; c < 4; ++c) o[c] = pa[c] * (1 - mb) + pb[c] * mb;
                } else if (type == "iris") {
                    float nx = (x + 0.5f) - w * 0.5f, ny = (y + 0.5f) - h * 0.5f;
                    float maxR = std::sqrt(w * w * 0.25f + h * h * 0.25f);
                    float d = std::sqrt(nx * nx + ny * ny) / maxR;
                    float r = uf * (1 + soft);
                    float mb = 1 - smoothstep(r - soft, r, d);
                    px(a, x, y, pa);
                    px(b, x, y, pb);
                    for (int c = 0; c < 4; ++c) o[c] = pa[c] * (1 - mb) + pb[c] * mb;
                } else if (type == "push" || type == "slide") {
                    int ox = 0, oy = 0;
                    int shiftB = 0;
                    switch (dir) {
                        case 0: ox = -int(std::lround(uf * w)); shiftB = w; break;    // left
                        case 1: ox = int(std::lround(uf * w)); shiftB = -w; break;    // right
                        case 2: oy = -int(std::lround(uf * h)); shiftB = h; break;    // up
                        default: oy = int(std::lround(uf * h)); shiftB = -h; break;   // down
                    }
                    bool horiz = dir <= 1;
                    if (type == "push") px(a, x - ox, y - oy, pa);
                    else px(a, x, y, pa);
                    int bx = x - ox - (horiz ? shiftB : 0), by = y - oy - (horiz ? 0 : shiftB);
                    px(b, bx, by, pb);
                    float k = 1 - pb[3];
                    for (int c = 0; c < 4; ++c) o[c] = pb[c] + pa[c] * k;
                } else if (type == "zoom") {
                    float sa = 1 + uf * strength, sb = 1 + (1 - uf) * strength;
                    float cx = w * 0.5f, cy = h * 0.5f;
                    float fxp = x + 0.5f, fyp = y + 0.5f;
                    if (!a.empty()) sample(a, cx + (fxp - cx) / sa, cy + (fyp - cy) / sa, pa);
                    else pa[0] = pa[1] = pa[2] = pa[3] = 0;
                    if (!b.empty()) sample(b, cx + (fxp - cx) / sb, cy + (fyp - cy) / sb, pb);
                    else pb[0] = pb[1] = pb[2] = pb[3] = 0;
                    for (int c = 0; c < 4; ++c) o[c] = pa[c] * (1 - uf) + pb[c] * uf;
                } else {  // cross_dissolve and fallback
                    px(a, x, y, pa);
                    px(b, x, y, pb);
                    for (int c = 0; c < 4; ++c) o[c] = pa[c] * (1 - uf) + pb[c] * uf;
                }
            }
        }
    });
    return out;
}

}  // namespace montage
