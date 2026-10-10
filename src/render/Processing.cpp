#include "Processing.h"
#include "Ofx.h"

#include "ColorSpace.h"
#include "Ocio.h"
#include "VideoFx.h"
#include "core/Cdl.h"
#include "core/Effects.h"
#include "core/ColorWarp.h"
#include "core/MaskPath.h"
#include "media/DepthMap.h"
#include "media/Matting.h"
#include "media/Inpaint.h"
#include "FaceRefine.h"
#include "Relight.h"
#include "FilmLook.h"
#include "StyleFx.h"
#include "Deconvolve.h"
#include "QualityCheck.h"
#include "media/Tracking.h"
#include "media/FaceTracks.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <array>
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

// A smooth bump: 1 at `centre`, falling to 0 at `width` either side.
inline float bump(float x, float centre, float width) {
    const float d = (x - centre) / width;
    return d * d >= 1 ? 0.0f : (1 - d * d) * (1 - d * d);
}

// Luma after the tone controls (-1..1 each): shadows and highlights are broad bumps
// round a quarter and three quarters, blacks and whites reach in from the ends.
float toneShift(float l, float highlights, float shadows, float whites, float blacks) {
    const float x = std::max(0.0f, l);
    const float wBlacks = x < 0.4f ? (1 - x / 0.4f) * (1 - x / 0.4f) : 0.0f;
    const float wWhites = x > 0.6f ? std::min(1.0f, ((x - 0.6f) / 0.4f) * ((x - 0.6f) / 0.4f)) : 0.0f;
    return l + 0.25f * (shadows * bump(x, 0.25f, 0.4f) + highlights * bump(x, 0.75f, 0.4f)) + 0.15f * (blacks * wBlacks + whites * wWhites);
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
    const float highlights = float(e.p("highlights", t)) / 100, shadows = float(e.p("shadows", t)) / 100;
    const float whites = float(e.p("whites", t)) / 100, blacks = float(e.p("blacks", t)) / 100;
    const float vibrance = float(e.p("vibrance", t)) / 100;
    const bool tone = highlights != 0 || shadows != 0 || whites != 0 || blacks != 0;
    // As a table over 0..2, kept rising, so no setting can turn tones over.
    constexpr int kTone = 1024;
    std::array<float, kTone + 1> toneLut{};
    if (tone)
        for (int i = 0; i <= kTone; ++i) {
            toneLut[size_t(i)] = toneShift(2.0f * float(i) / kTone, highlights, shadows, whites, blacks);
            if (i) toneLut[size_t(i)] = std::max(toneLut[size_t(i)], toneLut[size_t(i - 1)]);
        }
    auto toneAt = [&](float l) {
        if (l <= 0) return l + toneLut[0];
        if (l >= 2) return l - 2 + toneLut[kTone];
        const float x = l * kTone / 2;
        const int i = std::min(kTone - 1, int(x));
        return toneLut[size_t(i)] + (toneLut[size_t(i + 1)] - toneLut[size_t(i)]) * (x - float(i));
    };
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
        if (tone) {
            const float to = toneAt(l);
            // Brightness moves, colour stays: scaled where there is light to scale, shifted in the deepest blacks.
            const float m = smoothstep(0.02f, 0.08f, l), k = l > 1e-4f ? std::min(8.0f, to / l) : 1.0f;
            for (float& v : c) v = (v * k) * m + (v + to - l) * (1 - m);
            l = to;
        }
        float s = sat;
        if (vibrance != 0) {
            // Muted colours move most, already-saturated ones (and skin, mostly) least.
            const float mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]});
            const float cur = mx > 1e-4f ? clamp01((mx - mn) / mx) : 0.0f;
            s *= std::max(0.0f, 1 + vibrance * (1 - cur) * (1 - cur));
        }
        r = l + (c[0] - l) * s;
        g = l + (c[1] - l) * s;
        b = l + (c[2] - l) * s;
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

void hueCurves(const Effect& e, FrameTime t, Image& img);  // below, beside the curve building
void colorWarper(const Effect& e, FrameTime t, Image& img);  // below, as hue curves

// A transfer function and its inverse as tables (the HDR Palette decodes and encodes every pixel): code values to
// light over 0..1.25, light to code values over 2^-24..2^8 by its logarithm; beyond them the functions themselves, and
// negative values mirrored.
struct TransferTables {
    static constexpr int kDecode = 4096, kEncode = 4096;
    static constexpr float kDecodeMax = 1.25f, kLogMin = -24, kLogMax = 8;
    Transfer tf;
    std::vector<float> dec, enc;
    explicit TransferTables(Transfer t) : tf(t), dec(kDecode + 1), enc(kEncode + 1) {
        for (int i = 0; i <= kDecode; ++i) dec[size_t(i)] = float(toLinear(tf, double(kDecodeMax) * i / kDecode));
        for (int i = 0; i <= kEncode; ++i)
            enc[size_t(i)] = float(fromLinear(tf, std::exp2(double(kLogMin) + double(kLogMax - kLogMin) * i / kEncode)));
    }
    float decode(float v) const {
        if (v < 0) return -decode(-v);
        if (v >= kDecodeMax) return float(toLinear(tf, v));
        const float x = v / kDecodeMax * kDecode;
        const int i = std::min(kDecode - 1, int(x));
        return dec[size_t(i)] + (dec[size_t(i + 1)] - dec[size_t(i)]) * (x - float(i));
    }
    float encode(float l) const {
        if (l < 0) return -encode(-l);
        if (l <= std::exp2(kLogMin)) return enc[0] * l / std::exp2(kLogMin);  // a straight line to black
        const float lg = std::log2(l);
        if (lg >= kLogMax) return float(fromLinear(tf, l));
        const float x = (lg - kLogMin) / (kLogMax - kLogMin) * kEncode;
        const int i = std::min(kEncode - 1, int(x));
        return enc[size_t(i)] + (enc[size_t(i + 1)] - enc[size_t(i)]) * (x - float(i));
    }
};

// HDR Palette: each pixel's brightness in stops from mid grey (0.18 of reference white, in the light the working space
// shows: SDR white and HDR's 203 nits are both 1.0), moved by the global exposure and each zone's exposure as far as the
// pixel is in it (the curve kept rising), then the contrast round its pivot; the channels shifted by the zones' colour
// balance (stops per channel), the saturation scaled, the black offset added, and the result coded back.
void hdrPalette(const Effect& e, FrameTime t, Image& img) {
    const ColorSpace& space = currentWorkingSpace();
    const TransferTables tables(space.transfer);
    float kr = 0.2126f, kg = 0.7152f, kb = 0.0722f;
    if (space.primaries == Primaries::Bt2020) kr = 0.2627f, kg = 0.6780f, kb = 0.0593f;
    else if (space.primaries == Primaries::P3D65) kr = 0.2290f, kg = 0.6917f, kb = 0.0793f;
    struct ZoneValues {
        float exposure, saturation, balance[3], range, falloff;
        bool dark;
    };
    std::vector<ZoneValues> zones;
    for (const HdrZone& z : hdrZones()) {
        const std::string n = z.name;
        zones.push_back({float(e.p(n + "_exposure", t)),
                         float(e.p(n + "_saturation", t, 1)),
                         {float(e.p(n + "_r", t)), float(e.p(n + "_g", t)), float(e.p(n + "_b", t))},
                         float(e.p(n + "_range", t, z.range)),
                         std::max(0.05f, float(e.p(n + "_falloff", t, z.falloff))),
                         z.dark});
    }
    const float exposure = float(e.p("exposure", t)), saturation = float(e.p("saturation", t, 1));
    const float global[3] = {float(e.p("global_r", t)), float(e.p("global_g", t)), float(e.p("global_b", t))};
    const float contrast = float(e.p("contrast", t, 1)), pivot = float(e.p("pivot", t));
    const float blackOffset = float(e.p("black_offset", t)), mix = float(e.p("mix", t, 100)) / 100;
    // Per stop of brightness, from -16 to +12 in 1/32 steps: the brightness out, the balance and the saturation.
    constexpr float kEvMin = -16, kEvMax = 12;
    constexpr int kSteps = int((kEvMax - kEvMin) * 32);
    std::vector<float> evOut(kSteps + 1), sat(kSteps + 1), bal[3];
    for (auto& b : bal) b.assign(kSteps + 1, 0.0f);
    for (int i = 0; i <= kSteps; ++i) {
        const float ev = kEvMin + (kEvMax - kEvMin) * float(i) / kSteps;
        float stops = exposure, s = saturation;
        float c[3] = {global[0], global[1], global[2]};
        for (const ZoneValues& z : zones) {
            const float up = smoothstep(z.range - z.falloff / 2, z.range + z.falloff / 2, ev);
            const float w = z.dark ? 1 - up : up;
            if (w <= 0) continue;
            stops += w * z.exposure;
            s *= 1 + w * (z.saturation - 1);
            for (int k = 0; k < 3; ++k) c[k] += w * z.balance[k];
        }
        float out = (ev + stops - pivot) * contrast + pivot;
        if (i) out = std::max(out, evOut[size_t(i - 1)]);
        evOut[size_t(i)] = out;
        sat[size_t(i)] = std::max(0.0f, s);
        for (int k = 0; k < 3; ++k) bal[k][size_t(i)] = c[k];
    }
    // Each channel keeps rising too: a balance that falls faster than the brightness climbs is held level.
    for (int k = 0; k < 3; ++k)
        for (int i = 1; i <= kSteps; ++i)
            bal[k][size_t(i)] = std::max(bal[k][size_t(i)], evOut[size_t(i - 1)] + bal[k][size_t(i - 1)] - evOut[size_t(i)]);
    auto lookup = [&](const std::vector<float>& table, float x) {
        const int i = std::min(kSteps - 1, int(x));
        return table[size_t(i)] + (table[size_t(i + 1)] - table[size_t(i)]) * (x - float(i));
    };
    constexpr float kGrey = 0.18f;
    // HLG decodes to scene light with its peak at 1; its reference white (75%) is brought to 1 like SDR white and PQ's
    // 203 nits.
    const float toWhite = space.transfer == Transfer::Hlg ? float(1 / toLinear(Transfer::Hlg, 0.75)) : 1.0f;
    // perPixel hands over straight (unpremultiplied) colour and premultiplies after.
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        const float code[3] = {r, g, b};
        float lin[3];
        for (int k = 0; k < 3; ++k) lin[k] = tables.decode(code[k]) * toWhite;
        const float y = kr * lin[0] + kg * lin[1] + kb * lin[2];
        const float ev = y > 1e-9f ? std::log2(y / kGrey) : kEvMin;
        const float x = (std::clamp(ev, kEvMin, kEvMax) - kEvMin) / (kEvMax - kEvMin) * kSteps;
        // The gain taking the brightness to its new level; past the table's ends the nearest end's gain.
        const float evClamped = std::clamp(ev, kEvMin, kEvMax);
        const float gain = std::exp2(lookup(evOut, x) - evClamped);
        float out[3];
        for (int k = 0; k < 3; ++k) out[k] = lin[k] * gain * std::exp2(lookup(bal[k], x));
        const float s = lookup(sat, x);
        if (s != 1) {
            const float y2 = kr * out[0] + kg * out[1] + kb * out[2];
            for (float& v : out) v = y2 + (v - y2) * s;
        }
        for (int k = 0; k < 3; ++k) {
            const float coded = tables.encode((out[k] + blackOffset) / toWhite);
            out[k] = code[k] + (coded - code[k]) * mix;
        }
        r = out[0], g = out[1], b = out[2];
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

// ASC CDL, in the working space or converted into the space it was made in and back.
Cdl effectCdl(const Effect& e, FrameTime t) {
    Clip holder;
    holder.effects.push_back(e);
    holder.effects.back().enabled = true;
    Cdl cdl;
    clipCdl(holder, t, cdl);
    return cdl;
}

void applyCdlEffect(const Effect& e, FrameTime t, Image& img) {
    const Cdl cdl = effectCdl(e, t);
    if (cdl.identity()) return;
    const ColorSpace* space = nullptr;
    for (const auto& cs : colorSpaces())
        if (cs.label == e.s("space")) space = &cs;
    const ColorSpace& working = currentWorkingSpace();
    if (!space || space->id == working.id) {
        perPixel(img, [&](float& r, float& g, float& b, float&) { applyCdl(cdl, r, g, b); });
        return;
    }
    // Into that space and back exactly (linear light through its primaries and curve, no display rendering), so only
    // the CDL changes the picture: the whole round trip baked into a 65^3 LUT, kept for these values.
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<const Lut3D>> cache;
    std::string key = working.id + "|" + space->id + "|" + cdlSopText(cdl) + cdlNumber(cdl.saturation);
    std::shared_ptr<const Lut3D> lut;
    {
        std::lock_guard lock(m);
        if (auto it = cache.find(key); it != cache.end()) lut = it->second;
    }
    if (!lut) {
        double to[9], from[9];
        primariesMatrix(working.primaries, space->primaries, to);
        primariesMatrix(space->primaries, working.primaries, from);
        auto through = [](const double mat[9], Transfer a, Transfer b, float v[3]) {
            double l[3], o[3];
            for (int i = 0; i < 3; ++i) l[i] = toLinear(a, v[i]);
            for (int i = 0; i < 3; ++i) o[i] = mat[i * 3] * l[0] + mat[i * 3 + 1] * l[1] + mat[i * 3 + 2] * l[2];
            for (int i = 0; i < 3; ++i) v[i] = float(fromLinear(b, o[i]));
        };
        auto made = std::make_shared<Lut3D>();
        const int n = 65;
        made->size = n;
        made->data.resize(size_t(n) * n * n * 3);
        parallelRows(n, [&](int b0, int b1) {
            for (int bi = b0; bi < b1; ++bi)
                for (int gi = 0; gi < n; ++gi)
                    for (int ri = 0; ri < n; ++ri) {
                        float v[3] = {float(ri) / (n - 1), float(gi) / (n - 1), float(bi) / (n - 1)};
                        through(to, working.transfer, space->transfer, v);
                        applyCdl(cdl, v[0], v[1], v[2]);
                        through(from, space->transfer, working.transfer, v);
                        float* d = &made->data[(size_t(bi) * n * n + size_t(gi) * n + size_t(ri)) * 3];
                        d[0] = v[0], d[1] = v[1], d[2] = v[2];
                    }
        });
        lut = made;
        std::lock_guard lock(m);
        if (cache.size() > 64) cache.clear();  // (a keyframed CDL makes a new one each frame)
        cache[key] = lut;
    }
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        r = std::clamp(r, 0.0f, 1.0f), g = std::clamp(g, 0.0f, 1.0f), b = std::clamp(b, 0.0f, 1.0f);
        lut->apply(r, g, b);
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

// Separable running minimum or maximum of a plane over a window of 2r + 1 pixels.
void minMaxFilter(std::vector<float>& v, int w, int h, int r, bool maximum) {
    if (r <= 0) return;
    std::vector<float> tmp(v.size());
    auto pick = [maximum](float a, float b) { return maximum ? std::max(a, b) : std::min(a, b); };
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                float m = v[size_t(y) * size_t(w) + size_t(x)];
                for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k) m = pick(m, v[size_t(y) * size_t(w) + size_t(k)]);
                tmp[size_t(y) * size_t(w) + size_t(x)] = m;
            }
    });
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                float m = tmp[size_t(y) * size_t(w) + size_t(x)];
                for (int k = std::max(0, y - r); k <= std::min(h - 1, y + r); ++k) m = pick(m, tmp[size_t(k) * size_t(w) + size_t(x)]);
                v[size_t(y) * size_t(w) + size_t(x)] = m;
            }
    });
}

