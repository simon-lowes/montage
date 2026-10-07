#include "ColorSpace.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>

#include "Processing.h"

namespace montage {

namespace {

constexpr double kSdrWhiteNits = 203;  // BT.2408 reference white for HDR

// ---- PQ (SMPTE ST 2084) ----
constexpr double kM1 = 2610.0 / 16384, kM2 = 2523.0 / 4096 * 128, kC1 = 3424.0 / 4096, kC2 = 2413.0 / 4096 * 32,
                 kC3 = 2392.0 / 4096 * 32;
double pqToNits(double e) {
    e = std::clamp(e, 0.0, 1.0);
    const double p = std::pow(e, 1 / kM2);
    return 10000 * std::pow(std::max(p - kC1, 0.0) / (kC2 - kC3 * p), 1 / kM1);
}
double nitsToPq(double nits) {
    const double y = std::pow(std::clamp(nits / 10000, 0.0, 1.0), kM1);
    return std::pow((kC1 + kC2 * y) / (1 + kC3 * y), kM2);
}

// ---- HLG (BT.2100), scene light 0..1 ----
constexpr double kHa = 0.17883277, kHb = 1 - 4 * kHa, kHc = 0.55991073;  // c = 0.5 - a ln(4a)
double hlgInverseOetf(double v) {
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.5 ? v * v / 3 : (std::exp((v - kHc) / kHa) + kHb) / 12;
}
double hlgOetf(double e) {
    e = std::max(e, 0.0);
    return e <= 1.0 / 12 ? std::sqrt(3 * e) : kHa * std::log(12 * e - kHb) + kHc;
}
constexpr double kHlgPeak = 1000, kHlgGamma = 1.2;

double srgbToLinear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double linearToSrgb(double l) { return l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1 / 2.4) - 0.055; }

// ---- Camera log curves: code value <-> scene linear (18 % grey = 0.18) ----
double slog3ToLinear(double v) {
    const double x = v * 1023;
    return x >= 171.2102946929 ? std::pow(10.0, (x - 420) / 261.5) * 0.19 - 0.01 : (x - 95) * 0.01125 / (171.2102946929 - 95);
}
double linearToSlog3(double l) {
    return l >= 0.01125 ? (420 + std::log10((l + 0.01) / 0.19) * 261.5) / 1023 : (l * (171.2102946929 - 95) / 0.01125 + 95) / 1023;
}
constexpr double kL3cut = 0.010591, kL3a = 5.555556, kL3b = 0.052272, kL3c = 0.247190, kL3d = 0.385537, kL3e = 5.367655,
                 kL3f = 0.092809;
double logc3ToLinear(double t) { return t > kL3e * kL3cut + kL3f ? (std::pow(10.0, (t - kL3d) / kL3c) - kL3b) / kL3a : (t - kL3f) / kL3e; }
double linearToLogc3(double x) { return x > kL3cut ? kL3c * std::log10(kL3a * x + kL3b) + kL3d : kL3e * x + kL3f; }
const double kL4a = (std::pow(2.0, 18) - 16) / 117.45, kL4b = (1023.0 - 95) / 1023, kL4c = 95.0 / 1023;
const double kL4s = (7 * std::log(2.0) * std::pow(2.0, 7 - 14 * kL4c / kL4b)) / (kL4a * kL4b);
const double kL4t = (std::pow(2.0, 14 * (-kL4c / kL4b) + 6) - 64) / kL4a;
double logc4ToLinear(double e) { return e >= 0 ? (std::pow(2.0, 14 * (e - kL4c) / kL4b + 6) - 64) / kL4a : e * kL4s + kL4t; }
double linearToLogc4(double x) { return x >= kL4t ? (std::log2(kL4a * x + 64) - 6) / 14 * kL4b + kL4c : (x - kL4t) / kL4s; }
double vlogToLinear(double v) { return v < 0.181 ? (v - 0.125) / 5.6 : std::pow(10.0, (v - 0.598206) / 0.241514) - 0.00873; }
double linearToVlog(double l) { return l < 0.01 ? 5.6 * l + 0.125 : 0.241514 * std::log10(l + 0.00873) + 0.598206; }
double clog3ToLinear(double v) {
    double x;
    if (v < 0.097465473) x = -(std::pow(10.0, (0.12783901 - v) / 0.36726845) - 1) / 14.98325;
    else if (v <= 0.15277891) x = (v - 0.12512219) / 1.9754798;
    else x = (std::pow(10.0, (v - 0.12240537) / 0.36726845) - 1) / 14.98325;
    return x * 0.9;
}
double linearToClog3(double l) {
    const double x = l / 0.9;
    if (x < -0.014) return -0.36726845 * std::log10(-x * 14.98325 + 1) + 0.12783901;
    if (x <= 0.014) return 1.9754798 * x + 0.12512219;
    return 0.36726845 * std::log10(x * 14.98325 + 1) + 0.12240537;
}
double acescctToLinear(double v) {
    if (v <= 0.155251141552511) return (v - 0.0729055341958355) / 10.5402377416545;
    return std::min(65504.0, std::pow(2.0, v * 17.52 - 9.72));
}
double linearToAcescct(double l) { return l <= 0.0078125 ? 10.5402377416545 * l + 0.0729055341958355 : (std::log2(l) + 9.72) / 17.52; }

// ---- Primaries ----
struct Chroma {
    double rx, ry, gx, gy, bx, by, wx, wy;
};
Chroma chroma(Primaries p) {
    constexpr double wx = 0.3127, wy = 0.3290;
    switch (p) {
        case Primaries::Bt709: return {0.640, 0.330, 0.300, 0.600, 0.150, 0.060, wx, wy};
        case Primaries::Bt2020: return {0.708, 0.292, 0.170, 0.797, 0.131, 0.046, wx, wy};
        case Primaries::P3D65: return {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, wx, wy};
        case Primaries::SGamut3Cine: return {0.766, 0.275, 0.225, 0.800, 0.089, -0.087, wx, wy};
        case Primaries::AlexaWideGamut3: return {0.684, 0.313, 0.221, 0.848, 0.0861, -0.102, wx, wy};
        case Primaries::AlexaWideGamut4: return {0.7347, 0.2653, 0.1424, 0.8576, 0.0991, -0.0308, wx, wy};
        case Primaries::VGamut: return {0.730, 0.280, 0.165, 0.840, 0.100, -0.030, wx, wy};
        case Primaries::CinemaGamut: return {0.740, 0.270, 0.170, 1.140, 0.080, -0.100, wx, wy};
        case Primaries::Ap1: return {0.713, 0.293, 0.165, 0.830, 0.128, 0.044, 0.32168, 0.33767};
        case Primaries::Ap0: return {0.7347, 0.2653, 0.0, 1.0, 0.0001, -0.077, 0.32168, 0.33767};
    }
    return chroma(Primaries::Bt709);
}

void invert3(const double m[9], double out[9]) {
    const double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const double k = 1 / det;
    out[0] = (e * i - f * h) * k, out[1] = (c * h - b * i) * k, out[2] = (b * f - c * e) * k;
    out[3] = (f * g - d * i) * k, out[4] = (a * i - c * g) * k, out[5] = (c * d - a * f) * k;
    out[6] = (d * h - e * g) * k, out[7] = (b * g - a * h) * k, out[8] = (a * e - b * d) * k;
}
void mul3(const double a[9], const double b[9], double out[9]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) out[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c];
}
void rgbToXyz(const Chroma& p, double m[9]) {
    auto xyz = [](double x, double y, double out[3]) {
        out[0] = x / y;
        out[1] = 1;
        out[2] = (1 - x - y) / y;
    };
    double r[3], g[3], b[3], w[3];
    xyz(p.rx, p.ry, r), xyz(p.gx, p.gy, g), xyz(p.bx, p.by, b), xyz(p.wx, p.wy, w);
    const double base[9] = {r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};
    double inv[9];
    invert3(base, inv);
    const double s[3] = {inv[0] * w[0] + inv[1] * w[1] + inv[2] * w[2], inv[3] * w[0] + inv[4] * w[1] + inv[5] * w[2],
                         inv[6] * w[0] + inv[7] * w[1] + inv[8] * w[2]};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) m[row * 3 + col] = base[row * 3 + col] * s[col];
}
// Bradford chromatic adaptation between two whites.
void bradford(double wx1, double wy1, double wx2, double wy2, double m[9]) {
    static const double B[9] = {0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296};
    double Binv[9];
    invert3(B, Binv);
    auto cone = [&](double x, double y, double out[3]) {
        const double X = x / y, Y = 1, Z = (1 - x - y) / y;
        for (int i = 0; i < 3; ++i) out[i] = B[i * 3] * X + B[i * 3 + 1] * Y + B[i * 3 + 2] * Z;
    };
    double s[3], d[3];
    cone(wx1, wy1, s);
    cone(wx2, wy2, d);
    const double D[9] = {d[0] / s[0], 0, 0, 0, d[1] / s[1], 0, 0, 0, d[2] / s[2]};
    double t[9];
    mul3(D, B, t);
    mul3(Binv, t, m);
}

