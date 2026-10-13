#include "Gamut.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>

namespace montage {

namespace {
// Code values to linear light, keeping the sign of the display transfers' (which only take 0 and up).
bool mirrored(Transfer t) { return t == Transfer::Bt1886 || t == Transfer::Srgb || t == Transfer::Pq || t == Transfer::Hlg; }

double linearOf(Transfer t, double v) {
    if (mirrored(t) && v < 0) return -toLinear(t, -v);
    return toLinear(t, v);
}

double codeOf(Transfer t, double l) {
    if (mirrored(t) && l < 0) return -fromLinear(t, -l);
    return fromLinear(t, l);
}

void apply(const double m[9], const double in[3], double out[3]) {
    for (int r = 0; r < 3; ++r) out[r] = m[r * 3] * in[0] + m[r * 3 + 1] * in[1] + m[r * 3 + 2] * in[2];
}

}  // namespace

Xy xyToUv(Xy c) {
    const double d = -2 * c.x + 12 * c.y + 3;
    return d == 0 ? Xy{} : Xy{4 * c.x / d, 9 * c.y / d};
}

bool codeToXy(const ColorSpace& space, const float code[3], Xy& out) {
    double l[3], xyz[3], m[9];
    for (int i = 0; i < 3; ++i) l[i] = linearOf(space.transfer, code[i]);
    primariesToXyz(space.primaries, m);
    apply(m, l, xyz);
    const double sum = xyz[0] + xyz[1] + xyz[2];
    if (!(sum > 1e-9)) return false;
    out = {xyz[0] / sum, xyz[1] / sum};
    return true;
}

std::array<Xy, 4> gamutChromaticities(Primaries p) {
    double m[9];
    primariesToXyz(p, m);
    std::array<Xy, 4> out;
    for (int i = 0; i < 4; ++i) {
        // A primary is its matrix column; the white all three together.
        const double X = i < 3 ? m[i] : m[0] + m[1] + m[2];
        const double Y = i < 3 ? m[3 + i] : m[3] + m[4] + m[5];
        const double Z = i < 3 ? m[6 + i] : m[6] + m[7] + m[8];
        const double sum = X + Y + Z;
        out[size_t(i)] = sum != 0 ? Xy{X / sum, Y / sum} : Xy{};
    }
    return out;
}

const std::vector<Xy>& spectralLocus() {
    // The CIE 1931 2° standard observer's colour matching functions as chromaticities, 380 to 700 nm every 5 nm.
    static const std::vector<Xy> locus = {
        {0.17411, 0.00496}, {0.17401, 0.00498}, {0.17380, 0.00492}, {0.17356, 0.00492}, {0.17334, 0.00480}, {0.17302, 0.00478},
        {0.17258, 0.00480}, {0.17209, 0.00483}, {0.17141, 0.00510}, {0.17030, 0.00579}, {0.16888, 0.00690}, {0.16690, 0.00856},
        {0.16441, 0.01086}, {0.16110, 0.01379}, {0.15664, 0.01770}, {0.15099, 0.02274}, {0.14396, 0.02970}, {0.13550, 0.03988},
        {0.12412, 0.05780}, {0.10959, 0.08684}, {0.09129, 0.13270}, {0.06871, 0.20072}, {0.04539, 0.29498}, {0.02346, 0.41270},
        {0.00817, 0.53842}, {0.00386, 0.65482}, {0.01387, 0.75019}, {0.03885, 0.81202}, {0.07430, 0.83380}, {0.11416, 0.82621},
        {0.15472, 0.80586}, {0.19288, 0.78163}, {0.22962, 0.75433}, {0.26578, 0.72432}, {0.30160, 0.69231}, {0.33736, 0.65885},
        {0.37310, 0.62445}, {0.40874, 0.58961}, {0.44406, 0.55471}, {0.47877, 0.52020}, {0.51249, 0.48659}, {0.54479, 0.45443},
        {0.57515, 0.42423}, {0.60293, 0.39650}, {0.62704, 0.37249}, {0.64823, 0.35139}, {0.66576, 0.33401}, {0.68008, 0.31975},
        {0.69150, 0.30834}, {0.70061, 0.29930}, {0.70792, 0.29203}, {0.71403, 0.28593}, {0.71903, 0.28093}, {0.72303, 0.27695},
        {0.72599, 0.27401}, {0.72827, 0.27173}, {0.72997, 0.27003}, {0.73109, 0.26891}, {0.73199, 0.26801}, {0.73272, 0.26728},
        {0.73342, 0.26658}, {0.73405, 0.26595}, {0.73439, 0.26561}, {0.73459, 0.26541}, {0.73469, 0.26531},
    };
    return locus;
}

GamutCheck::GamutCheck(const ColorSpace& space, Primaries target) : space_(space) {
    primariesMatrix(space.primaries, target, m_);
    possible_ = std::any_of(m_, m_ + 9, [](double v) { return v < -1e-6; });
}

bool GamutCheck::outside(const float code[3]) const {
    double l[3];
    for (int i = 0; i < 3; ++i) l[i] = linearOf(space_.transfer, code[i]);
    return outsideLinear(l);
}

bool GamutCheck::outsideLinear(const double l[3]) const {
    double t[3];
    apply(m_, l, t);
    const double big = std::max({std::fabs(t[0]), std::fabs(t[1]), std::fabs(t[2])});
    // (Tolerance: a few thousandths of the colour's strength, so rounding at the edge is not marked.)
    return big > 1e-6 && std::min({t[0], t[1], t[2]}) < -0.004 * big;
}

const std::vector<float>& linearTable16(Transfer t) {
    static std::mutex m;
    static std::map<Transfer, std::unique_ptr<const std::vector<float>>> tables;
    std::lock_guard lock(m);
    auto& slot = tables[t];
    if (!slot) {
        auto made = std::make_unique<std::vector<float>>(65536);
        for (int i = 0; i < 65536; ++i) (*made)[size_t(i)] = float(toLinear(t, i / 65535.0));
        slot = std::move(made);
    }
    return *slot;
}

namespace {
// The RGC's distance curve (ACES 1.3's LMT.Academy.GamutCompress.ctl).
double compressDistance(double dist, double lim, double thr, double pwr, bool inverse) {
    if (dist < thr) return dist;
    const double scl = (lim - thr) / std::pow(std::pow((1 - thr) / (lim - thr), -pwr) - 1, 1 / pwr);
    const double nd = (dist - thr) / scl;
    const double p = std::pow(nd, pwr);
    if (!inverse) return thr + scl * nd / std::pow(1 + p, 1 / pwr);
    if (dist >= thr + scl) return dist;  // (beyond what the forward curve reaches)
    return thr + scl * std::pow(-(p / (p - 1)), 1 / pwr);
}
}  // namespace

void acesGamutCompress(float rgb[3], bool inverse) {
    static constexpr double kLimit[3] = {1.147, 1.264, 1.312};
    static constexpr double kThreshold[3] = {0.815, 0.803, 0.880};
    static constexpr double kPower = 1.2;
    const double ach = std::max({double(rgb[0]), double(rgb[1]), double(rgb[2])});
    if (ach == 0) return;
    for (int i = 0; i < 3; ++i) {
        const double dist = (ach - rgb[i]) / std::fabs(ach);
        rgb[i] = float(ach - compressDistance(dist, kLimit[i], kThreshold[i], kPower, inverse) * std::fabs(ach));
    }
}

AcesGamutCompressor::AcesGamutCompressor(const ColorSpace& space, bool inverse) : space_(space), inverse_(inverse) {
    primariesMatrix(space.primaries, Primaries::Ap1, toAp1_);
    primariesMatrix(Primaries::Ap1, space.primaries, fromAp1_);
}

void AcesGamutCompressor::operator()(float rgb[3]) const {
    double l[3], a[3], o[3];
    for (int i = 0; i < 3; ++i) l[i] = linearOf(space_.transfer, rgb[i]);
    apply(toAp1_, l, a);
    float f[3] = {float(a[0]), float(a[1]), float(a[2])};
    acesGamutCompress(f, inverse_);
    for (int i = 0; i < 3; ++i) a[i] = f[i];
    apply(fromAp1_, a, o);
    for (int i = 0; i < 3; ++i) rgb[i] = float(codeOf(space_.transfer, o[i]));
}

}  // namespace montage
