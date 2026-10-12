// Montage — a transfer function and its inverse as tables, for effects that work in light on every pixel.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "ColorSpace.h"

namespace montage {

// A transfer function and its inverse as tables (the HDR Palette and lens correction decode and encode every pixel):
// code values to light over 0..1.25, light to code values over 2^-24..2^8 by its logarithm; beyond them the functions
// themselves, and negative values mirrored.
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

}  // namespace montage
