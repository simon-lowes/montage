// Montage — OpenColorIO: colour space and display/view transforms from a
// studio's OCIO config (ACES and others), applied as a video effect. Built
// only when OpenColorIO 2.1+ is installed (MONTAGE_WITH_OCIO); otherwise
// these report that OCIO is unavailable.
#pragma once

#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

bool ocioAvailable();
std::string ocioVersion();
// Configs that ship inside OpenColorIO (2.2+), as "ocio://" names, newest ACES first.
std::vector<std::string> ocioBuiltinConfigs();

// The entries the "ocio" effect's string parameter `param` can take with the
// effect's current config (and display, for "view"): colour spaces for src
// and dst, displays, views, looks. Empty if the config cannot be loaded.
std::vector<std::string> ocioChoices(const Effect& e, const std::string& param);

// Applies the "ocio" effect to premultiplied RGBA in place. False (and the
// image untouched) if the config or transform is invalid.
bool applyOcio(const Effect& e, Image& img, std::string* error = nullptr);

// One of OCIO's built-in transforms (e.g. "ACEScct_to_ACES2065-1") on RGB
// triples in place; used to check Montage's own colour maths.
bool applyOcioBuiltin(const std::string& style, float* rgb, size_t count, bool inverse = false, std::string* error = nullptr);

}  // namespace montage