// Three box blurs of radius r: close to a Gaussian.
void boxBlurPlane(std::vector<float>& v, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<float> tmp(v.size());
    for (int pass = 0; pass < 3; ++pass) {
        parallelRows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const float* row = &v[size_t(y) * size_t(w)];
                double acc = 0;
                int n = 0;
                for (int k = 0; k <= std::min(w - 1, r); ++k) acc += row[k], ++n;
                for (int x = 0; x < w; ++x) {
                    tmp[size_t(y) * size_t(w) + size_t(x)] = float(acc / n);
                    if (x + r + 1 < w) acc += row[x + r + 1], ++n;
                    if (x - r >= 0) acc -= row[x - r], --n;
                }
            }
        });
        parallelRows(w, [&](int x0, int x1) {
            for (int x = x0; x < x1; ++x) {
                double acc = 0;
                int n = 0;
                for (int k = 0; k <= std::min(h - 1, r); ++k) acc += tmp[size_t(k) * size_t(w) + size_t(x)], ++n;
                for (int y = 0; y < h; ++y) {
                    v[size_t(y) * size_t(w) + size_t(x)] = float(acc / n);
                    if (y + r + 1 < h) acc += tmp[size_t(y + r + 1) * size_t(w) + size_t(x)], ++n;
                    if (y - r >= 0) acc -= tmp[size_t(y - r) * size_t(w) + size_t(x)], --n;
                }
            }
        });
    }
}

void screenKey(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const float sk[3] = {float(e.p("key.r", t, 0.1)), float(e.p("key.g", t, 0.75)), float(e.p("key.b", t, 0.2))};
    const int dom = (sk[1] >= sk[0] && sk[1] >= sk[2]) ? 1 : (sk[2] >= sk[0] ? 2 : 0);
    const int o1 = (dom + 1) % 3, o2 = (dom + 2) % 3;
    const float balance = float(std::clamp(e.p("balance", t, 0.5), 0.0, 1.0));
    const float gain = float(std::max(0.05, e.p("gain", t, 1.1)));
    const float black = float(e.p("clip_black", t, 0.05)), white = float(std::max(e.p("clip_white", t, 1), e.p("clip_black", t, 0.05) + 0.01));
    const float despill = float(e.p("despill", t, 100) / 100), restore = float(e.p("restore", t, 50) / 100);
    const float edgeDesat = float(e.p("edge_desat", t, 0) / 100);
    const int view = int(std::lround(e.p("view", t, 0)));
    // The other two channels as one: Screen Balance weighs the larger against the smaller.
    auto others = [&](const float* c) { return balance * std::max(c[o1], c[o2]) + (1 - balance) * std::min(c[o1], c[o2]); };
    const float screen = std::max(1e-3f, sk[dom] - others(sk));
    const int w = img.width, h = img.height;
    std::vector<float> matte(size_t(w) * size_t(h));
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* p = img.row(y);
            for (int x = 0; x < w; ++x, p += 4) {
                const float a = p[3];
                float m = 0;
                if (a > 0) {
                    const float c[3] = {p[0] / a, p[1] / a, p[2] / a};
                    const float d = (c[dom] - others(c)) / screen;  // 1 on the screen, 0 or less on the subject
                    m = std::clamp(1 - d * gain, 0.0f, 1.0f);
                    m = std::clamp((m - black) / (white - black), 0.0f, 1.0f);
                }
                matte[size_t(y) * size_t(w) + size_t(x)] = m;
            }
        }
    });
    const double shrink = e.p("shrink", t, 0) * pixelScale, soften = e.p("soften", t, 0) * pixelScale;
    if (std::lround(shrink) != 0) minMaxFilter(matte, w, h, int(std::lround(std::fabs(shrink))), shrink < 0);  // grow: maximum
    boxBlurPlane(matte, w, h, int(std::lround(soften / 2)));
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < w; ++x, p += 4) {
                const float a0 = p[3];
                const float m = matte[size_t(y) * size_t(w) + size_t(x)];
                const float na = a0 * m;
                if (view == 1) {  // the matte
                    p[0] = p[1] = p[2] = na;
                    p[3] = 1;
                    continue;
                }
                if (view == 2) {  // status: clean black and white, anything in between grey
                    const float v = na < 0.02f ? 0.0f : na > 0.98f ? 1.0f : 0.5f;
                    p[0] = p[1] = p[2] = v;
                    p[3] = 1;
                    continue;
                }
                if (a0 <= 0) continue;
                float c[3] = {p[0] / a0, p[1] / a0, p[2] / a0};
                // Despill: the screen's colour no stronger than the others, the brightness it took put back.
                const float before = luma(c[0], c[1], c[2]);
                const float limit = others(c);
                if (c[dom] > limit) c[dom] -= (c[dom] - limit) * despill;
                const float lost = (before - luma(c[0], c[1], c[2])) * restore;
                for (float& v : c) v += lost;
                if (edgeDesat > 0) {
                    const float edge = 4 * m * (1 - m) * edgeDesat, l = luma(c[0], c[1], c[2]);
                    for (float& v : c) v += (l - v) * edge;
                }
                p[0] = c[0] * na;
                p[1] = c[1] * na;
                p[2] = c[2] * na;
                p[3] = na;
            }
        }
    });
}

}  // namespace

bool estimateScreenColor(const Image& img, double rgb[3]) {
    if (img.width <= 0 || img.height <= 0) return false;
    const size_t total = size_t(img.width) * size_t(img.height);
    const size_t step = std::max<size_t>(1, total / 200000);
    std::vector<std::array<float, 4>> green, blue;  // r, g, b, margin
    size_t seen = 0;
    for (size_t i = 0; i < total; i += step, ++seen) {
        const float* p = &img.px[i * 4];
        if (p[3] <= 0.5f) continue;
        const float r = p[0] / p[3], g = p[1] / p[3], b = p[2] / p[3];
        if (g - std::max(r, b) > 0.1f) green.push_back({r, g, b, g - std::max(r, b)});
        else if (b - std::max(r, g) > 0.1f) blue.push_back({r, g, b, b - std::max(r, g)});
    }
    auto& pick = green.size() >= blue.size() ? green : blue;
    if (seen == 0 || pick.size() < seen / 50) return false;
    std::sort(pick.begin(), pick.end(), [](const auto& a, const auto& b) { return a[3] > b[3]; });
    pick.resize(std::max<size_t>(1, pick.size() / 2));
    for (int c = 0; c < 3; ++c) {
        std::vector<float> v(pick.size());
        for (size_t i = 0; i < pick.size(); ++i) v[i] = pick[i][size_t(c)];
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
        rgb[c] = v[v.size() / 2];
    }
    return true;
}

