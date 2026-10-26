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
// ACES 2.0's output transform for `display` and `view` of OpenColorIO's built-in ACES 2.0 studio config (OpenColorIO 2.5
// and later) on ACES2065-1 RGB triples in place, or display code values back to them (`inverse`); false (with why) where
// this OpenColorIO has not got it.
bool aces2OutputAvailable();
bool applyAces2Output(const std::string& display, const std::string& view, float* rgb, size_t count, bool inverse = false,
                      std::string* error = nullptr);
// A LUT or transform file OCIO reads (.clf, .cube, ...) on RGB triples in place; used to check Montage's own readers.
bool applyOcioFile(const std::string& path, float* rgb, size_t count, std::string* error = nullptr);

}  // namespace montage
