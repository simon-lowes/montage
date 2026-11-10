#include "Processing.h"
#include <cstring>
#include <set>
#include "SurfaceTrack.h"
#include "ColorChart.h"
#include "Ofx.h"

#include "ColorSpace.h"
#include "ColorSlice.h"
#include "CleanPlate.h"
#include "Crosstalk.h"
#include "Clf.h"
#include "Gamut.h"
#include "TransferTables.h"
#include "Ocio.h"
#include "MeshWarp.h"
#include "VideoFx.h"
#include "core/Cdl.h"
#include "core/Effects.h"
#include "core/ColorWarp.h"
#include "core/MaskPath.h"
#include "media/SuperScale.h"
#include "media/DepthMap.h"
#include "media/Exr.h"
#include "media/Matting.h"
#include "media/Inpaint.h"
#include "media/LearnedEffect.h"
#include "render/Compositor.h"
#include "FaceRefine.h"
#include "FaceShape.h"
#include "media/FaceMesh.h"
#include "Relight.h"
#include "FilmLook.h"
#include "PaintFx.h"
#include "StyleFx.h"
#include "Deconvolve.h"
#include "GpuCompositor.h"
#include "QualityCheck.h"
#include "Restoration.h"
#include "FilmScan.h"
#include "GrainMatch.h"
#include "LensBlur.h"
#include "media/MotionData.h"
#include "media/Tracking.h"
#include "media/FaceTracks.h"

#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <array>
#include <atomic>
#include <mutex>
#include <sstream>

namespace montage {

namespace {

constexpr float kLumaR = 0.2126f, kLumaG = 0.7152f, kLumaB = 0.0722f;

inline float clamp01(float v) { return !(v > 0) ? 0 : (v > 1 ? 1 : v); }  // (NaN to 0: the result indexes tables)
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

// Color Correct's settings at a frame, as the pixels use them (here and on the GPU, render/GpuCompositor.h).
struct ColorCorrectSetup {
    float exposure, contrast, pivot, sat, offset, vibrance;
    float lift[3], gam[3], gain[3], wb[3];
    float printer[3] = {1, 1, 1};  // printer lights as multipliers of each channel's light
    bool printing = false;
    bool tone;
    static constexpr int kTone = 1024;
    std::array<float, kTone + 1> toneLut{};  // luma after the tone controls, as a table over 0..2, kept rising
};

ColorCorrectSetup colorCorrectSetup(const Effect& e, FrameTime t) {
    ColorCorrectSetup s;
    s.exposure = std::pow(2.0f, float(e.p("exposure", t)));
    s.contrast = float(e.p("contrast", t, 1));
    s.pivot = float(e.p("pivot", t, 0.435));
    s.sat = float(e.p("saturation", t, 1));
    const float temp = float(e.p("temperature", t)) / 100.0f;
    const float tint = float(e.p("tint", t)) / 100.0f;
    s.offset = float(e.p("offset", t));
    const float liftM = float(e.p("lift", t)), gammaM = float(e.p("gamma", t)), gainM = float(e.p("gain", t, 1));
    const char* channel[3] = {"_r", "_g", "_b"};
    for (int i = 0; i < 3; ++i) {
        s.lift[i] = liftM + float(e.p(std::string("lift") + channel[i], t));
        s.gam[i] = std::pow(2.0f, gammaM + float(e.p(std::string("gamma") + channel[i], t)));
        s.gain[i] = gainM * float(e.p(std::string("gain") + channel[i], t, 1));
        const double points = e.p(std::string("printer") + channel[i], t);
        s.printer[i] = float(std::pow(10.0, points * e.p("printer_point", t, 0.025)));
        s.printing = s.printing || points != 0;
    }
    // White balance as channel multipliers (normalised to keep luma roughly constant).
    s.wb[0] = 1 + 0.3f * temp + 0.1f * tint, s.wb[1] = 1 - 0.25f * tint, s.wb[2] = 1 - 0.3f * temp + 0.1f * tint;
    const float wl = luma(s.wb[0], s.wb[1], s.wb[2]);
    for (float& w : s.wb) w /= wl;
    const float highlights = float(e.p("highlights", t)) / 100, shadows = float(e.p("shadows", t)) / 100;
    const float whites = float(e.p("whites", t)) / 100, blacks = float(e.p("blacks", t)) / 100;
    s.vibrance = float(e.p("vibrance", t)) / 100;
    s.tone = highlights != 0 || shadows != 0 || whites != 0 || blacks != 0;
    // As a table over 0..2, kept rising, so no setting can turn tones over.
    if (s.tone)
        for (int i = 0; i <= s.kTone; ++i) {
            s.toneLut[size_t(i)] = toneShift(2.0f * float(i) / s.kTone, highlights, shadows, whites, blacks);
            if (i) s.toneLut[size_t(i)] = std::max(s.toneLut[size_t(i)], s.toneLut[size_t(i - 1)]);
        }
    return s;
}

void colorCorrect(const Effect& e, FrameTime t, Image& img) {
    const ColorCorrectSetup cs = colorCorrectSetup(e, t);
    const float exposure = cs.exposure, contrast = cs.contrast, pivot = cs.pivot, sat = cs.sat, offset = cs.offset;
    const float* lift = cs.lift;
    const float* gam = cs.gam;
    const float* gain = cs.gain;
    const float* wb = cs.wb;
    const float vibrance = cs.vibrance;
    const bool tone = cs.tone;
    constexpr int kTone = ColorCorrectSetup::kTone;
    const auto& toneLut = cs.toneLut;
    auto toneAt = [&](float l) {
        if (l <= 0) return l + toneLut[0];
        if (l >= 2) return l - 2 + toneLut[kTone];
        const float x = l * kTone / 2;
        const int i = std::min(kTone - 1, int(x));
        return toneLut[size_t(i)] + (toneLut[size_t(i + 1)] - toneLut[size_t(i)]) * (x - float(i));
    };
    const Transfer transfer = currentWorkingSpace().transfer;
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        float c[3] = {r, g, b};
        // (Printer lights first, on the light itself: each a step of log exposure.)
        if (cs.printing)
            for (int i = 0; i < 3; ++i)
                if (cs.printer[i] != 1 && c[i] > 0) c[i] = float(fromLinear(transfer, toLinear(transfer, c[i]) * cs.printer[i]));
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
        // Past either end (a floating-point picture's highlights), the curve carries on at its end's slope (flat ends
        // stay flat, even for an infinite value); a NaN becomes 0, as the clamped path made it.
        if (std::isnan(v)) return 0.0f;
        if (v > 1) {
            const float slope = (lut[size_t(n)] - lut[size_t(n) - 1]) * float(n);
            return slope == 0 ? lut[size_t(n)] : lut[size_t(n)] + (v - 1) * slope;
        }
        if (v < 0) {
            const float slope = (lut[1] - lut[0]) * float(n);
            return slope == 0 ? lut[0] : lut[0] + v * slope;
        }
        float x = v * float(n);
        int i = std::min(n - 1, int(x));
        float f = x - float(i);
        return lut[size_t(i)] * (1 - f) + lut[size_t(i) + 1] * f;
    };
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        float nr = look(cr, look(m, r)), ng = look(cg, look(m, g)), nb = look(cb, look(m, b));
        if (mix >= 1) {
            r = nr, g = ng, b = nb;  // (so a NaN or infinite pixel takes the curve's value, not NaN from the mix)
            return;
        }
        r += (nr - r) * mix;
        g += (ng - g) * mix;
        b += (nb - b) * mix;
    });
}

void hueCurves(const Effect& e, FrameTime t, Image& img);  // below, beside the curve building

void rgbMixer(const Effect& e, FrameTime t, Image& img) {
    float m[9];
    bool identity = true;
    for (int o = 0; o < 3; ++o)
        for (int i = 0; i < 3; ++i) {
            const std::string name = std::string(1, "rgb"[o]) + "." + "rgb"[i];
            m[o * 3 + i] = float(e.p(name, t, o == i ? 1 : 0));
            identity = identity && m[o * 3 + i] == (o == i ? 1.0f : 0.0f);
        }
    if (identity) return;
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        const float nr = m[0] * r + m[1] * g + m[2] * b, ng = m[3] * r + m[4] * g + m[5] * b, nb = m[6] * r + m[7] * g + m[8] * b;
        r = nr, g = ng, b = nb;
    });
}
void colorWarper(const Effect& e, FrameTime t, Image& img);  // below, as hue curves

// HDR Palette: each pixel's brightness in stops from mid grey (0.18 of reference white, in the light the working space
// shows: SDR white and HDR's 203 nits are both 1.0), moved by the global exposure and each zone's exposure as far as the
// pixel is in it (the curve kept rising), then the contrast round its pivot; the channels shifted by the zones' colour
// balance (stops per channel), the saturation scaled, the black offset added, and the result coded back.
// Its settings at a time, as the pixels need them (gpufilter.frag takes the same): per stop of brightness, from -16 to
// +12 in 1/32 steps, the brightness out, the saturation and the balance.
struct HdrPaletteSetup {
    static constexpr float kEvMin = -16, kEvMax = 12;
    static constexpr int kSteps = int((kEvMax - kEvMin) * 32);
    float kr = 0.2126f, kg = 0.7152f, kb = 0.0722f, toWhite = 1, blackOffset = 0, mix = 1;
    std::vector<float> evOut, sat, bal[3];
};
HdrPaletteSetup hdrPaletteSetup(const Effect& e, FrameTime t, const ColorSpace& space) {
    HdrPaletteSetup h;
    if (space.primaries == Primaries::Bt2020) h.kr = 0.2627f, h.kg = 0.6780f, h.kb = 0.0593f;
    else if (space.primaries == Primaries::P3D65) h.kr = 0.2290f, h.kg = 0.6917f, h.kb = 0.0793f;
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
    h.blackOffset = float(e.p("black_offset", t)), h.mix = float(e.p("mix", t, 100)) / 100;
    constexpr int kSteps = HdrPaletteSetup::kSteps;
    constexpr float kEvMin = HdrPaletteSetup::kEvMin, kEvMax = HdrPaletteSetup::kEvMax;
    std::vector<float>&evOut = h.evOut, &sat = h.sat, (&bal)[3] = h.bal;
    evOut.resize(kSteps + 1), sat.resize(kSteps + 1);
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
    // HLG decodes to scene light with its peak at 1; its reference white (75%) is brought to 1 like SDR white and PQ's
    // 203 nits.
    h.toWhite = space.transfer == Transfer::Hlg ? float(1 / toLinear(Transfer::Hlg, 0.75)) : 1.0f;
    return h;
}

void hdrPalette(const Effect& e, FrameTime t, Image& img) {
    const ColorSpace& space = currentWorkingSpace();
    const TransferTables tables(space.transfer);
    const HdrPaletteSetup h = hdrPaletteSetup(e, t, space);
    constexpr int kSteps = HdrPaletteSetup::kSteps;
    constexpr float kEvMin = HdrPaletteSetup::kEvMin, kEvMax = HdrPaletteSetup::kEvMax;
    const float kr = h.kr, kg = h.kg, kb = h.kb, toWhite = h.toWhite, blackOffset = h.blackOffset, mix = h.mix;
    auto lookup = [&](const std::vector<float>& table, float x) {
        const int i = std::min(kSteps - 1, int(x));
        return table[size_t(i)] + (table[size_t(i + 1)] - table[size_t(i)]) * (x - float(i));
    };
    constexpr float kGrey = 0.18f;
    const std::vector<float>&evOut = h.evOut, &sat = h.sat, (&bal)[3] = h.bal;
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

// The colour space an effect's "Apply In" names (null: the working space).
const ColorSpace* applyInSpace(const Effect& e) {
    const ColorSpace& working = currentWorkingSpace();
    for (const auto& cs : colorSpaces())
        if (cs.label == e.s("space")) return cs.id == working.id ? nullptr : &cs;
    return nullptr;
}

// `fn` run in `space`: the working space's code values into it and back exactly (linear light through its primaries and
// curve, no display rendering), so only `fn` changes the picture; the round trip baked into a 65^3 LUT, kept for `key`.
// A colour from one space's code values to another's exactly: linear light through the first's curve, its primaries
// into the second's, out through the second's curve (no display rendering).
void through(const double mat[9], Transfer a, Transfer b, float v[3]) {
    double l[3], o[3];
    for (int i = 0; i < 3; ++i) l[i] = toLinear(a, v[i]);
    for (int i = 0; i < 3; ++i) o[i] = mat[i * 3] * l[0] + mat[i * 3 + 1] * l[1] + mat[i * 3 + 2] * l[2];
    for (int i = 0; i < 3; ++i) v[i] = float(fromLinear(b, o[i]));
}

std::shared_ptr<const Lut3D> bakedInSpace(const ColorSpace& space, const std::string& key, const std::function<void(float[3])>& fn) {
    const ColorSpace& working = currentWorkingSpace();
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<const Lut3D>> cache;
    const std::string full = working.id + "|" + space.id + "|" + key;
    {
        std::lock_guard lock(m);
        if (auto it = cache.find(full); it != cache.end()) return it->second;
    }
    double to[9], from[9];
    primariesMatrix(working.primaries, space.primaries, to);
    primariesMatrix(space.primaries, working.primaries, from);
    auto made = std::make_shared<Lut3D>();
    const int n = 65;
    made->size = n;
    made->data.resize(size_t(n) * n * n * 3);
    parallelRows(n, [&](int b0, int b1) {
        for (int bi = b0; bi < b1; ++bi)
            for (int gi = 0; gi < n; ++gi)
                for (int ri = 0; ri < n; ++ri) {
                    float v[3] = {float(ri) / (n - 1), float(gi) / (n - 1), float(bi) / (n - 1)};
                    through(to, working.transfer, space.transfer, v);
                    fn(v);
                    through(from, space.transfer, working.transfer, v);
                    float* d = &made->data[(size_t(bi) * n * n + size_t(gi) * n + size_t(ri)) * 3];
                    d[0] = v[0], d[1] = v[1], d[2] = v[2];
                }
    });
    std::lock_guard lock(m);
    if (cache.size() > 64) cache.clear();  // (a keyframed CDL makes a new one each frame)
    cache[full] = made;
    return made;
}

// A .cube or CLF file's transform on one colour; null (with the reason) when it cannot be read.
std::function<void(float[3])> lutFileTransform(const std::string& path, std::string* error) {
    const QString p = QString::fromStdString(path);
    if (p.endsWith(QLatin1String(".clf"), Qt::CaseInsensitive) || p.endsWith(QLatin1String(".ctf"), Qt::CaseInsensitive)) {
        auto clf = loadClf(path, error);
        if (!clf) return nullptr;
        return [clf](float v[3]) { clf->apply(v); };
    }
    auto lut = loadCubeLut(path, error);
    if (!lut) return nullptr;
    return [lut](float v[3]) { lut->apply(v[0], v[1], v[2]); };
}

void applyLut(const Effect& e, FrameTime t, Image& img) {
    std::string path = e.s("path");
    if (path.empty()) return;
    auto fn = lutFileTransform(path, nullptr);
    if (!fn) return;
    float strength = float(e.p("strength", t, 100)) / 100.0f;
    std::shared_ptr<const Lut3D> baked;
    if (const ColorSpace* space = applyInSpace(e)) {
        const QFileInfo fi(QString::fromStdString(path));
        baked = bakedInSpace(*space, "lut|" + path + "|" + std::to_string(fi.size()) + "|" +
                                         std::to_string(fi.lastModified().toMSecsSinceEpoch()), fn);
    }
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        float v[3] = {r, g, b};
        if (baked) {
            for (float& c : v) c = std::clamp(c, 0.0f, 1.0f);
            baked->apply(v[0], v[1], v[2]);
        } else {
            fn(v);
        }
        r += (v[0] - r) * strength;
        g += (v[1] - g) * strength;
        b += (v[2] - b) * strength;
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
    const ColorSpace* space = applyInSpace(e);
    if (!space) {
        perPixel(img, [&](float& r, float& g, float& b, float&) { applyCdl(cdl, r, g, b); });
        return;
    }
    auto lut = bakedInSpace(*space, "cdl|" + cdlSopText(cdl) + cdlNumber(cdl.saturation),
                            [&](float v[3]) { applyCdl(cdl, v[0], v[1], v[2]); });
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

// Three box blurs of radius r: close to a Gaussian (or `passes` of them; one is the box's own mean).
void boxBlurPlane(std::vector<float>& v, int w, int h, int r, int passes = 3) {
    if (r <= 0) return;
    std::vector<float> tmp(v.size());
    for (int pass = 0; pass < passes; ++pass) {
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
    // With a clean plate, the screen's colour where each pixel is (render/CleanPlate.h): an uneven screen keyed as if
    // it were even.
    const Image plate = keyerPlate(e, t, img, pixelScale);
    const float plateScreen = std::max(1e-3f, float(std::clamp(e.p("plate_threshold", t, 50), 1.0, 100.0) / 100) * screen);
    auto plateColour = [&](int x, int y, float c[3]) {
        const float* q = plate.at(x, y);
        const float a = q[3] > 0 ? q[3] : 1;
        for (int k = 0; k < 3; ++k) c[k] = q[k] / a;
    };
    if (!plate.empty() && (view == 3 || view == 4)) {
        parallelRows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float* p = img.row(y);
                for (int x = 0; x < w; ++x, p += 4) {
                    float c[3];
                    plateColour(x, y, c);
                    if (view == 3) {  // the plate
                        p[0] = c[0], p[1] = c[1], p[2] = c[2], p[3] = 1;
                        continue;
                    }
                    // the screen evened out: each channel scaled by the screen colour over the plate's (alpha kept)
                    // (at most 4 times, so a plate channel near 0 does not blow up)
                    for (int k = 0; k < 3; ++k) p[k] *= sk[k] / std::max(c[k], 0.25f * sk[k] + 1e-4f);
                }
            }
        });
        return;
    }
    std::vector<float> matte(size_t(w) * size_t(h));
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* p = img.row(y);
            for (int x = 0; x < w; ++x, p += 4) {
                const float a = p[3];
                float m = 0;
                if (a > 0) {
                    const float c[3] = {p[0] / a, p[1] / a, p[2] / a};
                    float here = screen;
                    if (!plate.empty()) {  // (where the plate is not screen, a set's floor say, the screen colour)
                        float q[3];
                        plateColour(x, y, q);
                        const float lead = q[dom] - others(q);
                        if (lead >= plateScreen) here = lead;
                    }
                    const float d = (c[dom] - others(c)) / here;  // 1 on the screen, 0 or less on the subject
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

namespace {
// The radii of gaussianBlur's box passes (each direction), none below half a pixel.
std::vector<int> gaussianBoxRadii(double radius) {
    std::vector<int> radii;
    if (radius < 0.5) return radii;
    // Three box passes approximate a Gaussian with sigma ~ radius / 2.
    double sigma = radius / 2.0;
    int n = 3;
    double wIdeal = std::sqrt(12.0 * sigma * sigma / n + 1.0);
    int wl = int(std::floor(wIdeal));
    if (wl % 2 == 0) --wl;
    int wu = wl + 2;
    double mIdeal = (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0);
    int m = int(std::lround(mIdeal));
    for (int i = 0; i < n; ++i)
        if (const int r = ((i < m ? wl : wu) - 1) / 2; r > 0) radii.push_back(r);
    return radii;
}
}  // namespace

void gaussianBlur(Image& img, double radius, bool horizontal, bool vertical) {
    if (radius < 0.5 || img.empty()) return;
    const std::vector<int> radii = gaussianBoxRadii(radius);
    Image tmp(img.width, img.height);
    auto passes = [&](Image& a) {
        Image t2(a.width, a.height);
        for (int r : radii) {
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
SourceSecondsScope::SourceSecondsScope(double seconds) : previous_(tSourceSeconds) { tSourceSeconds = seconds; }
SourceSecondsScope::~SourceSecondsScope() { tSourceSeconds = previous_; }
const ValueMap* currentPersonMatte() { return tPerson.get(); }
PersonScope::PersonScope(std::shared_ptr<const ValueMap> matte) : previous_(std::move(tPerson)) { tPerson = std::move(matte); }
PersonScope::~PersonScope() { tPerson = std::move(previous_); }
thread_local SlowEffects tSlow;
const SlowEffects& currentSlowEffects() { return tSlow; }
SlowEffectsScope::SlowEffectsScope(SlowEffects slow) : previous_(tSlow) { tSlow = slow; }
SlowEffectsScope::~SlowEffectsScope() { tSlow = previous_; }
thread_local SurfaceContext tSurface;
const SurfaceContext* currentSurface() { return tSurface.track ? &tSurface : nullptr; }
SurfaceScope::SurfaceScope(SurfaceContext context) : previous_(std::move(tSurface)) { tSurface = std::move(context); }
SurfaceScope::~SurfaceScope() { tSurface = std::move(previous_); }
thread_local std::string tCryptoFile;
const std::string& currentCryptomatteFile() { return tCryptoFile; }
CryptomatteScope::CryptomatteScope(std::string file) : previous_(std::move(tCryptoFile)) { tCryptoFile = std::move(file); }
CryptomatteScope::~CryptomatteScope() { tCryptoFile = std::move(previous_); }

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

void consolidateMatte(std::vector<float>& matte, int W, int H, double px, double feather) {
    if (matte.size() != size_t(W) * size_t(H) || W <= 0 || H <= 0 || px < 0.5) return;
    // A closing: the shape grown by `px` and shrunk back, so holes and gaps narrower than twice that fill in. It works on
    // a copy with a ring of background round it, so beyond the frame counts as outside and no object is joined to the
    // frame's edge.
    const int pad = int(std::ceil(px)) + 1, PW = W + 2 * pad, PH = H + 2 * pad;
    const size_t pn = size_t(PW) * size_t(PH);
    auto at = [&](int x, int y) { return size_t(y + pad) * size_t(PW) + size_t(x + pad); };
    std::vector<uint8_t> inside(pn, 0), grownOut(pn), added(pn, 0);
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) inside[at(x, y)] = matte[size_t(y) * size_t(W) + size_t(x)] > 0.5f;
    });
    {
        const std::vector<float> toInside = distanceTransform(inside, PW, PH);
        for (size_t i = 0; i < pn; ++i) grownOut[i] = toInside[i] > px;
    }
    const std::vector<float> toOutside = distanceTransform(grownOut, PW, PH);
    // The pixels it adds; none (the usual case) leaves the mask as it was.
    std::atomic<bool> any{false};
    parallelRows(H, [&](int y0, int y1) {
        bool found = false;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = at(x, y);
                added[i] = !inside[i] && toOutside[i] > px;
                found = found || added[i];
            }
        if (found) any = true;
    });
    if (!any) return;
    // Near what it adds, the mask takes the closed shape's, feathered as the shape is (its signed distance, negative
    // inside, through the same smoothstep), and keeps whichever is more; elsewhere the shape's own edge stays as it was.
    // (Distances run between pixel centres: the closed shape's edge lies half a pixel out from its outermost pixels.)
    const std::vector<float> toAdded = distanceTransform(added, PW, PH);
    // Outside the closed shape, its distance comes from a transform of its own (the grown-out set's distance stops at
    // the edge of the growth, which would leave a haze of the feather all round).
    std::vector<uint8_t> closedSet(pn);
    for (size_t i = 0; i < pn; ++i) closedSet[i] = inside[i] || toOutside[i] > px;
    const std::vector<float> toClosed = distanceTransform(closedSet, PW, PH);
    const double fe = std::max(1.0, feather), reach = fe + 1;
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = at(x, y);
                if (toAdded[i] > 2 * reach) continue;
                const double d = closedSet[i] ? px + 0.5 - toOutside[i] : toClosed[i] - 0.5;
                const double k = std::clamp(0.5 - d / fe, 0.0, 1.0);
                const float closed = float(k * k * (3 - 2 * k));
                const float w = float(std::clamp(2 - toAdded[i] / reach, 0.0, 1.0));  // (fading out further away)
                float& m = matte[size_t(y) * size_t(W) + size_t(x)];
                m += std::max(0.f, closed - m) * w;
            }
    });
}