namespace {

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

namespace {

// Percentiles 5, 10, 20 ... 90, 95 of each unpremultiplied channel (opaque pixels, up to ~100k of them).
constexpr int kMatchQuantiles = 11;
bool channelPercentiles(const Image& img, double out[3][kMatchQuantiles]) {
    if (img.empty()) return false;
    const size_t n = img.px.size() / 4, stride = std::max<size_t>(1, n / 100000);
    std::vector<float> ch[3];
    for (size_t i = 0; i < n; i += stride) {
        const float* p = &img.px[i * 4];
        if (p[3] < 0.5f) continue;
        for (int k = 0; k < 3; ++k) ch[k].push_back(p[k] / p[3]);
    }
    if (ch[0].size() < 16) return false;
    static const double q[kMatchQuantiles] = {0.05, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95};
    for (int k = 0; k < 3; ++k) {
        std::sort(ch[k].begin(), ch[k].end());
        for (int j = 0; j < kMatchQuantiles; ++j) out[k][j] = ch[k][size_t(std::llround(q[j] * double(ch[k].size() - 1)))];
    }
    return true;
}

}  // namespace

Effect colorMatchCorrection(const Image& img, const Image& reference, Id effectId) {
    Effect e = makeEffect("color_correct", effectId);
    double s[3][kMatchQuantiles], r[3][kMatchQuantiles];
    if (!channelPercentiles(img, s) || !channelPercentiles(reference, r)) return e;
    static const char* const names[3][3] = {{"lift_r", "gain_r", "gamma_r"}, {"lift_g", "gain_g", "gamma_g"}, {"lift_b", "gain_b", "gamma_b"}};
    for (int c = 0; c < 3; ++c) {
        // Color Correct does v * gain, then lift (v + lift * (1 - v)), then v^(1 / gamma): an affine map
        // then a power. For a power, the affine part is the least-squares line from the image's
        // percentiles to the reference's taken back through the power; the power is the one whose
        // result lies closest to the reference's percentiles.
        auto fit = [&](double gamma, double& a, double& b) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0;
            for (int j = 0; j < kMatchQuantiles; ++j) {
                const double x = s[c][j], y = std::pow(std::max(0.0, r[c][j]), gamma);
                sx += x;
                sy += y;
                sxx += x * x;
                sxy += x * y;
            }
            const double n = kMatchQuantiles, den = n * sxx - sx * sx;
            a = den > 1e-12 ? (n * sxy - sx * sy) / den : 1.0;
            b = (sy - a * sx) / n;
            double err = 0;
            for (int j = 0; j < kMatchQuantiles; ++j) {
                const double v = a * s[c][j] + b;
                const double out = v > 0 ? std::pow(v, 1 / gamma) : v;
                err += (out - r[c][j]) * (out - r[c][j]);
            }
            return err;
        };
        // Golden-section search over gamma 0.5..2 (in log2, as the parameter is).
        double lo = -1, hi = 1, a = 1, b = 0;
        const double g = (std::sqrt(5.0) - 1) / 2;
        double x1 = hi - g * (hi - lo), x2 = lo + g * (hi - lo);
        double f1 = fit(std::exp2(x1), a, b), f2 = fit(std::exp2(x2), a, b);
        for (int it = 0; it < 40; ++it) {
            if (f1 < f2) {
                hi = x2;
                x2 = x1;
                f2 = f1;
                x1 = hi - g * (hi - lo);
                f1 = fit(std::exp2(x1), a, b);
            } else {
                lo = x1;
                x1 = x2;
                f1 = f2;
                x2 = lo + g * (hi - lo);
                f2 = fit(std::exp2(x2), a, b);
            }
        }
        const double lg = (lo + hi) / 2;
        fit(std::exp2(lg), a, b);
        const double lift = std::clamp(b, -1.0, 0.9);
        e.params[names[c][0]] = lift;
        e.params[names[c][1]] = std::clamp(a / (1 - lift), 0.0, 4.0);
        e.params[names[c][2]] = lg;
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

namespace {
constexpr double kFar = 1e30;
// Squared distance transform of one row or column: the lower envelope of the
// parabolas rooted at the finite entries of f (seeds are 0, others kFar).
void edt1d(const double* f, double* d, int n, int* v, double* z) {
    int k = -1;
    for (int q = 0; q < n; ++q) {
        if (f[q] >= kFar) continue;
        double s = -kFar;
        while (k >= 0) {
            const int p = v[k];
            s = ((f[q] + double(q) * q) - (f[p] + double(p) * p)) / (2.0 * (q - p));
            if (s > z[k]) break;
            --k;
        }
        ++k;
        v[k] = q;
        z[k] = k == 0 ? -kFar : s;
        z[k + 1] = kFar;
    }
    if (k < 0) {
        std::fill(d, d + n, kFar);
        return;
    }
    int j = 0;
    for (int q = 0; q < n; ++q) {
        while (z[j + 1] < q) ++j;
        d[q] = double(q - v[j]) * (q - v[j]) + f[v[j]];
    }
}
}  // namespace

std::vector<float> distanceTransform(const std::vector<uint8_t>& seed, int w, int h) {
    std::vector<double> g(size_t(w) * size_t(h));
    for (size_t i = 0; i < g.size(); ++i) g[i] = seed[i] ? 0.0 : kFar;
    // Columns, then rows.
    parallelRows(w, [&](int x0, int x1) {
        std::vector<double> f(static_cast<size_t>(h)), d(static_cast<size_t>(h)), z(static_cast<size_t>(h) + 1);
        std::vector<int> v(static_cast<size_t>(h));
        for (int x = x0; x < x1; ++x) {
            for (int y = 0; y < h; ++y) f[size_t(y)] = g[size_t(y) * size_t(w) + size_t(x)];
            edt1d(f.data(), d.data(), h, v.data(), z.data());
            for (int y = 0; y < h; ++y) g[size_t(y) * size_t(w) + size_t(x)] = d[size_t(y)];
        }
    });
    std::vector<float> out(g.size());
    parallelRows(h, [&](int y0, int y1) {
        std::vector<double> d(static_cast<size_t>(w)), z(static_cast<size_t>(w) + 1);
        std::vector<int> v(static_cast<size_t>(w));
        for (int y = y0; y < y1; ++y) {
            edt1d(&g[size_t(y) * size_t(w)], d.data(), w, v.data(), z.data());
            for (int x = 0; x < w; ++x) out[size_t(y) * size_t(w) + size_t(x)] = float(d[size_t(x)] >= kFar ? 1e15 : std::sqrt(d[size_t(x)]));
        }
    });
    return out;
}

// A matte from a field that is positive inside and crosses zero at the edge,
// feathered and expanded by signed distance to that edge.
std::vector<float> fieldMatte(const std::vector<float>& L, int W, int H, double feather, double expand) {
    std::vector<float> matte(L.size(), 0.f);
    // Signed distance to the edge (negative inside). Next to the edge it comes
    // from the field's zero crossing (sub-pixel, so the edge is smooth);
    // further away from an exact distance transform, needed only when the edge
    // is expanded, contracted or feathered beyond a couple of pixels.
    const bool far = std::fabs(expand) > 0.25 || feather > 2.5;
    std::vector<float> distIn, distOut;
    if (far) {
        std::vector<uint8_t> inside(L.size()), outside(L.size());
        for (size_t i = 0; i < L.size(); ++i) {
            inside[i] = L[i] > 0;
            outside[i] = !inside[i];
        }
        distOut = distanceTransform(inside, W, H);  // from outside pixels to the object
        distIn = distanceTransform(outside, W, H);  // from inside pixels to the background
    }
    const double fe = std::max(1.0, feather);
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * size_t(W) + size_t(x);
                const float l = L[i];
                const float gx = L[size_t(y) * size_t(W) + size_t(std::min(x + 1, W - 1))] - L[size_t(y) * size_t(W) + size_t(std::max(x - 1, 0))];
                const float gy = L[size_t(std::min(y + 1, H - 1)) * size_t(W) + size_t(x)] - L[size_t(std::max(y - 1, 0)) * size_t(W) + size_t(x)];
                const double grad = 0.5 * std::sqrt(double(gx) * gx + double(gy) * gy);
                double d = grad > 1e-6 ? -l / grad : (l > 0 ? -1e6 : 1e6);
                if (far && std::fabs(d) > 1.0) d = l > 0 ? -(distIn[i] - 0.5) : distOut[i] - 0.5;
                d -= expand;
                const double k = std::clamp(0.5 - d / fe, 0.0, 1.0);
                matte[i] = float(k * k * (3 - 2 * k));
            }
    });
    return matte;
}

std::vector<float> objectMatte(const std::vector<float>& logits, int W, int H, double feather, double expand) {
    std::vector<float> matte(size_t(W) * size_t(H), 0.f);
    constexpr int G = kObjectGrid;
    if (logits.size() != size_t(G) * G || W <= 0 || H <= 0) return matte;
    // The logits resampled to the image (bilinear, pixel centres aligned as the model's upsampling is).
    std::vector<float> L(size_t(W) * size_t(H));
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const double gy = std::clamp((y + 0.5) * G / H - 0.5, 0.0, G - 1.0);
            const int ya = int(gy), yb = std::min(ya + 1, G - 1);
            const float fy = float(gy - ya);
            for (int x = 0; x < W; ++x) {
                const double gx = std::clamp((x + 0.5) * G / W - 0.5, 0.0, G - 1.0);
                const int xa = int(gx), xb = std::min(xa + 1, G - 1);
                const float fx = float(gx - xa);
                const float a = logits[size_t(ya) * G + size_t(xa)] + (logits[size_t(ya) * G + size_t(xb)] - logits[size_t(ya) * G + size_t(xa)]) * fx;
                const float b = logits[size_t(yb) * G + size_t(xa)] + (logits[size_t(yb) * G + size_t(xb)] - logits[size_t(yb) * G + size_t(xa)]) * fx;
                L[size_t(y) * size_t(W) + size_t(x)] = a + (b - a) * fy;
            }
        }
    });
    return fieldMatte(L, W, H, feather, expand);
}

namespace {
thread_local std::shared_ptr<const DepthMap> tDepth;
thread_local std::shared_ptr<const ValueMap> tPerson;
thread_local std::shared_ptr<const std::vector<FaceBox>> tFaces;
// The media time of the frame applyVideoEffect() is working on, for the effects that follow the footage.
thread_local double tSourceSeconds = -1;
// The media it comes from (0: not known) and whether it was reframed from 360° (TrackedSourceScope).
thread_local uint64_t tSourceMedia = 0;
thread_local bool tReframed = false;
}  // namespace

const std::vector<FaceBox>* currentFaces() { return tFaces.get(); }
FaceScope::FaceScope(std::shared_ptr<const std::vector<FaceBox>> faces) : previous_(std::move(tFaces)) { tFaces = std::move(faces); }
FaceScope::~FaceScope() { tFaces = std::move(previous_); }

const DepthMap* currentDepth() { return tDepth.get(); }
DepthScope::DepthScope(std::shared_ptr<const DepthMap> depth) : previous_(std::move(tDepth)) { tDepth = std::move(depth); }
DepthScope::~DepthScope() { tDepth = std::move(previous_); }
const ValueMap* currentPersonMatte() { return tPerson.get(); }
PersonScope::PersonScope(std::shared_ptr<const ValueMap> matte) : previous_(std::move(tPerson)) { tPerson = std::move(matte); }
PersonScope::~PersonScope() { tPerson = std::move(previous_); }

void refineMatte(std::vector<float>& matte, int W, int H, double expand, double feather, double blur) {
    if (matte.size() != size_t(W) * size_t(H)) return;
    if (std::fabs(expand) >= 0.25) {
        std::vector<uint8_t> inside(matte.size()), outside(matte.size());
        for (size_t i = 0; i < matte.size(); ++i) {
            inside[i] = matte[i] > 0.5f;
            outside[i] = !inside[i];
        }
        const std::vector<float> distOut = distanceTransform(inside, W, H), distIn = distanceTransform(outside, W, H);
        const double fe = std::max(1.0, feather);
        for (size_t i = 0; i < matte.size(); ++i) {
            const double d = (inside[i] ? -(distIn[i] - 0.5) : distOut[i] - 0.5) - expand;
            const double k = std::clamp(0.5 - d / fe, 0.0, 1.0);
            matte[i] = float(k * k * (3 - 2 * k));
        }
        return;
    }
    if (blur < 0.5) return;
    Image m(W, H, Image::Uninitialized{});
    for (size_t i = 0; i < matte.size(); ++i) std::fill_n(&m.px[i * 4], 4, matte[i]);
    gaussianBlur(m, blur);
    for (size_t i = 0; i < matte.size(); ++i) matte[i] = m.px[i * 4];
}

