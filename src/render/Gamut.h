// Montage — gamut tools: where colours sit on the CIE 1931 xy and 1976 u'v' chromaticity diagrams (the chromaticity
// scope), whether they fall outside a delivery gamut (the monitors' out-of-gamut check), and the ACES Reference Gamut
// Compression (ACES 1.3), which brings colours a camera saw outside AP1 back inside it smoothly.
#pragma once

#include <array>
#include <vector>

#include "ColorSpace.h"

namespace montage {

struct Xy {
    double x = 0, y = 0;
};

// CIE 1976 u'v' of an xy chromaticity.
Xy xyToUv(Xy c);
// The chromaticity of a pixel's code values in `space` (D65-relative, as primariesToXyz gives it). False for black
// and for colours with no light (X + Y + Z at or below 0).
bool codeToXy(const ColorSpace& space, const float code[3], Xy& out);
// The xy of a gamut's red, green and blue primaries and its white, as the scope draws them.
std::array<Xy, 4> gamutChromaticities(Primaries p);
// The spectral locus (the edge of the colours light can have), 380 to 700 nm every 5 nm, from the CIE 1931 2°
// standard observer.
const std::vector<Xy>& spectralLocus();

// Outside `target`: the colour (code values in `space`) needs a negative amount of one of the target's primaries
// (beyond a small tolerance for rounding). Black and white are always inside.
class GamutCheck {
public:
    GamutCheck(const ColorSpace& space, Primaries target);
    bool outside(const float code[3]) const;
    // The same on linear light in the space's primaries.
    bool outsideLinear(const double linear[3]) const;
    // Whether any colour of the space (with no negative code values) can be outside: not when its primaries are inside
    // the target's.
    bool possible() const { return possible_; }

private:
    const ColorSpace& space_;
    double m_[9];
    bool possible_ = true;
};

// Linear light (as toLinear gives it) of every 16-bit code value 0..65535 in transfer `t`, worked out once.
const std::vector<float>& linearTable16(Transfer t);

// ACES Reference Gamut Compression on linear AP1 (ACEScg) RGB: each channel's distance from the achromatic axis
// (the largest channel) compressed above its threshold so the distance at the limit lands on the gamut's edge. The
// published parameters: limits cyan 1.147, magenta 1.264, yellow 1.312; thresholds 0.815, 0.803, 0.880; power 1.2.
void acesGamutCompress(float rgb[3], bool inverse = false);
// The same on code values in `space`: into linear AP1 and back (a display transfer's negative values kept).
class AcesGamutCompressor {
public:
    explicit AcesGamutCompressor(const ColorSpace& space, bool inverse = false);
    void operator()(float rgb[3]) const;

private:
    const ColorSpace& space_;
    bool inverse_;
    double toAp1_[9], fromAp1_[9];
};

}  // namespace montage