void refineMatteEdges(std::vector<float>& matte, const Image& guide, double radius, double amount) {
    const int W = guide.width, H = guide.height;
    const size_t n = size_t(W) * size_t(H);
    if (matte.size() != n || W <= 0 || H <= 0 || !(amount > 0)) return;
    // The mask is sure (inside or outside) more than `radius` from its edge, and unknown in the band within it and along
    // a soft edge (a wide feather) that runs on from the band, so no hard step is left where the band ends. (A soft
    // edge is where the mask is partway and still changing: a flat partway interior stays sure, and a stray partway
    // pixel away from the edge is not part of it.) Each unknown pixel takes the place of its colour between the sure
    // object's colours and the sure background's near it (a colour-line matte, as matting tools estimate alpha), so the
    // edge lands on the picture's own and fine detail (hair, fur, leaves) comes through partly. Transparent pixels have
    // no colour and are left alone.
    const double r = std::max(1.0, radius);
    std::vector<uint8_t> in(n), out(n);
    for (size_t i = 0; i < n; ++i) {
        in[i] = matte[i] >= 0.5f;
        out[i] = !in[i];
    }
    const std::vector<float> toOut = distanceTransform(out, W, H), toIn = distanceTransform(in, W, H);
    auto opaque = [&](size_t i) { return guide.px[i * 4 + 3] > 1e-3f; };
    auto colour = [&](size_t i, int k) { return guide.px[i * 4 + k] / guide.px[i * 4 + 3]; };  // (unpremultiplied, HDR kept)
    auto ramp = [&](int x, int y) {
        const size_t i = size_t(y) * size_t(W) + size_t(x);
        if (!(matte[i] > 0.02f && matte[i] < 0.98f)) return false;
        const float gx = matte[i - size_t(x) + size_t(std::min(x + 1, W - 1))] - matte[i - size_t(x) + size_t(std::max(x - 1, 0))];
        const float gy = matte[size_t(std::min(y + 1, H - 1)) * size_t(W) + size_t(x)] - matte[size_t(std::max(y - 1, 0)) * size_t(W) + size_t(x)];
        return 0.25f * (gx * gx + gy * gy) > 1e-6f;  // (changing by more than 0.001 a pixel)
    };
    std::vector<uint8_t> unknown(n, 0);
    std::vector<int> queue;
    for (size_t i = 0; i < n; ++i)
        if ((in[i] && toOut[i] <= r) || (out[i] && toIn[i] <= r)) {
            unknown[i] = 1;
            queue.push_back(int(i));
        }
    if (queue.empty()) return;
    double soft = 0;  // how far the soft edge reaches past the band, so the sure colours are looked for beyond it
    for (size_t q = 0; q < queue.size(); ++q) {
        const int x = queue[q] % W, y = queue[q] / W;
        const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
        for (int k = 0; k < 4; ++k) {
            if (nx[k] < 0 || ny[k] < 0 || nx[k] >= W || ny[k] >= H) continue;
            const size_t j = size_t(ny[k]) * size_t(W) + size_t(nx[k]);
            if (unknown[j] || !ramp(nx[k], ny[k])) continue;
            unknown[j] = 1;
            soft = std::max(soft, double(in[j] ? toOut[j] : toIn[j]) - r);
            queue.push_back(int(j));
        }
    }
    queue = {};
    auto sureIn = [&](size_t i) { return in[i] && !unknown[i]; };
    auto sureOut = [&](size_t i) { return out[i] && !unknown[i]; };
    // The sure colours' local means, on a smaller copy (they change slowly), over twice the band and the soft edge.
    const int sub = std::max(2, int(r / 3));
    const int w = (W + sub - 1) / sub, h = (H + sub - 1) / sub;
    const size_t m = size_t(w) * size_t(h);
    std::vector<float> fg[4], bg[4];  // colour sums and counts
    for (int k = 0; k < 4; ++k) fg[k].assign(m, 0.f), bg[k].assign(m, 0.f);
    std::atomic<bool> band{false};
    parallelRows(h, [&](int g0, int g1) {  // (by rows of the smaller copy, so no two threads add to one cell)
        bool found = false;
        for (int y = g0 * sub; y < std::min(H, g1 * sub); ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * size_t(W) + size_t(x), j = size_t(y / sub) * size_t(w) + size_t(x / sub);
                const bool si = sureIn(i), so = !si && sureOut(i);
                found = found || (!si && !so);
                std::vector<float>* into = si ? fg : so ? bg : nullptr;
                if (!into || !opaque(i)) continue;
                for (int k = 0; k < 3; ++k) into[k][j] += colour(i, k);
                into[3][j] += 1;
            }
        if (found) band = true;
    });
    if (!band) return;
    const int reach = std::max(1, int(std::ceil((2 * r + soft) / sub)));
    for (int k = 0; k < 4; ++k) {
        boxBlurPlane(fg[k], w, h, reach, 1);
        boxBlurPlane(bg[k], w, h, reach, 1);
    }
    const float a = float(std::min(1.0, amount));
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float gy = std::clamp((y + 0.5f) / float(sub) - 0.5f, 0.f, float(h - 1));
            const int ya = int(gy), yb = std::min(ya + 1, h - 1);
            const float fy = gy - float(ya);
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * size_t(W) + size_t(x);
                if (sureIn(i) || sureOut(i) || !opaque(i)) continue;
                const float gx = std::clamp((x + 0.5f) / float(sub) - 0.5f, 0.f, float(w - 1));
                const int xa = int(gx), xb = std::min(xa + 1, w - 1);
                const float fx = gx - float(xa);
                auto at = [&](const std::vector<float>& v) {
                    const float top = v[size_t(ya) * size_t(w) + size_t(xa)] + (v[size_t(ya) * size_t(w) + size_t(xb)] - v[size_t(ya) * size_t(w) + size_t(xa)]) * fx;
                    const float bot = v[size_t(yb) * size_t(w) + size_t(xa)] + (v[size_t(yb) * size_t(w) + size_t(xb)] - v[size_t(yb) * size_t(w) + size_t(xa)]) * fx;
                    return top + (bot - top) * fy;
                };
                const float nf = at(fg[3]), nb = at(bg[3]);
                if (nf < 1e-4f || nb < 1e-4f) continue;  // (no sure object or background near enough to compare with)
                float d[3], v[3], dd = 0, proj = 0;
                for (int k = 0; k < 3; ++k) {
                    const float F = at(fg[k]) / nf, B = at(bg[k]) / nb;
                    d[k] = F - B;
                    v[k] = colour(i, k) - B;
                    dd += d[k] * d[k];
                    proj += v[k] * d[k];
                }
                if (dd < 1e-6f) continue;
                // Where object and background are alike the colours say little: the mask moves less, not abruptly.
                const float trust = dd / (dd + 2e-3f);
                const float alpha = std::clamp(proj / dd, 0.f, 1.f);
                matte[i] += (alpha - matte[i]) * a * trust;
            }
        }
    });
}

std::vector<std::array<double, 2>> maskOutline(const Effect& e, FrameTime t, int W, int H) {
    std::vector<std::array<double, 2>> out;
    const int shape = int(std::lround(e.p("mask.shape", t)));
    if (shape == 5) {
        const std::vector<PathPoint> pts = maskPath(e, t);
        if (pts.size() < 3) return out;
        const MaskBox box = maskBox(e, t);
        for (auto [x, y] : flattenMaskPath(pts, true, 24)) {
            double u = 0, v = 0;
            boxToFrame(box, W, H, x, y, u, v);
            out.push_back({u * W, v * H});
        }
    } else if (shape == 1 || shape == 2) {
        const double cx = e.p("mask.x", t, 0.5) * W, cy = e.p("mask.y", t, 0.5) * H;
        const double a = std::max(0.5, e.p("mask.w", t, 0.4) * W / 2), b = std::max(0.5, e.p("mask.h", t, 0.4) * H / 2);
        const double rot = e.p("mask.rotation", t) * M_PI / 180.0, cr = std::cos(rot), sr = std::sin(rot);
        auto put = [&](double u, double v) { out.push_back({cx + u * cr - v * sr, cy + u * sr + v * cr}); };
        if (shape == 1)
            for (int k = 0; k < 72; ++k) put(a * std::cos(2 * M_PI * k / 72), b * std::sin(2 * M_PI * k / 72));
        else
            put(-a, -b), put(a, -b), put(a, b), put(-a, b);
    }
    return out;
}

namespace {
// A drawn mask bent with the clip's surface track: drawn as on the track's reference frame, carried to this one (and
// a little past the mesh's edge, for its feather; none further off, where the surface does not say where it goes).
void bendWithSurface(std::vector<float>& matte, int W, int H, double sourceSeconds) {
    const SurfaceContext* sc = currentSurface();
    if (sourceSeconds < 0) sourceSeconds = tSourceSeconds;
    if (!sc || sourceSeconds < 0) return;
    const SurfaceTrack& tr = *sc->track;
    const int frame = tr.frameAt(sourceSeconds);
    if (frame < 0 || tr.width <= 0 || tr.height <= 0) return;
    const double sx = double(W) / tr.width, sy = double(H) / tr.height;
    std::vector<MeshPoint> on = tr.frames[size_t(frame)], rest = tr.mesh.rest;
    for (MeshPoint& p : on) p[0] *= sx, p[1] *= sy;
    for (MeshPoint& p : rest) p[0] *= sx, p[1] *= sy;
    const std::vector<float> map = meshMapReaching(tr.mesh, on, rest, W, H, 32 * std::max(sx, sy));
    const std::vector<float> drawn = matte;
    auto at = [&](double x, double y) {
        x = std::clamp(x - 0.5, 0.0, double(W - 1)), y = std::clamp(y - 0.5, 0.0, double(H - 1));
        const int x0 = std::min(int(x), W - 1), y0 = std::min(int(y), H - 1), x1 = std::min(x0 + 1, W - 1), y1 = std::min(y0 + 1, H - 1);
        const double fx = x - x0, fy = y - y0;
        auto v = [&](int xx, int yy) { return double(drawn[size_t(yy) * size_t(W) + size_t(xx)]); };
        return (v(x0, y0) * (1 - fx) + v(x1, y0) * fx) * (1 - fy) + (v(x0, y1) * (1 - fx) + v(x1, y1) * fx) * fy;
    };
    for (size_t i = 0; i < matte.size(); ++i) matte[i] = std::isnan(map[i * 2]) ? 0.f : float(at(map[i * 2], map[i * 2 + 1]));
}

// An effect mask's drawn shape (an ellipse, a rectangle, a path or a gradient) into `matte`, a picture's size.
void drawMaskShape(const Effect& e, FrameTime t, int shape, std::vector<float>& matte, int W, int H, double pixelScale) {
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
}
}  // namespace