// How much of each pixel a polygon (pixel coordinates, non-zero winding)
// covers: four scanlines a row, each span's ends to a fraction of a pixel.
std::vector<float> polygonCoverage(const std::vector<std::pair<double, double>>& poly, int W, int H) {
    std::vector<float> cov(size_t(W) * size_t(H), 0.f);
    const size_t n = poly.size();
    if (n < 3 || W <= 0 || H <= 0) return cov;
    constexpr int kSub = 4;
    parallelRows(H, [&](int y0, int y1) {
        std::vector<std::pair<double, int>> xs;
        for (int y = y0; y < y1; ++y) {
            float* out = &cov[size_t(y) * size_t(W)];
            for (int s = 0; s < kSub; ++s) {
                const double sy = y + (s + 0.5) / kSub;
                xs.clear();
                for (size_t i = 0; i < n; ++i) {
                    const auto& [ax, ay] = poly[i];
                    const auto& [bx, by] = poly[(i + 1) % n];
                    if ((ay <= sy) == (by <= sy)) continue;
                    xs.emplace_back(ax + (sy - ay) * (bx - ax) / (by - ay), by > ay ? 1 : -1);
                }
                std::sort(xs.begin(), xs.end());
                int winding = 0;
                double start = 0;
                for (const auto& [x, dir] : xs) {
                    const int before = winding;
                    winding += dir;
                    if (before == 0 && winding != 0) {
                        start = x;
                    } else if (before != 0 && winding == 0) {
                        const double a = std::clamp(start, 0.0, double(W)), b = std::clamp(x, 0.0, double(W));
                        if (b <= a) continue;
                        const int ia = int(a), ib = int(b);
                        constexpr float w = 1.0f / kSub;
                        if (ia == ib) {
                            out[std::min(ia, W - 1)] += float(b - a) * w;
                            continue;
                        }
                        out[ia] += float(ia + 1 - a) * w;
                        for (int i = ia + 1; i < ib; ++i) out[i] += w;
                        if (ib < W) out[ib] += float(b - ib) * w;
                    }
                }
            }
            for (int x = 0; x < W; ++x) out[x] = std::min(out[x], 1.0f);
        }
    });
    return cov;
}

std::vector<float> effectMatte(const Effect& e, FrameTime t, const Image& img, double pixelScale, double sourceSeconds) {
    std::vector<float> matte;
    if (img.empty() || !hasMask(e, t)) return matte;
    const int W = img.width, H = img.height;
    matte.assign(size_t(W) * size_t(H), 1.0f);
    const int shape = int(std::lround(e.p("mask.shape", t)));
    if (shape == 3) {
        // An object picked by the model: nothing where it was not segmented.
        std::vector<float> logits;
        if (e.object && sourceSeconds >= 0 && e.object->logitsAt(sourceSeconds, logits))
            matte = objectMatte(logits, W, H, e.p("mask.feather", t, 20) * pixelScale, e.p("mask.expansion", t) * pixelScale);
        else
            std::fill(matte.begin(), matte.end(), 0.f);
    }
    if (shape == 4) {
        // The people in the picture; nothing without the model.
        const ValueMap* person = currentPersonMatte();
        if (person && !person->empty()) {
            matte = person->resized(W, H);
            refineMatte(matte, W, H, e.p("mask.expansion", t) * pixelScale, e.p("mask.feather", t, 20) * pixelScale,
                        e.p("mask.feather", t, 20) * pixelScale * 0.25);
        } else {
            std::fill(matte.begin(), matte.end(), 0.f);
        }
    }
    if (shape == 5) {
        // A drawn path, in the mask's box: nothing until it has three points.
        const std::vector<PathPoint> pts = maskPath(e, t);
        std::fill(matte.begin(), matte.end(), 0.f);
        if (pts.size() >= 3) {
            const MaskBox box = maskBox(e, t);
            std::vector<std::pair<double, double>> poly = flattenMaskPath(pts, true, 24);
            for (auto& [x, y] : poly) {
                double u = 0, v = 0;
                boxToFrame(box, W, H, x, y, u, v);
                x = u * W;
                y = v * H;
            }
            std::vector<float> field = polygonCoverage(poly, W, H);
            for (float& f : field) f -= 0.5f;
            matte = fieldMatte(field, W, H, e.p("mask.feather", t, 20) * pixelScale, e.p("mask.expansion", t) * pixelScale);
        }
    }
    if (shape == 6) {
        // A gradient across the box: all of the effect above its top edge, none
        // below its bottom edge (in its rotated axes), widened by the feather.
        const double cy = e.p("mask.y", t, 0.5) * H, cx = e.p("mask.x", t, 0.5) * W;
        const double b = std::max(0.5, e.p("mask.h", t, 0.4) * H / 2);
        const double rot = e.p("mask.rotation", t) * M_PI / 180.0, cr = std::cos(rot), sr = std::sin(rot);
        const double span = 2 * b + std::max(0.0, e.p("mask.feather", t, 20) * pixelScale);
        const double expand = e.p("mask.expansion", t) * pixelScale;
        parallelRows(H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < W; ++x) {
                    const double px = x + 0.5 - cx, py = y + 0.5 - cy;
                    const double v = -px * sr + py * cr - expand;
                    const double k = std::clamp(0.5 - v / span, 0.0, 1.0);
                    matte[size_t(y) * size_t(W) + size_t(x)] = float(k * k * (3 - 2 * k));
                }
        });
    }
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
    if (e.p("mask.depth", t) > 0.5) {
        // A range of distances: 1 between From and To, falling to 0 over the softness either side.
        const DepthMap* depth = currentDepth();
        const double lo = e.p("mask.depth_low", t, 50) / 100, hi = e.p("mask.depth_high", t, 100) / 100;
        const double ramp = std::max(1e-4, e.p("mask.depth_soft", t, 10) / 100);
        if (!depth) {
            std::fill(matte.begin(), matte.end(), 0.f);
        } else {
            const std::vector<float> d = depth->resized(W, H);
            for (size_t i = 0; i < matte.size(); ++i) {
                const double v = d[i];
                const double out = v < lo ? lo - v : v > hi ? v - hi : 0.0;
                const double k = std::clamp(1.0 - out / ramp, 0.0, 1.0);
                matte[i] *= float(k * k * (3 - 2 * k));
            }
        }
    }
    const bool invert = e.p("mask.invert", t) > 0.5;
    const float opacity = float(std::clamp(e.p("mask.opacity", t, 100) / 100.0, 0.0, 1.0));
    for (float& m : matte) m = (invert ? 1 - m : m) * opacity;
    return matte;
}

namespace {
void applyEffectUnmasked(const Effect& e, FrameTime t, Image& img, double pixelScale);
}

