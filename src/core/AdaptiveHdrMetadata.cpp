// Montage — adaptive HDR metadata (SMPTE ST 2094-50): the message and its maths. See AdaptiveHdrMetadata.h.
#include "AdaptiveHdrMetadata.h"

#include <algorithm>
#include <cmath>

namespace montage {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The provider: United States (T.35 country code), SMPTE, application #5.
constexpr uint8_t kCountry = 0xB5;
constexpr uint16_t kProvider = 0x0090, kOriented = 0x0001;

const double kChromaPresets[3][8] = {
    {0.64, 0.33, 0.3, 0.6, 0.15, 0.06, 0.3127, 0.329},          // Rec.709
    {0.68, 0.32, 0.265, 0.69, 0.15, 0.06, 0.3127, 0.329},       // P3-D65 (ST 2113)
    {0.708, 0.292, 0.17, 0.797, 0.131, 0.046, 0.3127, 0.329}};  // Rec.2020

class BitWriter {
public:
    void put(uint32_t v, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            if (n_ % 8 == 0) out.push_back(0);
            if ((v >> i) & 1) out.back() |= uint8_t(0x80 >> (n_ % 8));
            ++n_;
        }
    }
    std::vector<uint8_t> out;

private:
    size_t n_ = 0;
};

class BitReader {
public:
    BitReader(const uint8_t* d, size_t n) : d_(d), n_(n * 8) {}
    // False once past the end (the value then reads 0).
    bool get(int bits, uint32_t& v) {
        v = 0;
        if (pos_ + size_t(bits) > n_) return false;
        for (int i = 0; i < bits; ++i, ++pos_) v = (v << 1) | ((d_[pos_ / 8] >> (7 - pos_ % 8)) & 1);
        return true;
    }

private:
    const uint8_t* d_;
    size_t n_, pos_ = 0;
};

uint32_t code(double v, double scale, uint32_t max) {
    return uint32_t(std::clamp(std::lround(v * scale), 0L, long(max)));
}

// The mix's coded type: 0 the brightest component, 1 each component, 2 a sixth of each plus half the brightest, 3 given.
int mixType(const double k[6]) {
    auto is = [&](std::initializer_list<double> v) {
        int i = 0;
        for (double x : v)
            if (std::fabs(k[i++] - x) > 1e-9) return false;
        return true;
    };
    if (is({0, 0, 0, 1, 0, 0})) return 0;
    if (is({0, 0, 0, 0, 0, 1})) return 1;
    if (is({1.0 / 6, 1.0 / 6, 1.0 / 6, 0.5, 0, 0})) return 2;
    return 3;
}

void setMixType(double k[6], int type) {
    static const double types[3][6] = {{0, 0, 0, 1, 0, 0}, {0, 0, 0, 0, 0, 1}, {1.0 / 6, 1.0 / 6, 1.0 / 6, 0.5, 0, 0}};
    std::copy(types[type], types[type] + 6, k);
}

// The quadratic Bézier tone curve of C.3.8 (knee, a middle point from the highlight compression, maximum) at t, as
// light in, log2 gain and that gain's slope (C.6).
void bezierPoint(double t, double xKnee, double yKnee, double xMax, double yMax, double kappa, double& gx, double& gy, double& gm) {
    const double xMid = (1 - kappa) * xKnee + kappa * (xKnee * yMax / yKnee);
    const double yMid = (1 - kappa) * yKnee + kappa * yMax;
    const double ax = xKnee - 2 * xMid + xMax, bx = 2 * xMid - 2 * xKnee, cx = xKnee;
    const double ay = yKnee - 2 * yMid + yMax, by = 2 * yMid - 2 * yKnee, cy = yKnee;
    const double x = ax * t * t + bx * t + cx, y = ay * t * t + by * t + cy;
    const double dx = 2 * ax * t + bx;
    const double m = std::fabs(dx) > 1e-12 ? (2 * ay * t + by) / dx : 0;
    gx = x;
    gy = std::log2(y / x);
    gm = (x * m - y) / (std::log(2.0) * x * y);
}

}  // namespace

bool isAdaptiveHdrT35(const uint8_t* t35, size_t size) {
    return t35 && size >= 5 && t35[0] == kCountry && ((t35[1] << 8) | t35[2]) == kProvider && ((t35[3] << 8) | t35[4]) == kOriented;
}