double luminance2020(double r, double g, double b) { return 0.2627 * r + 0.6780 * g + 0.0593 * b; }

}  // namespace

const std::vector<ColorSpace>& colorSpaces() {
    static const std::vector<ColorSpace> spaces = {
        {"rec709", "Rec.709 (SDR video)", Primaries::Bt709, Transfer::Bt1886, false},
        {"srgb", "sRGB", Primaries::Bt709, Transfer::Srgb, false},
        {"rec2020", "Rec.2020 (SDR)", Primaries::Bt2020, Transfer::Bt1886, false},
        {"p3d65", "Display P3", Primaries::P3D65, Transfer::Srgb, false},
        {"rec2100pq", "Rec.2100 PQ (HDR10)", Primaries::Bt2020, Transfer::Pq, false},
        {"rec2100hlg", "Rec.2100 HLG", Primaries::Bt2020, Transfer::Hlg, false},
        {"slog3-sgamut3cine", "Sony S-Log3 / S-Gamut3.Cine", Primaries::SGamut3Cine, Transfer::SLog3, true},
        {"logc3-awg3", "ARRI LogC3 / ALEXA Wide Gamut 3", Primaries::AlexaWideGamut3, Transfer::LogC3, true},
        {"logc4-awg4", "ARRI LogC4 / ALEXA Wide Gamut 4", Primaries::AlexaWideGamut4, Transfer::LogC4, true},
        {"vlog-vgamut", "Panasonic V-Log / V-Gamut", Primaries::VGamut, Transfer::VLog, true},
        {"clog3-cinemagamut", "Canon Log 3 / Cinema Gamut", Primaries::CinemaGamut, Transfer::CLog3, true},
        {"acescct", "ACEScct", Primaries::Ap1, Transfer::AcesCct, true},
        {"aces2065-1", "ACES2065-1 (linear AP0)", Primaries::Ap0, Transfer::Linear, true},
        {"linear-rec709", "Linear Rec.709 (scene)", Primaries::Bt709, Transfer::Linear, true},
    };
    return spaces;
}

