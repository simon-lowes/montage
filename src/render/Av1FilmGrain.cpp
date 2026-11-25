// Montage — AV1 film grain parameters and synthesis (see Av1FilmGrain.h).
#include "render/Av1FilmGrain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "render/ColorSpace.h"
#include "render/GrainMatch.h"

namespace montage {

namespace {

const int16_t kGaussian[2048] = {
#include "render/Av1GrainTable.inc"
};

constexpr int kLumaW = 82, kLumaH = 73;

inline int round2(int x, int n) { return n == 0 ? x : (x + (1 << (n - 1))) >> n; }

// The synthesis's 16-bit shift register.
struct GrainRandom {
    uint16_t r = 0;
    int next(int bits) {
        const uint16_t bit = uint16_t((r ^ (r >> 1) ^ (r >> 3) ^ (r >> 12)) & 1);
        r = uint16_t((r >> 1) | (bit << 15));
        return (r >> (16 - bits)) & ((1 << bits) - 1);
    }
};

// The grain templates: 73 rows of 82 for the luma, and for each chroma plane 38 or 73 rows of 44 or 82.
struct GrainBlocks {
    int luma[kLumaH][kLumaW];
    int cb[kLumaH][kLumaW];
    int cr[kLumaH][kLumaW];
    int chromaW = kLumaW, chromaH = kLumaH;
};

// The random grain of each plane, shaped by the autoregression (7.18.3.3).
void makeGrain(const Av1FilmGrain& g, int bitDepth, bool subX, bool subY, bool mono, GrainBlocks& b) {
    const int center = 128 << (bitDepth - 8), lo = -center, hi = (256 << (bitDepth - 8)) - 1 - center;
    const int shift = 12 - bitDepth + g.grainScaleShift;
    const int lag = g.arLag;
    GrainRandom rnd;
    rnd.r = g.seed;
    for (int y = 0; y < kLumaH; ++y)
        for (int x = 0; x < kLumaW; ++x) b.luma[y][x] = g.numY > 0 ? round2(kGaussian[rnd.next(11)], shift) : 0;
    for (int y = 3; y < kLumaH; ++y)
        for (int x = 3; x < kLumaW - 3; ++x) {
            int sum = 0, pos = 0;
            for (int dy = -lag; dy <= 0; ++dy)
                for (int dx = -lag; dx <= lag; ++dx) {
                    if (dy == 0 && dx == 0) break;
                    sum += g.arY[pos++] * b.luma[y + dy][x + dx];
                }
            b.luma[y][x] = std::clamp(b.luma[y][x] + round2(sum, g.arShift), lo, hi);
        }
    if (mono) return;
    b.chromaW = subX ? 44 : 82, b.chromaH = subY ? 38 : 73;
    const bool anyCb = g.numCb > 0 || g.chromaFromLuma, anyCr = g.numCr > 0 || g.chromaFromLuma;
    rnd.r = uint16_t(g.seed ^ 0xb524);
    for (int y = 0; y < b.chromaH; ++y)
        for (int x = 0; x < b.chromaW; ++x) b.cb[y][x] = anyCb ? round2(kGaussian[rnd.next(11)], shift) : 0;
    rnd.r = uint16_t(g.seed ^ 0x49d8);
    for (int y = 0; y < b.chromaH; ++y)
        for (int x = 0; x < b.chromaW; ++x) b.cr[y][x] = anyCr ? round2(kGaussian[rnd.next(11)], shift) : 0;
    for (int y = 3; y < b.chromaH; ++y)
        for (int x = 3; x < b.chromaW - 3; ++x) {
            int sum0 = 0, sum1 = 0, pos = 0;
            for (int dy = -lag; dy <= 0; ++dy)
                for (int dx = -lag; dx <= lag; ++dx) {
                    const int c0 = g.arCb[pos], c1 = g.arCr[pos];
                    if (dy == 0 && dx == 0) {
                        if (g.numY > 0) {
                            // (the luma's grain under this chroma sample)
                            int luma = 0;
                            const int lx = ((x - 3) << int(subX)) + 3, ly = ((y - 3) << int(subY)) + 3;
                            for (int i = 0; i <= int(subY); ++i)
                                for (int j = 0; j <= int(subX); ++j) luma += b.luma[ly + i][lx + j];
                            luma = round2(luma, int(subX) + int(subY));
                            sum0 += luma * c0, sum1 += luma * c1;
                        }
                        break;
                    }
                    sum0 += c0 * b.cb[y + dy][x + dx];
                    sum1 += c1 * b.cr[y + dy][x + dx];
                    ++pos;
                }
            b.cb[y][x] = std::clamp(b.cb[y][x] + round2(sum0, g.arShift), lo, hi);
            b.cr[y][x] = std::clamp(b.cr[y][x] + round2(sum1, g.arShift), lo, hi);
        }
}

// A plane's scaling function at each 8-bit level, straight lines between its points (7.18.3.4).
void scalingTable(int n, const uint8_t* value, const uint8_t* scaling, int lut[256]) {
    if (n <= 0) {
        std::fill(lut, lut + 256, 0);
        return;
    }
    for (int i = 0; i < value[0]; ++i) lut[i] = scaling[0];
    for (int i = 0; i + 1 < n; ++i) {
        const int dy = scaling[i + 1] - scaling[i], dx = value[i + 1] - value[i];
        if (dx <= 0) continue;
        const int delta = dy * ((65536 + (dx >> 1)) / dx);
        for (int x = 0; x < dx; ++x) lut[value[i] + x] = scaling[i] + ((x * delta + 32768) >> 16);
    }
    for (int i = value[n - 1]; i < 256; ++i) lut[i] = scaling[n - 1];
}

inline int scaleAt(const int lut[256], int index, int bitDepth) {
    const int shift = bitDepth - 8, x = index >> shift, rem = index - (x << shift);
    if (bitDepth == 8 || x == 255) return lut[x];
    return lut[x] + round2((lut[x + 1] - lut[x]) * rem, shift);
}

}  // namespace

bool Av1FilmGrain::sameParameters(const Av1FilmGrain& o) const {
    Av1FilmGrain a = *this, b = o;
    a.seed = b.seed = 0;
    // (what is past each count is not carried: not compared)
    auto tidy = [](Av1FilmGrain& g) {
        for (int i = g.numY; i < 14; ++i) g.yValue[i] = g.yScaling[i] = 0;
        for (int i = g.numCb; i < 10; ++i) g.cbValue[i] = g.cbScaling[i] = 0;
        for (int i = g.numCr; i < 10; ++i) g.crValue[i] = g.crScaling[i] = 0;
        const int n = g.lumaCoefficients();
        for (int i = g.numY > 0 ? n : 0; i < 24; ++i) g.arY[i] = 0;
        const int chroma = g.numY > 0 ? n + 1 : n;
        for (int i = (g.chromaFromLuma || g.numCb > 0) ? chroma : 0; i < 25; ++i) g.arCb[i] = 0;
        for (int i = (g.chromaFromLuma || g.numCr > 0) ? chroma : 0; i < 25; ++i) g.arCr[i] = 0;
        if (g.numCb == 0) g.cbMult = 128, g.cbLumaMult = 192, g.cbOffset = 256;
        if (g.numCr == 0) g.crMult = 128, g.crLumaMult = 192, g.crOffset = 256;
    };
    tidy(a), tidy(b);
    return a.apply == b.apply && a.numY == b.numY && std::memcmp(a.yValue, b.yValue, 14) == 0 && std::memcmp(a.yScaling, b.yScaling, 14) == 0 &&
           a.chromaFromLuma == b.chromaFromLuma && a.numCb == b.numCb && a.numCr == b.numCr && std::memcmp(a.cbValue, b.cbValue, 10) == 0 &&
           std::memcmp(a.cbScaling, b.cbScaling, 10) == 0 && std::memcmp(a.crValue, b.crValue, 10) == 0 && std::memcmp(a.crScaling, b.crScaling, 10) == 0 &&
           a.scalingShift == b.scalingShift && a.arLag == b.arLag && std::memcmp(a.arY, b.arY, 24) == 0 && std::memcmp(a.arCb, b.arCb, 25) == 0 &&
           std::memcmp(a.arCr, b.arCr, 25) == 0 && a.arShift == b.arShift && a.grainScaleShift == b.grainScaleShift && a.cbMult == b.cbMult &&
           a.cbLumaMult == b.cbLumaMult && a.cbOffset == b.cbOffset && a.crMult == b.crMult && a.crLumaMult == b.crLumaMult && a.crOffset == b.crOffset &&
           a.overlap == b.overlap && a.clipRestricted == b.clipRestricted;
}

bool Av1FilmGrain::valid(std::string* why, bool subsampled) const {
    auto no = [&](const char* text) {
        if (why) *why = text;
        return false;
    };
    if (numY < 0 || numY > 14 || numCb < 0 || numCb > 10 || numCr < 0 || numCr > 10) return no("too many scaling points");
    if (scalingShift < 8 || scalingShift > 11) return no("the scaling shift is 8 to 11");
    if (arLag < 0 || arLag > 3) return no("the autoregression lag is 0 to 3");
    if (arShift < 6 || arShift > 9) return no("the autoregression shift is 6 to 9");
    if (grainScaleShift < 0 || grainScaleShift > 3) return no("the grain scale shift is 0 to 3");
    if (cbMult < 0 || cbMult > 255 || cbLumaMult < 0 || cbLumaMult > 255 || cbOffset < 0 || cbOffset > 511 || crMult < 0 || crMult > 255 ||
        crLumaMult < 0 || crLumaMult > 255 || crOffset < 0 || crOffset > 511)
        return no("a chroma multiplier or offset is out of range");
    for (int i = 1; i < numY; ++i)
        if (yValue[i] <= yValue[i - 1]) return no("the luma's scaling points must rise");
    for (int i = 1; i < numCb; ++i)
        if (cbValue[i] <= cbValue[i - 1]) return no("the Cb scaling points must rise");
    for (int i = 1; i < numCr; ++i)
        if (crValue[i] <= crValue[i - 1]) return no("the Cr scaling points must rise");
    if (chromaFromLuma && (numCb > 0 || numCr > 0)) return no("chroma scaled from the luma has no points of its own");
    if (subsampled && (numCb > 0) != (numCr > 0)) return no("4:2:0 grain has points for both Cb and Cr or for neither");
    return true;
}

void av1ApplyFilmGrain(const Av1FilmGrain& g, Av1GrainPicture& pic) {
    if (!g.apply || pic.width <= 0 || pic.height <= 0 || !pic.plane[0]) return;
    const int w = pic.width, h = pic.height, depth = pic.bitDepth;
    const bool mono = pic.mono || !pic.plane[1] || !pic.plane[2];
    const bool subX = !mono && pic.subX, subY = !mono && pic.subY;
    const int planes = mono ? 1 : 3;
    auto blocks = std::make_unique<GrainBlocks>();
    makeGrain(g, depth, subX, subY, mono, *blocks);
    const int center = 128 << (depth - 8), lo = -center, hi = (256 << (depth - 8)) - 1 - center;

    // The noise in stripes of 32 luma rows (two more for the overlap), each block of 32 taken from the template at a
    // random offset (7.18.3.5).
    const int stripes = (((h + 1) / 2) + 15) / 16;
    struct Stripe {
        std::vector<int16_t> v[3];
    };
    std::vector<Stripe> stripe(static_cast<size_t>(stripes));
    int wide[3], tall[3];
    for (int p = 0; p < planes; ++p) {
        const bool sx = p > 0 && subX, sy = p > 0 && subY;
        wide[p] = ((w + 1) / 2 + 16) * (sx ? 1 : 2) + 36;
        tall[p] = 34 >> int(sy);
    }
    GrainRandom rnd;
    for (int num = 0; num < stripes; ++num) {
        for (int p = 0; p < planes; ++p) stripe[size_t(num)].v[p].assign(size_t(wide[p]) * size_t(tall[p]), 0);
        rnd.r = g.seed;
        rnd.r ^= uint16_t(((num * 37 + 178) & 255) << 8);
        rnd.r ^= uint16_t((num * 173 + 105) & 255);
        for (int x = 0; x < (w + 1) / 2; x += 16) {
            const int r = rnd.next(8), offsetX = r >> 4, offsetY = r & 15;
            for (int p = 0; p < planes; ++p) {
                const bool sx = p > 0 && subX, sy = p > 0 && subY;
                const int ox = sx ? 6 + offsetX : 9 + offsetX * 2, oy = sy ? 6 + offsetY : 9 + offsetY * 2;
                int16_t* out = stripe[size_t(num)].v[p].data();
                const auto& block = p == 0 ? blocks->luma : p == 1 ? blocks->cb : blocks->cr;
                for (int i = 0; i < tall[p]; ++i)
                    for (int j = 0; j < (34 >> int(sx)); ++j) {
                        int v = block[oy + i][ox + j];
                        const size_t at = size_t(i) * size_t(wide[p]) + size_t(sx ? x + j : x * 2 + j);
                        if (!sx) {
                            if (j < 2 && g.overlap && x > 0) {
                                const int old = out[at];
                                v = j == 0 ? old * 27 + v * 17 : old * 17 + v * 27;
                                v = std::clamp(round2(v, 5), lo, hi);
                            }
                        } else if (j == 0 && g.overlap && x > 0) {
                            const int old = out[at];
                            v = std::clamp(round2(old * 23 + v * 22, 5), lo, hi);
                        }
                        out[at] = int16_t(v);
                    }
            }
        }
    }
    // The noise of a plane's sample, the stripes blended where they meet.
    auto noiseAt = [&](int p, int x, int y) {
        const bool sy = p > 0 && subY;
        const int num = y >> (5 - int(sy)), i = y - (num << (5 - int(sy)));
        int v = stripe[size_t(num)].v[p][size_t(i) * size_t(wide[p]) + size_t(x)];
        if (num > 0 && g.overlap) {
            if (!sy && i < 2) {
                const int old = stripe[size_t(num - 1)].v[p][size_t(i + 32) * size_t(wide[p]) + size_t(x)];
                v = i == 0 ? old * 27 + v * 17 : old * 17 + v * 27;
                v = std::clamp(round2(v, 5), lo, hi);
            } else if (sy && i < 1) {
                const int old = stripe[size_t(num - 1)].v[p][size_t(i + 16) * size_t(wide[p]) + size_t(x)];
                v = std::clamp(round2(old * 23 + v * 22, 5), lo, hi);
            }
        }
        return v;
    };

    int lut[3][256];
    scalingTable(g.numY, g.yValue, g.yScaling, lut[0]);
    if (g.chromaFromLuma) {
        std::memcpy(lut[1], lut[0], sizeof lut[0]);
        std::memcpy(lut[2], lut[0], sizeof lut[0]);
    } else {
        scalingTable(g.numCb, g.cbValue, g.cbScaling, lut[1]);
        scalingTable(g.numCr, g.crValue, g.crScaling, lut[2]);
    }
    const int top = (256 << (depth - 8)) - 1;
    const int minValue = g.clipRestricted ? 16 << (depth - 8) : 0;
    const int maxLuma = g.clipRestricted ? 235 << (depth - 8) : top;
    const int maxChroma = g.clipRestricted ? (pic.identityMatrix ? maxLuma : 240 << (depth - 8)) : top;
    // The chroma first: it is scaled by the luma as decoded, before the luma's own grain.
    if (!mono) {
        const int cw = (w + int(subX)) >> int(subX), ch = (h + int(subY)) >> int(subY);
        for (int p = 1; p < 3; ++p) {
            const int points = p == 1 ? g.numCb : g.numCr;
            if (points <= 0 && !g.chromaFromLuma) continue;
            const int mult = p == 1 ? g.cbMult : g.crMult, lumaMult = p == 1 ? g.cbLumaMult : g.crLumaMult, offset = p == 1 ? g.cbOffset : g.crOffset;
            for (int y = 0; y < ch; ++y) {
                const uint16_t* lumaRow = pic.plane[0] + ptrdiff_t(y << int(subY)) * pic.stride[0];
                uint16_t* row = pic.plane[p] + ptrdiff_t(y) * pic.stride[p];
                for (int x = 0; x < cw; ++x) {
                    const int lx = x << int(subX), next = std::min(lx + 1, w - 1);
                    const int average = subX ? round2(lumaRow[lx] + lumaRow[next], 1) : lumaRow[lx];
                    const int orig = row[x];
                    int merged = average;
                    if (!g.chromaFromLuma) {
                        const int combined = average * (lumaMult - 128) + orig * (mult - 128);
                        merged = std::clamp((combined >> 6) + ((offset - 256) << (depth - 8)), 0, top);
                    }
                    const int noise = round2(scaleAt(lut[p], merged, depth) * noiseAt(p, x, y), g.scalingShift);
                    row[x] = uint16_t(std::clamp(orig + noise, minValue, maxChroma));
                }
            }
        }
    }
    if (g.numY > 0)
        for (int y = 0; y < h; ++y) {
            uint16_t* row = pic.plane[0] + ptrdiff_t(y) * pic.stride[0];
            for (int x = 0; x < w; ++x) {
                const int orig = row[x];
                const int noise = round2(scaleAt(lut[0], orig, depth) * noiseAt(0, x, y), g.scalingShift);
                row[x] = uint16_t(std::clamp(orig + noise, minValue, maxLuma));
            }
        }
}

Av1GrainTemplate av1GrainTemplate(const Av1FilmGrain& in, int bitDepth, bool subX, bool subY) {
    Av1GrainTemplate t;
    Av1FilmGrain g = in;
    auto blocks = std::make_unique<GrainBlocks>();
    double sum2[3] = {}, cross[2] = {}, under2[2] = {}, neighbour = 0;
    int64_t n[3] = {};
    int64_t pairs = 0;
    constexpr int kSeeds = 24;
    for (int k = 0; k < kSeeds; ++k) {
        g.seed = av1GrainSeed(k * 7 + 1, 0x51ed);
        makeGrain(g, bitDepth, subX, subY, false, *blocks);
        // (the part the stripes take their blocks from)
        for (int y = 9; y < kLumaH; ++y)
            for (int x = 9; x < kLumaW - 3; ++x) {
                const double v = blocks->luma[y][x];
                sum2[0] += v * v, ++n[0];
                if (x + 1 < kLumaW - 3) neighbour += v * blocks->luma[y][x + 1], ++pairs;
            }
        for (int y = 6; y < blocks->chromaH; ++y)
            for (int x = 6; x < blocks->chromaW - 3; ++x) {
                double luma = 0;
                const int lx = ((x - 3) << int(subX)) + 3, ly = ((y - 3) << int(subY)) + 3;
                for (int i = 0; i <= int(subY); ++i)
                    for (int j = 0; j <= int(subX); ++j) luma += blocks->luma[ly + i][lx + j];
                luma /= double((1 + int(subX)) * (1 + int(subY)));
                const double cb = blocks->cb[y][x], cr = blocks->cr[y][x];
                sum2[1] += cb * cb, sum2[2] += cr * cr, ++n[1], ++n[2];
                cross[0] += cb * luma, cross[1] += cr * luma;
                under2[0] += luma * luma;
            }
    }
    under2[1] = under2[0];
    for (int p = 0; p < 3; ++p) t.sigma[p] = n[p] ? std::sqrt(sum2[p] / double(n[p])) : 0;
    for (int p = 0; p < 2; ++p) {
        const double d = std::sqrt(sum2[p + 1] * under2[p]);
        t.chromaWithLuma[p] = d > 0 ? cross[p] / d : 0;
    }
    t.lumaNeighbour = sum2[0] > 0 && pairs ? (neighbour / double(pairs)) / (sum2[0] / double(n[0])) : 0;
    return t;
}

uint16_t av1GrainSeed(int64_t frame, uint32_t stream) {
    // (a mix of the frame's number and the stream's, so neighbouring frames share nothing)
    uint64_t x = uint64_t(frame) * 0x9E3779B97F4A7C15ull + (uint64_t(stream) << 32 | 0x7f4a7c15u);
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    const uint16_t seed = uint16_t(x & 0xffff);
    return seed ? seed : uint16_t(0x5a17);
}

namespace {

// The profile's power at `f` cycles a pixel of the frame it was measured on (as far as it was measured: nothing past
// the diagonal's end).
double powerAt(const GrainProfile& g, double f) {
    const double x = f * 2.0 * GrainProfile::kBins - 0.5;
    if (x <= 0) return g.spectrum[0];
    if (x >= GrainProfile::kBins - 1) return f > 0.7072 ? 0.0 : g.spectrum[GrainProfile::kBins - 1];
    const int i = int(x);
    return g.spectrum[i] + (g.spectrum[i + 1] - g.spectrum[i]) * (x - i);
}

// The grain's autocorrelation on a plane whose samples are `sx` by `sy` pixels of the measured frame, at whole samples'
// distances (dx to ±6, dy to ±3), 1 at no distance; and the share of the grain's power the plane keeps (what is finer
// than its samples is averaged away).
struct PlaneGrain {
    double r[7][13] = {};  // [dy + 3][dx + 6]
    double kept = 1;
    double at(int dx, int dy) const { return r[dy + 3][dx + 6]; }
};
PlaneGrain planeGrain(const GrainProfile& g, double sx, double sy) {
    PlaneGrain out;
    constexpr int n = 48;
    double total = 0, all = 0;
    for (int v = 0; v < n; ++v)
        for (int u = 0; u < n; ++u) {
            // (the plane's frequencies, to half a cycle a sample each way; one quadrant, the others its mirror)
            const double fx = (u + 0.5) / n * 0.5, fy = (v + 0.5) / n * 0.5;
            const double p = powerAt(g, std::hypot(fx / sx, fy / sy));
            total += p;
            for (int dy = -3; dy <= 3; ++dy)
                for (int dx = -6; dx <= 6; ++dx) out.r[dy + 3][dx + 6] += p * std::cos(2 * M_PI * fx * dx) * std::cos(2 * M_PI * fy * dy);
            all += powerAt(g, std::hypot(fx, fy));
        }
    if (total > 0)
        for (auto& row : out.r)
            for (double& v : row) v /= total;
    // (the plane's quadrant covers 1 / (sx sy) of the measured frame's when its samples are larger)
    out.kept = all > 0 ? std::min(1.0, total / (sx * sy) / all) : 1;
    if (sx <= 1 && sy <= 1) out.kept = 1;
    return out;
}

// The autoregression coefficients (in the order a frame header lists them) that give a plane's grain that
// autocorrelation: the normal equations over the samples before each one, to `lag` away. `white` is a share of white
// grain mixed in before they are solved: soft grain's own equations are near singular, and their coefficients large
// ones of both signs that do not survive being rounded to the header's whole numbers.
bool autoregression(const PlaneGrain& pg, int lag, double white, std::vector<double>& a) {
    std::vector<std::pair<int, int>> pos;
    for (int dy = -lag; dy <= 0; ++dy)
        for (int dx = -lag; dx <= lag; ++dx) {
            if (dy == 0 && dx == 0) break;
            pos.push_back({dx, dy});
        }
    const size_t n = pos.size();
    a.assign(n, 0);
    if (n == 0) return true;
    std::vector<double> m(n * (n + 1));
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) m[i * (n + 1) + j] = pg.at(pos[i].first - pos[j].first, pos[i].second - pos[j].second) + (i == j ? white : 0);
        m[i * (n + 1) + n] = pg.at(pos[i].first, pos[i].second);
    }
    for (size_t c = 0; c < n; ++c) {
        size_t best = c;
        for (size_t r = c + 1; r < n; ++r)
            if (std::fabs(m[r * (n + 1) + c]) > std::fabs(m[best * (n + 1) + c])) best = r;
        if (std::fabs(m[best * (n + 1) + c]) < 1e-12) return false;
        if (best != c)
            for (size_t k = 0; k <= n; ++k) std::swap(m[c * (n + 1) + k], m[best * (n + 1) + k]);
        for (size_t r = 0; r < n; ++r) {
            if (r == c) continue;
            const double f = m[r * (n + 1) + c] / m[c * (n + 1) + c];
            if (f == 0) continue;
            for (size_t k = c; k <= n; ++k) m[r * (n + 1) + k] -= f * m[c * (n + 1) + k];
        }
    }
    for (size_t i = 0; i < n; ++i) a[i] = m[i * (n + 1) + n] / m[i * (n + 1) + i];
    return std::all_of(a.begin(), a.end(), [](double v) { return std::isfinite(v); });
}