std::vector<uint8_t> adaptiveHdrPayload(const AdaptiveHdr& in) {
    AdaptiveHdr md = in;
    sanitizeAdaptiveHdr(md);
    BitWriter w;
    // Application info: version 0, readable by version-0 parsers.
    w.put(0, 3);
    w.put(0, 3);
    w.put(0, 2);
    // Colour volume transform.
    const uint32_t white = code(md.referenceWhite, 5, 50000);
    const bool customWhite = std::max<uint32_t>(white, 1) != 1015;  // (203 cd/m², the default, is 1015 fifths)
    w.put(customWhite, 1);
    w.put(md.toneMap, 1);
    w.put(0, 6);
    if (customWhite) w.put(std::max<uint32_t>(white, 1), 16);
    if (!md.toneMap) return w.out;
    // Headroom-adaptive tone map.
    w.put(code(md.baseline, 10000, 60000), 16);
    w.put(md.computed, 1);
    if (md.computed) {
        w.put(0, 7);
        return w.out;
    }
    const size_t n = std::min<size_t>(md.alternate.size(), 4);
    w.put(uint32_t(n), 3);
    w.put(uint32_t(md.chromaMode), 2);
    bool commonMix = n > 1, commonCurve = n > 1;
    for (size_t a = 1; a < n; ++a) {
        if (!std::equal(md.gains[a].mix, md.gains[a].mix + 6, md.gains[0].mix)) commonMix = false;
        const AdaptiveGainCurve &g = md.gains[a], &g0 = md.gains[0];
        if (g.x.size() != g0.x.size() || g.pchip != g0.pchip) commonCurve = false;
        else
            for (size_t c = 0; c < g.x.size(); ++c)
                if (code(g.x[c], 1000, 64000) != code(g0.x[c], 1000, 64000)) commonCurve = false;
    }
    w.put(commonMix, 1);
    w.put(commonCurve, 1);
    if (md.chromaMode == 3)
        for (double v : md.chroma) w.put(code(v, 50000, 50000), 16);
    for (size_t a = 0; a < n; ++a) {
        const AdaptiveGainCurve& g = md.gains[a];
        const uint32_t alt = code(md.alternate[a], 10000, 60000);
        w.put(alt, 16);
        if (a == 0 || !commonMix) {
            const int type = mixType(g.mix);
            w.put(uint32_t(type), 2);
            if (type != 3) {
                w.put(0, 6);
            } else {
                uint32_t k[6];
                for (int i = 0; i < 6; ++i) k[i] = code(g.mix[i], 50000, 50000);
                for (int i = 0; i < 6; ++i) w.put(k[i] != 0, 1);
                for (int i = 0; i < 6; ++i)
                    if (k[i]) w.put(k[i], 16);
            }
        }
        const size_t points = g.x.size();
        if (a == 0 || !commonCurve) {
            w.put(uint32_t(points - 1), 5);
            w.put(g.pchip, 1);
            w.put(0, 2);
            for (double x : g.x) w.put(code(x, 1000, 64000), 16);
        }
        // Gains are sent as magnitudes: their sign is the side of the baseline the rendition is on.
        for (double y : g.y) w.put(code(std::fabs(y), 10000, 60000), 16);
        if (!g.pchip)
            for (double m : g.m) w.put(uint32_t(std::clamp(std::lround(std::atan(m) * 36000 / kPi + 18000), 1L, 35999L)), 16);
    }
    return w.out;
}