namespace {
// Stabilize: the analysed camera path (strings["motion"]) smoothed, and the
// frame moved by the difference, zoomed so no edge shows. With a rolling
// shutter readout (Stabilize's Rolling Shutter, or Rolling Shutter Repair on
// its own), each row is first moved back to where the picture was mid-frame:
// it was read out later (lower) or earlier (higher) by its share of the
// readout, while the camera moved at the speed measured around that frame.
struct StabilizePlan {
    double fps = 0, start = 0, zoom = 1, readout = 0;
    std::vector<Similarity> corrections;
    std::vector<Similarity> velocity;  // camera speed per frame (fractions of the width, radians)
};

double rollingReadout(const Effect& e, FrameTime t) {
    if (e.type == "rolling_shutter") return std::clamp(e.p("readout", t, 50) / 100, 0.0, 1.0);
    return std::clamp(e.p("rolling_shutter", t, 0) / 100, 0.0, 1.0);
}
std::shared_ptr<const StabilizePlan> stabilizePlan(const Effect& e, FrameTime t, double aspect) {
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<const StabilizePlan>> cache;
    const std::string& data = e.s("motion");
    if (data.empty()) return nullptr;
    const bool steady = e.type == "stabilize";
    const double smooth = e.p("smoothness", t, 1.5);
    const int method = int(e.p("method", t, 2));
    const bool fill = e.p("framing", t, 0) < 0.5;
    const double readout = rollingReadout(e, t);
    char key[128];
    std::snprintf(key, sizeof key, "%d|%.4f|%d|%d|%.4f|%.4f|%zu", steady ? 1 : 0, smooth, method, fill ? 1 : 0, aspect, readout,
                  std::hash<std::string>{}(data));
    std::lock_guard lock(m);
    auto& slot = cache[key];
    if (slot) return slot;
    CameraMotion motion;
    if (!cameraMotionFromString(data, motion)) return nullptr;
    auto plan = std::make_shared<StabilizePlan>();
    plan->fps = motion.fps;
    plan->start = motion.start;
    const MotionModel model = method == 0 ? MotionModel::Translation : method == 1 ? MotionModel::TranslationScale : MotionModel::Similarity;
    plan->corrections = steady ? stabilizationCorrections(motion, smooth, model) : std::vector<Similarity>(motion.steps.size());
    plan->zoom = fill && steady ? stabilizationZoom(plan->corrections, aspect) : 1.0;
    plan->readout = readout;
    if (readout > 0 && !motion.steps.empty()) {
        // The speed at a frame: the mean of the steps into and out of it (steps[0] is the identity).
        const size_t n = motion.steps.size();
        plan->velocity.resize(n);
        double reach = 0;  // the furthest a top or bottom row moves, as a share of the frame
        for (size_t i = 0; i < n; ++i) {
            const Similarity& in = motion.steps[std::min(std::max<size_t>(i, 1), n - 1)];
            const Similarity& out = motion.steps[std::min(i + 1, n - 1)];
            Similarity& v = plan->velocity[i];
            v.tx = (in.tx + out.tx) / 2;
            v.ty = (in.ty + out.ty) / 2;
            v.angle = (in.angle + out.angle) / 2;
            reach = std::max({reach, std::fabs(v.tx) * readout / 2, std::fabs(v.ty) * readout / 2 / std::max(1e-6, aspect),
                              std::fabs(v.angle) * readout / 2 * 0.5 * std::hypot(1.0, aspect) / std::max(1e-6, aspect)});
        }
        if (fill) plan->zoom *= 1 + 2 * std::min(reach, 0.25);
    }
    if (cache.size() > 32) cache.clear();
    slot = plan;
    return plan;
}

// The analysis of a Redact Faces effect, read once per version of it.
std::shared_ptr<const FaceTracks> faceTracksOf(const Effect& e) {
    const std::string& text = e.s("tracks");
    if (text.empty()) return nullptr;
    static std::mutex m;
    static std::map<size_t, std::shared_ptr<const FaceTracks>> cache;
    const size_t key = std::hash<std::string>{}(text);
    std::lock_guard lock(m);
    auto& slot = cache[key];
    if (!slot) {
        auto t = std::make_shared<FaceTracks>();
        if (!faceTracksFromString(text, *t)) return nullptr;
        if (cache.size() > 32) cache.clear();
        cache[key] = t;
        return t;
    }
    return slot;
}

// Whether the analysis is of this picture: made from this media (when the effect says which) and not reframed since.
bool tracksOfThisSource(const Effect& e) {
    if (tReframed) return false;
    const std::string& media = e.s("media");
    return media.empty() || !tSourceMedia || media == std::to_string(tSourceMedia);
}

// Whether the analysis decides the frame alone: it found faces, it is of this picture, and the frame is within the
// seconds analysed (a still's: always). Beyond them, even by a frame, the frame's own faces are found as well.
bool tracksCover(const FaceTracks& tr, const Effect& e, double sourceSeconds) {
    if (tr.tracks.empty() || !tracksOfThisSource(e)) return false;
    if (tr.step <= 0) return true;
    if (sourceSeconds < 0) return false;
    const double slack = 0.5 / tr.fps + 0.5 * tr.step;
    return sourceSeconds >= tr.start - slack && sourceSeconds <= tr.end + slack;
}

void redactFacesEffect(const Effect& e, FrameTime t, Image& img, double sourceSeconds) {
    std::vector<FaceBox> covered, shown;
    auto tracks = faceTracksOf(e);
    const int hold = int(std::lround(e.p("hold", t, 12)));
    if (tracks && tracksCover(*tracks, e, sourceSeconds)) {
        const std::set<int> keep = trackIdsFromString(e.s("keep"));
        for (const TrackedFace& f : trackedFacesAt(*tracks, sourceSeconds, hold)) (keep.count(f.track) ? shown : covered).push_back(f.box);
    } else {
        if (const std::vector<FaceBox>* faces = currentFaces()) covered = *faces;
        // Just past the analysis, the faces it followed are still held there too.
        if (tracks && tracksOfThisSource(e) && tracks->step > 0 && sourceSeconds >= 0) {
            const std::set<int> keep = trackIdsFromString(e.s("keep"));
            for (const TrackedFace& f : trackedFacesAt(*tracks, sourceSeconds, hold))
                if (!keep.count(f.track)) covered.push_back(f.box);
        }
    }
    RedactSettings rs;
    rs.style = int(std::lround(e.p("style", t)));
    rs.strength = e.p("strength", t, 70) / 100;
    rs.ellipse = e.p("shape", t) < 0.5;
    rs.expand = e.p("expand", t, 30) / 100;
    rs.feather = e.p("feather", t, 15) / 100;
    rs.color[0] = float(e.p("color.r", t, 0)), rs.color[1] = float(e.p("color.g", t, 0)), rs.color[2] = float(e.p("color.b", t, 0));
    redactFaces(img, covered, rs);
    if (e.p("show", t) > 0.5) {
        std::vector<FaceBox> all = covered;
        all.insert(all.end(), shown.begin(), shown.end());
        std::vector<bool> red(covered.size(), true);
        red.resize(all.size(), false);
        outlineFaces(img, all, red, rs.expand);
    }
}

void stabilize(const Effect& e, FrameTime t, Image& img, double sourceSeconds) {
    if (sourceSeconds < 0 || img.empty()) return;
    auto plan = stabilizePlan(e, t, double(img.height) / img.width);
    if (!plan || plan->corrections.empty()) return;
    const long i = std::clamp<long>(std::lround((sourceSeconds - plan->start) * plan->fps), 0, long(plan->corrections.size()) - 1);
    const Similarity& c = plan->corrections[size_t(i)];
    const double zoom = plan->zoom * (1 + (e.type == "stabilize" ? e.p("extra_zoom", t, 0) / 100 : 0.0));
    const Similarity v = plan->velocity.empty() ? Similarity{} : plan->velocity[size_t(i)];
    const bool rolling = plan->readout > 0 && (std::fabs(v.tx) > 1e-9 || std::fabs(v.ty) > 1e-9 || std::fabs(v.angle) > 1e-12);
    if (!rolling && std::fabs(c.tx) < 1e-7 && std::fabs(c.ty) < 1e-7 && std::fabs(c.angle) < 1e-9 && std::fabs(c.scale - 1) < 1e-9 &&
        zoom == 1)
        return;
    // Output pixel q (centred, in widths) shows source point R(-a)/s * (q / zoom - t).
    const Image src = img;
    const double W = img.width, H = img.height;
    const double co = std::cos(-c.angle) / c.scale, si = std::sin(-c.angle) / c.scale;
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* o = img.row(y);
            for (int x = 0; x < img.width; ++x, o += 4) {
                const double qx = ((x + 0.5) - W / 2) / W / zoom - c.tx, qy = ((y + 0.5) - H / 2) / W / zoom - c.ty;
                double sx = (co * qx - si * qy) * W + W / 2 - 0.5, sy = (si * qx + co * qy) * W + H / 2 - 0.5;
                if (rolling) {
                    // The row was read `dt` frames after (or before) the middle one: the picture had moved on by the
                    // camera's speed times dt, so the point is taken from there.
                    const double dt = ((sy + 0.5) / H - 0.5) * plan->readout;
                    const double px = sx + 0.5 - W / 2, py = sy + 0.5 - H / 2;
                    const double a = v.angle * dt, ca = std::cos(a), sa = std::sin(a);
                    sx = ca * px - sa * py + v.tx * dt * W + W / 2 - 0.5;
                    sy = sa * px + ca * py + v.ty * dt * W + H / 2 - 0.5;
                }
                if (sx < -1 || sy < -1 || sx > W || sy > H) {
                    o[0] = o[1] = o[2] = o[3] = 0;
                    continue;
                }
                const int x0 = int(std::floor(sx)), yy0 = int(std::floor(sy));
                const float fx = float(sx - x0), fy = float(sy - yy0);
                const int xa = std::clamp(x0, 0, src.width - 1), xb = std::clamp(x0 + 1, 0, src.width - 1);
                const int ya = std::clamp(yy0, 0, src.height - 1), yb = std::clamp(yy0 + 1, 0, src.height - 1);
                const float *p00 = src.at(xa, ya), *p10 = src.at(xb, ya), *p01 = src.at(xa, yb), *p11 = src.at(xb, yb);
                for (int k = 0; k < 4; ++k) {
                    const float a = p00[k] + (p10[k] - p00[k]) * fx, b = p01[k] + (p11[k] - p01[k]) * fx;
                    o[k] = a + (b - a) * fy;
                }
            }
        }
    });
}
}  // namespace

namespace {
thread_local const ColorSpace* tWorkingSpace = nullptr;
}  // namespace

const ColorSpace& currentWorkingSpace() { return tWorkingSpace ? *tWorkingSpace : rec709Space(); }

void applyCdlValues(const Effect& e, FrameTime t, Image& img) {
    const Cdl cdl = effectCdl(e, t);
    if (!cdl.identity()) perPixel(img, [&](float& r, float& g, float& b, float&) { applyCdl(cdl, r, g, b); });
}
WorkingSpaceScope::WorkingSpaceScope(const ColorSpace* space) : previous_(tWorkingSpace) { tWorkingSpace = space; }
WorkingSpaceScope::~WorkingSpaceScope() { tWorkingSpace = previous_; }

TrackedSourceScope::TrackedSourceScope(uint64_t media, bool reframed) : previousMedia_(tSourceMedia), previousReframed_(tReframed) {
    tSourceMedia = media;
    tReframed = reframed;
}
TrackedSourceScope::~TrackedSourceScope() {
    tSourceMedia = previousMedia_;
    tReframed = previousReframed_;
}

bool redactNeedsLiveFaces(const Effect& e, double sourceSeconds) {
    if (!e.enabled || e.type != "redact_faces") return false;
    auto tracks = faceTracksOf(e);
    return !tracks || !tracksCover(*tracks, e, sourceSeconds);
}

void applyVideoEffect(const Effect& e, FrameTime t, Image& img, double pixelScale, double sourceSeconds) {
    if (!e.enabled || img.empty()) return;
    struct SourceTime {
        double previous;
        explicit SourceTime(double s) : previous(tSourceSeconds) { tSourceSeconds = s; }
        ~SourceTime() { tSourceSeconds = previous; }
    } sourceTime(sourceSeconds);
    if (e.type == "stabilize" || e.type == "rolling_shutter") {
        stabilize(e, t, img, sourceSeconds);  // moves the whole frame: masks do not apply
        return;
    }
    if (e.type == "object_removal") {
        // It fills its mask, so there is nothing to do without one (or without the model).
        if (!hasMask(e, t) || !inpaintAvailable() || !inpaintModel().installed()) return;
        std::vector<float> matte = effectMatte(e, t, img, pixelScale, sourceSeconds);
        const double grow = e.p("grow", t, 4) * pixelScale;
        if (grow >= 0.25) refineMatte(matte, img.width, img.height, grow, 2, 0);
        if (e.p("mask.show", t) > 0.5) {
            for (size_t i = 0; i < matte.size(); ++i) {
                float* p = &img.px[i * 4];
                p[0] = p[1] = p[2] = matte[i] * p[3];
            }
            return;
        }
        Image filled;
        cachedInpaint(img, matte, filled);
        img = std::move(filled);
        return;
    }
    if (!hasMask(e, t)) {
        applyEffectUnmasked(e, t, img, pixelScale);
        return;
    }
    const std::vector<float> matte = effectMatte(e, t, img, pixelScale, sourceSeconds);
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
// Depth Map, Depth Fog and Lens Blur (Depth), from currentDepth(); nothing without one.
void depthEffect(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const DepthMap* depth = currentDepth();
    if (!depth || depth->empty()) return;
    const int W = img.width, H = img.height;
    const std::vector<float> d = depth->resized(W, H);
    if (e.type == "depth_map") {
        const bool farWhite = e.p("invert", t) > 0.5;
        const float mix = float(std::clamp(e.p("mix", t, 100) / 100, 0.0, 1.0));
        for (size_t i = 0; i < d.size(); ++i) {
            float* p = &img.px[i * 4];
            const float g = (farWhite ? 1 - d[i] : d[i]) * p[3];
            for (int c = 0; c < 3; ++c) p[c] += (g - p[c]) * mix;
        }
        return;
    }
    if (e.type == "depth_fog") {
        // Thickening with distance beyond Starts At, towards the colour.
        const double start = std::max(0.01, e.p("start", t, 60) / 100), amount = std::clamp(e.p("amount", t, 70) / 100, 0.0, 1.0);
        const double curve = std::max(0.05, e.p("curve", t, 1.5));
        const float col[3] = {float(e.p("color.r", t, 0.78)), float(e.p("color.g", t, 0.82)), float(e.p("color.b", t, 0.88))};
        for (size_t i = 0; i < d.size(); ++i) {
            const double far = std::clamp((start - d[i]) / start, 0.0, 1.0);
            const float f = float(amount * std::pow(far, curve));
            float* p = &img.px[i * 4];
            for (int c = 0; c < 3; ++c) p[c] += (col[c] * p[3] - p[c]) * f;
        }
        return;
    }
    // Lens Blur: sharp within the focus range, blurring over the falloff to the full radius. The
    // frame is blurred at a few radii and each pixel takes the two nearest its own.
    const double focus = e.p("focus", t, 80) / 100, half = e.p("range", t, 10) / 200;
    const double falloff = std::max(0.01, e.p("falloff", t, 30) / 100);
    const bool blurNear = e.p("near", t, 1) > 0.5;
    const double radius = std::max(0.0, e.p("radius", t, 12) * pixelScale);
    if (radius < 0.25) return;
    std::vector<float> amount(d.size());
    for (size_t i = 0; i < d.size(); ++i) {
        double off = std::fabs(d[i] - focus) - half;
        if (!blurNear && d[i] > focus) off = 0;
        amount[i] = float(std::clamp(off / falloff, 0.0, 1.0));
    }
    constexpr int kLevels = 5;  // 0, 1/4, 1/2, 3/4 and the full radius
    std::vector<Image> levels(kLevels);
    levels[0] = img;
    for (int l = 1; l < kLevels; ++l) {
        levels[size_t(l)] = img;
        gaussianBlur(levels[size_t(l)], radius * l / (kLevels - 1));
    }
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * size_t(W) + size_t(x);
                const float pos = amount[i] * (kLevels - 1);
                const int a = std::min(kLevels - 2, int(pos));
                const float f = pos - a;
                const float* pa = &levels[size_t(a)].px[i * 4];
                const float* pb = &levels[size_t(a + 1)].px[i * 4];
                float* o = &img.px[i * 4];
                for (int c = 0; c < 4; ++c) o[c] = pa[c] + (pb[c] - pa[c]) * f;
            }
    });
}