const ColorSpace* findColorSpace(const std::string& id) {
    for (const auto& c : colorSpaces())
        if (c.id == id) return &c;
    return nullptr;
}

std::string colorSpaceFromTags(const std::string& primaries, const std::string& transfer) {
    if (transfer == "smpte2084") return "rec2100pq";
    if (transfer == "arib-std-b67") return "rec2100hlg";
    if (primaries == "bt2020") return "rec2020";
    if (primaries == "smpte432") return "p3d65";
    // sRGB-tagged files are read as Rec.709, as editors conventionally do.
    return "rec709";
}

std::vector<const ColorSpace*> displayColorSpaces() {
    std::vector<const ColorSpace*> out;
    for (const auto& c : colorSpaces())
        if (!c.sceneReferred) out.push_back(&c);
    return out;
}

const ColorSpace& rec709Space() { return colorSpaces().front(); }

const ColorSpace& mediaColorSpace(const MediaItem& m) {
    if (const ColorSpace* c = findColorSpace(m.colorOverride)) return *c;
    if (const ColorSpace* c = findColorSpace(m.colorSpace)) return *c;
    return rec709Space();
}

const ColorSpace& sequenceColorSpace(const Sequence& s) {
    const ColorSpace* c = findColorSpace(s.colorSpace);
    return c && !c->sceneReferred ? *c : rec709Space();
}