std::vector<uint8_t> adaptiveHdrT35(const AdaptiveHdr& md) {
    std::vector<uint8_t> out{kCountry, uint8_t(kProvider >> 8), uint8_t(kProvider & 0xff), uint8_t(kOriented >> 8), uint8_t(kOriented & 0xff)};
    const std::vector<uint8_t> payload = adaptiveHdrPayload(md);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool parseAdaptiveHdrPayload(const uint8_t* data, size_t size, AdaptiveHdr& out) {
    if (!data) return false;
    BitReader r(data, size);
    uint32_t v = 0, version = 0, minimum = 0;
    if (!r.get(3, version) || !r.get(3, minimum) || !r.get(2, v)) return false;
    if (minimum != 0) return false;  // (a later version's, which this parser may not understand)
    AdaptiveHdr md;
    uint32_t customWhite = 0, toneMap = 0;
    if (!r.get(1, customWhite) || !r.get(1, toneMap) || !r.get(6, v)) return false;
    if (customWhite) {
        if (!r.get(16, v)) return false;
        md.referenceWhite = std::clamp(double(v), 1.0, 50000.0) / 5;
    }
    md.toneMap = toneMap;
    if (!toneMap) {
        out = md;
        return true;
    }
    uint32_t baseline = 0, computed = 0;
    if (!r.get(16, baseline) || !r.get(1, computed)) return false;
    md.baseline = std::min<uint32_t>(baseline, 60000) / 10000.0;
    md.computed = computed;
    if (computed) {
        computeAdaptiveCurves(md);
        out = md;
        return true;
    }
    uint32_t count = 0, chromaMode = 0, commonMix = 0, commonCurve = 0;
    if (!r.get(3, count) || !r.get(2, chromaMode) || !r.get(1, commonMix) || !r.get(1, commonCurve)) return false;
    md.chromaMode = int(chromaMode);
    if (chromaMode == 3) {
        for (double& c : md.chroma) {
            if (!r.get(16, v)) return false;
            c = std::min<uint32_t>(v, 50000) / 50000.0;
        }
    } else {
        std::copy(kChromaPresets[chromaMode], kChromaPresets[chromaMode] + 8, md.chroma);
    }
    const size_t n = std::min<uint32_t>(count, 4);
    for (size_t a = 0; a < n; ++a) {
        uint32_t alt = 0;
        if (!r.get(16, alt)) return false;
        md.alternate.push_back(std::min<uint32_t>(alt, 60000) / 10000.0);
        AdaptiveGainCurve g;
        if (a == 0 || !commonMix) {
            uint32_t type = 0;
            if (!r.get(2, type)) return false;
            if (type != 3) {
                if (!r.get(6, v)) return false;
                setMixType(g.mix, int(type));
            } else {
                uint32_t has[6];
                for (uint32_t& h : has)
                    if (!r.get(1, h)) return false;
                double p[6], sum = 0;
                for (int k = 0; k < 6; ++k) {
                    p[k] = 0;
                    if (has[k]) {
                        if (!r.get(16, v)) return false;
                        p[k] = std::min<uint32_t>(v, 50000) / 50000.0;
                    }
                    sum += p[k];
                }
                if (sum <= 0) return false;
                for (int k = 0; k < 6; ++k) g.mix[k] = p[k] / sum;
            }
        } else {
            std::copy(md.gains[0].mix, md.gains[0].mix + 6, g.mix);
        }
        size_t points = 0;
        if (a == 0 || !commonCurve) {
            uint32_t minus1 = 0, pchip = 0;
            if (!r.get(5, minus1) || !r.get(1, pchip) || !r.get(2, v)) return false;
            points = minus1 + 1;
            g.pchip = pchip;
            for (size_t c = 0; c < points; ++c) {
                if (!r.get(16, v)) return false;
                g.x.push_back(std::min<uint32_t>(v, 64000) / 1000.0);
            }
        } else {
            g.pchip = md.gains[0].pchip;
            g.x = md.gains[0].x;
            points = g.x.size();
        }
        const double sign = baseline < alt ? 1 : -1;
        for (size_t c = 0; c < points; ++c) {
            if (!r.get(16, v)) return false;
            g.y.push_back(sign * std::min<uint32_t>(v, 60000) / 10000.0);
        }
        if (g.pchip) {
            pchipSlopes(g);
        } else {
            for (size_t c = 0; c < points; ++c) {
                if (!r.get(16, v)) return false;
                g.m.push_back(std::tan((std::clamp(double(v), 1.0, 35999.0) - 18000) * kPi / 36000));
            }
        }
        md.gains.push_back(std::move(g));
    }
    // (Anything after is padding or a later version's, which parsers skip.)
    out = md;
    return true;
}

bool parseAdaptiveHdrT35(const uint8_t* t35, size_t size, AdaptiveHdr& md) {
    return isAdaptiveHdrT35(t35, size) && parseAdaptiveHdrPayload(t35 + 5, size - 5, md);
}

void computeAdaptiveCurves(AdaptiveHdr& md) {
    md.alternate.clear();
    md.gains.clear();
    md.chromaMode = 2;
    std::copy(kChromaPresets[2], kChromaPresets[2] + 8, md.chroma);
    if (md.baseline <= 0) return;
    const double share = std::min(md.baseline / std::log2(1000.0 / 203), 1.0);
    md.alternate = {0, std::log2(8.0 / 3) * share};
    const double yWhite[2] = {1 - 0.5 * share, 1};
    constexpr int kPoints = 8;
    constexpr double kKappa = 0.65;
    for (int a = 0; a < 2; ++a) {
        AdaptiveGainCurve g;
        g.pchip = false;
        for (int c = 0; c < kPoints; ++c) {
            double x, y, m;
            bezierPoint(double(c) / (kPoints - 1), 1, yWhite[a], std::exp2(md.baseline), std::exp2(md.alternate[size_t(a)]), kKappa, x, y, m);
            g.x.push_back(x);
            g.y.push_back(y);
            g.m.push_back(m);
        }
        md.gains.push_back(std::move(g));
    }
}

void pchipSlopes(AdaptiveGainCurve& g) {
    const size_t n = std::min(g.x.size(), g.y.size());
    g.m.assign(n, 0);
    if (n < 2) return;
    std::vector<double> h(n - 1), s(n - 1);
    for (size_t i = 0; i + 1 < n; ++i) {
        h[i] = g.x[i + 1] - g.x[i];
        s[i] = h[i] > 0 ? (g.y[i + 1] - g.y[i]) / h[i] : 0;
    }
    if (n == 2) {
        g.m[0] = g.m[1] = s[0];
        return;
    }
    auto end = [](double h0, double h1, double s0, double s1) { return h0 + h1 > 0 ? ((2 * h0 + h1) * s0 - h0 * s1) / (h0 + h1) : 0; };
    g.m[0] = end(h[0], h[1], s[0], s[1]);
    g.m[n - 1] = end(h[n - 2], h[n - 3], s[n - 2], s[n - 3]);
    auto sign = [](double v) { return (v > 0) - (v < 0); };
    for (size_t i = 1; i + 1 < n; ++i) {
        if (sign(s[i - 1]) != sign(s[i])) {
            g.m[i] = 0;
        } else {
            const double den = (2 * h[i - 1] + h[i]) * s[i - 1] + (h[i - 1] + 2 * h[i]) * s[i];
            g.m[i] = den != 0 ? 3 * (h[i - 1] + h[i]) * s[i - 1] * s[i] / den : 0;
        }
    }
}

double adaptiveGainCurve(const AdaptiveGainCurve& g, double x) {
    const size_t n = std::min({g.x.size(), g.y.size(), g.m.size()});
    if (n == 0) return 0;
    if (!(x > g.x[0])) return g.y[0];  // (and x = x0, and not-a-number)
    if (x >= g.x[n - 1]) return g.y[n - 1] + std::log2(g.x[n - 1] / x);
    const size_t i = size_t(std::upper_bound(g.x.begin(), g.x.begin() + std::ptrdiff_t(n), x) - g.x.begin()) - 1;
    const double h = g.x[i + 1] - g.x[i];
    if (h <= 0) return g.y[i];
    const double t = (x - g.x[i]) / h, m0 = h * g.m[i], m1 = h * g.m[i + 1], y0 = g.y[i], y1 = g.y[i + 1];
    const double c3 = 2 * y0 + m0 - 2 * y1 + m1, c2 = -3 * y0 + 3 * y1 - 2 * m0 - m1;
    return ((c3 * t + c2) * t + m0) * t + y0;
}

void adaptiveGain(const AdaptiveHdr& md, const double c[3], double headroom, double gain[3]) {
    gain[0] = gain[1] = gain[2] = 0;
    if (!md.toneMap) return;
    const size_t n = std::min(md.alternate.size(), md.gains.size());
    // The headrooms with the baseline's among them (its gain zero), and the two the display's lies between.
    std::vector<double> hs;
    std::vector<int> which;  // the curve each is (-1: the baseline)
    bool placed = false;
    for (size_t a = 0; a < n; ++a) {
        if (!placed && md.baseline < md.alternate[a]) hs.push_back(md.baseline), which.push_back(-1), placed = true;
        hs.push_back(md.alternate[a]), which.push_back(int(a));
    }
    if (!placed) hs.push_back(md.baseline), which.push_back(-1);
    const double h = std::clamp(headroom, hs.front(), hs.back());
    size_t i = 0;
    while (i + 1 < hs.size() && hs[i + 1] <= h) ++i;
    auto addCurve = [&](int a, double weight) {
        if (a < 0 || weight == 0) return;
        const AdaptiveGainCurve& g = md.gains[size_t(a)];
        const double common = c[0] * g.mix[0] + c[1] * g.mix[1] + c[2] * g.mix[2] + std::max({c[0], c[1], c[2]}) * g.mix[3] +
                              std::min({c[0], c[1], c[2]}) * g.mix[4];
        if (g.mix[5] == 0) {
            const double v = weight * adaptiveGainCurve(g, common);
            for (int k = 0; k < 3; ++k) gain[k] += v;
        } else {
            for (int k = 0; k < 3; ++k) gain[k] += weight * adaptiveGainCurve(g, c[k] * g.mix[5] + common);
        }
    };
    if (hs[i] == h || i + 1 >= hs.size()) {
        addCurve(which[i], 1);
        return;
    }
    const double w1 = (h - hs[i]) / (hs[i + 1] - hs[i]);
    addCurve(which[i], 1 - w1);
    addCurve(which[i + 1], w1);
}

bool adaptiveToneMaps(const AdaptiveHdr& md, double headroom) {
    if (!md.toneMap || md.gains.empty()) return false;
    const double lo = std::min(md.baseline, md.alternate.front()), hi = std::max(md.baseline, md.alternate.back());
    return std::fabs(std::clamp(headroom, lo, hi) - md.baseline) > 1e-9;
}

void adaptiveToneMap(const AdaptiveHdr& md, double c[3], double headroom) {
    double g[3];
    adaptiveGain(md, c, headroom, g);
    for (int k = 0; k < 3; ++k) c[k] *= std::exp2(g[k]);
}

void sanitizeAdaptiveHdr(AdaptiveHdr& md) {
    md.referenceWhite = std::isfinite(md.referenceWhite) ? std::clamp(md.referenceWhite, 0.2, 10000.0) : 203;
    md.baseline = std::isfinite(md.baseline) ? std::clamp(md.baseline, 0.0, 6.0) : 0;
    md.chromaMode = std::clamp(md.chromaMode, 0, 3);
    if (md.chromaMode < 3) std::copy(kChromaPresets[md.chromaMode], kChromaPresets[md.chromaMode] + 8, md.chroma);
    for (double& c : md.chroma) c = std::isfinite(c) ? std::clamp(c, 0.0, 1.0) : 0;
    if (md.computed) return;
    // Renditions in order of headroom, none at the baseline's, at most four.
    std::vector<std::pair<double, AdaptiveGainCurve>> alts;
    for (size_t a = 0; a < std::min(md.alternate.size(), md.gains.size()); ++a) {
        const double h = std::isfinite(md.alternate[a]) ? std::clamp(md.alternate[a], 0.0, 6.0) : 0;
        if (std::fabs(h - md.baseline) < 1e-4 || md.gains[a].x.empty()) continue;
        alts.emplace_back(h, md.gains[a]);
    }
    std::stable_sort(alts.begin(), alts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    alts.erase(std::unique(alts.begin(), alts.end(), [](const auto& a, const auto& b) { return std::fabs(a.first - b.first) < 1e-4; }), alts.end());
    if (alts.size() > 4) alts.resize(4);
    md.alternate.clear();
    md.gains.clear();
    for (auto& [h, g] : alts) {
        const double sign = h > md.baseline ? 1 : -1;
        double sum = 0;
        for (double& k : g.mix) k = std::isfinite(k) ? std::clamp(k, 0.0, 1.0) : 0, sum += k;
        if (sum <= 0) std::fill(g.mix, g.mix + 6, 0.0), g.mix[3] = 1, sum = 1;
        for (double& k : g.mix) k /= sum;
        const size_t n = std::min<size_t>({g.x.size(), g.y.size(), 32});
        g.x.resize(n);
        g.y.resize(n);
        double last = 0;
        for (size_t c = 0; c < n; ++c) {
            g.x[c] = std::isfinite(g.x[c]) ? std::clamp(g.x[c], last, 64.0) : last;
            last = g.x[c];
            const double y = std::isfinite(g.y[c]) ? std::clamp(g.y[c], -6.0, 6.0) : 0;
            g.y[c] = sign > 0 ? std::max(y, 0.0) : std::min(y, 0.0);
        }
        if (g.pchip || g.m.size() < n) {
            g.pchip = true;
            pchipSlopes(g);
        } else {
            g.m.resize(n);
            for (double& m : g.m) m = std::isfinite(m) ? m : 0;
        }
        md.alternate.push_back(h);
        md.gains.push_back(std::move(g));
    }
}

}  // namespace montage