void applyEffectUnmasked(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const std::string& ty = e.type;
    if (ty == "color_correct") colorCorrect(e, t, img);
    else if (ty == "curves") curves(e, t, img);
    else if (ty == "hue_curves") hueCurves(e, t, img);
    else if (ty == "color_warper") colorWarper(e, t, img);
    else if (ty == "hdr_palette") hdrPalette(e, t, img);
    else if (ty == "hue_sat") hueSat(e, t, img);
    else if (ty == "lut") applyLut(e, t, img);
    else if (ty == "cdl") applyCdlEffect(e, t, img);
    else if (ty == "color_space_transform") {
        const ColorSpace* from = nullptr;
        const ColorSpace* to = nullptr;
        for (const auto& cs : colorSpaces()) {
            if (cs.label == e.s("from")) from = &cs;
            if (cs.label == e.s("to")) to = &cs;
        }
        if (from && to) convertColor(img, *from, *to);
    } else if (ty == "ocio")
        applyOcio(e, img);
    else if (ty == "ofx")
        ofx::applyEffect(e, double(t), img, pixelScale, ofx::currentFetch());
    else if (ty == "chroma_key") chromaKey(e, t, img);
    else if (ty == "screen_key") screenKey(e, t, img, pixelScale);
    else if (ty == "luma_key") lumaKey(e, t, img);
    else if (ty == "black_white") blackWhite(e, t, img);
    else if (ty == "invert") invert(e, t, img);
    else if (ty == "vignette") vignette(e, t, img);
    else if (ty == "mosaic") mosaic(e, t, img, pixelScale);
    else if (ty == "mirror") mirror(e, t, img);
    else if (ty == "drop_shadow") dropShadow(e, t, img, pixelScale);
    else if (ty == "levels") vfx::levels(e, t, img);
    else if (ty == "broadcast_safe")
        broadcastSafe(img.px.data(), img.width, img.height, e.p("limits", t, 0) >= 0.5, e.p("knee", t, 5) / 100.0, e.p("highlight", t, 0) >= 0.5);
    else if (ty == "glow") vfx::glow(e, t, img, pixelScale);
    else if (ty == "film_grain") vfx::filmGrain(e, t, img, pixelScale);
    else if (ty == "film_look") filmLook(img, filmLookSettings(e, t), t);
    else if (ty == "directional_blur") vfx::directionalBlur(e, t, img, pixelScale);
    else if (ty == "chromatic_aberration") vfx::chromaticAberration(e, t, img, pixelScale);
    else if (ty == "lens_distortion") vfx::lensDistortion(e, t, img);
    else if (ty == "magnify") vfx::magnify(e, t, img, pixelScale);
    else if (ty == "channel_blur") vfx::channelBlur(e, t, img, pixelScale);
    else if (ty == "noise") vfx::noise(e, t, img);
    else if (ty == "corner_pin") vfx::cornerPin(e, t, img);
    else if (ty == "letterbox") vfx::letterbox(e, t, img);
    else if (ty == "posterize") vfx::posterize(e, t, img);
    else if (ty == "wave_warp") sfx::waveWarp(e, t, img, pixelScale);
    else if (ty == "twirl") sfx::twirl(e, t, img, pixelScale);
    else if (ty == "spherize") sfx::spherize(e, t, img, pixelScale);
    else if (ty == "ripple") sfx::ripple(e, t, img, pixelScale);
    else if (ty == "turbulent_displace") sfx::turbulentDisplace(e, t, img, pixelScale);
    else if (ty == "motion_tile") sfx::motionTile(e, t, img, pixelScale);
    else if (ty == "find_edges") sfx::findEdges(e, t, img);
    else if (ty == "emboss") sfx::emboss(e, t, img, pixelScale);
    else if (ty == "halftone") sfx::halftone(e, t, img, pixelScale);
    else if (ty == "duotone") sfx::duotone(e, t, img);
    else if (ty == "vhs") sfx::vhs(e, t, img, pixelScale);
    else if (ty == "lens_flare") sfx::lensFlare(e, t, img);
    else if (ty == "tilt_shift") sfx::tiltShift(e, t, img, pixelScale);
    else if (ty == "camera_shake") sfx::cameraShake(e, t, img, pixelScale);
    else if (ty == "depth_map" || ty == "depth_fog" || ty == "depth_blur")
        depthEffect(e, t, img, pixelScale);
    else if (ty == "relight") {
        if (const DepthMap* depth = currentDepth()) {
            RelightSettings rs;
            rs.azimuth = e.p("direction", t, 135);
            rs.elevation = e.p("elevation", t, 35);
            rs.color[0] = float(e.p("color.r", t, 1.0)), rs.color[1] = float(e.p("color.g", t, 0.95)), rs.color[2] = float(e.p("color.b", t, 0.85));
            rs.intensity = e.p("intensity", t, 100) / 100;
            rs.shadows = e.p("shadows", t, 50) / 100;
            rs.relief = e.p("relief", t, 3);
            rs.smoothness = e.p("smoothness", t, 8) * pixelScale;
            rs.reach = e.p("reach", t, 100) / 100;
            rs.showNormals = e.p("normals", t) > 0.5;
            relight(img, *depth, rs);
        }
    }
    else if (ty == "face_refine") {
        if (const std::vector<FaceBox>* faces = currentFaces()) {
            FaceRefineSettings fs;
            fs.smooth = e.p("smooth", t, 40) / 100;
            fs.lighten = e.p("lighten", t) / 100;
            fs.eyesBright = e.p("eyes_bright", t, 20) / 100;
            fs.eyesSharp = e.p("eyes_sharp", t, 30) / 100;
            fs.showMask = e.p("show", t) > 0.5;
            refineFaces(img, *faces, fs);
        }
    } else if (ty == "redact_faces") {
        redactFacesEffect(e, t, img, tSourceSeconds);
    } else if (ty == "blemish_remover") {
        if (const std::vector<FaceBox>* faces = currentFaces()) {
            BlemishSettings bs;
            bs.amount = e.p("amount", t, 100) / 100;
            bs.sensitivity = e.p("sensitivity", t, 50) / 100;
            bs.maxSize = e.p("size", t, 6) / 100;
            bs.showSpots = e.p("show", t) > 0.5;
            removeBlemishes(img, *faces, bs);
        }
    } else if (ty == "remove_background") {
        // Transparent where no one is (or where someone is, keeping the background); nothing without the model.
        const ValueMap* person = currentPersonMatte();
        if (person && !person->empty()) {
            std::vector<float> m = person->resized(img.width, img.height);
            const double soften = e.p("soften", t) * pixelScale;
            refineMatte(m, img.width, img.height, e.p("shift", t) * pixelScale, soften, soften);
            const bool keepBackground = e.p("keep", t) > 0.5;
            for (size_t i = 0; i < m.size(); ++i) {
                const float k = keepBackground ? 1 - m[i] : m[i];
                for (int c = 0; c < 4; ++c) img.px[i * 4 + size_t(c)] *= k;
            }
        }
    }
    else if (ty == "gaussian_blur") {
        int dir = int(e.p("direction", t));
        gaussianBlur(img, e.p("radius", t, 10) * pixelScale, dir != 2, dir != 1);
    } else if (ty == "focus_repair") {
        FocusRepair o;
        o.blur = e.p("blur", t, 1.5) * pixelScale;
        o.iterations = int(std::lround(e.p("iterations", t, 20)));
        o.strength = e.p("strength", t, 100) / 100.0;
        o.noise = e.p("noise", t, 1) / 100.0;
        focusRepair(img, o);
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

namespace {
std::mutex gLutMutex;
std::map<std::string, std::shared_ptr<const Lut3D>> gLutCache;
}  // namespace

void forgetCubeLut(const std::string& path) {
    std::lock_guard lock(gLutMutex);
    gLutCache.erase(path);
}

std::shared_ptr<const Lut3D> loadCubeLut(const std::string& path, std::string* error) {
    auto& m = gLutMutex;
    auto& cache = gLutCache;
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

std::vector<float> buildFlatCurve(const std::string& pointsStr, int n, bool cyclic) {
    std::vector<std::pair<double, double>> pts;
    std::istringstream is(pointsStr);
    std::string tok;
    while (is >> tok) {
        auto comma = tok.find(',');
        if (comma == std::string::npos) continue;
        try {
            pts.push_back({std::clamp(std::stod(tok.substr(0, comma)), 0.0, 1.0), std::clamp(std::stod(tok.substr(comma + 1)), 0.0, 1.0)});
        } catch (...) {
        }
    }
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end(), [](auto& a, auto& b) { return std::fabs(a.first - b.first) < 1e-9; }), pts.end());
    std::vector<float> lut(size_t(n) + 1, 0.5f);
    if (pts.empty()) return lut;
    if (pts.size() == 1) {
        std::fill(lut.begin(), lut.end(), float(pts[0].second));
        return lut;
    }
    // Neighbours beyond the ends: the points again a turn away (hue), or the end held (levels).
    std::vector<std::pair<double, double>> ext;
    const size_t k = pts.size();
    if (cyclic) {
        ext.push_back({pts[k - 2].first - 1, pts[k - 2].second});
        ext.push_back({pts[k - 1].first - 1, pts[k - 1].second});
        ext.insert(ext.end(), pts.begin(), pts.end());
        ext.push_back({pts[0].first + 1, pts[0].second});
        ext.push_back({pts[1].first + 1, pts[1].second});
    } else {
        ext.push_back({-1.0, pts[0].second});
        ext.push_back({std::min(-1e-6, pts[0].first - 1e-6), pts[0].second});
        ext.insert(ext.end(), pts.begin(), pts.end());
        ext.push_back({std::max(1 + 1e-6, pts[k - 1].first + 1e-6), pts[k - 1].second});
        ext.push_back({2.0, pts[k - 1].second});
    }
    for (int i = 0; i <= n; ++i) {
        const double x = double(i) / n;
        size_t j = 1;
        while (j + 2 < ext.size() && ext[j + 1].first < x) ++j;
        // A Hermite segment between ext[j] and ext[j + 1] with monotone tangents (harmonic
        // means of the neighbouring slopes, flat at a peak): no overshoot, so a stretch
        // left flat changes nothing.
        const auto& p0 = ext[j - 1];
        const auto& p1 = ext[j];
        const auto& p2 = ext[j + 1];
        const auto& p3 = ext[std::min(j + 2, ext.size() - 1)];
        const double dx = std::max(1e-9, p2.first - p1.first);
        const double u = std::clamp((x - p1.first) / dx, 0.0, 1.0);
        auto slope = [](const std::pair<double, double>& a, const std::pair<double, double>& b) {
            return (b.second - a.second) / std::max(1e-9, b.first - a.first);
        };
        auto tangent = [](double d0, double d1) { return d0 * d1 <= 0 ? 0.0 : 2 * d0 * d1 / (d0 + d1); };
        const double d0 = slope(p0, p1), d1 = slope(p1, p2), d2 = slope(p2, p3);
        const double m1 = tangent(d0, d1) * dx, m2 = tangent(d1, d2) * dx;
        const double u2 = u * u, u3 = u2 * u;
        const double y = (2 * u3 - 3 * u2 + 1) * p1.second + (u3 - 2 * u2 + u) * m1 + (-2 * u3 + 3 * u2) * p2.second + (u3 - u2) * m2;
        lut[size_t(i)] = float(std::clamp(y, 0.0, 1.0));
    }
    return lut;
}

namespace {

void hueCurves(const Effect& e, FrameTime t, Image& img) {
    const std::string hh = e.s("hue_hue"), hs = e.s("hue_sat"), hl = e.s("hue_luma"), ls = e.s("luma_sat"), ss = e.s("sat_sat");
    if (hh.empty() && hs.empty() && hl.empty() && ls.empty() && ss.empty()) return;
    constexpr int n = 360;
    const auto lHH = buildFlatCurve(hh, n, true), lHS = buildFlatCurve(hs, n, true), lHL = buildFlatCurve(hl, n, true);
    const auto lLS = buildFlatCurve(ls, n, false), lSS = buildFlatCurve(ss, n, false);
    const float mix = float(std::clamp(e.p("mix", t, 100) / 100.0, 0.0, 1.0));
    auto look = [](const std::vector<float>& lut, float x) {
        const float f = clamp01(x) * float(n);
        const int i = std::min(n - 1, int(f));
        return lut[size_t(i)] + (lut[size_t(i) + 1] - lut[size_t(i)]) * (f - float(i));
    };
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        const float v = std::max({r, g, b}), mn = std::min({r, g, b}), c = v - mn;
        if (v <= 0) return;
        const float s = c / v;
        float h = 0;
        if (c > 1e-6f) {
            if (v == r) h = std::fmod((g - b) / c + 6.0f, 6.0f);
            else if (v == g) h = (b - r) / c + 2;
            else h = (r - g) / c + 4;
            h /= 6;
        }
        const float l = luma(r, g, b);
        // Hue turned by up to half a turn, saturation scaled up to twice, brightness with the colour's saturation.
        float nh = h + (look(lHH, h) - 0.5f);
        nh -= std::floor(nh);
        const float ns = std::clamp(s * 2 * look(lHS, h) * 2 * look(lLS, l) * 2 * look(lSS, s), 0.0f, 1.0f);
        const float nv = v * std::max(0.0f, 1 + (look(lHL, h) - 0.5f) * 2 * s);
        // Back from hue, saturation and value.
        const float cc = nv * ns, hp = nh * 6, xx = cc * (1 - std::fabs(std::fmod(hp, 2.0f) - 1)), m = nv - cc;
        float rr, gg, bb;
        switch (int(hp) % 6) {
            case 0: rr = cc, gg = xx, bb = 0; break;
            case 1: rr = xx, gg = cc, bb = 0; break;
            case 2: rr = 0, gg = cc, bb = xx; break;
            case 3: rr = 0, gg = xx, bb = cc; break;
            case 4: rr = xx, gg = 0, bb = cc; break;
            default: rr = cc, gg = 0, bb = xx; break;
        }
        r += (rr + m - r) * mix;
        g += (gg + m - g) * mix;
        b += (bb + m - b) * mix;
    });
}

