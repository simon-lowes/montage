// Montage — adaptive HDR metadata (SMPTE ST 2094-50, colour volume transform application #5, published September
// 2026): a picture's HDR reference white, and gain curves that tone map it to whatever headroom a display has. HDR and
// SDR pictures composited on one screen then sit at the same reference white, and each display tones the picture
// down (or up) by curves the grade chose instead of its own. The message travels as ITU-T T.35 user data (HEVC SEI,
// AV1 metadata OBUs). This file has the message and its maths; render/AdaptiveHdr.h reads it from files, applies it
// to pictures and makes it for exports.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Model.h"

namespace montage {

// ---- The message ------------------------------------------------------------------------------------------------
// The metadata as an ITU-T T.35 message, from the country code on (0xB5, SMPTE's provider code 0x0090, its oriented
// code 0x0001, then the binary encoding of the standard's Annex C). Values are rounded to the encoding's steps.
std::vector<uint8_t> adaptiveHdrT35(const AdaptiveHdr& md);
// The application data alone (Annex C, after the oriented code).
std::vector<uint8_t> adaptiveHdrPayload(const AdaptiveHdr& md);
// Reads such a message (`t35` from the country code on); false if it is another provider's, needs a newer parser,
// or is cut short. Curves sent as "work them out" (and slopes left to PCHIP) are worked out.
bool parseAdaptiveHdrT35(const uint8_t* t35, size_t size, AdaptiveHdr& md);
bool parseAdaptiveHdrPayload(const uint8_t* data, size_t size, AdaptiveHdr& md);
// Whether a T.35 message (from the country code on) is ST 2094-50's.
bool isAdaptiveHdrT35(const uint8_t* t35, size_t size);

// ---- Its maths --------------------------------------------------------------------------------------------------
// The standard's own curves for a baseline headroom (C.3.8): an SDR rendition and one at a third of the way, each a
// knee at reference white with highlights compressed above it. Fills `alternate` and `gains` from `baseline`.
void computeAdaptiveCurves(AdaptiveHdr& md);
// PCHIP slopes for a curve's points (C.3.9).
void pchipSlopes(AdaptiveGainCurve& g);
// A curve's log2 gain at `x` (light relative to reference white).
double adaptiveGainCurve(const AdaptiveGainCurve& g, double x);
// The per-component log2 gain at colour `c` (in the gain application space, relative to reference white) for a
// display with `headroom` (log2 of its peak over reference white).
void adaptiveGain(const AdaptiveHdr& md, const double c[3], double headroom, double gain[3]);
// `c` tone mapped in place: c × 2^gain. Unchanged without curves, or at the picture's own headroom or above it when
// every rendition is below it.
void adaptiveToneMap(const AdaptiveHdr& md, double c[3], double headroom);
// Whether tone mapping to `headroom` changes anything.
bool adaptiveToneMaps(const AdaptiveHdr& md, double headroom);
// Brings values into the standard's ranges: headrooms 0..6 with no alternate at the baseline, ascending and at most
// four, curves with 1..32 points in 0..64 and log2 gains -6..6 on the side of their headroom, mixes summing to 1.
void sanitizeAdaptiveHdr(AdaptiveHdr& md);

}  // namespace montage
