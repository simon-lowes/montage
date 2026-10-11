#include "HdrView.h"

#include <QtCore/qfloat16.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace montage {

namespace {

constexpr double kSdrWhiteNits = 203;  // BT.2408 reference white
constexpr double kHlgPeak = 1000, kHlgGamma = 1.2;
constexpr int kLutSize = 4096;

// Above 75 % of the headroom, the brightest channel rolls off towards the headroom, all three scaled alike (hue and
// saturation kept), when the picture can be brighter than the display.
void rollOff(double l[3], double headroom, double contentPeak) {
    if (contentPeak <= headroom) return;
    const double mx = std::max({l[0], l[1], l[2]});
    const double knee = 0.75 * headroom;
    if (mx <= knee) return;
    const double out = knee + (headroom - knee) * (1 - std::exp(-(mx - knee) / (headroom - knee)));
    for (int i = 0; i < 3; ++i) l[i] *= out / mx;
}

// A display space's code values to linear light (relative to SDR white; HLG's scene light), interpolated in a table.
struct Decoder {
    std::array<float, kLutSize + 1> lut{};
    explicit Decoder(Transfer t) {
        for (int i = 0; i <= kLutSize; ++i) lut[size_t(i)] = float(toLinear(t, double(i) / kLutSize));
    }
    float operator()(float code) const {
        const float x = std::clamp(code, 0.0f, 1.0f) * kLutSize;
        const int i = std::min(int(x), kLutSize - 1);
        const float f = x - float(i);
        return lut[size_t(i)] + (lut[size_t(i) + 1] - lut[size_t(i)]) * f;
    }
};

}  // namespace

double hdrContentPeak(const ColorSpace& space, double hdrPeakNits) {
    if (space.transfer == Transfer::Pq) return std::max(hdrPeakNits, 1.0) / kSdrWhiteNits;
    if (space.transfer == Transfer::Hlg) return kHlgPeak / kSdrWhiteNits;
    return 1;
}

void hdrViewPixel(float rgb[3], const ColorSpace& space, double hdrPeakNits, double headroom) {
    headroom = std::max(headroom, 1.0);
    double l[3];
    if (space.sceneReferred) {
        // Camera log and linear: their display rendering for SDR.
        convertPixel(rgb, space, rec709Space(), hdrPeakNits);
        for (int i = 0; i < 3; ++i) l[i] = toLinear(Transfer::Bt1886, rgb[i]);
    } else {
        double c[3];
        for (int i = 0; i < 3; ++i) c[i] = toLinear(space.transfer, std::clamp(rgb[i], 0.0f, 1.0f));
        if (space.transfer == Transfer::Hlg) {
            // HLG's OOTF on its 1000-nit reference display: scene light to display light.
            const double ys = std::max(0.0, 0.2627 * c[0] + 0.6780 * c[1] + 0.0593 * c[2]);
            const double k = kHlgPeak * std::pow(ys, kHlgGamma - 1) / kSdrWhiteNits;
            for (double& v : c) v *= k;
        }
        double m[9];
        primariesMatrix(space.primaries, Primaries::Bt709, m);
        for (int r = 0; r < 3; ++r) l[r] = m[r * 3] * c[0] + m[r * 3 + 1] * c[1] + m[r * 3 + 2] * c[2];
        rollOff(l, headroom, hdrContentPeak(space, hdrPeakNits));
    }
    for (int i = 0; i < 3; ++i) rgb[i] = float(l[i]);
}

void hdrViewImage(const Image& in, const ColorSpace& space, double hdrPeakNits, double headroom, std::vector<uint16_t>& out, float scale) {
    out.resize(size_t(std::max(0, in.width)) * size_t(std::max(0, in.height)) * 4);
    if (in.empty()) return;
    headroom = std::max(headroom, 1.0);
    if (space.sceneReferred) {
        // (rare: sequences work in display spaces) one pixel at a time
        parallelRows(in.height, [&](int r0, int r1) {
            for (int y = r0; y < r1; ++y) {
                const float* p = in.row(y);
                std::vector<float> row(size_t(in.width) * 4);
                for (int x = 0; x < in.width; ++x) {
                    float c[3] = {p[x * 4], p[x * 4 + 1], p[x * 4 + 2]};
                    hdrViewPixel(c, space, hdrPeakNits, headroom);
                    for (int i = 0; i < 3; ++i) row[size_t(x) * 4 + size_t(i)] = c[i] * scale;
                    row[size_t(x) * 4 + 3] = 1;
                }
                qFloatToFloat16(reinterpret_cast<qfloat16*>(&out[size_t(y) * size_t(in.width) * 4]), row.data(), qsizetype(row.size()));
            }
        });
        return;
    }
    const Decoder decode(space.transfer);
    double md[9];
    primariesMatrix(space.primaries, Primaries::Bt709, md);
    float m[9];
    for (int i = 0; i < 9; ++i) m[i] = float(md[i]);
    const bool hlg = space.transfer == Transfer::Hlg;
    const double contentPeak = hdrContentPeak(space, hdrPeakNits);
    parallelRows(in.height, [&](int r0, int r1) {
        std::vector<float> row(size_t(in.width) * 4);
        for (int y = r0; y < r1; ++y) {
            const float* p = in.row(y);
            for (int x = 0; x < in.width; ++x) {
                float c[3] = {decode(p[x * 4]), decode(p[x * 4 + 1]), decode(p[x * 4 + 2])};
                if (hlg) {
                    const float ys = std::max(0.0f, 0.2627f * c[0] + 0.6780f * c[1] + 0.0593f * c[2]);
                    const float k = float(kHlgPeak / kSdrWhiteNits) * std::pow(ys, float(kHlgGamma - 1));
                    for (float& v : c) v *= k;
                }
                double l[3];
                for (int r = 0; r < 3; ++r) l[r] = m[r * 3] * c[0] + m[r * 3 + 1] * c[1] + m[r * 3 + 2] * c[2];
                rollOff(l, headroom, contentPeak);
                float* o = &row[size_t(x) * 4];
                for (int i = 0; i < 3; ++i) o[i] = float(l[i]) * scale;
                o[3] = 1;
            }
            qFloatToFloat16(reinterpret_cast<qfloat16*>(&out[size_t(y) * size_t(in.width) * 4]), row.data(), qsizetype(row.size()));
        }
    });
}

}  // namespace montage