double toLinear(Transfer t, double v) {
    switch (t) {
        case Transfer::Bt1886: return std::pow(std::max(v, 0.0), 2.4);
        case Transfer::Srgb: return srgbToLinear(std::max(v, 0.0));
        case Transfer::Linear: return v;
        case Transfer::Pq: return pqToNits(v) / kSdrWhiteNits;
        case Transfer::Hlg: return hlgInverseOetf(v);  // scene light; convertPixel applies the OOTF
        case Transfer::SLog3: return slog3ToLinear(v);
        case Transfer::LogC3: return logc3ToLinear(v);
        case Transfer::LogC4: return logc4ToLinear(v);
        case Transfer::VLog: return vlogToLinear(v);
        case Transfer::CLog3: return clog3ToLinear(v);
        case Transfer::AcesCct: return acescctToLinear(v);
    }
    return v;
}

double fromLinear(Transfer t, double l) {
    switch (t) {
        case Transfer::Bt1886: return std::pow(std::max(l, 0.0), 1 / 2.4);
        case Transfer::Srgb: return linearToSrgb(std::max(l, 0.0));
        case Transfer::Linear: return l;
        case Transfer::Pq: return nitsToPq(l * kSdrWhiteNits);
        case Transfer::Hlg: return hlgOetf(l);
        case Transfer::SLog3: return linearToSlog3(l);
        case Transfer::LogC3: return linearToLogc3(l);
        case Transfer::LogC4: return linearToLogc4(l);
        case Transfer::VLog: return linearToVlog(l);
        case Transfer::CLog3: return linearToClog3(l);
        case Transfer::AcesCct: return linearToAcescct(l);
    }
    return l;
}

void primariesMatrix(Primaries from, Primaries to, double m[9]) {
    double a[9], b[9], bInv[9];
    primariesToXyz(from, a);
    primariesToXyz(to, b);
    invert3(b, bInv);
    mul3(bInv, a, m);
}

namespace {
// convertPixel with the primaries matrix already worked out.
void convertPixelWith(float rgb[3], const ColorSpace& from, const ColorSpace& to, double hdrPeakNits, const double m[9]) {
    // 1. Code values to linear light in the source primaries.
    double c[3];
    for (int i = 0; i < 3; ++i) c[i] = toLinear(from.transfer, rgb[i]);
    if (from.transfer == Transfer::Hlg) {
        // HLG OOTF (1000-nit display, system gamma 1.2): scene light to display light.
        const double ys = std::max(0.0, luminance2020(c[0], c[1], c[2]));
        const double k = kHlgPeak * std::pow(ys, kHlgGamma - 1) / kSdrWhiteNits;
        for (double& v : c) v *= k;
    }
    // 2. Into the target primaries.
    double l[3];
    for (int r = 0; r < 3; ++r) l[r] = m[r * 3] * c[0] + m[r * 3 + 1] * c[1] + m[r * 3 + 2] * c[2];
    const bool targetDisplay = !to.sceneReferred;
    if (targetDisplay) {
        for (double& v : l) v = std::max(v, 0.0);
        const double peakRel = (to.hdr() ? (to.transfer == Transfer::Hlg ? kHlgPeak : hdrPeakNits) : kSdrWhiteNits) / kSdrWhiteNits;
        if (from.sceneReferred) {
            // Display rendering for camera footage: 18 % grey to ~26 % of white
            // (SDR) or 26 nits (HDR), with a filmic shoulder up to the peak.
            for (double& v : l) {
                if (!to.hdr()) {
                    const double x = v * 0.6;
                    v = std::clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
                } else {
                    const double x = v * (26.0 / kSdrWhiteNits) / 0.18;
                    v = x / (1 + x / peakRel);
                }
            }
        } else if (from.hdr() && !to.hdr()) {
            // HDR into SDR: roll the brightest channel off above 75 % of SDR
            // white, scaling all three so hue and saturation are kept.
            const double mx = std::max({l[0], l[1], l[2]});
            constexpr double knee = 0.75;
            if (mx > knee) {
                const double out = knee + (1 - knee) * (1 - std::exp(-(mx - knee) / (1 - knee)));
                for (double& v : l) v *= out / mx;
            }
        }
        for (double& v : l) v = std::min(v, peakRel);
    }
    if (to.transfer == Transfer::Hlg) {
        // Inverse OOTF: display light back to scene light.
        const double yd = std::max(1e-12, luminance2020(l[0], l[1], l[2]) * kSdrWhiteNits / kHlgPeak);
        const double k = std::pow(yd, (1 - kHlgGamma) / kHlgGamma) * kSdrWhiteNits / kHlgPeak;
        for (double& v : l) v *= k;
    }
    for (int i = 0; i < 3; ++i) {
        const double e = fromLinear(to.transfer, l[i]);
        rgb[i] = float(targetDisplay ? std::clamp(e, 0.0, 1.0) : e);
    }
}
}  // namespace