// How far the spectrum of the grain these coefficients make is from the profile's, for the luma and the chroma: an
// autoregression's spectrum is one over the squared size of one less its coefficients' transform, so it is had from the
// header's whole numbers as they are. Both are taken with a power of one over the plane's frequencies; the squares of
// their differences, summed, over the profile's own squares.
double spectrumError(const Av1FilmGrain& g, const GrainProfile& profile, const double sampleX[2], const double sampleY[2]) {
    constexpr int n = 20;
    double error = 0;
    for (int p = 0; p < 2; ++p) {
        const int8_t* coeffs = p == 0 ? g.arY : g.arCb;
        std::vector<double> made, want;
        made.reserve(size_t(n) * 2 * n), want.reserve(size_t(n) * 2 * n);
        double madeSum = 0, wantSum = 0;
        for (int v = -n; v < n; ++v)
            for (int u = 0; u < n; ++u) {
                const double fx = (u + 0.5) / n * 0.5, fy = (v + 0.5) / n * 0.5;
                double re = 1, im = 0;
                int pos = 0;
                for (int dy = -g.arLag; dy <= 0; ++dy)
                    for (int dx = -g.arLag; dx <= g.arLag; ++dx) {
                        if (dy == 0 && dx == 0) break;
                        const double a = double(coeffs[pos++]) / double(1 << g.arShift), w = 2 * M_PI * (fx * dx + fy * dy);
                        re -= a * std::cos(w), im -= a * std::sin(w);
                    }
                const double m = 1 / std::max(1e-9, re * re + im * im), q = powerAt(profile, std::hypot(fx / sampleX[p], fy / sampleY[p]));
                made.push_back(m), want.push_back(q);
                madeSum += m, wantSum += q;
            }
        if (!(madeSum > 0) || !(wantSum > 0)) continue;
        double d = 0, w2 = 0;
        for (size_t i = 0; i < made.size(); ++i) {
            const double x = made[i] / madeSum - want[i] / wantSum;
            d += x * x, w2 += (want[i] / wantSum) * (want[i] / wantSum);
        }
        error += d / w2;
    }
    return error;
}

}  // namespace