void colorWarper(const Effect& e, FrameTime t, Image& img) {
    ColorWarp w;
    if (!parseColorWarp(e.s("mesh"), w) || w.empty()) return;
    const WarpField field(w);
    const float mix = float(std::clamp(e.p("mix", t, 100) / 100.0, 0.0, 1.0));
    const bool keepLuma = e.p("preserve_luma", t) > 0.5;
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        const float v = std::max({r, g, b}), mn = std::min({r, g, b}), c = v - mn;
        if (v <= 0) return;
        const float s = c / v;
        float h = 0;
        if (c > 1e-6f) {
            if (v == r) h = std::fmod((g - b) / c + 6.0f, 6.0f);
            else if (v == g) h = (b - r) / c + 2;
            else h = (r - g) / c + 4;
            h /= 6;
        }
        double nhd, nsd, k;
        field.apply(h, s, nhd, nsd, k);
        const float nh = float(nhd), ns = float(nsd), nv = v * float(k);
        const float cc = nv * ns, hp = nh * 6, xx = cc * (1 - std::fabs(std::fmod(hp, 2.0f) - 1)), m = nv - cc;
        float rr, gg, bb;
        switch (int(hp) % 6) {
            case 0: rr = cc, gg = xx, bb = 0; break;
            case 1: rr = xx, gg = cc, bb = 0; break;
            case 2: rr = 0, gg = cc, bb = xx; break;
            case 3: rr = 0, gg = xx, bb = cc; break;
            case 4: rr = xx, gg = 0, bb = cc; break;
            default: rr = cc, gg = 0, bb = xx; break;
        }
        rr += m, gg += m, bb += m;
        if (keepLuma) {
            // The new colour at the old brightness (times the point's own brightness change).
            const float before = luma(r, g, b) * float(k), after = luma(rr, gg, bb);
            if (after > 1e-6f) {
                const float f = before / after;
                rr *= f, gg *= f, bb *= f;
            }
        }
        r += (rr - r) * mix;
        g += (gg - g) * mix;
        b += (bb - b) * mix;
    });
}

}  // namespace

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