void primariesToXyz(Primaries p, double m[9]) {
    // The makers' published matrices to ACES AP0 (the ACES IDTs).
    static const double sgamut3cine[9] = {0.6387886672, 0.2723514337, 0.0888598992, -0.0039159061, 1.0880732308,
                                          -0.0841573249, -0.0299072021, -0.0264325799, 1.0563397820};
    static const double awg3[9] = {0.680206, 0.236137, 0.083658, 0.085415, 1.017471, -0.102886, 0.002057, -0.062563, 1.060506};
    static const double cinemaGamut[9] = {0.763064455, 0.149021161, 0.087914384, 0.003657457, 1.10696038,
                                          -0.110617837, -0.009407794, -0.218383305, 1.227791099};
    const double* toAp0 = p == Primaries::SGamut3Cine       ? sgamut3cine
                          : p == Primaries::AlexaWideGamut3 ? awg3
                          : p == Primaries::CinemaGamut     ? cinemaGamut
                                                            : nullptr;
    if (toAp0) {
        double ap0[9];
        primariesToXyz(Primaries::Ap0, ap0);
        mul3(ap0, toAp0, m);
        return;
    }
    const Chroma c = chroma(p);
    double toXyz[9];
    rgbToXyz(c, toXyz);
    double adapt[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    if (std::fabs(c.wx - 0.3127) > 1e-6 || std::fabs(c.wy - 0.3290) > 1e-6) bradford(c.wx, c.wy, 0.3127, 0.3290, adapt);
    mul3(adapt, toXyz, m);
}

void convertPixel(float rgb[3], const ColorSpace& from, const ColorSpace& to, double hdrPeakNits) {
    double m[9];
    primariesMatrix(from.primaries, to.primaries, m);
    convertPixelWith(rgb, from, to, hdrPeakNits, m);
}

void convertColor(Image& img, const ColorSpace& from, const ColorSpace& to, double hdrPeakNits) {
    if (img.empty() || from.id == to.id) return;
    // The whole conversion as a 65^3 LUT, built once per pair.
    static std::mutex m;
    static std::map<std::tuple<std::string, std::string, int>, std::shared_ptr<Lut3D>> cache;
    std::shared_ptr<Lut3D> lut;
    {
        std::lock_guard lock(m);
        auto& slot = cache[{from.id, to.id, int(std::lround(hdrPeakNits))}];
        if (!slot) {
            auto l = std::make_shared<Lut3D>();
            const int n = 65;
            l->size = n;
            l->data.resize(size_t(n) * n * n * 3);
            double mat[9];
            primariesMatrix(from.primaries, to.primaries, mat);
            for (int b = 0; b < n; ++b)
                for (int g = 0; g < n; ++g)
                    for (int r = 0; r < n; ++r) {
                        float px[3] = {float(r) / (n - 1), float(g) / (n - 1), float(b) / (n - 1)};
                        convertPixelWith(px, from, to, hdrPeakNits, mat);
                        float* d = &l->data[(size_t(b) * n * n + size_t(g) * n + size_t(r)) * 3];
                        d[0] = px[0], d[1] = px[1], d[2] = px[2];
                    }
            slot = l;
        }
        lut = slot;
    }
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                const float a = p[3];
                if (a <= 0) continue;
                float r = std::clamp(p[0] / a, 0.0f, 1.0f), g = std::clamp(p[1] / a, 0.0f, 1.0f), b = std::clamp(p[2] / a, 0.0f, 1.0f);
                lut->apply(r, g, b);
                p[0] = r * a, p[1] = g * a, p[2] = b * a;
            }
        }
    });
}

}  // namespace montage