Av1FilmGrain av1FilmGrainFrom(const GrainProfile& g, const ColorSpace& space, const Av1GrainTarget& t, double amount) {
    Av1FilmGrain out;
    if (!g.valid() || amount <= 0 || t.width <= 0) return out;
    const int depth = std::clamp(t.bitDepth, 8, 12);
    const double kr = t.kr, kb = t.kb, kg = 1 - kr - kb;
    // Each plane's weights on R′G′B′, and its code values' span on the 8-bit scale.
    const double weights[3][3] = {{kr, kg, kb},
                                  {-kr / (2 * (1 - kb)), -kg / (2 * (1 - kb)), 0.5},
                                  {0.5, -kg / (2 * (1 - kr)), -kb / (2 * (1 - kr))}};
    const double span[3] = {t.fullRange ? 255.0 : 219.0, t.fullRange ? 255.0 : 224.0, t.fullRange ? 255.0 : 224.0};
    const double lowest = t.fullRange ? 0 : 16, highest = t.fullRange ? 255 : 235;
    // How large a sample of each plane is, in pixels of the frame the grain was measured on.
    const double scale = double(std::max(1, g.width)) / double(t.width);
    const PlaneGrain luma = planeGrain(g, scale, scale);
    const PlaneGrain chroma = planeGrain(g, scale * (t.subX ? 2 : 1), scale * (t.subY ? 2 : 1));

    // The standard deviation of each plane's grain, in 8-bit code values, where the luma is at 8-bit level `level`
    // (grey there); and how the chroma planes' grain goes with the luma's.
    auto sigmas = [&](double level, double sigma[3], double with[2]) {
        const double code = std::clamp((level - lowest) / (highest - lowest), 0.0, 1.0);
        const double light = std::max(0.0, double(toLinear(space.transfer, code)));
        // (each channel's grain in code values: half of what one deviation up and one down span through the transfer
        // function, which stays finite at black, where the function's slope does not)
        double s[3];
        for (int c = 0; c < 3; ++c) {
            const double a = grainAmplitude(g, c, std::max(1e-6, light)) * amount;
            s[c] = std::max(0.0, double(fromLinear(space.transfer, light + a)) - double(fromLinear(space.transfer, std::max(0.0, light - a)))) / 2;
        }
        const double rho[3][3] = {{1, g.correlation[0], g.correlation[1]}, {g.correlation[0], 1, g.correlation[2]}, {g.correlation[1], g.correlation[2], 1}};
        auto cov = [&](const double a[3], const double b[3]) {
            double v = 0;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) v += a[i] * b[j] * rho[i][j] * s[i] * s[j];
            return v;
        };
        for (int p = 0; p < 3; ++p) sigma[p] = std::sqrt(std::max(0.0, cov(weights[p], weights[p]))) * span[p] * std::sqrt(p == 0 ? luma.kept : chroma.kept);
        for (int p = 1; p < 3; ++p) {
            const double d = std::sqrt(std::max(0.0, cov(weights[0], weights[0]) * cov(weights[p], weights[p])));
            with[p - 1] = d > 1e-18 ? std::clamp(cov(weights[0], weights[p]) / d, -0.98, 0.98) : 0;
        }
    };
    // The scaling functions' points: black, where there is no grain (bars and fades to black stay clean; half of it
    // would be clipped away there in any case), four levels above it, and from there to white evenly.
    constexpr int kPoints = 14, kChromaPoints = 10;
    auto levelOf = [&](int i, int count) { return i == 0 ? int(lowest) : int(std::lround(lowest + 4 + (highest - lowest - 4) * (i - 1) / (count - 2))); };
    double ySigma[kPoints] = {}, cSigma[2][kChromaPoints] = {}, with[2] = {}, strongest[3] = {};
    int yLevel[kPoints], cLevel[kChromaPoints];
    {
        double weight = 0, sum[2] = {};
        yLevel[0] = cLevel[0] = levelOf(0, kPoints);
        for (int i = 1; i < kPoints; ++i) {
            yLevel[i] = levelOf(i, kPoints);
            double s[3], w[2];
            sigmas(yLevel[i], s, w);
            ySigma[i] = s[0];
            strongest[0] = std::max(strongest[0], s[0]);
            // (the chroma's share of the luma's grain: where the grain is strong counts most)
            weight += s[0], sum[0] += w[0] * s[0], sum[1] += w[1] * s[0];
        }
        for (int i = 1; i < kChromaPoints; ++i) {
            cLevel[i] = levelOf(i, kChromaPoints);
            double s[3], w[2];
            sigmas(cLevel[i], s, w);
            cSigma[0][i] = s[1], cSigma[1][i] = s[2];
            strongest[1] = std::max(strongest[1], s[1]), strongest[2] = std::max(strongest[2], s[2]);
        }
        if (weight > 0) with[0] = sum[0] / weight, with[1] = sum[1] / weight;
    }
    // (grain under a twentieth of an 8-bit code value is none)
    if (strongest[0] < 0.05) return out;
    out.apply = true;
    out.overlap = true;
    out.clipRestricted = !t.fullRange;
    // (in 4:2:0 both chroma planes have points or neither does: a decoder refuses one without the other)
    bool anyCb = strongest[1] >= 0.05, anyCr = strongest[2] >= 0.05;
    if (t.subX && t.subY) anyCb = anyCr = anyCb || anyCr;

    // The autoregression: to three samples away where the grain is soft, none where it is white. Its coefficients
    // in the header's whole numbers, at the finest shift they fit. Of the lags and the shares of white grain mixed in
    // before the equations are solved, the one whose spectrum is nearest the profile's, among those whose grain, drawn
    // as a decoder draws it, settles.
    auto setCoefficients = [&](int lag, double mixed) {
        std::vector<double> ay, ac;
        if (!autoregression(luma, lag, mixed, ay) || !autoregression(chroma, lag, mixed, ac)) return false;
        double largest = 0;
        for (double v : ay) largest = std::max(largest, std::fabs(v));
        for (double v : ac) largest = std::max(largest, std::fabs(v));
        int shift = 9;
        while (shift > 6 && largest * (1 << shift) > 127) --shift;
        if (largest * (1 << shift) > 127) return false;
        out.arLag = lag, out.arShift = shift;
        std::memset(out.arY, 0, sizeof out.arY), std::memset(out.arCb, 0, sizeof out.arCb), std::memset(out.arCr, 0, sizeof out.arCr);
        for (size_t i = 0; i < ay.size(); ++i) out.arY[i] = int8_t(std::lround(ay[i] * (1 << shift)));
        for (size_t i = 0; i < ac.size(); ++i) out.arCb[i] = out.arCr[i] = int8_t(std::lround(ac[i] * (1 << shift)));
        return true;
    };
    // (points to make the template with, while the coefficients are settled)
    out.numY = 1, out.numCb = anyCb ? 1 : 0, out.numCr = anyCr ? 1 : 0;
    const double white = 512.0 / double(1 << (12 - depth));  // (the template's deviation with no autoregression)
    Av1GrainTemplate made;
    {
        const double sampleX[2] = {scale, scale * (t.subX ? 2 : 1)}, sampleY[2] = {scale, scale * (t.subY ? 2 : 1)};
        Av1FilmGrain best = out;  // (no autoregression: white grain)
        best.arLag = 0;
        double bestError = spectrumError(best, g, sampleX, sampleY);
        const bool isWhite = std::fabs(luma.at(1, 0)) < 0.03 && std::fabs(luma.at(1, 1)) < 0.03;
        for (int lag = 3; lag >= 1 && !isWhite; --lag)
            for (const double mixed : {1e-4, 3e-4, 1e-3, 2e-3, 5e-3, 1e-2, 2e-2, 5e-2, 0.1}) {
                if (!setCoefficients(lag, mixed)) continue;
                made = av1GrainTemplate(out, depth, t.subX, t.subY);
                // (settled: no more than eight times the white grain it is made from, and not clipped flat)
                if (!(made.sigma[0] > 0 && made.sigma[0] < white * 8 && made.sigma[1] < white * 8)) continue;
                const double error = spectrumError(out, g, sampleX, sampleY);
                if (error < bestError) best = out, bestError = error;
            }
        out = best;
        made = av1GrainTemplate(out, depth, t.subX, t.subY);
    }
    // The chroma's share of the luma's grain: the coefficient (one for each chroma plane, after the others) whose
    // template goes with the luma's as the grain's colours do.
    const int at = out.lumaCoefficients();
    for (int p = 0; p < 2; ++p) {
        if (!(p == 0 ? anyCb : anyCr)) continue;
        int8_t* coeffs = p == 0 ? out.arCb : out.arCr;
        int lo = -128, hi = 127;
        while (hi - lo > 1) {
            const int mid = (lo + hi) / 2;
            coeffs[at] = int8_t(mid);
            (av1GrainTemplate(out, depth, t.subX, t.subY).chromaWithLuma[p] < with[p] ? lo : hi) = mid;
        }
        coeffs[at] = int8_t(lo);
        const double below = av1GrainTemplate(out, depth, t.subX, t.subY).chromaWithLuma[p];
        coeffs[at] = int8_t(hi);
        const double above = av1GrainTemplate(out, depth, t.subX, t.subY).chromaWithLuma[p];
        coeffs[at] = int8_t(std::fabs(below - with[p]) <= std::fabs(above - with[p]) ? lo : hi);
    }
    made = av1GrainTemplate(out, depth, t.subX, t.subY);

    // The scaling functions: the grain's deviation at each level over the template's, at the finest shifts that hold
    // the strongest. (The template's deviation is in samples of the coded depth; on the 8-bit scale, 2^(depth - 8) less.)
    const double unit = double(1 << (depth - 8));
    auto need = [&](int p, double sigma, int scalingShift) { return made.sigma[p] > 0 ? sigma * unit * double(1 << scalingShift) / made.sigma[p] : 0.0; };
    int scalingShift = 11, grainShift = 0;
    auto largest = [&](int shift) {
        double v = need(0, strongest[0], shift);
        if (anyCb) v = std::max(v, need(1, strongest[1], shift));
        if (anyCr) v = std::max(v, need(2, strongest[2], shift));
        return v;
    };
    while (scalingShift > 8 && largest(scalingShift) > 255) --scalingShift;
    if (scalingShift == 11) {
        // (weak grain: a smaller template, so its scaling keeps its steps fine; each shift halves the template)
        while (grainShift < 3 && largest(11) * double(1 << (grainShift + 1)) <= 255 && 12 - depth + grainShift + 1 <= 7) ++grainShift;
        if (grainShift > 0) {
            out.grainScaleShift = grainShift;
            made = av1GrainTemplate(out, depth, t.subX, t.subY);
        }
    }
    out.scalingShift = scalingShift;
    auto scaling = [&](int p, double sigma) { return uint8_t(std::clamp(std::lround(need(p, sigma, scalingShift)), 0L, 255L)); };
    out.numY = kPoints;
    for (int i = 0; i < kPoints; ++i) out.yValue[i] = uint8_t(yLevel[i]), out.yScaling[i] = scaling(0, ySigma[i]);
    out.numCb = anyCb ? kChromaPoints : 0, out.numCr = anyCr ? kChromaPoints : 0;
    for (int i = 0; i < kChromaPoints; ++i) {
        out.cbValue[i] = out.crValue[i] = uint8_t(cLevel[i]);
        out.cbScaling[i] = anyCb ? scaling(1, cSigma[0][i]) : 0;
        out.crScaling[i] = anyCr ? scaling(2, cSigma[1][i]) : 0;
    }
    // (the chroma's scaling is looked up by the luma under it alone: the grain's strength follows the picture's light)
    out.cbMult = out.crMult = 128, out.cbLumaMult = out.crLumaMult = 192, out.cbOffset = out.crOffset = 256;
    if (!anyCb) std::memset(out.arCb, 0, sizeof out.arCb);
    if (!anyCr) std::memset(out.arCr, 0, sizeof out.arCr);
    return out;
}

