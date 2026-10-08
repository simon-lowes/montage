// Montage — Generate LUT (Resolve's Generate 3D LUT, Premiere's Export .cube): a clip's colour grade baked into a
// .cube file for on-set monitors, cameras and other applications. Only effects that change each colour the same way
// wherever it is in the picture can be held by a LUT; spatial effects and masked ones are left out and named.
#pragma once

#include <string>
#include <vector>

#include "Processing.h"
#include "core/Model.h"

namespace montage {

// The effect maps each colour to one colour regardless of where it is (a grade a LUT can hold).
bool isColorOnlyEffect(const Effect& e, FrameTime t);

// The enabled colour-only effects of `effects`, in order, evaluated at `t`, sampled on a size^3 lattice over 0..1.
// `skipped` receives the display names of enabled effects that could not be included.
Lut3D bakeLut(const std::vector<Effect>& effects, FrameTime t, int size = 33, std::vector<std::string>* skipped = nullptr);

// Writes a 3D LUT as an Adobe / Resolve .cube file (red varying fastest).
bool writeCubeLut(const Lut3D& lut, const std::string& path, const std::string& title = {}, std::string* error = nullptr);

}  // namespace montage
