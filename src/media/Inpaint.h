// Montage — object removal (like Resolve's Object Removal or After Effects'
// Content-Aware Fill): what is under a mask painted out and filled with
// what would plausibly be behind it, by LaMa (Roman Suvorov et al.,
// Apache-2.0) on ONNX Runtime. The area round the mask is worked on at
// 512 x 512, so small objects keep their surroundings' detail.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Image.h"
#include "ModelFiles.h"

namespace montage {

// The model (208 MB), downloaded on first use; $MONTAGE_INPAINT_MODEL names
// another folder, $MONTAGE_INPAINT_MODEL_URL a mirror.
const ModelPack& inpaintModel();
bool inpaintAvailable();  // built with ONNX Runtime

// `img` (premultiplied) with the area where `mask` (one value per pixel, 0..1) is over
// half filled in; the mask's soft edge blends the fill in. False if the model is
// missing or fails; `out` is then a copy of `img`.
bool inpaint(const Image& img, const std::vector<float>& mask, Image& out, std::string* error = nullptr);

// The same, remembering the last few results by picture and mask (sampled).
bool cachedInpaint(const Image& img, const std::vector<float>& mask, Image& out);

}  // namespace montage