namespace {

// Distance to the centre in units of the shape's size (1 on its edge), for Shape Wipe.
float shapeDistance(int shape, float qx, float qy) {
    const float ax = std::fabs(qx), ay = std::fabs(qy), r = std::hypot(qx, qy);
    switch (shape) {
        case 1: return ax + ay;                                           // diamond
        case 2: return std::max(ax, ay);                                  // square
        case 3: {                                                         // five-pointed star
            const float t = std::atan2(-qy, qx) - float(M_PI) / 2;
            return r / (0.62f + 0.38f * std::cos(5 * t));
        }
        case 4: {  // heart: a polar heart, its notch at the top and point at the bottom
            const float t = std::atan2(-qy + 0.35f * r, qx), s = std::sin(t), c = std::cos(t);
            const float rho = (2 - 2 * s + s * std::sqrt(std::fabs(c)) / (s + 1.4f)) / 2.4f;
            return r / std::max(0.05f, rho);
        }
        case 5: return std::min(std::max(ax / 0.35f, ay), std::max(ax, ay / 0.35f));  // cross
        default: return r;                                                             // circle
    }
}

// A unit square, turned in 3D and seen in perspective, as the inverse of its projection:
// screen (x, y) to texture (u, v). `corners` are TL, TR, BR, BL in 3D (camera at z = -f).
struct Face {
    double inv[9];
    bool facing = false;
};
Face projectFace(const double corners[4][3], double f, int w, int h) {
    Face face;
    double quad[4][2];
    for (int i = 0; i < 4; ++i) {
        const double k = f / (f + corners[i][2]);
        quad[i][0] = w / 2.0 + corners[i][0] * k;
        quad[i][1] = h / 2.0 + corners[i][1] * k;
    }
    // Facing the camera when the projected corners go round clockwise (as the picture's do).
    double area = 0;
    for (int i = 0; i < 4; ++i) area += quad[i][0] * quad[(i + 1) % 4][1] - quad[(i + 1) % 4][0] * quad[i][1];
    face.facing = area > 1e-6;
    double hm[9];
    if (!face.facing || !vfx::squareToQuad(quad, hm)) {
        face.facing = false;
        return face;
    }
    const double a = hm[0], b = hm[1], c = hm[2], d = hm[3], e = hm[4], g = hm[5], p = hm[6], q = hm[7], r = hm[8];
    const double det = a * (e * r - g * q) - b * (d * r - g * p) + c * (d * q - e * p);
    if (std::fabs(det) < 1e-12) {
        face.facing = false;
        return face;
    }
    const double k = 1 / det;
    const double inv[9] = {(e * r - g * q) * k, (c * q - b * r) * k, (b * g - c * e) * k, (g * p - d * r) * k, (a * r - c * p) * k,
                           (c * d - a * g) * k, (d * q - e * p) * k, (b * p - a * q) * k, (a * e - b * d) * k};
    std::copy(inv, inv + 9, face.inv);
    return face;
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
    const float pi = float(M_PI);
    const float bell = std::sin(pi * uf);  // 0 at the ends, 1 halfway
    auto zero = [](float o[4]) { o[0] = o[1] = o[2] = o[3] = 0; };
    auto sampleOr = [&](const Image& img, float sx, float sy, float o[4]) {
        if (img.empty()) zero(o);
        else sample(img, sx, sy, o);
    };
    auto lumaOf = [](const float* p) { return p[3] > 1e-6f ? (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / p[3] : 0.0f; };
    auto hashf = [](uint32_t x) {
        x ^= x >> 16, x *= 0x7feb352dU, x ^= x >> 15, x *= 0x846ca68bU, x ^= x >> 16;
        return float(x & 0xffffff) / float(0xffffff);
    };
    const float turn = float(params.p("turn", 0, 180)) * pi / 180;
    const float leak[3] = {float(params.p("color.r", 0, 1.0)), float(params.p("color.g", 0, 0.55)), float(params.p("color.b", 0, 0.2))};
    const bool brightsFirst = params.p("invert", 0) > 0.5;
    const uint32_t glitchSeed = uint32_t(uf * 40);  // the pattern jumps a few times through the transition
    // Shape Wipe: square cells of the shape (one over the whole frame for a count of 1), and how far
    // out the shape must grow to cover a cell's corners.
    const int shape = int(std::lround(params.p("shape", 0, 0)));
    const int count = std::clamp(int(std::lround(params.p("count", 0, 1))), 1, 64);
    const float cell = count == 1 ? float(std::max(w, h)) : float(w) / count;
    const float rot = float(params.p("angle", 0, 0)) * pi / 180, rc = std::cos(rot), rs = std::sin(rot);
    float reach = 0;
    if (type == "shape_wipe") {
        const float hx = count == 1 ? w / (2 * cell) : 1.0f, hy = count == 1 ? h / (2 * cell) : 1.0f;
        for (int i = 0; i <= 64; ++i)
            for (int side = 0; side < 4; ++side) {
                // The cell's edge, walked all round (a shape's farthest point need not be a corner).
                const float t = float(i) / 64 * 2 - 1;
                const float ex = side < 2 ? (side ? hx : -hx) : t * hx, ey = side < 2 ? t * hy : (side == 3 ? hy : -hy);
                reach = std::max(reach, shapeDistance(shape, ex * rc + ey * rs, -ex * rs + ey * rc));
            }
    }
    // 3D Cube and Flip: the faces' projections for this moment.
    Face faces[2];
    const Image* faceImg[2] = {&a, &b};
    const bool threeD = type == "cube" || type == "flip";
    if (threeD && uf > 0 && uf < 1) {
        const double f = 2.0 * std::max(w, h), W2 = w / 2.0, H2 = h / 2.0;
        const bool vertical = type == "cube" ? dir >= 2 : dir == 1;
        // Turn about the vertical axis (or the horizontal one), then push back a little mid-way.
        auto place = [&](double pts[4][3], double angle, double centreZ) {
            const double c = std::cos(angle), s = std::sin(angle);
            for (int i = 0; i < 4; ++i) {
                double& p = vertical ? pts[i][1] : pts[i][0];
                const double z = pts[i][2];
                const double np = p * c + z * s, nz = -p * s + z * c;
                p = np;
                pts[i][2] = nz + centreZ;
            }
        };
        if (type == "cube") {
            const double D2 = vertical ? H2 : W2;  // half the cube's depth
            const double sign = dir == 1 || dir == 3 ? -1 : 1;
            const double angle = sign * uf * pi / 2, back = D2 + 0.6 * D2 * bell;
            double front[4][3] = {{-W2, -H2, -D2}, {W2, -H2, -D2}, {W2, H2, -D2}, {-W2, H2, -D2}};
            double side[4][3];
            if (!vertical && sign > 0) {  // B on the right face
                const double s[4][3] = {{W2, -H2, -D2}, {W2, -H2, D2}, {W2, H2, D2}, {W2, H2, -D2}};
                std::copy(&s[0][0], &s[0][0] + 12, &side[0][0]);
            } else if (!vertical) {  // on the left
                const double s[4][3] = {{-W2, -H2, D2}, {-W2, -H2, -D2}, {-W2, H2, -D2}, {-W2, H2, D2}};
                std::copy(&s[0][0], &s[0][0] + 12, &side[0][0]);
            } else if (sign > 0) {  // below
                const double s[4][3] = {{-W2, H2, -D2}, {W2, H2, -D2}, {W2, H2, D2}, {-W2, H2, D2}};
                std::copy(&s[0][0], &s[0][0] + 12, &side[0][0]);
            } else {  // above
                const double s[4][3] = {{-W2, -H2, D2}, {W2, -H2, D2}, {W2, -H2, -D2}, {-W2, -H2, -D2}};
                std::copy(&s[0][0], &s[0][0] + 12, &side[0][0]);
            }
            place(front, angle, back);
            place(side, angle, back);
            faces[0] = projectFace(front, f, w, h);
            faces[1] = projectFace(side, f, w, h);
        } else {
            // A card: A on its front for the first half, B on its back for the second.
            const bool second = uf >= 0.5f;
            const double angle = second ? (uf - 1) * pi : uf * pi, back = 0.5 * std::max(W2, H2) * bell;
            double card[4][3] = {{-W2, -H2, 0}, {W2, -H2, 0}, {W2, H2, 0}, {-W2, H2, 0}};
            place(card, angle, back);
            faces[second ? 1 : 0] = projectFace(card, f, w, h);
        }
    }
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
                } else if (type == "whip_pan") {
                    // A pushed out and B pushed in, fast in the middle, smeared along the move.
                    const bool horiz = dir <= 1;
                    const float sign = (dir == 0 || dir == 2) ? -1.f : 1.f;  // which way the pictures travel
                    const float span = horiz ? float(w) : float(h);
                    const float e = uf * uf * (3 - 2 * uf);
                    const float travel = sign * e * span, blur = strength * 0.25f * span * bell;
                    // Enough samples that the smear has no visible steps (one every 1.5 px).
                    const int kTaps = std::clamp(int(blur / 1.5f) + 1, 1, 48);
                    float sa[4] = {0, 0, 0, 0}, sb[4] = {0, 0, 0, 0};
                    for (int k = 0; k < kTaps; ++k) {
                        const float s = (kTaps > 1 ? (float(k) / (kTaps - 1) - 0.5f) * blur : 0.f);
                        float qa[4], qb[4];
                        const float ax = horiz ? x + 0.5f - travel - s : x + 0.5f, ay = horiz ? y + 0.5f : y + 0.5f - travel - s;
                        const float bx = horiz ? ax + sign * span : ax, by = horiz ? ay : ay + sign * span;
                        sampleOr(a, ax, ay, qa);
                        sampleOr(b, bx, by, qb);
                        for (int c = 0; c < 4; ++c) sa[c] += qa[c] / kTaps, sb[c] += qb[c] / kTaps;
                    }
                    for (int c = 0; c < 4; ++c) o[c] = sb[c] + sa[c] * (1 - sb[3]);
                } else if (type == "zoom_blur" || type == "spin") {
                    // A first, then B, each moved (zoomed in, or turned) and smeared along that motion,
                    // most at the cut; a short dissolve hides the cut itself.
                    const float cx = w * 0.5f, cy = h * 0.5f, fxp = x + 0.5f - cx, fyp = y + 0.5f - cy;
                    auto moved = [&](const Image& img, float k, float out[4]) {  // k: 0 at rest, 1 at the cut
                        zero(out);
                        if (img.empty()) return;
                        const float smear = strength * k;
                        // Samples by how far this pixel's smear reaches (one every 1.5 px).
                        const float reach = type == "zoom_blur" ? 0.25f * smear * std::hypot(fxp, fyp)
                                                                : 0.075f * smear * std::fabs(turn) * k * std::hypot(fxp, fyp);
                        const int kTaps = std::clamp(int(reach / 1.5f) + 1, 1, 48);
                        for (int t = 0; t < kTaps; ++t) {
                            const float f = kTaps > 1 ? float(t) / (kTaps - 1) : 0.f;
                            float q[4], sx, sy;
                            if (type == "zoom_blur") {
                                const float sc = 1 + k * (1 + strength) * (1 + 0.25f * smear * f);
                                sx = cx + fxp / sc, sy = cy + fyp / sc;
                            } else {
                                const float ang = k * 0.5f * turn * (1 + 0.15f * smear * f);
                                const float sc = 1 - 0.15f * k;
                                const float cs = std::cos(-ang), sn = std::sin(-ang);
                                sx = cx + (fxp * cs - fyp * sn) / sc, sy = cy + (fxp * sn + fyp * cs) / sc;
                            }
                            sample(img, sx, sy, q);
                            for (int c = 0; c < 4; ++c) out[c] += q[c] / kTaps;
                        }
                    };
                    float qa[4], qb[4];
                    moved(a, std::min(1.f, uf * 2), qa);
                    moved(b, std::min(1.f, (1 - uf) * 2), qb);
                    const float mix = smoothstep(0.45f, 0.55f, uf);
                    for (int c = 0; c < 4; ++c) o[c] = qa[c] * (1 - mix) + qb[c] * mix;
                } else if (type == "glitch") {
                    // Bands of rows thrown sideways, colours split, pieces of B flashing in early and A late.
                    const float amount = strength * (1 - std::fabs(2 * uf - 1));
                    const int band = y / std::max(2, h / 24);
                    const float r1 = hashf(uint32_t(band) * 2654435761U ^ glitchSeed * 40503U);
                    const float r2 = hashf(uint32_t(band) * 97U ^ glitchSeed * 2246822519U);
                    const float shift = (r1 < amount * 0.6f) ? (r2 - 0.5f) * 0.25f * w * amount : 0.f;
                    const bool flip = hashf(uint32_t(band) * 31U ^ glitchSeed * 3266489917U) < amount * 0.35f;
                    const Image& src = ((uf >= 0.5f) != flip) ? b : a;
                    const float split = 0.02f * w * amount;
                    float qr[4], qg[4], qbb[4];
                    sampleOr(src, x + 0.5f - shift + split, y + 0.5f, qr);
                    sampleOr(src, x + 0.5f - shift, y + 0.5f, qg);
                    sampleOr(src, x + 0.5f - shift - split, y + 0.5f, qbb);
                    o[0] = qr[0], o[1] = qg[1], o[2] = qbb[2], o[3] = std::max({qr[3], qg[3], qbb[3]});
                } else if (type == "light_leak") {
                    // A warm light sweeping across while the pictures dissolve beneath it.
                    px(a, x, y, pa);
                    px(b, x, y, pb);
                    const float mix = smoothstep(0.3f, 0.7f, uf);
                    for (int c = 0; c < 4; ++c) o[c] = pa[c] * (1 - mix) + pb[c] * mix;
                    const float nx = (x + 0.5f) / w, ny = (y + 0.5f) / h;
                    const float c1 = -0.3f + 1.6f * uf, c2 = 1.2f - 1.4f * uf;
                    const float l = strength * bell *
                                    (std::exp(-((nx - c1) * (nx - c1) / 0.08f + (ny - 0.35f) * (ny - 0.35f) / 0.3f)) +
                                     0.6f * std::exp(-((nx - c2) * (nx - c2) / 0.05f + (ny - 0.8f) * (ny - 0.8f) / 0.15f)));
                    for (int c = 0; c < 3; ++c) o[c] += l * leak[c];
                    o[3] = std::min(1.f, o[3] + l);
                } else if (type == "luma_wipe") {
                    // B shows through A's darkest parts first (or brightest), spreading to the rest.
                    px(a, x, y, pa);
                    px(b, x, y, pb);
                    float la = lumaOf(pa);
                    if (brightsFirst) la = 1 - la;
                    const float s = std::max(1e-3f, soft), tt = uf * (1 + 2 * s) - s;
                    const float mb = 1 - smoothstep(tt, tt + s, la);
                    for (int c = 0; c < 4; ++c) o[c] = pa[c] * (1 - mb) + pb[c] * mb;
                } else if (type == "shape_wipe") {
                    // B inside the shapes, which grow from nothing until they meet over all of A.
                    float qx, qy;
                    if (count == 1) {
                        qx = (x + 0.5f - w * 0.5f) / cell, qy = (y + 0.5f - h * 0.5f) / cell;
                    } else {
                        const float cx = (std::floor((x + 0.5f) / cell) + 0.5f) * cell, cy = (std::floor((y + 0.5f) / cell) + 0.5f) * cell;
                        qx = (x + 0.5f - cx) / (cell / 2), qy = (y + 0.5f - cy) / (cell / 2);
                    }
                    const float d = shapeDistance(shape, qx * rc + qy * rs, -qx * rs + qy * rc);
                    const float s = std::max(1e-3f, soft) * reach, tt = uf * (reach + s);
                    const float mb = 1 - smoothstep(tt - s, tt, d);
                    px(a, x, y, pa);
                    px(b, x, y, pb);
                    for (int c = 0; c < 4; ++c) o[c] = pa[c] * (1 - mb) + pb[c] * mb;
                } else if (threeD) {
                    if (uf <= 0 || uf >= 1) {
                        px(uf <= 0 ? a : b, x, y, o);
                        continue;
                    }
                    zero(o);
                    for (int k = 0; k < 2; ++k) {
                        const Face& fc = faces[k];
                        if (!fc.facing || faceImg[k]->empty()) continue;
                        const double sx = x + 0.5, sy = y + 0.5;
                        const double tw = fc.inv[6] * sx + fc.inv[7] * sy + fc.inv[8];
                        const double tu = (fc.inv[0] * sx + fc.inv[1] * sy + fc.inv[2]) / tw, tv = (fc.inv[3] * sx + fc.inv[4] * sy + fc.inv[5]) / tw;
                        if (tu < 0 || tu > 1 || tv < 0 || tv > 1) continue;
                        sampleOr(*faceImg[k], float(tu * faceImg[k]->width), float(tv * faceImg[k]->height), o);
                        break;  // the faces of a cube that face the camera never overlap
                    }
                } else if (type == "clock_wipe") {
                    // A hand sweeping round from twelve o'clock.
                    const float dx = x + 0.5f - w * 0.5f, dy = y + 0.5f - h * 0.5f;
                    float ang = std::atan2(dx, -dy) / (2 * pi);
                    if (ang < 0) ang += 1;
                    const float s = std::max(1e-3f, soft), tt = uf * (1 + s);
                    const float mb = 1 - smoothstep(tt - s, tt, ang);
                    px(a, x, y, pa);
                    px(b, x, y, pb);
                    for (int c = 0; c < 4; ++c) o[c] = pa[c] * (1 - mb) + pb[c] * mb;
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