std::vector<float> effectMatte(const Effect& e, FrameTime t, const Image& img, double pixelScale, double sourceSeconds) {
    std::vector<float> matte;
    if (img.empty() || !hasMask(e, t)) return matte;
    const int W = img.width, H = img.height;
    matte.assign(size_t(W) * size_t(H), 1.0f);
    const int shape = int(std::lround(e.p("mask.shape", t)));
    if (shape == 3) {
        // The objects picked by the model (those added, less those subtracted): nothing where none was segmented.
        std::vector<float> logits;
        if (e.object && sourceSeconds >= 0 && e.object->maskLogitsAt(sourceSeconds, logits))
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
    if (shape == 7) {
        // The objects picked from the render's Cryptomatte: their coverage, as exact as the render's own edges (motion
        // blur, depth of field and transparency included), grown or shrunk by Expansion; nothing where the frame has
        // no Cryptomatte.
        std::fill(matte.begin(), matte.end(), 0.f);
        const std::string& file = currentCryptomatteFile();
        const std::vector<float> ids = parseCryptomatteIds(e.s("mask.crypto_ids"));
        if (!file.empty() && !ids.empty())
            if (const auto cm = cachedCryptomatte(file, e.s("mask.crypto_layer")); cm && cm->width > 0) {
                ValueMap full;
                full.width = cm->width, full.height = cm->height, full.values = cm->matte(ids);
                matte = W == full.width && H == full.height ? std::move(full.values) : full.resized(W, H);
                if (const double grow = e.p("mask.expansion", t) * pixelScale; grow != 0) refineMatte(matte, W, H, grow, 0, 0);
            }
    }
    drawMaskShape(e, t, shape, matte, W, H, pixelScale);
    if (shape > 0 && shape != 6) {
        // The shape's holes and gaps closed (Consolidate), then its edge settled on the picture's own (Refine Edges,
        // as Photoshop's Refine Edge and Resolve's Smart Refine). A gradient has neither holes nor an object's edge.
        consolidateMatte(matte, W, H, e.p("mask.consolidate", t) * pixelScale, e.p("mask.feather", t, 20) * pixelScale);
        // (A shape drawn on the surface track's reference frame bent as the surface is on this one, before its edge
        // is fitted to this frame's picture.)
        if ((shape == 1 || shape == 2 || shape == 5) && e.p("mask.surface", t) > 0.5) bendWithSurface(matte, W, H, sourceSeconds);
        const double refine = std::clamp(e.p("mask.refine", t) / 100.0, 0.0, 1.0);
        if (refine > 0) refineMatteEdges(matte, img, std::max(1.0, e.p("mask.refine_radius", t, 8) * pixelScale), refine);
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
    if (cache.size() > 32) cache.clear();  // (then `slot` is gone: stored afresh)
    cache[key] = plan;
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

// Camera Gyro: the camera's orientation from its motion data (strings["gyro"]), smoothed and levelled; each frame
// turned from where the camera pointed to the steady view, each row by where it pointed as that row was read.
struct GyroPlan {
    GyroPath path;
    std::vector<GyroCorrection> corrections;
    double zoom = 1, readout = 0;
};

std::shared_ptr<const GyroPlan> gyroPlan(const Effect& e, FrameTime t, double aspect) {
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<const GyroPlan>> cache;
    const auto found = e.strings.find("gyro");  // (not copied: it can run to megabytes)
    if (found == e.strings.end() || found->second.empty()) return nullptr;
    const std::string& data = found->second;
    GyroStabilizeSettings gs;
    gs.smoothSeconds = e.p("smoothness", t, 1.5);
    gs.horizon = std::clamp(e.p("horizon", t, 0) / 100, 0.0, 1.0);
    gs.offset = e.p("gyro_offset", t, 0) / 1000;
    const bool fill = e.p("framing", t, 0) < 0.5;
    const double readoutParam = rollingReadout(e, t);
    char key[160];
    std::snprintf(key, sizeof key, "%.4f|%.4f|%.5f|%d|%.4f|%.4f|%zu", gs.smoothSeconds, gs.horizon, gs.offset, fill ? 1 : 0, aspect,
                  readoutParam, std::hash<std::string>{}(data));
    std::lock_guard lock(m);
    auto& slot = cache[key];
    if (slot) return slot;
    auto plan = std::make_shared<GyroPlan>();
    if (!gyroPathFromString(data, plan->path) || plan->path.focal <= 0) return nullptr;
    plan->corrections = gyroCorrections(plan->path, gs);
    // (Rolling Shutter at 0 takes the readout the data gives, when it gives one)
    plan->readout = readoutParam > 0 ? readoutParam : plan->path.readout > 0 ? std::min(1.0, plan->path.readout * plan->path.fps) : 0.0;
    plan->zoom = fill ? gyroZoom(plan->corrections, plan->path.focal, plan->readout, aspect) : 1.0;
    if (cache.size() > 32) cache.clear();  // (then `slot` is gone: stored afresh)
    cache[key] = plan;
    return plan;
}

bool gyroStabilize(const Effect& e, FrameTime t, Image& img, double sourceSeconds) {
    auto plan = gyroPlan(e, t, double(img.height) / img.width);
    if (!plan || plan->corrections.empty()) return false;
    const long i = std::clamp<long>(std::lround((sourceSeconds - plan->path.start) * plan->path.fps), 0, long(plan->corrections.size()) - 1);
    const GyroCorrection& c = plan->corrections[size_t(i)];
    const double zoom = plan->zoom * (1 + e.p("extra_zoom", t, 0) / 100);
    const Image src = img;
    const double W = img.width, H = img.height, aspect = H / W, f = plan->path.focal, readout = plan->readout;
    // The turn for each row of the source as it was read (the middle row's is none).
    std::vector<std::array<double, 9>> turns;
    if (readout > 0) {
        turns.resize(size_t(img.height));
        for (int r = 0; r < img.height; ++r) turns[size_t(r)] = gyroTurnAt(c, ((r + 0.5) / H - 0.5) * readout);
    }
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* o = img.row(y);
            for (int x = 0; x < img.width; ++x, o += 4) {
                const double qx = ((x + 0.5) - W / 2) / W / zoom, qy = ((y + 0.5) - H / 2) / W / zoom;
                const auto& r = c.r;
                double cx = r[0] * qx / f + r[1] * qy / f + r[2], cy = r[3] * qx / f + r[4] * qy / f + r[5], cz = r[6] * qx / f + r[7] * qy / f + r[8];
                bool ok = cz > 1e-6;
                double sx = ok ? f * cx / cz : 0, sy = ok ? f * cy / cz : 0;
                for (int it = 0; ok && readout > 0 && it < 2; ++it) {
                    // Found on a row read earlier or later: where the camera pointed then.
                    const int row = std::clamp(int(std::floor(sy * W + H / 2)), 0, img.height - 1);
                    const auto& m = turns[size_t(row)];
                    const double mx = m[0] * cx + m[1] * cy + m[2] * cz, my = m[3] * cx + m[4] * cy + m[5] * cz, mz = m[6] * cx + m[7] * cy + m[8] * cz;
                    ok = mz > 1e-6;
                    if (ok) sx = f * mx / mz, sy = f * my / mz;
                }
                const double px = sx * W + W / 2 - 0.5, py = sy * W + H / 2 - 0.5;
                if (!ok || px < -1 || py < -1 || px > W || py > H) {
                    o[0] = o[1] = o[2] = o[3] = 0;
                    continue;
                }
                const int x0 = int(std::floor(px)), yy0 = int(std::floor(py));
                const float fx = float(px - x0), fy = float(py - yy0);
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
    return true;
}

void stabilize(const Effect& e, FrameTime t, Image& img, double sourceSeconds) {
    if (sourceSeconds < 0 || img.empty()) return;
    // Camera Gyro, when the analysis found the camera's motion data (else the measured movement, with rotation).
    if (e.type == "stabilize" && std::lround(e.p("method", t, 2)) == 3 && gyroStabilize(e, t, img, sourceSeconds)) return;
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
void paintStrokes(const Effect& e, FrameTime t, Image& img);  // (Paint, below)
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

EffectDomain effectDomain(const std::string& type) {
    static const std::map<std::string, EffectDomain> domains = [] {
        std::map<std::string, EffectDomain> m;
        for (const char* t : {"color_correct", "curves", "hue_curves", "color_warper", "cdl", "lut", "hue_sat", "levels"})
            m[t] = EffectDomain::Log;
        for (const char* t : {"gaussian_blur", "directional_blur", "radial_blur", "channel_blur", "glow", "depth_blur", "depth_fog", "lens_flare",
                              "light_rays", "aperture_glare", "tilt_shift", "chromatic_aberration", "relight", "vignette", "drop_shadow",
                              "rgb_mixer", "mosaic", "focus_repair"})
            m[t] = EffectDomain::Linear;
        for (const char* t : {"posterize", "painterly", "halftone", "duotone", "vhs", "find_edges", "emboss", "invert", "chroma_key", "luma_key",
                              "screen_key", "clean_plate", "broadcast_safe", "noise", "depth_map"})
            m[t] = EffectDomain::Display;
        return m;
    }();
    const auto it = domains.find(type);
    return it == domains.end() ? EffectDomain::AsIs : it->second;
}

namespace {
void applyVideoEffectHere(const Effect& e, FrameTime t, Image& img, double pixelScale, double sourceSeconds);
}

void applyVideoEffect(const Effect& e, FrameTime t, Image& img, double pixelScale, double sourceSeconds) {
    // (a grade node's settings act through the node graph, render/GradeGraph.h; a 3D Camera Tracker only holds its solve)
    // (Sky Replacement runs when tracks are composited, render/SkyReplace.h)
    // (a compositing node's settings act through its graph, render/CompositeRender.h)
    if (!e.enabled || img.empty() || e.type == "grade_node" || e.type == "camera_track" || e.type == "sky_replace" ||
        e.type == "displace_by_layer" ||
        e.type.rfind("composite_", 0) == 0)
        return;
    // A sequence graded scene-referred: the effect in the values it is made for, and back.
    const SceneGrading* sg = currentSceneGrading();
    const ColorSpace& here = currentWorkingSpace();
    if (sg && (&here == sg->working || &here == sg->linear)) {
        const EffectDomain d = effectDomain(e.type);
        // (log: the working space itself when it is one, a camera's own log included, as a show LUT expects)
        const ColorSpace* into = d == EffectDomain::Log      ? (here.transfer == Transfer::Linear ? &logColorSpace(here.primaries) : nullptr)
                                 : d == EffectDomain::Linear ? sg->linear
                                 : d == EffectDomain::Display ? sg->display
                                                              : nullptr;
        if (into && into != &here) {
            if (d == EffectDomain::Display) outputTransform(img, here, *into, sg->peakNits, sg->kind, true);
            else convertExactly(img, here, *into);
            {
                const WorkingSpaceScope inside(into);
                applyVideoEffectHere(e, t, img, pixelScale, sourceSeconds);
            }
            if (d == EffectDomain::Display) inverseOutputTransform(img, *into, here, sg->peakNits, sg->kind);
            else convertExactly(img, *into, here);
            return;
        }
    }
    applyVideoEffectHere(e, t, img, pixelScale, sourceSeconds);
}

namespace {
void applyVideoEffectHere(const Effect& e, FrameTime t, Image& img, double pixelScale, double sourceSeconds) {
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
    if (e.type == "patch_fill") {
        // The mask is where the patch goes; it is copied from the same shape moved by Source X and Y.
        if (!hasMask(e, t)) return;
        const std::vector<float> matte = effectMatte(e, t, img, pixelScale, sourceSeconds);
        if (e.p("mask.show", t) > 0.5) {
            for (size_t i = 0; i < matte.size(); ++i) {
                float* p = &img.px[i * 4];
                p[0] = p[1] = p[2] = matte[i] * p[3];
            }
            return;
        }
        pasteShifted(img, matte, e.p("source_x", t, 0.15) * img.width, e.p("source_y", t, 0) * img.height, std::lround(e.p("blend", t, 0)) == 0);
        return;
    }
    if (e.type == "paint") {
        // Its mask is not a limit: it is what tracked strokes follow.
        paintStrokes(e, t, img);
        return;
    }
    if (e.type == "surface_track") {
        // Nor the Surface Tracker's: it outlines the surface tracked.
        surfaceTrackEffect(e, t, img, sourceSeconds);
        return;
    }
    if (e.type == "light_rays" && e.p("follow", t, 0) > 0.5) {
        // Nor for rays from the mask's track: it is where they come from.
        applyEffectUnmasked(e, t, img, pixelScale);
        return;
    }
    if (e.type == "ofx" && ofx::usesOwnMask(e) && e.p("mask.show", t) <= 0.5) {
        // An OpenFX plugin with an input on its own mask takes the mask itself (render/Ofx.h); its result is not cut by
        // the mask again.
        const std::vector<float> matte = hasMask(e, t) ? effectMatte(e, t, img, pixelScale, sourceSeconds) : std::vector<float>{};
        const ofx::OwnMaskScope scope(matte.empty() ? nullptr : &matte);
        applyEffectUnmasked(e, t, img, pixelScale);
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
}  // namespace

double depthBlurFocus(const Effect& e, FrameTime t, const std::vector<float>& depth, int W, int H) {
    const int follow = int(std::lround(e.p("follow", t, 0)));
    const double manual = e.p("focus", t, 80) / 100;
    if (follow == 2) {
        // The nearest face: the median depth of the middle of each face's box, the nearest of them.
        const std::vector<FaceBox>* faces = currentFaces();
        double nearest = -1;
        if (faces)
            for (const FaceBox& f : *faces) nearest = std::max(nearest, depthAt(depth, W, H, f.x + f.w / 2, f.y + f.h / 2));
        return nearest >= 0 ? nearest : manual;
    }
    if (follow != 1) return manual;
    // A point: the depth there. With keys, a rack: the depths at the keys either side, on this frame, mixed as the
    // keys ease from one to the other (so the focus moves in depth, not across the picture).
    const auto px = e.params.find("point_x"), py = e.params.find("point_y");
    auto at = [&](FrameTime k) { return depthAt(depth, W, H, e.p("point_x", k, 0.5), e.p("point_y", k, 0.5)); };
    std::vector<FrameTime> keys;
    for (auto it : {px, py})
        if (it != e.params.end())
            for (const Keyframe& k : it->second.keys) keys.push_back(k.t);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    if (keys.size() < 2 || t <= keys.front() || t >= keys.back()) return at(t);
    const auto next = std::upper_bound(keys.begin(), keys.end(), t);
    const FrameTime a = *(next - 1), b = *next;
    if (a == t) return at(t);
    double w = double(t - a) / double(b - a);
    for (const char* name : {"point_x", "point_y"}) {
        const double va = e.p(name, a, 0.5), vb = e.p(name, b, 0.5);
        if (std::fabs(vb - va) > 1e-9) {
            w = (e.p(name, t, 0.5) - va) / (vb - va);
            break;
        }
    }
    const double da = at(a), db = at(b);
    return da + (db - da) * std::clamp(w, 0.0, 1.0);
}

namespace {
// Lens Blur (Depth)'s settings at t, over the frame's depth (W x H), as both renderers draw it.
LensBlurSettings depthBlurSettings(const Effect& e, FrameTime t, const std::vector<float>& d, int W, int H, double pixelScale) {
    LensBlurSettings ls;
    ls.focus = depthBlurFocus(e, t, d, W, H);
    ls.half = e.p("range", t, 10) / 200;
    ls.falloff = std::max(0.01, e.p("falloff", t, 30) / 100);
    ls.blurNear = e.p("near", t, 1) > 0.5;
    // The radius is the blur at f/2.8; a wider aperture blurs more, in proportion.
    ls.radius = std::max(0.0, e.p("radius", t, 12) * pixelScale) * 2.8 / std::clamp(e.p("fstop", t, 2.8), 0.7, 64.0);
    ls.aperture.blades = int(std::lround(e.p("blades", t, 0)));
    if (ls.aperture.blades > 0 && ls.aperture.blades < 3) ls.aperture.blades = 3;  // the fewest a polygon has
    ls.aperture.rotation = e.p("blade_rotation", t, 0);
    ls.aperture.squeeze = std::max(1.0, e.p("squeeze", t, 1));
    ls.aperture.catEye = std::clamp(e.p("cat_eye", t, 0) / 100, 0.0, 1.0);
    ls.highlights = std::clamp(e.p("highlights", t, 0) / 100, 0.0, 1.0);
    ls.threshold = std::clamp(e.p("threshold", t, 80) / 100, 0.05, 1.0);
    return ls;
}
}  // namespace

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
        // Thinning with height above the horizon (haze lies low), by up to e^-8 at the top of the frame.
        const double horizon = std::clamp(e.p("horizon", t, 50) / 100, 0.0, 1.0), lift = std::clamp(e.p("height", t, 0) / 100, 0.0, 1.0) * 8;
        for (size_t i = 0; i < d.size(); ++i) {
            const double far = std::clamp((start - d[i]) / start, 0.0, 1.0);
            const double above = std::max(0.0, horizon - (double(i / size_t(W)) + 0.5) / H);
            const float f = float(amount * std::pow(far, curve) * std::exp(-lift * above));
            float* p = &img.px[i * 4];
            for (int c = 0; c < 3; ++c) p[c] += (col[c] * p[3] - p[c]) * f;
        }
        return;
    }
    // Lens Blur (render/LensBlur.h): each pixel blurred by its own circle of confusion, in layers far to near.
    const LensBlurSettings ls = depthBlurSettings(e, t, d, W, H, pixelScale);
    const ColorSpace& working = currentWorkingSpace();
    std::unique_ptr<TransferTables> tables;
    if (working.transfer != Transfer::Linear) tables = std::make_unique<TransferTables>(working.transfer);
    lensBlur(img, d, ls, tables.get());
    // Focus breathing: the picture a little larger focused near (up to 4 %) and as it is focused farthest, so its
    // edges are never pulled in.
    const double breathing = std::clamp(e.p("breathing", t, 0) / 100, 0.0, 1.0);
    if (breathing > 0) breathe(img, 1 + 0.04 * breathing * ls.focus);
}

}  // namespace

namespace {

// What a restoration effect found, shown in red over the picture.
void showFound(Image& img, const std::vector<float>& matte) {
    for (size_t i = 0; i < matte.size() && i * 4 < img.px.size(); ++i) {
        float* p = &img.px[i * 4];
        const float m = matte[i];
        p[0] += (p[3] - p[0]) * m;
        p[1] -= p[1] * m;
        p[2] -= p[2] * m;
    }
}

// Through the effect's mask, when it has one: `fn` changes a copy, mixed back by the mask.
void throughMask(const Effect& e, FrameTime lt, Image& src, double pixelScale, double sourceSeconds, const std::function<void(Image&)>& fn) {
    if (!hasMask(e, lt)) {
        fn(src);
        return;
    }
    const std::vector<float> matte = effectMatte(e, lt, src, pixelScale, sourceSeconds);
    if (e.p("mask.show", lt) > 0.5) {
        for (size_t i = 0; i < matte.size(); ++i) {
            float* q = &src.px[i * 4];
            q[0] = q[1] = q[2] = matte[i] * q[3];
        }
        return;
    }
    Image done = src;
    fn(done);
    for (size_t i = 0; i < matte.size() && i * 4 < src.px.size(); ++i)
        for (int k = 0; k < 4; ++k) src.px[i * 4 + size_t(k)] += (done.px[i * 4 + size_t(k)] - src.px[i * 4 + size_t(k)]) * matte[i];
}

void paintStrokes(const Effect& e, FrameTime t, Image& img) {
    for (const PaintStrokeAt& s : paintStrokesAt(e, t, img.width, img.height))
        paintStroke(img, s.points, s.radius, s.softness, s.dx, s.dy, s.heal);
}

}  // namespace

std::vector<PaintStrokeAt> paintStrokesAt(const Effect& e, FrameTime t, int W, int H) {
    std::vector<PaintStrokeAt> out;
    if (W <= 0 || H <= 0) return out;
    for (const PaintStroke& s : paintStrokesFromString(e.s("strokes"))) {
        if (!s.hold && s.frame != t) continue;
        PaintStrokeAt a;
        a.heal = s.heal;
        a.radius = s.radius * W;
        a.softness = s.softness;
        // A tracked stroke moves as the mask's box has since the frame it was drawn on (its source offset too); with
        // the surface track followed, as the surface has.
        const MaskBox from = maskBox(e, FrameTime(s.frame)), to = maskBox(e, t);
        const SurfaceContext* sc = e.p("surface", t) > 0.5 ? currentSurface() : nullptr;
        const int surfaceFrom = sc ? sc->track->frameAt(sc->mediaSeconds(FrameTime(s.frame))) : -1;
        const int surfaceTo = sc ? sc->track->frameAt(sc->mediaSeconds(t)) : -1;
        std::vector<std::array<double, 2>> uv = s.points;
        uv.push_back({s.points[0][0] + s.dx, s.points[0][1] + s.dy});  // (where it copies from)
        if (surfaceFrom >= 0 && surfaceTo >= 0) {
            const SurfaceTrack& tr = *sc->track;
            std::vector<MeshPoint> px(uv.size());
            for (size_t i = 0; i < uv.size(); ++i) px[i] = {uv[i][0] * tr.width, uv[i][1] * tr.height};
            px = surfaceCarry(tr, surfaceFrom, surfaceTo, px);
            for (size_t i = 0; i < uv.size(); ++i) uv[i] = {px[i][0] / tr.width, px[i][1] / tr.height};
        } else if (s.track) {
            for (auto& [u, v] : uv) {
                double bx, by;
                frameToBox(from, W, H, u, v, bx, by);
                boxToFrame(to, W, H, bx, by, u, v);
            }
        }
        for (size_t i = 0; i + 1 < uv.size(); ++i) a.points.push_back({uv[i][0] * W - 0.5, uv[i][1] * H - 0.5});
        a.dx = (uv.back()[0] * W - 0.5) - a.points[0][0], a.dy = (uv.back()[1] * H - 0.5) - a.points[0][1];
        if (s.track && (surfaceFrom < 0 || surfaceTo < 0)) a.radius *= std::sqrt(std::max(1e-6, (to.w * to.h) / std::max(1e-12, from.w * from.h)));
        out.push_back(std::move(a));
    }
    return out;
}

void applyDirtRemoval(const Effect& e, FrameTime lt, Image& src, const Image* prev, const Image* next, const MotionFn& toPrev,
                      const MotionFn& toNext, double pixelScale, double sourceSeconds, const Image* prev2, const Image* next2,
                      const MotionFn& toPrev2, const MotionFn& toNext2) {
    if (!e.enabled || !prev || !next) return;
    throughMask(e, lt, src, pixelScale, sourceSeconds, [&](Image& img) {
        DirtSettings s;
        s.sensitivity = e.p("sensitivity", lt, 50);
        s.maxSize = std::max(1.0, e.p("size", lt, 12) * pixelScale);
        std::vector<float> m = detectDirt(img, *prev, *next, toPrev, toNext, s);
        if (m.empty()) return;
        // Then what lasts two frames, against the frames two away (once the rest is out).
        const bool show = e.p("show", lt) > 0.5;
        if (!show) fillFromNeighbours(img, m, *prev, *next, toPrev, toNext, s.maxSize / 2 + 4);
        std::vector<float> lasting;
        if (prev2 && next2) lasting = detectLastingDirt(img, *prev, *next, *prev2, *next2, toPrev, toNext, toPrev2, toNext2, s);
        if (show) {
            for (size_t i = 0; i < lasting.size(); ++i) m[i] = std::max(m[i], lasting[i]);
            showFound(img, m);
        } else if (!lasting.empty()) {
            fillFromNeighbours(img, lasting, *prev2, *next2, toPrev2, toNext2, s.maxSize / 2 + 4);
        }
    });
}

void applyDustSpots(const Effect& e, FrameTime lt, Image& src, const Image* prev, const Image* next, const MotionFn& toPrev,
                     const MotionFn& toNext, double pixelScale, double sourceSeconds) {
    if (!e.enabled) return;
    std::vector<DustSpot> here;
    for (const DustSpot& d : dustSpotsFromString(e.s("spots")))
        if (d.frame == lt) here.push_back(d);
    if (here.empty()) return;
    throughMask(e, lt, src, pixelScale, sourceSeconds, [&](Image& img) {
        const double sensitivity = e.p("sensitivity", lt, 50);
        const bool both = prev && next;
        for (const DustSpot& d : here) {
            const LocalMatte m = dustArea(img, prev, next, toPrev, toNext, d, sensitivity);
            if (e.p("show", lt) > 0.5) showFound(img, m.full(img.width, img.height));
            else if (both) fillFromNeighbours(img, m, *prev, *next, toPrev, toNext, d.r * img.width + 4);
            else fillFromSurround(img, m);
        }
    });
}

void applyFilmNegative(const Effect& e, FrameTime t, Image& img, Transfer transfer) {
    NegativeSettings s;
    s.encoding = ScanEncoding(std::clamp(int(std::lround(e.p("scan", t))), 0, 2));
    if (e.p("base_from", t) > 0.5 || !filmBase(img, s.encoding, transfer, s.base)) {
        s.base[0] = e.p("base_r", t, 0.9), s.base[1] = e.p("base_g", t, 0.55), s.base[2] = e.p("base_b", t, 0.35);
    }
    s.gamma = e.p("gamma", t, 0.6);
    s.exposure = e.p("exposure", t);
    invertNegative(img, s, transfer);
}

void applyPerfLock(const Effect& e, FrameTime t, Image& img) {
    PerfSettings s;
    s.side = e.p("side", t) > 0.5 ? 1 : 0;
    s.rebate = e.p("rebate", t, 12) / 100;
    const bool show = e.p("show", t) > 0.5;
    std::vector<float> holes;
    const std::vector<Point2> now = findPerforations(img, s, show ? &holes : nullptr);
    if (show) {
        showFound(img, holes);
        return;
    }
    double dx = 0, dy = 0;
    if (perforationOffset(now, perforationsFromString(e.s("reference"), img.width, img.height), dx, dy, img.height)) img = shiftedImage(img, dx, dy);
}

namespace {

void applyEffectUnmasked(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const std::string& ty = e.type;
    if (ty == "scratch_removal") {
        ScratchSettings s;
        s.sensitivity = e.p("sensitivity", t, 50);
        s.maxWidth = std::max(1.0, e.p("width", t, 4) * pixelScale);
        s.polarity = int(std::lround(e.p("polarity", t, 0)));
        s.follow = e.p("follow", t, 0) > 0.5;  // (on in new effects; projects from before it keep their look)
        const std::vector<float> m = detectScratches(img, s);
        if (e.p("show", t) > 0.5) showFound(img, m);
        else fillAcross(img, m);
        return;
    }
    if (ty == "paint") {
        paintStrokes(e, t, img);
        return;
    }
    if (ty == "learned_effect") {
        // (Without its network file, or when it fails, the picture is left as it is.)
        const float amount = float(std::clamp(e.p("amount", t, 100) / 100, 0.0, 1.0));
        if (amount <= 0) return;
        const SlowEffects& slow = currentSlowEffects();
        Image learned;
        std::string why;
        if (!learn::applySaved(e.s("network"), img, learned, &why, slow.cancel, slow.run)) {
            if (slow.issues)
                slow.issues->note(why.empty() ? "The Learned Effect is shown once the frames are rendered (Render In to Out)"
                                              : "Learned Effect: " + why);
            return;
        }
        for (size_t i = 0; i < img.px.size(); ++i) img.px[i] += (learned.px[i] - img.px[i]) * amount;
        return;
    }
    if (ty == "color_correct") colorCorrect(e, t, img);
    else if (ty == "chart_match") applyChartMatch(e, t, img);
    else if (ty == "curves") curves(e, t, img);
    else if (ty == "hue_curves") hueCurves(e, t, img);
    else if (ty == "rgb_mixer") rgbMixer(e, t, img);
    else if (ty == "color_warper") colorWarper(e, t, img);
    else if (ty == "hdr_palette") hdrPalette(e, t, img);
    else if (ty == "hue_sat") hueSat(e, t, img);
    else if (ty == "lut") applyLut(e, t, img);
    else if (ty == "cdl") applyCdlEffect(e, t, img);
    else if (ty == "aces_gamut_compress") applyAcesGamutCompress(e, t, currentWorkingSpace(), img);
    else if (ty == "color_slice") colorSlice(e, t, currentWorkingSpace(), img);
    else if (ty == "crosstalk") crosstalk(e, t, currentWorkingSpace(), img);
    else if (ty == "colour_correlation") colourCorrelation(e, t, currentWorkingSpace(), img);
    else if (ty == "texture_pop") textureDetail(e, t, currentWorkingSpace(), img, pixelScale);
    else if (ty == "color_space_transform") {
        const ColorSpace* from = nullptr;
        const ColorSpace* to = nullptr;
        for (const auto& cs : colorSpaces()) {
            if (cs.label == e.s("from")) from = &cs;
            if (cs.label == e.s("to")) to = &cs;
        }
        if (from && to && e.s("rendering") == "None") {
            // Exactly, as a LUT's "Apply In" goes into its space and back.
            double mat[9];
            primariesMatrix(from->primaries, to->primaries, mat);
            if (from->id != to->id)
                perPixel(img, [&](float& r, float& g, float& b, float&) {
                    float v[3] = {r, g, b};
                    through(mat, from->transfer, to->transfer, v);
                    r = v[0], g = v[1], b = v[2];
                });
        } else if (from && to)
            convertColor(img, *from, *to);
    } else if (ty == "ocio")
        applyOcio(e, img, nullptr, t);
    else if (ty == "ofx")
        ofx::applyEffect(e, double(t), img, pixelScale, ofx::currentFetch(), nullptr, &currentWorkingSpace());
    else if (ty == "chroma_key") chromaKey(e, t, img);
    else if (ty == "screen_key") screenKey(e, t, img, pixelScale);
    else if (ty == "clean_plate") cleanPlate(e, t, img, pixelScale);
    else if (ty == "edge_extend") edgeExtend(e, t, img);
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
    else if (ty == "grain_match") grainMatch(e, t, img);
    else if (ty == "film_look") filmLook(img, filmLookSettings(e, t), t);
    else if (ty == "directional_blur") vfx::directionalBlur(e, t, img, pixelScale);
    else if (ty == "radial_blur") sfx::radialBlur(e, t, img, pixelScale);
    else if (ty == "chromatic_aberration") vfx::chromaticAberration(e, t, img, pixelScale);
    else if (ty == "lens_distortion") vfx::lensDistortion(e, t, img);
    else if (ty == "lens_correction") vfx::lensCorrection(e, t, img);
    else if (ty == "magnify") vfx::magnify(e, t, img, pixelScale);
    else if (ty == "channel_blur") vfx::channelBlur(e, t, img, pixelScale);
    else if (ty == "noise") vfx::noise(e, t, img);
    else if (ty == "corner_pin") vfx::cornerPin(e, t, img);
    else if (ty == "mesh_warp") meshWarp(e, t, img, pixelScale);
    else if (ty == "grid_warp") gridWarp(e, t, img);
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
    else if (ty == "painterly") paintEffect(e, t, img, pixelScale);
    else if (ty == "duotone") sfx::duotone(e, t, img);
    else if (ty == "vhs") sfx::vhs(e, t, img, pixelScale);
    else if (ty == "lens_flare") sfx::lensFlare(e, t, img);
    else if (ty == "light_rays") sfx::lightRays(e, t, img, pixelScale);
    else if (ty == "aperture_glare") sfx::apertureGlare(e, t, img, pixelScale);
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
    } else if (ty == "face_shape" || ty == "look_at_lens") {
        const std::vector<FaceBox>* faces = currentFaces();
        if (faces && !faces->empty() && faceMeshAvailable() && faceMeshModel().installed())
            if (std::shared_ptr<const std::vector<FaceMesh>> meshes = cachedFaceMeshes(img, *faces)) {
                if (ty == "face_shape") {
                    FaceShapeSettings fs;
                    fs.faceWidth = e.p("face_width", t) / 100, fs.jaw = e.p("jaw", t) / 100, fs.chin = e.p("chin", t) / 100;
                    fs.forehead = e.p("forehead", t) / 100, fs.noseWidth = e.p("nose_width", t) / 100;
                    fs.noseLength = e.p("nose_length", t) / 100, fs.eyeSize = e.p("eye_size", t) / 100;
                    fs.eyeDistance = e.p("eye_distance", t) / 100, fs.mouthWidth = e.p("mouth_width", t) / 100;
                    fs.mouthHeight = e.p("mouth_height", t) / 100;
                    fs.showMesh = e.p("show", t) > 0.5;
                    reshapeFaces(img, *meshes, fs);
                } else {
                    LookAtLensSettings ls;
                    ls.amount = e.p("amount", t, 100) / 100;
                    ls.limit = e.p("limit", t, 20);
                    ls.showIrises = e.p("show", t) > 0.5;
                    lookAtLens(img, *meshes, ls);
                }
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
    auto at = [&](int x, int y, int z) { return &data[((size_t(z) * size_t(size) + size_t(y)) * size_t(size) + size_t(x)) * 3]; };
    // Tetrahedral: the cube cut into six tetrahedra along its grey diagonal, so a grey is interpolated from greys only
    // (trilinear mixes in the corners off it, which a tone mapping's highlights pull down).
    const float* c000 = at(x0, y0, z0);
    const float* c111 = at(x0 + 1, y0 + 1, z0 + 1);
    const float *a, *b2;
    float w0, w1, w2, w3;
    if (fx > fy) {
        if (fy > fz) a = at(x0 + 1, y0, z0), b2 = at(x0 + 1, y0 + 1, z0), w0 = 1 - fx, w1 = fx - fy, w2 = fy - fz, w3 = fz;
        else if (fx > fz) a = at(x0 + 1, y0, z0), b2 = at(x0 + 1, y0, z0 + 1), w0 = 1 - fx, w1 = fx - fz, w2 = fz - fy, w3 = fy;
        else a = at(x0, y0, z0 + 1), b2 = at(x0 + 1, y0, z0 + 1), w0 = 1 - fz, w1 = fz - fx, w2 = fx - fy, w3 = fy;
    } else {
        if (fz > fy) a = at(x0, y0, z0 + 1), b2 = at(x0, y0 + 1, z0 + 1), w0 = 1 - fz, w1 = fz - fy, w2 = fy - fx, w3 = fx;
        else if (fz > fx) a = at(x0, y0 + 1, z0), b2 = at(x0, y0 + 1, z0 + 1), w0 = 1 - fy, w1 = fy - fz, w2 = fz - fx, w3 = fx;
        else a = at(x0, y0 + 1, z0), b2 = at(x0 + 1, y0 + 1, z0), w0 = 1 - fy, w1 = fy - fx, w2 = fx - fz, w3 = fz;
    }
    float out[3];
    for (int c = 0; c < 3; ++c) out[c] = w0 * c000[c] + w1 * a[c] + w2 * b2[c] + w3 * c111[c];
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
        // hard mix: 1 where the two reach 1, else 0. The line is half an 8-bit step short of 1, as far as it can be from
        // any two 8-bit values' sum, so a picture's codes land the same side however each path rounds them (the GPU's
        // half floats too).
        case 13: return cb + cs >= 1 - 0.5f / 255 ? 1.0f : 0.0f;
        default: return cs;
    }
}

int modeIndex(const std::string& mode) {
    static const std::map<std::string, int> idx = {
        {"normal", 0},   {"add", 1},         {"multiply", 2},    {"screen", 3},      {"overlay", 4},
        {"darken", 5},   {"lighten", 6},     {"difference", 7},  {"soft_light", 8},  {"hard_light", 9},
        {"color_dodge", 10}, {"color_burn", 11}, {"subtract", 12}, {"hard_mix", 13}};
    auto it = idx.find(mode);
    return it == idx.end() ? 0 : it->second;
}

}  // namespace

int blendModeIndex(const std::string& mode) { return modeIndex(mode); }
float blendModeChannel(int mode, float backdrop, float source) { return blendChannel(mode, backdrop, source); }

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


// How far out Shape Wipe's shape must grow to cover a cell's corners (its edge walked all round: a shape's farthest
// point need not be a corner).
float shapeReach(int shape, int count, float cell, float rc, float rs, int w, int h) {
    float reach = 0;
    const float hx = count == 1 ? w / (2 * cell) : 1.0f, hy = count == 1 ? h / (2 * cell) : 1.0f;
    for (int i = 0; i <= 64; ++i)
        for (int side = 0; side < 4; ++side) {
            const float t = float(i) / 64 * 2 - 1;
            const float ex = side < 2 ? (side ? hx : -hx) : t * hx, ey = side < 2 ? t * hy : (side == 3 ? hy : -hy);
            reach = std::max(reach, shapeDistance(shape, ex * rc + ey * rs, -ex * rs + ey * rc));
        }
    return reach;
}

// 3D Cube's and Flip's faces at uf (0 < uf < 1): A's face, then B's.
void transitionFaces(const std::string& type, int dir, float uf, int w, int h, Face faces[2]) {
    const float pi = float(M_PI);
    const float bell = std::sin(pi * uf);
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

}  // namespace

Image transitionMix(const std::string& type, const Effect& params, const Image& aIn, const Image& bIn, double u, int w,
                    int h, double t, double scale) {
    if (type == "ofx") {
        // An OpenFX transition plug-in (render/Ofx.h).
        Image mixed;
        if (ofx::applyTransition(params, t, u, aIn, bIn, mixed, scale, nullptr, &currentWorkingSpace()) && !mixed.empty())
            return mixed.width == w && mixed.height == h ? mixed : resizeImage(mixed, w, h);
        return transitionMix("cross_dissolve", params, aIn, bIn, u, w, h);
    }
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
    // The settings as the GPU takes them too (gpuTransitionParams), so the two read them alike.
    float q[44];
    gpuTransitionParams(type, params, u, w, h, q);
    const float uf = q[1];
    const float soft = q[2];
    const int dir = int(q[3]);
    const float ang = q[4];
    const float strength = q[5];
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
    const float turn = q[6];
    const float leak[3] = {q[8], q[9], q[10]};
    const bool brightsFirst = q[7] > 0.5f;
    const uint32_t glitchSeed = uint32_t(q[11]);  // the pattern jumps a few times through the transition
    // Shape Wipe: square cells of the shape (one over the whole frame for a count of 1), and how far out the shape
    // must grow to cover a cell's corners.
    const int shape = int(q[12]);
    const int count = int(q[13]);
    const float cell = q[14], reach = q[15], rc = q[16], rs = q[17];
    // 3D Cube and Flip: the faces' projections for this moment.
    Face faces[2];
    const Image* faceImg[2] = {&a, &b};
    const bool threeD = type == "cube" || type == "flip";
    if (threeD && uf > 0 && uf < 1) transitionFaces(type, dir, uf, w, h, faces);
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

int gpuTransitionParams(const std::string& type, const Effect& params, double u, int w, int h, float q[44]) {
    static const char* const kinds[] = {"cross_dissolve", "dip_to_black", "dip_to_white", "wipe", "iris", "push", "slide", "zoom", "whip_pan",
                                        "zoom_blur", "spin", "glitch", "light_leak", "luma_wipe", "shape_wipe", "cube", "flip", "clock_wipe"};
    int kind = 0;  // (any other: a cross dissolve, as transitionMix mixes it)
    for (int i = 0; i < int(std::size(kinds)); ++i)
        if (type == kinds[i]) kind = i;
    // As transitionMix works them out, in gpumix.frag's order.
    std::fill(q, q + 44, 0.0f);
    const float pi = float(M_PI);
    const float uf = float(std::clamp(u, 0.0, 1.0));
    const int dir = int(params.p("direction", 0, 0));
    q[0] = float(kind), q[1] = uf, q[2] = float(params.p("softness", 0, 0.05)), q[3] = float(dir);
    q[4] = float(params.p("angle", 0, 0)) * float(M_PI) / 180.0f, q[5] = float(params.p("strength", 0, 1));
    q[6] = float(params.p("turn", 0, 180)) * pi / 180, q[7] = params.p("invert", 0) > 0.5 ? 1 : 0;
    q[8] = float(params.p("color.r", 0, 1.0)), q[9] = float(params.p("color.g", 0, 0.55)), q[10] = float(params.p("color.b", 0, 0.2));
    q[11] = float(uint32_t(uf * 40));
    const int shape = int(std::lround(params.p("shape", 0, 0)));
    const int count = std::clamp(int(std::lround(params.p("count", 0, 1))), 1, 64);
    const float cell = count == 1 ? float(std::max(w, h)) : float(w) / count;
    const float rot = float(params.p("angle", 0, 0)) * pi / 180, rc = std::cos(rot), rs = std::sin(rot);
    q[12] = float(shape), q[13] = float(count), q[14] = cell, q[15] = type == "shape_wipe" ? shapeReach(shape, count, cell, rc, rs, w, h) : 0.0f;
    q[16] = rc, q[17] = rs;
    if ((type == "cube" || type == "flip") && uf > 0 && uf < 1) {
        q[18] = 1;
        Face faces[2] = {};
        transitionFaces(type, dir, uf, w, h, faces);
        for (int k = 0; k < 2; ++k) {
            for (int i = 0; i < 9; ++i) q[20 + k * 12 + i] = float(faces[k].inv[i]);
            q[20 + k * 12 + 9] = faces[k].facing ? 1 : 0;
        }
    }
    return type == "ofx" || type == "smooth_cut" ? -1 : kind;
}

const ColorSpace* sourceLookSpace(const Effect& e) {
    if (e.type != "cdl" && e.type != "lut") return nullptr;
    for (const auto& cs : colorSpaces())
        if (cs.label == e.s("space")) return cs.sceneReferred ? &cs : nullptr;
    return nullptr;
}

void applySourceLook(const Effect& e, FrameTime t, const ColorSpace& media, Image& img) {
    const ColorSpace* space = sourceLookSpace(e);
    if (!space) return;
    std::function<void(float[3])> fn;
    float strength = 1;
    Cdl cdl;
    if (e.type == "cdl") {
        cdl = effectCdl(e, t);
        if (cdl.identity()) return;
        fn = [&cdl](float v[3]) { applyCdl(cdl, v[0], v[1], v[2]); };
    } else {
        if (e.s("path").empty()) return;
        fn = lutFileTransform(e.s("path"), nullptr);
        strength = float(e.p("strength", t, 100)) / 100.0f;
        if (!fn || strength <= 0) return;
    }
    // Into the look's space and back exactly, by scene light (both are scene-referred, so nothing is clipped).
    const bool same = space->id == media.id;
    double to[9], from[9];
    primariesMatrix(media.primaries, space->primaries, to);
    primariesMatrix(space->primaries, media.primaries, from);
    auto through = [](const double mat[9], Transfer a, Transfer b, float v[3]) {
        double l[3];
        for (int i = 0; i < 3; ++i) l[i] = toLinear(a, v[i]);
        for (int i = 0; i < 3; ++i) v[i] = float(fromLinear(b, mat[i * 3] * l[0] + mat[i * 3 + 1] * l[1] + mat[i * 3 + 2] * l[2]));
    };
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        float v[3] = {r, g, b};
        if (!same) through(to, media.transfer, space->transfer, v);
        const float before[3] = {v[0], v[1], v[2]};
        fn(v);
        for (int i = 0; i < 3; ++i) v[i] = before[i] + (v[i] - before[i]) * strength;
        if (!same) through(from, space->transfer, media.transfer, v);
        r = v[0], g = v[1], b = v[2];
    });
}

void applyAcesGamutCompress(const Effect& e, FrameTime t, const ColorSpace& space, Image& img) {
    // In AP1 from `space` and back (render/Gamut.h); Inverse takes compressed colours back out.
    const AcesGamutCompressor rgc(space, e.p("direction", t, 0) >= 0.5);
    perPixel(img, [&](float& r, float& g, float& b, float&) {
        float v[3] = {r, g, b};
        rgc(v);
        r = v[0], g = v[1], b = v[2];
    });
}

namespace {
// The effects the GPU draws through gpufilter.frag.
bool isGpuFilter(const std::string& type) {
    static const std::set<std::string> filters = {"gaussian_blur", "sharpen", "glow", "channel_blur", "directional_blur", "vignette",
                                                  "posterize", "letterbox", "mirror", "mosaic", "chromatic_aberration", "noise",
                                                  "crosstalk", "colour_correlation", "hdr_palette", "color_warper", "color_slice",
                                                  "film_grain", "rgb_mixer", "grain_match", "edge_extend", "wave_warp", "twirl", "spherize",
                                                  "ripple", "turbulent_displace", "motion_tile", "camera_shake", "corner_pin",
                                                  "find_edges", "emboss", "halftone", "duotone", "vhs", "tilt_shift", "mesh_warp",
                                                  "grid_warp", "depth_map", "depth_fog", "light_rays", "lens_flare", "radial_blur",
                                                  "aperture_glare", "painterly", "depth_blur"};
    return filters.count(type) > 0;
}

// The working space's transfer function as gpufilter.frag knows it (the display curves the GPU composites in), or -1.
int gpuTransfer(const ColorSpace& space) {
    switch (space.transfer) {
        case Transfer::Bt1886:
        case Transfer::Srgb:
        case Transfer::Linear:
        case Transfer::Pq:
        case Transfer::Hlg:
        case Transfer::Gamma22: return int(space.transfer);
        default: return -1;
    }
}

// A filter pass's table with the working space's transfer tables in its row 0 (decoding in x, encoding in y), `rows`
// rows in all.
void transferTableRows(gpu::ColorOp::FilterPass& fp, const ColorSpace& space, int rows) {
    const TransferTables tables(space.transfer);
    fp.tableWidth = TransferTables::kDecode + 1;
    fp.table.assign(size_t(fp.tableWidth) * 4 * size_t(rows), 0.0f);
    for (size_t i = 0; i < tables.dec.size(); ++i) fp.table[i * 4] = tables.dec[i];
    for (size_t i = 0; i < tables.enc.size(); ++i) fp.table[i * 4 + 1] = tables.enc[i];
}
}  // namespace

bool gpuMask(const Effect& e, FrameTime t, gpu::ColorOp& op, int width, int height, double pixelScale, bool showing) {
    const int shape = int(std::lround(e.p("mask.shape", t)));
    if (shape == 4 || shape > 6 || (!showing && e.p("mask.show", t) > 0.5) || e.p("mask.surface", t) > 0.5 ||
        (shape > 0 && shape != 6 && e.p("mask.refine", t) > 0))
        return false;  // (what the picture holds, or the mask shown)
    // As effectMatte makes it: the shape (an object's from its segmentation) with its holes closed and the range of
    // depths here, the qualifier, inverting and fading there.
    op.masked = true;
    const bool depthRange = e.p("mask.depth", t) > 0.5;
    if ((shape > 0 || depthRange) && width > 0 && height > 0) {
        op.mask.assign(size_t(width) * size_t(height), 1.0f);
        if (shape == 3) {
            std::vector<float> logits;
            if (e.object && tSourceSeconds >= 0 && e.object->maskLogitsAt(tSourceSeconds, logits))
                op.mask = objectMatte(logits, width, height, e.p("mask.feather", t, 20) * pixelScale, e.p("mask.expansion", t) * pixelScale);
            else
                std::fill(op.mask.begin(), op.mask.end(), 0.f);
        }
        if (shape > 0) drawMaskShape(e, t, shape, op.mask, width, height, pixelScale);
        if (shape > 0 && shape != 6) consolidateMatte(op.mask, width, height, e.p("mask.consolidate", t) * pixelScale, e.p("mask.feather", t, 20) * pixelScale);
        if (depthRange) {
            const DepthMap* depth = currentDepth();
            const double lo = e.p("mask.depth_low", t, 50) / 100, hi = e.p("mask.depth_high", t, 100) / 100;
            const double ramp = std::max(1e-4, e.p("mask.depth_soft", t, 10) / 100);
            if (!depth) {
                std::fill(op.mask.begin(), op.mask.end(), 0.f);
            } else {
                const std::vector<float> d = depth->resized(width, height);
                for (size_t i = 0; i < op.mask.size(); ++i) {
                    const double v = d[i];
                    const double out = v < lo ? lo - v : v > hi ? v - hi : 0.0;
                    const double k = std::clamp(1.0 - out / ramp, 0.0, 1.0);
                    op.mask[i] *= float(k * k * (3 - 2 * k));
                }
            }
        }
    }
    const bool qualify = e.p("mask.qualify", t) > 0.5;
    const float m[12] = {shape > 0 || depthRange ? 1.0f : 0.0f,
                         qualify ? 1.0f : 0.0f,
                         e.p("mask.invert", t) > 0.5 ? 1.0f : 0.0f,
                         float(std::clamp(e.p("mask.opacity", t, 100) / 100.0, 0.0, 1.0)),
                         float(e.p("mask.hue", t)),
                         float(e.p("mask.hue_width", t, 60) / 2),
                         float(std::clamp(e.p("mask.softness", t, 20) / 100.0, 0.0, 1.0)),
                         float(e.p("mask.sat_low", t, 15) / 100),
                         float(e.p("mask.sat_high", t, 100) / 100),
                         float(e.p("mask.lum_low", t, 5) / 100),
                         float(e.p("mask.lum_high", t, 100) / 100),
                         0};
    std::copy(m, m + 12, op.maskParams);
    return true;
}

bool gpuColorOps(const Effect& e, FrameTime t, std::vector<gpu::ColorOp>& out, int width, int height, double pixelScale) {
    if (!e.enabled) return true;
    gpu::ColorOp op;
    if (hasMask(e, t) && !gpuMask(e, t, op, 0, 0, pixelScale)) return false;
    if (e.type == "color_correct") {
        const ColorCorrectSetup s = colorCorrectSetup(e, t);
        if (s.printing) return false;  // (printer lights on the light, which the shader does not decode)
        op.kind = gpu::ColorOp::ColorCorrect;
        // In gpucolor.frag's order: p[1] exposure, contrast, pivot, saturation; p[2] offset, vibrance, tone; p[3..6]
        // lift, gamma, gain, white balance.
        const float head[8] = {s.exposure, s.contrast, s.pivot, s.sat, s.offset, s.vibrance, s.tone ? 1.0f : 0.0f, 0};
        std::copy(head, head + 8, op.params);
        for (int i = 0; i < 3; ++i) {
            op.params[8 + i] = s.lift[i];
            op.params[12 + i] = s.gam[i];
            op.params[16 + i] = s.gain[i];
            op.params[20 + i] = s.wb[i];
        }
        op.tableWidth = ColorCorrectSetup::kTone + 1;
        op.table.assign(size_t(op.tableWidth) * 4, 0.0f);
        for (int i = 0; i < op.tableWidth; ++i) op.table[size_t(i) * 4] = s.toneLut[size_t(i)];
    } else if (e.type == "curves") {
        op.kind = gpu::ColorOp::Curves;
        const std::vector<float> c[4] = {buildCurve(e.s("master", "0,0 1,1")), buildCurve(e.s("red", "0,0 1,1")),
                                         buildCurve(e.s("green", "0,0 1,1")), buildCurve(e.s("blue", "0,0 1,1"))};
        op.params[0] = float(e.p("mix", t, 100)) / 100.0f;
        op.tableWidth = int(c[0].size());
        op.table.resize(size_t(op.tableWidth) * 4);
        for (int i = 0; i < op.tableWidth; ++i)
            for (int k = 0; k < 4; ++k) op.table[size_t(i) * 4 + size_t(k)] = c[k][size_t(i)];
    } else if (e.type == "hue_curves") {
        const std::string hh = e.s("hue_hue"), hs = e.s("hue_sat"), hl = e.s("hue_luma"), ls = e.s("luma_sat"), ss = e.s("sat_sat");
        if (hh.empty() && hs.empty() && hl.empty() && ls.empty() && ss.empty()) return true;  // (nothing to do)
        op.kind = gpu::ColorOp::HueCurves;
        const std::vector<float> c[5] = {buildFlatCurve(hh, 360, true), buildFlatCurve(hs, 360, true), buildFlatCurve(hl, 360, true),
                                         buildFlatCurve(ls, 360, false), buildFlatCurve(ss, 360, false)};
        op.params[0] = float(std::clamp(e.p("mix", t, 100) / 100.0, 0.0, 1.0));
        op.tableWidth = 361;
        op.table.assign(size_t(op.tableWidth) * 8, 0.0f);  // two rows
        for (int i = 0; i < op.tableWidth; ++i) {
            for (int k = 0; k < 4; ++k) op.table[size_t(i) * 4 + size_t(k)] = c[k][size_t(i)];
            op.table[size_t(op.tableWidth + i) * 4] = c[4][size_t(i)];
        }
    } else if (e.type == "lut") {
        const std::string path = e.s("path");
        if (path.empty()) return true;
        const QString name = QString::fromStdString(path);
        if (name.endsWith(QLatin1String(".clf"), Qt::CaseInsensitive) || name.endsWith(QLatin1String(".ctf"), Qt::CaseInsensitive))
            return false;  // (CLF transforms run on the CPU)
        auto lut = loadCubeLut(path, nullptr);
        if (!lut) return true;  // (unreadable: the CPU leaves the picture alone too)
        op.kind = gpu::ColorOp::Lut;
        op.strength = float(e.p("strength", t, 100)) / 100.0f;
        op.lut = lut;
        if (const ColorSpace* space = applyInSpace(e)) {
            const QFileInfo fi(name);
            op.lut = bakedInSpace(*space, "lut|" + path + "|" + std::to_string(fi.size()) + "|" + std::to_string(fi.lastModified().toMSecsSinceEpoch()),
                                  [lut](float v[3]) { lut->apply(v[0], v[1], v[2]); });
            op.clampFirst = true;
        }
    } else if (e.type == "ocio") {
        // OCIO's own GPU shader (within its last digits of the CPU's transform), without a mask.
        if (op.masked || !gpu::takesShaders()) return false;
        std::shared_ptr<const gpu::ShaderStep> shader = ocioShaderStep(e);
        if (!shader) {
            Image probe(1, 1);
            return !applyOcio(e, probe);  // (a broken config: the CPU leaves the picture alone too; else the CPU draws it)
        }
        if (!gpu::takesShaders(shader->key)) return false;  // (it would not build: the CPU draws it)
        // (its live exposure, contrast and gamma, OCIO's dynamic properties: values the shader is drawn with)
        for (const std::string& name : shader->uniforms) {
            auto ends = [&](const char* tail) { return name.size() >= std::strlen(tail) && name.compare(name.size() - std::strlen(tail), std::string::npos, tail) == 0; };
            if (ends("exposureVal")) op.values.push_back(float(e.p("exposure", t, 0)));
            else if (ends("contrastVal")) op.values.push_back(float(e.p("contrast", t, 1)));
            else if (ends("gammaVal")) op.values.push_back(float(e.p("gamma", t, 1)));
            else return false;  // (a value the effect does not set)
        }
        op.kind = gpu::ColorOp::Shader;
        op.shader = std::move(shader);
    } else if (e.type == "cdl") {
        const Cdl cdl = effectCdl(e, t);
        if (cdl.identity()) return true;
        if (const ColorSpace* space = applyInSpace(e)) {  // (baked as applyCdlEffect bakes it)
            op.kind = gpu::ColorOp::Lut;
            op.lut = bakedInSpace(*space, "cdl|" + cdlSopText(cdl) + cdlNumber(cdl.saturation),
                                  [cdl](float v[3]) { applyCdl(cdl, v[0], v[1], v[2]); });
            op.clampFirst = true;
        } else {
            op.kind = gpu::ColorOp::Cdl;
            for (int i = 0; i < 3; ++i) {
                op.params[i] = float(cdl.slope[i]);
                op.params[4 + i] = float(cdl.offset[i]);
                op.params[8 + i] = float(cdl.power[i]);
            }
            op.params[3] = float(cdl.saturation);
        }
    } else if (e.type == "color_space_transform") {
        const ColorSpace* from = nullptr;
        const ColorSpace* to = nullptr;
        for (const auto& cs : colorSpaces()) {
            if (cs.label == e.s("from")) from = &cs;
            if (cs.label == e.s("to")) to = &cs;
        }
        if (!from || !to || from->id == to->id) return true;
        return false;  // (convertColor works out colours past 0..1 exactly, which a table cannot)
    } else if (e.type == "invert") {
        op.kind = gpu::ColorOp::Invert;
        op.params[0] = float(e.p("amount", t, 100)) / 100.0f;
    } else if (e.type == "black_white") {
        op.kind = gpu::ColorOp::BlackWhite;
        op.params[0] = float(e.p("amount", t, 100)) / 100.0f;
        op.params[4] = float(e.p("tint.r", t, 1)), op.params[5] = float(e.p("tint.g", t, 1)), op.params[6] = float(e.p("tint.b", t, 1));
    } else if (e.type == "hue_sat") {
        op.kind = gpu::ColorOp::HueSat;
        const float hue = float(e.p("hue", t)) * float(M_PI) / 180.0f;
        op.params[0] = std::cos(hue), op.params[1] = std::sin(hue);
        op.params[2] = float(e.p("saturation", t, 1)), op.params[3] = float(e.p("lightness", t));
        op.params[4] = float(e.p("vibrance", t));
    } else if (e.type == "chroma_key") {
        // (as chromaKey: the key's Cb and Cr, its strongest channel and the distance's scale worked out here)
        op.kind = gpu::ColorOp::ChromaKey;
        const float kr = float(e.p("key.r", t, 0)), kg = float(e.p("key.g", t, 1)), kb = float(e.p("key.b", t, 0));
        const float ky = luma(kr, kg, kb), kcb = (kb - ky) / 1.8556f, kcr = (kr - ky) / 1.5748f;
        op.params[0] = kcb, op.params[1] = kcr;
        op.params[2] = std::max(0.05f, std::sqrt(kcb * kcb + kcr * kcr) * 2.0f);
        op.params[3] = std::max(0.0f, float(e.p("tolerance", t, 0.25)) + float(e.p("choke", t, 0)) * 0.2f);
        op.params[4] = std::max(0.001f, float(e.p("softness", t, 0.1)));
        op.params[5] = float(e.p("spill", t, 0.6));
        op.params[6] = float((kg >= kr && kg >= kb) ? 1 : (kb >= kr ? 2 : 0));
        op.params[7] = e.p("show_matte", t) > 0.5 ? 1 : 0;
    } else if (e.type == "luma_key") {
        op.kind = gpu::ColorOp::LumaKey;
        op.params[0] = float(e.p("threshold", t, 0.1)), op.params[1] = float(e.p("softness", t, 0.05));
        op.params[2] = e.p("invert", t) > 0.5 ? 1 : 0;
    } else if (e.type == "screen_key") {
        const int plateMode = int(std::lround(e.p("clean_plate", t, 0)));
        if (plateMode == 1) return false;  // (a clean plate made from the picture itself: on the CPU, which has it)
        // (as screenKey, the matte shrunk or grown and softened by passes of its own when asked)
        const double shrink = e.p("shrink", t, 0) * pixelScale, soften = e.p("soften", t, 0) * pixelScale;
        op.kind = gpu::ColorOp::ScreenKey;
        const float sk[3] = {float(e.p("key.r", t, 0.1)), float(e.p("key.g", t, 0.75)), float(e.p("key.b", t, 0.2))};
        const int dom = (sk[1] >= sk[0] && sk[1] >= sk[2]) ? 1 : (sk[2] >= sk[0] ? 2 : 0);
        const float balance = float(std::clamp(e.p("balance", t, 0.5), 0.0, 1.0));
        const float a = sk[(dom + 1) % 3], b = sk[(dom + 2) % 3];
        op.params[0] = float(dom), op.params[1] = balance, op.params[2] = float(std::max(0.05, e.p("gain", t, 1.1)));
        op.params[3] = std::max(1e-3f, sk[dom] - (balance * std::max(a, b) + (1 - balance) * std::min(a, b)));
        op.params[4] = float(e.p("clip_black", t, 0.05));
        op.params[5] = float(std::max(e.p("clip_white", t, 1), e.p("clip_black", t, 0.05) + 0.01));
        op.params[6] = float(e.p("despill", t, 100) / 100), op.params[7] = float(e.p("restore", t, 50) / 100);
        op.params[8] = float(e.p("edge_desat", t, 0) / 100), op.params[9] = float(std::lround(e.p("view", t, 0)));
        op.params[10] = float(std::lround(shrink)), op.params[11] = float(std::lround(soften / 2));
        // A plate track's picture as keyerPlate makes it (resized, in the effect's colours, blurred), on the CPU while the
        // clip's layer is set up (render/Compositor's cleanPlateFor), where each pixel's screen colour is read.
        if (plateMode == 2 && width > 0 && height > 0) {
            Image size;
            size.width = width, size.height = height;
            Image plate = keyerPlate(e, t, size, pixelScale);
            if (!plate.empty() && plate.width == width && plate.height == height) {
                op.plate.assign(plate.px.begin(), plate.px.end());
                op.params[12] = 1;
                op.params[13] = std::max(1e-3f, float(std::clamp(e.p("plate_threshold", t, 50), 1.0, 100.0) / 100) * op.params[3]);
            }
        }
        for (int k = 0; k < 3; ++k) op.params[16 + k] = sk[k];
    } else if (e.type == "track_matte" || e.type == "displace_by_layer") {
        return true;  // (the compositor applies them, render/Compositor)
    } else if (isGpuFilter(e.type)) {
        // gpufilter.frag's passes, as the effect runs on the CPU; through its mask, marked before and mixed after.
        gpu::ColorOp f;
        f.kind = gpu::ColorOp::Filter;
        auto add = [&](int mode, std::initializer_list<float> params) {
            gpu::ColorOp::FilterPass fp;
            fp.mode = mode;
            std::copy(params.begin(), params.end(), fp.params);
            f.passes.push_back(fp);
        };
        // gaussianBlur's box passes along rows, then columns, in the channels given.
        auto blur = [&](double radius, bool rows, bool columns, const float (&channels)[4]) {
            const std::vector<int> radii = gaussianBoxRadii(radius);
            for (int dim = 0; dim < 2; ++dim)
                if (dim ? columns : rows)
                    for (int r : radii) add(1, {float(r), float(dim), 0, 0, channels[0], channels[1], channels[2], channels[3]});
        };
        const float all[4] = {1, 1, 1, 1};
        const std::string& ty = e.type;
        if (ty == "gaussian_blur") {
            const int dir = int(e.p("direction", t));
            blur(e.p("radius", t, 10) * pixelScale, dir != 2, dir != 1, all);
        } else if (ty == "sharpen") {
            blur(e.p("radius", t, 1.5) * pixelScale, true, true, all);
            add(3, {float(e.p("amount", t, 1))});
        } else if (ty == "glow") {
            const double radius = e.p("radius", t, 20) * pixelScale;
            add(4, {float(e.p("threshold", t, 0.7))});
            if (radius > 0.5) blur(radius, true, true, all);
            add(5, {float(e.p("intensity", t, 1))});
        } else if (ty == "channel_blur") {
            const char* names[4] = {"red", "green", "blue", "alpha"};
            const int dims = int(std::lround(e.p("dimensions", t, 0)));
            for (int c = 0; c < 4; ++c) {
                const double r = std::max(0.0, e.p(names[c], t, 0)) * pixelScale;
                if (r < 0.05) continue;
                float only[4] = {0, 0, 0, 0};
                only[c] = 1;
                blur(r, dims != 2, dims != 1, only);
            }
        } else if (ty == "directional_blur") {
            const double length = e.p("length", t, 20) * pixelScale;
            if (length >= 0.5) {
                const double ang = e.p("angle", t, 0) * M_PI / 180;
                add(2, {float(length), float(std::clamp(int(std::ceil(length)), 2, 96)), float(std::cos(ang)), float(std::sin(ang))});
            }
        } else if (ty == "radial_blur") {
            sfx::RadialBlur rb;
            if (sfx::radialBlurSetup(e, t, width, height, rb)) add(39, {rb.cx, rb.cy, rb.zoom, rb.spin});
        } else if (ty == "vignette") {
            add(6, {float(e.p("amount", t, 0.5)), float(e.p("size", t, 0.75)), float(e.p("softness", t, 0.5))});
        } else if (ty == "posterize") {
            add(7, {float(std::clamp(e.p("levels", t, 6), 2.0, 64.0)) - 1});
        } else if (ty == "letterbox") {
            static const double ratios[] = {2.39, 2.0, 1.85, 4.0 / 3.0, 1.0, 9.0 / 16.0};
            const int choice = std::clamp(int(e.p("aspect", t, 0)), 0, int(std::size(ratios)) - 1);
            add(8, {float(ratios[choice]), float(std::clamp(e.p("opacity", t, 100) / 100.0, 0.0, 1.0))});
        } else if (ty == "mirror") {
            add(9, {float(int(e.p("mode", t)))});
        } else if (ty == "mosaic") {
            if (const int size = std::max(1, int(std::lround(e.p("size", t, 16) * pixelScale))); size > 1) {
                add(10, {float(size), 0});
                add(10, {float(size), 1});
            }
        } else if (ty == "chromatic_aberration") {
            if (const double amount = e.p("amount", t, 3) * pixelScale; std::fabs(amount) >= 0.01) add(11, {float(amount)});
        } else if (ty == "noise") {
            const float amount = float(std::clamp(e.p("amount", t, 20) / 100.0, 0.0, 1.0));
            const uint32_t frame = uint32_t(t);
            if (amount > 0)
                add(12, {amount, e.p("color", t, 1) >= 0.5 ? 1.0f : 0.0f, e.p("clip", t, 1) >= 0.5 ? 1.0f : 0.0f, 0, float(frame & 0xffffu),
                         float(frame >> 16)});
        } else if (ty == "film_grain") {
            // (its noise grid at the grain's size, new every frame, made in the shader by the CPU's hash)
            const double size = std::max(0.5, e.p("size", t, 1.5) * pixelScale);
            const int gw = std::max(1, int(std::ceil(width / size))), gh = std::max(1, int(std::ceil(height / size)));
            const uint32_t seed = vfx::grainSeed(t);
            add(18, {float(e.p("amount", t, 0.15)), e.p("color", t) > 0.5 ? 1.0f : 0.0f, float(gw), float(gh),
                     float(double(gw) / std::max(1, width)), float(double(gh) / std::max(1, height)), float(seed & 0xffffu), float(seed >> 16)});
        } else if (ty == "edge_extend") {
            // fillHoles's pyramid (render/CleanPlate): the solid colours known, pulled up a level at a time (each kept),
            // pushed back down and relaxed on each level (red then black), then mixed into the edges.
            const float amount = float(std::clamp(e.p("amount", t, 100), 0.0, 100.0) / 100);
            const float solid = float(std::clamp(e.p("solid", t, 95), 1.0, 100.0) / 100);
            if (amount > 0 && width > 0 && height > 0) {
                std::vector<std::pair<int, int>> levels{{width, height}};
                while (levels.back().first > 1 || levels.back().second > 1)
                    levels.push_back({(levels.back().first + 1) / 2, (levels.back().second + 1) / 2});
                const int top = int(levels.size()) - 1;
                auto sized = [&](int level) {
                    f.passes.back().width = levels[size_t(level)].first, f.passes.back().height = levels[size_t(level)].second;
                };
                add(21, {solid});
                f.passes.back().save = 0;
                for (int l = 1; l <= top; ++l) {
                    add(22, {});
                    sized(l);
                    f.passes.back().save = l;
                }
                constexpr int kSweeps = 2;  // (as edgeExtend asks fillHoles for)
                for (int l = top - 1; l >= 0; --l) {
                    add(23, {});
                    sized(l);
                    f.passes.back().extra = l;
                    const int sweeps = l == 0 ? std::max(1, kSweeps / 8) : l == 1 ? std::max(1, kSweeps / 4) : kSweeps;
                    for (int k = 0; k < sweeps; ++k)
                        for (int colour = 0; colour < 2; ++colour) {
                            add(24, {float(colour)});
                            sized(l);
                        }
                }
                add(25, {solid, amount});
                f.passes.back().tableSlot = top;
            }
        } else if (ty == "wave_warp" || ty == "twirl" || ty == "spherize" || ty == "ripple" || ty == "turbulent_displace" ||
                   ty == "motion_tile" || ty == "camera_shake" || ty == "corner_pin") {
            // render/StyleFx's and render/VideoFx's warps: each pixel from where the warp says (gpufilter.frag's mode 26),
            // what is the same for the whole frame worked out here (as the CPU does, before any pixel)
            constexpr double kDeg = M_PI / 180;
            const double W = width, H = height;
            auto centre = [&](double& cx, double& cy) {
                cx = W / 2.0 + e.p("center_x", t) * pixelScale;
                cy = H / 2.0 + e.p("center_y", t) * pixelScale;
            };
            if (width <= 0 || height <= 0) {
                // (the sizes are not known yet: only whether it can be drawn here)
            } else if (ty == "wave_warp") {
                const double waveHeight = e.p("height", t, 10) * pixelScale, waveWidth = std::max(1.0, e.p("width", t, 40) * pixelScale);
                if (std::fabs(waveHeight) >= 1e-3) {
                    const double dir = e.p("direction", t, 0) * kDeg;
                    const double phase = std::fmod(e.p("phase", t, 0) * kDeg + double(t) * e.p("speed", t, 0) * kDeg, 2 * M_PI);
                    add(26, {0, float(std::cos(dir)), float(std::sin(dir)), float(waveHeight), float(waveWidth), float(phase),
                             float(std::lround(e.p("shape", t, 0)))});
                }
            } else if (ty == "twirl") {
                const double angle = e.p("angle", t, 90) * kDeg;
                if (std::fabs(angle) >= 1e-6) {
                    double cx, cy;
                    centre(cx, cy);
                    const double R = std::max(1.0, e.p("radius", t, 50) / 100 * std::hypot(W, H) / 2);
                    add(26, {1, 0, 0, 0, float(cx), float(cy), float(R), float(angle)});
                }
            } else if (ty == "spherize") {
                const double amount = std::clamp(e.p("amount", t, 50) / 100, -1.0, 1.0);
                if (std::fabs(amount) >= 1e-4) {
                    double cx, cy;
                    centre(cx, cy);
                    const double R = std::max(1.0, e.p("radius", t, 50) / 100 * std::min(W, H) / 2.0);
                    const double power = amount > 0 ? 1 + amount * 0.9 : 1 / (1 - amount * 0.9);
                    add(26, {2, 0, 0, 0, float(cx), float(cy), float(R), float(power)});
                }
            } else if (ty == "ripple") {
                const double amp = e.p("amplitude", t, 8) * pixelScale, lambda = std::max(1.0, e.p("wavelength", t, 40) * pixelScale);
                if (std::fabs(amp) >= 1e-3) {
                    double cx, cy;
                    centre(cx, cy);
                    const double R = std::max(1.0, e.p("radius", t, 100) / 100 * std::hypot(W, H) / 2);
                    const double phase = std::fmod(double(t) * e.p("speed", t, 20) * kDeg, 2 * M_PI);
                    add(26, {3, 0, 0, 0, float(cx), float(cy), float(R), float(amp), float(lambda), float(phase)});
                }
            } else if (ty == "turbulent_displace") {
                const double amount = e.p("amount", t, 20) * pixelScale, size = std::max(1.0, e.p("size", t, 100) * pixelScale);
                if (std::fabs(amount) >= 1e-3) {
                    const int octaves = std::clamp(int(std::lround(e.p("complexity", t, 3))), 1, 8);
                    const double z = (e.p("evolution", t, 0) + double(t) * e.p("speed", t, 0)) / 360;
                    const uint32_t seed = uint32_t(std::lround(e.p("seed", t, 0)));
                    const uint32_t sx = seed * 2 + 1, sy = seed * 2 + 2;
                    add(26, {4, 0, 0, 0, float(size), float(amount), float(z), float(octaves), float(sx & 0xffffu), float(sx >> 16),
                             float(sy & 0xffffu), float(sy >> 16)});
                }
            } else if (ty == "motion_tile") {
                const double tw = std::clamp(e.p("tile_width", t, 100), 1.0, 100.0) / 100, th = std::clamp(e.p("tile_height", t, 100), 1.0, 100.0) / 100;
                const double ox = e.p("offset_x", t) * pixelScale, oy = e.p("offset_y", t) * pixelScale;
                if (!(tw == 1 && th == 1 && ox == 0 && oy == 0))
                    add(26, {5, 0, 0, 0, float(ox), float(oy), float(W * tw), float(H * th), e.p("mirror", t) > 0.5 ? 1.0f : 0.0f});
            } else if (ty == "camera_shake") {
                sfx::ShakeMove m;
                if (sfx::cameraShakeMove(e, t, width, height, pixelScale, m))
                    add(26, {6, 0, 0, 0, float(W / 2), float(H / 2), float(m.ox), float(m.oy), float(m.zoom), float(std::cos(-m.angle)),
                             float(std::sin(-m.angle))});
            } else {
                double q[4][2] = {{e.p("tl_x", t, 0) * W, e.p("tl_y", t, 0) * H},
                                  {e.p("tr_x", t, 1) * W, e.p("tr_y", t, 0) * H},
                                  {e.p("br_x", t, 1) * W, e.p("br_y", t, 1) * H},
                                  {e.p("bl_x", t, 0) * W, e.p("bl_y", t, 1) * H}};
                double m[9];
                if (vfx::squareToQuad(q, m)) {
                    const double inv[9] = {m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
                                           m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
                                           m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3]};
                    // (scaled so the largest is 1: the same map, within a float's range)
                    double big = 0;
                    for (double v : inv) big = std::max(big, std::fabs(v));
                    if (big > 0) {
                        add(26, {7});
                        for (int r = 0; r < 3; ++r)
                            for (int c = 0; c < 3; ++c) f.passes.back().params[4 + r * 4 + c] = float(inv[r * 3 + c] / big);
                    }
                }
            }
        } else if (ty == "mesh_warp" || ty == "grid_warp") {
            // drawWarp's triangles (render/MeshWarp: the mesh solved on the CPU, as there), listed by the 8-pixel tiles
            // they may reach, in the order drawn (gpufilter.frag's mode 33)
            WarpMesh mesh;
            std::vector<MeshPoint> moved;
            std::vector<double> depth;
            const int how = warpDrawing(e, t, width, height, pixelScale, mesh, moved, depth);
            if (how < 0) return false;
            const std::vector<WarpTriangle> tris = how > 0 ? warpTriangles(mesh, moved, depth, width, height) : std::vector<WarpTriangle>{};
            if (how > 0) {
                constexpr int kTile = 8;
                const int across = (width + kTile - 1) / kTile, down = (height + kTile - 1) / kTile;
                std::vector<std::vector<int>> lists(size_t(across) * size_t(down));
                for (size_t i = 0; i < tris.size(); ++i)
                    for (int ty0 = tris[i].y0 / kTile; ty0 <= tris[i].y1 / kTile; ++ty0)
                        for (int tx0 = tris[i].x0 / kTile; tx0 <= tris[i].x1 / kTile; ++tx0) lists[size_t(ty0) * size_t(across) + size_t(tx0)].push_back(int(i));
                size_t entries = 0;
                for (const auto& l : lists) entries += l.size();
                const size_t start = lists.size() + entries, texels = start + tris.size() * 6;
                add(33, {float(kTile), float(across), float(start)});
                gpu::ColorOp::FilterPass& fp = f.passes.back();
                fp.tableWidth = int(std::min<size_t>(4096, std::max<size_t>(1, texels)));
                const size_t rows = (texels + size_t(fp.tableWidth) - 1) / size_t(fp.tableWidth);
                fp.table.assign(rows * size_t(fp.tableWidth) * 4, 0.0f);
                float* tb = fp.table.data();
                size_t at = lists.size();
                for (size_t k = 0; k < lists.size(); ++k) {
                    tb[k * 4] = float(at), tb[k * 4 + 1] = float(lists[k].size());
                    for (int i : lists[k]) tb[at++ * 4] = float(i);
                }
                for (size_t i = 0; i < tris.size(); ++i) {
                    const WarpTriangle& d = tris[i];
                    float* q = tb + (start + i * 6) * 4;
                    for (int k = 0; k < 3; ++k) {
                        q[k * 4] = float(d.A[k]), q[k * 4 + 1] = float(d.B[k]);
                        q[k * 4 + 2] = float(d.P[k][0]), q[k * 4 + 3] = float(d.P[k][1]);
                    }
                    q[12] = float(d.at[0]), q[13] = float(d.at[1]), q[14] = float(d.at[2]);
                    q[15] = float((d.own[0] ? 1 : 0) | (d.own[1] ? 2 : 0) | (d.own[2] ? 4 : 0));
                    q[16] = float(d.r[0][0]), q[17] = float(d.r[0][1]), q[18] = float(d.r[1][0]), q[19] = float(d.r[1][1]);
                    q[20] = float(d.r[2][0]), q[21] = float(d.r[2][1]);
                }
            }
        } else if (ty == "depth_blur") {
            // lensBlur's plan (render/LensBlur), from the frame's depth, as passes: the state S (what is laid down, in
            // light, beside its coverage) twice the layer's width, each layer laid down sharp, or what is behind it
            // filled through a pyramid and the layer gathered at 1/f through the aperture's taps and laid over S. (The
            // nearest face's depth needs the faces, found on the CPU.)
            if (std::lround(e.p("follow", t, 0)) == 2) return false;
            const DepthMap* depth = currentDepth();
            if (width > 0 && height > 0 && depth && !depth->empty()) {
                const std::vector<float> d = depth->resized(width, height);
                const LensBlurSettings ls = depthBlurSettings(e, t, d, width, height, pixelScale);
                const ColorSpace& space = currentWorkingSpace();
                const bool light = space.transfer != Transfer::Linear;
                const int tf = light ? gpuTransfer(space) : 0;
                if (light && tf < 0) return false;  // (a curve the shader does not have)
                std::unique_ptr<TransferTables> tables;
                if (light) tables = std::make_unique<TransferTables>(space.transfer);
                const LensBlurPlan plan = lensBlurPlan(d, width, height, ls, tables.get());
                const int W = width, H = height;
                enum { kLB = 1, kS = 2, kLevels = 4 };
                auto pass = [&](int mode, int w, int h, int save, int extra, int tableSlot) -> gpu::ColorOp::FilterPass& {
                    add(mode, {});
                    gpu::ColorOp::FilterPass& fp = f.passes.back();
                    fp.width = w, fp.height = h, fp.save = save, fp.extra = extra, fp.tableSlot = tableSlot;
                    return fp;
                };
                auto set = [](gpu::ColorOp::FilterPass& fp, std::initializer_list<float> v) { std::copy(v.begin(), v.end(), fp.params); };
                if (!plan.none) {
                    {
                        gpu::ColorOp::FilterPass& fp = pass(58, 2 * W, H, kLB, -1, -1);
                        set(fp, {light ? 1.0f : 0.0f, float(plan.liftThreshold), float(plan.lift), float(W), float(tf)});
                        fp.tableWidth = std::max(TransferTables::kDecode + 1, W);
                        fp.table.assign(size_t(fp.tableWidth) * 4 * size_t(H + 1), 0.0f);
                        if (tables) {
                            for (size_t i = 0; i < tables->dec.size(); ++i) fp.table[i * 4] = tables->dec[i];
                            for (size_t i = 0; i < tables->enc.size(); ++i) fp.table[i * 4 + 1] = tables->enc[i];
                        }
                        for (int y = 0; y < H; ++y)
                            for (int x = 0; x < W; ++x) {
                                const size_t i = size_t(y) * size_t(W) + size_t(x), o = (size_t(y + 1) * size_t(fp.tableWidth) + size_t(x)) * 4;
                                fp.table[o] = float(plan.bin0[i]), fp.table[o + 1] = plan.share[i];
                            }
                    }
                    set(pass(59, 2 * W, H, kS, -1, -1), {0, 0, 0, float(W)});
                    for (const LensBlurLayer& L : plan.layers) {
                        if (L.sharp) {
                            set(pass(60, 2 * W, H, kS, kS, kLB), {0, 0, 0, float(W), float(L.k), float(L.bx0), float(L.by0), float(L.bx1), float(L.by1)});
                            continue;
                        }
                        if (L.fill) {
                            // fillBehind's pyramid over the box, pulled to one cell, its means pushed back down
                            std::vector<std::pair<int, int>> levels;
                            int w = L.fx1 - L.fx0 + 1, h = L.fy1 - L.fy0 + 1;
                            while (w > 1 || h > 1) {
                                const int nw = (w + 1) / 2, nh = (h + 1) / 2;
                                const bool fromS = levels.empty();
                                set(pass(61, 2 * nw, nh, kLevels + int(levels.size()), fromS ? kS : -1, -1),
                                    {0, 0, 0, float(nw), fromS ? 1.0f : 0.0f, fromS ? float(L.fx0) : 0.0f, fromS ? float(L.fy0) : 0.0f, float(w), float(h),
                                     fromS ? float(W) : float(levels.back().first)});
                                levels.push_back({nw, nh});
                                w = nw, h = nh;
                            }
                            if (!levels.empty()) {
                                for (size_t lv = levels.size(); lv-- > 0;)
                                    set(pass(63, levels[lv].first, levels[lv].second, -1, kLevels + int(lv), -1),
                                        {0, 0, 0, float(levels[lv].first), lv + 1 < levels.size() ? 1.0f : 0.0f});
                                set(pass(64, 2 * W, H, kS, kS, kLevels + int(levels.size()) - 1),
                                    {0, 0, 0, float(W), float(L.fx0), float(L.fy0), float(L.fx1), float(L.fy1)});
                            }
                        }
                        if (L.taps.empty()) continue;
                        set(pass(65, 2 * L.lw, L.lh, -1, kLB, -1),
                            {0, 0, 0, float(L.lw), float(L.k), float(L.x0), float(L.y0), float(L.x1), float(L.y1), float(L.f),
                             1.0f / float(L.f * L.f), float(W)});
                        {
                            gpu::ColorOp::FilterPass& fp = pass(66, 2 * L.lw, L.lh, -1, -1, -1);
                            const double cx = W / 2.0, cy = H / 2.0;
                            set(fp, {0, 0, 0, float(L.lw), float(L.taps.size()), float(L.lh), float(L.rr), float(ls.aperture.catEye),
                                     float(std::max(1.0, ls.aperture.squeeze)), float(L.x0), float(L.y0), float(L.f), float(cx), float(cy),
                                     float(std::hypot(cx, cy)), 0, L.x0 == 0 ? 1.0f : 0.0f, L.x1 == W - 1 ? 1.0f : 0.0f, L.y0 == 0 ? 1.0f : 0.0f,
                                     L.y1 == H - 1 ? 1.0f : 0.0f});
                            fp.tableWidth = 1024;
                            fp.table.assign(((L.taps.size() * 2 + 1023) / 1024) * 1024 * 4, 0.0f);
                            for (size_t i = 0; i < L.taps.size(); ++i) {
                                const LensBlurTap& tp = L.taps[i];
                                const float v[8] = {float(tp.dx), float(tp.dy), float(tp.kx), float(tp.ky), tp.w, 0, 0, 0};
                                std::copy(v, v + 8, &fp.table[i * 8]);
                            }
                        }
                        set(pass(67, 2 * W, H, kS, kS, -1),
                            {0, 0, 0, float(W), float(L.x0), float(L.y0), float(L.x1), float(L.y1), float(L.f), float(L.lw), float(L.lh)});
                    }
                    gpu::ColorOp::FilterPass& fp = pass(68, 0, 0, -1, kS, -1);
                    set(fp, {light ? 1.0f : 0.0f, float(plan.liftThreshold), float(plan.lift), float(W), float(tf)});
                    if (tables) transferTableRows(fp, space, 1);
                }
                // Focus breathing: a little larger focused near (up to 4 %), as depthEffect does it.
                const double breathing = std::clamp(e.p("breathing", t, 0) / 100, 0.0, 1.0);
                if (const double scale = 1 + 0.04 * breathing * ls.focus; breathing > 0 && std::fabs(scale - 1) >= 1e-4)
                    set(pass(69, 0, 0, -1, -1, -1), {0, 0, 0, float(W), float(scale)});
            }
        } else if (ty == "depth_map" || ty == "depth_fog") {
            // depthEffect's colour and amount at each pixel, from the frame's depth (currentDepth(): nothing without one)
            const DepthMap* depth = currentDepth();
            if (width > 0 && height > 0 && depth && !depth->empty()) {
                const std::vector<float> d = depth->resized(width, height);
                add(34, {});
                gpu::ColorOp::FilterPass& fp = f.passes.back();
                fp.tableWidth = width;
                fp.table.assign(d.size() * 4, 0.0f);
                if (ty == "depth_map") {
                    const bool farWhite = e.p("invert", t) > 0.5;
                    const float mix = float(std::clamp(e.p("mix", t, 100) / 100, 0.0, 1.0));
                    for (size_t i = 0; i < d.size(); ++i) {
                        const float g = farWhite ? 1 - d[i] : d[i];
                        fp.table[i * 4] = fp.table[i * 4 + 1] = fp.table[i * 4 + 2] = g;
                        fp.table[i * 4 + 3] = mix;
                    }
                } else {
                    const double start = std::max(0.01, e.p("start", t, 60) / 100), amount = std::clamp(e.p("amount", t, 70) / 100, 0.0, 1.0);
                    const double curve = std::max(0.05, e.p("curve", t, 1.5));
                    const float col[3] = {float(e.p("color.r", t, 0.78)), float(e.p("color.g", t, 0.82)), float(e.p("color.b", t, 0.88))};
                    const double horizon = std::clamp(e.p("horizon", t, 50) / 100, 0.0, 1.0), lift = std::clamp(e.p("height", t, 0) / 100, 0.0, 1.0) * 8;
                    for (size_t i = 0; i < d.size(); ++i) {
                        const double far = std::clamp((start - d[i]) / start, 0.0, 1.0);
                        const double above = std::max(0.0, horizon - (double(i / size_t(width)) + 0.5) / height);
                        std::copy(col, col + 3, &fp.table[i * 4]);
                        fp.table[i * 4 + 3] = float(amount * std::pow(far, curve) * std::exp(-lift * above));
                    }
                }
            }
        } else if (ty == "find_edges") {
            add(27, {e.p("invert", t, 1) > 0.5 ? 1.0f : 0.0f, float(std::clamp(e.p("blend", t, 0) / 100, 0.0, 1.0))});
        } else if (ty == "emboss") {
            const double dir = e.p("direction", t, 135) * M_PI / 180, relief = std::max(0.25, e.p("relief", t, 2) * pixelScale);
            add(28, {float(std::cos(dir) * relief), float(-std::sin(dir) * relief), float(e.p("contrast", t, 100) / 100) * 2,
                     float(std::clamp(e.p("blend", t, 0) / 100, 0.0, 1.0))});
        } else if (ty == "halftone") {
            constexpr double kDeg = M_PI / 180;
            const double angle = e.p("angle", t, 45) * kDeg;
            const double a0 = angle + 15 * kDeg, a1 = angle + 75 * kDeg;
            add(29, {float(std::max(2.0, e.p("dot_size", t, 8) * pixelScale)), e.p("mode", t, 0) > 0.5 ? 1.0f : 0.0f, 0, 0,
                     float(std::cos(a0)), float(std::sin(a0)), float(std::cos(a1)), float(std::sin(a1)), float(std::cos(angle)),
                     float(std::sin(angle)), float(std::cos(angle)), float(std::sin(angle))});
        } else if (ty == "duotone") {
            add(30, {float(e.p("shadows.r", t, 0.08)), float(e.p("shadows.g", t, 0.1)), float(e.p("shadows.b", t, 0.35)),
                     float(std::clamp(e.p("mix", t, 100) / 100, 0.0, 1.0)), float(e.p("highlights.r", t, 1)), float(e.p("highlights.g", t, 0.85)),
                     float(e.p("highlights.b", t, 0.6))});
        } else if (ty == "vhs") {
            sfx::VhsSetup vs;
            if (width > 0 && height > 0 && sfx::vhsSetup(e, t, width, height, pixelScale, vs)) {
                const float amount = float(std::clamp(e.p("amount", t, 100) / 100, 0.0, 1.0));
                const double bleed = e.p("bleed", t, 4) * pixelScale * amount;
                add(31, {amount, float(bleed), float(e.p("noise", t, 30) / 100) * amount, 0, float(1.2 * pixelScale * amount),
                         float(vs.seed & 0xffffu), float(vs.seed >> 16), float(std::max(1, int(std::ceil(bleed)))),
                         float(std::max(1, int(3 * pixelScale)))});
                gpu::ColorOp::FilterPass& fp = f.passes.back();
                fp.tableWidth = height;
                fp.table.assign(size_t(height) * 4, 0.0f);
                for (int y = 0; y < height; ++y) {
                    fp.table[size_t(y) * 4] = float(vs.shift[size_t(y)]);
                    fp.table[size_t(y) * 4 + 1] = vs.torn[size_t(y)];
                    fp.table[size_t(y) * 4 + 2] = vs.line[size_t(y)];
                }
            }
        } else if (ty == "tilt_shift") {
            // (the Gaussian of the whole picture, then the band kept sharp and the colour pushed, as tiltShift does)
            const double radius = e.p("blur", t, 12) * pixelScale;
            const double focus = e.p("focus", t, 50) / 100, bandWidth = std::max(0.0, e.p("width", t, 20) / 100);
            const double angle = e.p("angle", t, 0) * M_PI / 180;
            const float sat = float(e.p("saturation", t, 120) / 100);
            const bool blurred = radius >= 0.5;
            if (blurred) blur(radius, true, true, all);
            if (blurred || std::fabs(sat - 1) > 1e-4f)
                add(32, {float(width / 2.0), float(focus * height), float(-std::sin(angle)), float(std::cos(angle)), float(height),
                         float(bandWidth / 2), float(bandWidth / 2 + std::max(0.02, bandWidth)), sat, blurred ? 1.0f : 0.0f});
        } else if (ty == "light_rays") {
            // lightRays' light above the threshold, its three passes towards the centre and the sum (gpufilter.frag's
            // modes 35 to 37); with Shadows Block Rays, the small copy of what blocks them (40) and the light let through
            // it from the centre's side (41) between, the sum taking the rays from where they were kept
            const double block = std::clamp(e.p("block", t, 0) / 100, 0.0, 1.0);
            const double W = width, H = height;
            double cx = e.p("pos_x", t, 0.5) * W, cy = e.p("pos_y", t, 0.25) * H;
            if (e.p("follow", t, 0) > 0.5) {
                const MaskBox box = maskBox(e, t);
                cx = box.x * W, cy = box.y * H;
            }
            cx = std::clamp(std::isfinite(cx) ? cx : W / 2.0, -4.0 * W, 5.0 * W), cy = std::clamp(std::isfinite(cy) ? cy : H / 2.0, -4.0 * H, 5.0 * H);
            const double threshold = std::max(0.0, e.p("threshold", t, 0.8)), intensity = std::max(0.0, e.p("intensity", t, 1.0));
            const double length = std::clamp(e.p("length", t, 60) / 100, 0.0, 1.0), decay = std::clamp(e.p("decay", t, 50) / 100, 0.0, 1.0);
            if (intensity > 0 && length > 0) {
                add(35, {float(threshold), 0, 0, 0, float(e.p("color.r", t, 1.0)), float(e.p("color.g", t, 0.95)), float(e.p("color.b", t, 0.85))});
                constexpr int kSamples = 8, kPasses = 3;
                const double first = length / (std::pow(double(kSamples), kPasses) - 1), rate = decay * 6 / length;
                for (int pass = 0; pass < kPasses; ++pass) {
                    const double step = first * std::pow(double(kSamples), pass);
                    double w[kSamples], sum = 0;
                    for (int j = 0; j < kSamples; ++j) sum += w[j] = std::exp(-rate * j * step);
                    add(36, {float(cx), float(cy), float(step), 0, float(w[0] / sum), float(w[1] / sum), float(w[2] / sum), float(w[3] / sum),
                             float(w[4] / sum), float(w[5] / sum), float(w[6] / sum), float(w[7] / sum)});
                }
                if (block > 0 && width > 0 && height > 0) {
                    f.passes.back().save = 0;
                    const sfx::RayShadowGrid grid = sfx::rayShadowGrid(width, height);
                    add(40, {float(std::max(threshold, 1e-3)), float(block)});
                    gpu::ColorOp::FilterPass& occ = f.passes.back();
                    occ.width = grid.tw, occ.height = grid.th;
                    occ.tableWidth = std::max(grid.tw, grid.th);
                    occ.table.assign(size_t(occ.tableWidth) * 4, 0.0f);
                    for (int x = 0; x < grid.tw; ++x) occ.table[size_t(x) * 4] = float(grid.x0[size_t(x)]), occ.table[size_t(x) * 4 + 1] = float(grid.x1[size_t(x)]);
                    for (int y = 0; y < grid.th; ++y) occ.table[size_t(y) * 4 + 2] = float(grid.y0[size_t(y)]), occ.table[size_t(y) * 4 + 3] = float(grid.y1[size_t(y)]);
                    add(41, {float(cx * grid.shrink), float(cy * grid.shrink), float(length), float(160.0 / std::max(grid.tw, grid.th))});
                    f.passes.back().width = grid.tw, f.passes.back().height = grid.th;
                    add(37, {float(intensity), 1});
                    f.passes.back().extra = 0;
                    f.passes.back().width = width, f.passes.back().height = height;
                } else {
                    add(37, {float(intensity)});
                }
            }
        } else if (ty == "painterly") {
            // paintEffect's passes: the structure tensor (49), smoothed as gaussian smooths it (1's boxes or 48's kernel),
            // its flow (50); then the Kuwahara filter (52; a wide brush on the picture and its squares through boxes, 55
            // and 51) or the smear (53), each mixed with the picture as it ends
            PaintSetup ps;
            if (paintSetup(e, t, pixelScale, ps)) {
                const GaussianPlan plan = gaussianPlan(ps.style == 1 ? ps.smear.tensorBlur : ps.kuwahara.tensorBlur);
                add(49, {});
                if (plan.byBoxes) {
                    for (int dim = 0; dim < 2; ++dim)
                        for (int r : plan.boxes) add(1, {float(r), float(dim), 0, 0, 1, 1, 1, 1});
                } else if (!plan.kernel.empty()) {
                    for (int dim = 0; dim < 2; ++dim) {
                        add(48, {float(plan.kernel.size() - 1), float(dim), 0, 0});
                        std::copy(plan.kernel.begin(), plan.kernel.begin() + std::min<size_t>(plan.kernel.size(), 56), f.passes.back().params + 4);
                    }
                }
                add(50, {});
                if (ps.style == 1) {
                    add(53, {float(std::max(1L, std::lround(ps.smear.length))), float(std::max(0.5, ps.smear.length / 2)), float(ps.smear.levels),
                             float(ps.mix)});
                } else {
                    const KuwaharaSettings& k = ps.kuwahara;
                    const int step = std::max(1, int(std::ceil(k.radius / kKuwaharaSamples)));
                    f.passes.back().save = 0;  // (the flow)
                    if (step > 1) {
                        for (int which : {51, 55}) {
                            add(which, {});
                            for (int dim = 0; dim < 2; ++dim) add(1, {float(step / 2), float(dim), 0, 0, 1, 1, 1, 1});
                            if (which == 51) f.passes.back().save = 1;  // (the squares' box)
                        }
                    }
                    add(52, {float(k.radius), float(std::max(0.01, k.anisotropy)), float(std::max(0.0, k.sharpness)), float(step), float(ps.mix)});
                    f.passes.back().extra = 0;
                    if (step > 1) f.passes.back().tableSlot = 1;
                }
            }
        } else if (ty == "aperture_glare") {
            // apertureGlare's convolution by FFTs in tiles (overlap-add), tiled as the CPU tiles it: which tiles have any
            // light (70 and 71, read back: those with none passed over, as the CPU passes over them); the glare cleared
            // into slot 0 (45); each lit tile's light above the threshold, shrunk (42), transformed along rows then
            // columns (43, Stockham radix 2, a pass a stage), through the kernel (44, its spectra kept on the GPU while
            // the kernel lives), back again (43, inverse) and added into the glare (46); then the glare drawn back up and
            // added to the picture (47)
            sfx::GlareSetup g;
            const bool glare = sfx::apertureGlareSetup(e, t, width, height, pixelScale, g);
            // (a glare whose transforms would be 4096 square or more, 256 MB a pass, is left to the CPU)
            if (glare && g.M > 2048) return false;
            if (glare) {
                const int M = g.M, across = (g.GW + g.T - 1) / g.T, down = (g.GH + g.T - 1) / g.T;
                auto sized = [&](int w, int h) {
                    f.passes.back().width = w, f.passes.back().height = h;
                    return &f.passes.back();
                };
                const float lit[7] = {float(g.threshold), float(g.f), 0, 0, float(g.T), float(g.GW), float(g.GH)};
                add(70, {lit[0], lit[1], lit[2], lit[3], lit[4], lit[5], lit[6]});
                sized(across, g.GH);
                add(71, {lit[0], lit[1], lit[2], lit[3], lit[4], lit[5], lit[6]});
                sized(across, down)->flags = true;
                add(45, {});
                sized(g.GW, g.GH)->save = 0;
                int tile = 0;
                auto transform = [&](bool inverse) {
                    for (int dir = 0; dir < 2; ++dir)
                        for (int ns = 1; ns < M; ns *= 2) {
                            add(43, {float(M), float(ns), float(dir), inverse ? 1.0f : 0.0f});
                            gpu::ColorOp::FilterPass* fp = sized(M, M);
                            fp->sharedTable = g.twiddles, fp->tableWidth = M / 2, fp->when = tile;
                        }
                };
                for (int row = 0; row < g.GH; row += g.T)
                    for (int tx = 0; tx < g.GW; tx += g.T, ++tile) {
                        const int tw = std::min(g.T, g.GW - tx), th = std::min(g.T, g.GH - row);
                        add(42, {float(g.threshold), float(g.f), float(tx), float(row), float(tw), float(th)});
                        sized(M, M)->when = tile;
                        transform(false);
                        add(44, {float(M)});
                        gpu::ColorOp::FilterPass* kp = sized(M, M);
                        kp->sharedTable = g.spectra, kp->tableWidth = M, kp->when = tile;
                        transform(true);
                        add(46, {float(tx), float(row), float(tw), float(th), float(g.radius), float(M), 1.0f / (float(M) * float(M)), 1.0f});
                        gpu::ColorOp::FilterPass* acc = sized(g.GW, g.GH);
                        acc->save = 0, acc->extra = 0, acc->when = tile;
                    }
                add(47, {float(g.f), float(g.intensity)});
                sized(width, height)->extra = 0;
            }
        } else if (ty == "lens_flare") {
            // lensFlare's light, star, ring and ghosts at each pixel (gpufilter.frag's mode 38), the ghosts in the table
            sfx::FlareGhosts g;
            const int lens = std::clamp(int(e.p("lens", t, 0)), 0, 3);
            const double bright = std::max(0.0, e.p("brightness", t, 100) / 100);
            if (bright > 0 && sfx::lensFlareGhosts(lens, g)) {
                add(38, {float(e.p("pos_x", t, 0.3) * width), float(e.p("pos_y", t, 0.3) * height), float(std::hypot(width, height) / 2),
                         float(lens), float(width / 2.0), float(height / 2.0), float(bright),
                         float(std::max(0.0, e.p("ghosts", t, 100) / 100)), float(e.p("color.r", t, 1.0)), float(e.p("color.g", t, 0.9)),
                         float(e.p("color.b", t, 0.75)), float(lens == 1 ? 6 : lens == 2 ? 8 : 4)});
                gpu::ColorOp::FilterPass& fp = f.passes.back();
                fp.tableWidth = 2 * g.count;
                fp.table.assign(size_t(fp.tableWidth) * 4, 0.0f);
                for (int k = 0; k < g.count; ++k) {
                    float* q = &fp.table[size_t(k) * 8];
                    q[0] = float(g.at[k]), q[1] = float(g.radius[k]), q[2] = float(g.strength[k]);
                    std::copy(g.rgb[k], g.rgb[k] + 3, q + 4);
                }
            }
        } else if (ty == "rgb_mixer") {
            float m[12] = {};
            bool identity = true;
            for (int o = 0; o < 3; ++o)
                for (int i = 0; i < 3; ++i) {
                    const std::string name = std::string(1, "rgb"[o]) + "." + "rgb"[i];
                    m[o * 4 + i] = float(e.p(name, t, o == i ? 1 : 0));
                    identity = identity && m[o * 4 + i] == (o == i ? 1.0f : 0.0f);
                }
            if (!identity) add(19, {0, 0, 0, 0, m[0], m[1], m[2], 0, m[4], m[5], m[6], 0, m[8], m[9], m[10]});
        } else if (ty == "color_warper") {
            ColorWarp w;
            if (parseColorWarp(e.s("mesh"), w) && !w.empty()) {
                const WarpField field(w);
                add(16, {float(std::clamp(e.p("mix", t, 100) / 100.0, 0.0, 1.0)), e.p("preserve_luma", t) > 0.5 ? 1.0f : 0.0f});
                gpu::ColorOp::FilterPass& fp = f.passes.back();
                fp.tableWidth = kWarpSpokes * (kWarpRings + 1);
                fp.table.assign(size_t(fp.tableWidth) * 4, 0.0f);
                for (int i = 0; i < kWarpSpokes; ++i)
                    for (int j = 0; j <= kWarpRings; ++j) {
                        float* d = &fp.table[size_t(i * (kWarpRings + 1) + j) * 4];
                        d[0] = float(field.dh[i][j]), d[1] = float(field.ds[i][j]), d[2] = float(field.dl[i][j]);
                    }
            }
        } else {
            // The colour tools on the working space's light, through its transfer function.
            const ColorSpace& space = currentWorkingSpace();
            const int tf = gpuTransfer(space);
            if (tf < 0) return false;  // (a curve the shader does not have)
            gpu::ColorOp::FilterPass fp;
            auto rows3 = [&](int at, const double m[9]) {
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c) fp.params[(at - 1) * 4 + r * 4 + c] = float(m[r * 3 + c]);
            };
            fp.params[0] = float(tf);
            if (ty == "crosstalk") {
                double m[9];
                if (crosstalkMatrix(e, t, space, m)) {
                    fp.mode = 13;
                    rows3(2, m);
                }
            } else if (ty == "colour_correlation") {
                const CorrelationSettings cs = correlationSettings(e, t, space);
                fp.mode = 14;
                fp.params[1] = float(cs.saturation), fp.params[2] = float(cs.start);
                fp.params[4] = float(cs.low), fp.params[5] = float(cs.high), fp.params[6] = float(cs.soft);
                rows3(3, cs.toBoundary);
                rows3(6, cs.fromBoundary);
                for (int k = 0; k < 3; ++k) fp.params[32 + k] = float(cs.luma[k]);
            } else if (ty == "hdr_palette") {
                const HdrPaletteSetup h = hdrPaletteSetup(e, t, space);
                fp.mode = 15;
                fp.params[1] = h.toWhite, fp.params[2] = h.blackOffset, fp.params[3] = h.mix;
                fp.params[4] = h.kr, fp.params[5] = h.kg, fp.params[6] = h.kb;
                transferTableRows(fp, space, 3);
                const size_t row = size_t(fp.tableWidth) * 4;
                for (int i = 0; i <= HdrPaletteSetup::kSteps; ++i) {
                    float* d = &fp.table[row + size_t(i) * 4];
                    d[0] = h.evOut[size_t(i)], d[1] = h.sat[size_t(i)], d[2] = h.bal[0][size_t(i)], d[3] = h.bal[1][size_t(i)];
                    fp.table[2 * row + size_t(i) * 4] = h.bal[2][size_t(i)];
                }
            } else if (ty == "color_slice") {
                const ColorSliceSetup cs = colorSliceSetup(e, t);
                if (cs.any) {
                    fp.mode = 17;
                    fp.params[1] = space.transfer == Transfer::Hlg ? float(1 / toLinear(Transfer::Hlg, 0.75)) : 1.0f;
                    fp.params[2] = float(cs.vecs.size()), fp.params[3] = cs.mix;
                    fp.params[4] = cs.globalSat, fp.params[5] = cs.globalDensity, fp.params[6] = cs.depth, fp.params[7] = cs.protect;
                    fp.params[8] = cs.subtractive ? 1.0f : 0.0f, fp.params[9] = space.primaries != Primaries::Bt709 ? 1.0f : 0.0f;
                    double toRef[9], fromRef[9];
                    primariesMatrix(space.primaries, Primaries::Bt709, toRef);
                    primariesMatrix(Primaries::Bt709, space.primaries, fromRef);
                    rows3(4, toRef);
                    rows3(7, fromRef);
                    const float skin[5] = {cs.skin.centre, cs.skin.width, cs.skin.hue, cs.skin.sat, cs.skin.density};
                    std::copy(skin, skin + 5, fp.params + 36);
                    transferTableRows(fp, space, 3);
                    const size_t row = size_t(fp.tableWidth) * 4;
                    for (size_t i = 0; i < cs.vecs.size(); ++i) {
                        const ColorSliceSetup::Vec& v = cs.vecs[i];
                        const float d[4] = {v.centre, v.width, v.hue, v.sat};
                        std::copy(d, d + 4, &fp.table[row + i * 4]);
                        fp.table[2 * row + i * 4] = v.density;
                    }
                }
            } else if (ty == "grain_match") {
                // (its grain's fields made, and kept, on the CPU as addGrain makes them; added to the light here)
                const GrainProfile g = effectGrainProfile(e);
                GrainFields gf;
                if (grainFields(width, height, g, grainMatchSettings(e, t), t, gf)) {
                    fp.mode = 20;
                    fp.params[1] = float(gf.amount), fp.params[2] = float(gf.power), fp.params[3] = float(gf.shapeScale);
                    rows3(2, &gf.L[0][0]);
                    int model = 0;  // (by channel: the amplitudes a model's, following the light)
                    for (int c = 0; c < 3; ++c)
                        if (g.response > 0 && std::all_of(g.amplitude[c], g.amplitude[c] + GrainProfile::kBands, [](double a) { return a > 0; }))
                            model |= 1 << c;
                    const double pqTop = toLinear(Transfer::Pq, 1), pqSlope = (pqTop - toLinear(Transfer::Pq, 1 - 1e-4)) / 1e-4;
                    fp.params[16] = float(g.response), fp.params[17] = float(model);
                    fp.params[18] = float(pqTop), fp.params[19] = float(pqSlope);
                    for (int c = 0; c < 3; ++c)
                        for (int b = 0; b < GrainProfile::kBands; ++b) fp.params[20 + c * 8 + b] = float(g.amplitude[c][b]);
                    fp.table = std::move(gf.z);
                    fp.tableWidth = width;
                }
            } else {
                return false;  // (a filter without its passes here: the CPU draws it)
            }
            if (fp.mode) f.passes.push_back(std::move(fp));
        }
        if (f.passes.empty()) return true;  // (nothing changes)
        // (rays from the mask's track take it as where they come from, not as a limit, as applyVideoEffect does)
        if (!op.masked || (ty == "light_rays" && e.p("follow", t, 0) > 0.5)) {
            out.push_back(std::move(f));
            return true;
        }
        gpu::ColorOp mark;
        mark.kind = gpu::ColorOp::Mark;
        out.push_back(std::move(mark));
        out.push_back(std::move(f));
        op.kind = gpu::ColorOp::Mix;
        gpuMask(e, t, op, width, height, pixelScale);
        out.push_back(std::move(op));
        return true;
    } else if (e.type == "levels") {
        op.kind = gpu::ColorOp::Levels;
        const float inB = float(e.p("in_black", t, 0)), inW = float(e.p("in_white", t, 1));
        op.params[0] = inB, op.params[1] = std::max(1e-4f, inW - inB);
        op.params[2] = 1.f / float(std::max(0.05, e.p("gamma", t, 1)));
        op.params[4] = float(e.p("out_black", t, 0)), op.params[5] = float(e.p("out_white", t, 1));
    } else {
        return false;
    }
    if (op.masked) gpuMask(e, t, op, width, height, pixelScale);  // (now with its shape drawn)
    out.push_back(std::move(op));
    return true;
}

std::function<void(float[3])> lutTransform(const std::string& path, std::string* error) { return lutFileTransform(path, error); }

const ColorSpace* effectApplySpace(const Effect& e) { return applyInSpace(e); }

}  // namespace montage