std::string av1FilmGrainTable(const std::vector<Av1GrainTableEntry>& entries) {
    std::ostringstream o;
    o << "filmgrn1\n";
    const Av1FilmGrain* last = nullptr;
    for (const Av1GrainTableEntry& e : entries) {
        const Av1FilmGrain& g = e.grain;
        const bool update = g.apply && (!last || !last->sameParameters(g));
        o << "E " << e.start << ' ' << e.end << ' ' << (g.apply ? 1 : 0) << ' ' << g.seed << ' ' << (update ? 1 : 0) << '\n';
        if (!update) continue;
        last = &g;
        o << "\tp " << g.arLag << ' ' << g.arShift << ' ' << g.grainScaleShift << ' ' << g.scalingShift << ' ' << (g.chromaFromLuma ? 1 : 0) << ' '
          << (g.overlap ? 1 : 0) << ' ' << g.cbMult << ' ' << g.cbLumaMult << ' ' << g.cbOffset << ' ' << g.crMult << ' ' << g.crLumaMult << ' '
          << g.crOffset << '\n';
        auto points = [&](const char* label, int n, const uint8_t* value, const uint8_t* scaling) {
            o << '\t' << label << ' ' << n;
            for (int i = 0; i < n; ++i) o << ' ' << int(value[i]) << ' ' << int(scaling[i]);
            o << '\n';
        };
        points("sY", g.numY, g.yValue, g.yScaling);
        points("sCb", g.numCb, g.cbValue, g.cbScaling);
        points("sCr", g.numCr, g.crValue, g.crScaling);
        auto coeffs = [&](const char* label, int n, const int8_t* c) {
            o << '\t' << label;
            for (int i = 0; i < n; ++i) o << ' ' << int(c[i]);
            o << '\n';
        };
        const int n = g.lumaCoefficients();
        coeffs("cY", n, g.arY);
        coeffs("cCb", n + 1, g.arCb);
        coeffs("cCr", n + 1, g.arCr);
    }
    return o.str();
}

bool parseAv1FilmGrainTable(const std::string& text, std::vector<Av1GrainTableEntry>& out, std::string* error) {
    out.clear();
    auto no = [&](const std::string& why) {
        if (error) *error = why;
        out.clear();
        return false;
    };
    std::istringstream in(text);
    std::string token;
    if (!(in >> token) || token != "filmgrn1") return no("not a film grain table (no filmgrn1)");
    auto number = [&](long long lo, long long hi, long long& v) {
        std::string s;
        if (!(in >> s) || s.empty() || s.size() > 20) return false;
        char* end = nullptr;
        v = std::strtoll(s.c_str(), &end, 10);
        return end && *end == 0 && v >= lo && v <= hi;
    };
    bool model = false;
    Av1FilmGrain last;
    while (in >> token) {
        if (token != "E") return no("an entry must begin with E");
        Av1GrainTableEntry e;
        long long start, end, apply, seed, update;
        if (!number(0, INT64_MAX, start) || !number(0, INT64_MAX, end) || !number(0, 1, apply) || !number(0, 65535, seed) || !number(0, 1, update))
            return no("an entry's line is not five numbers");
        if (start >= end || (!out.empty() && start < out.back().end)) return no("the entries' times must rise");
        if (apply && !update && !model) return no("the first grain has no parameters");
        Av1FilmGrain g = last;
        if (update) {
            g = Av1FilmGrain{};
            long long v[12];
            if (!(in >> token) || token != "p") return no("no p line");
            const long long hi[12] = {3, 9, 3, 11, 1, 1, 255, 255, 511, 255, 255, 511}, lo[12] = {0, 6, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0};
            for (int i = 0; i < 12; ++i)
                if (!number(lo[i], hi[i], v[i])) return no("a parameter of the p line is out of range");
            g.arLag = int(v[0]), g.arShift = int(v[1]), g.grainScaleShift = int(v[2]), g.scalingShift = int(v[3]);
            g.chromaFromLuma = v[4] != 0, g.overlap = v[5] != 0;
            g.cbMult = int(v[6]), g.cbLumaMult = int(v[7]), g.cbOffset = int(v[8]), g.crMult = int(v[9]), g.crLumaMult = int(v[10]), g.crOffset = int(v[11]);
            auto points = [&](const char* label, int most, int& n, uint8_t* value, uint8_t* scaling) {
                long long count, a, b;
                if (!(in >> token) || token != label || !number(0, most, count)) return false;
                n = int(count);
                for (int i = 0; i < n; ++i) {
                    if (!number(0, 255, a) || !number(0, 255, b)) return false;
                    value[i] = uint8_t(a), scaling[i] = uint8_t(b);
                }
                return true;
            };
            if (!points("sY", 14, g.numY, g.yValue, g.yScaling) || !points("sCb", 10, g.numCb, g.cbValue, g.cbScaling) ||
                !points("sCr", 10, g.numCr, g.crValue, g.crScaling))
                return no("a line of scaling points is wrong");
            auto coeffs = [&](const char* label, int n, int8_t* c) {
                long long a;
                if (!(in >> token) || token != label) return false;
                for (int i = 0; i < n; ++i) {
                    if (!number(-128, 127, a)) return false;
                    c[i] = int8_t(a);
                }
                return true;
            };
            const int n = g.lumaCoefficients();
            if (!coeffs("cY", n, g.arY) || !coeffs("cCb", n + 1, g.arCb) || !coeffs("cCr", n + 1, g.arCr)) return no("a line of coefficients is wrong");
            model = true;
            last = g;
        }
        g.apply = apply != 0;
        g.seed = uint16_t(seed);
        std::string why;
        if (g.apply && !g.valid(&why)) return no(why);
        e.start = start, e.end = end, e.grain = g;
        out.push_back(e);
    }
    if (out.empty()) return no("the table has no entries");
    return true;
}

}  // namespace montage
