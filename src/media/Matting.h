// Montage — people cut out of their background without a green screen (like
// CapCut's Remove Background or Resolve's Magic Mask on a person), with
// MODNet (Zhanghan Ke et al., Apache-2.0), a portrait matting model that
// keeps hair and soft edges. It runs on ONNX Runtime.
#pragma once

#include <memory>
#include <string>

#include "Image.h"
#include "ModelFiles.h"
#include "ValueMap.h"

namespace montage {

// The model (26 MB), downloaded on first use; $MONTAGE_MATTE_MODEL names
// another folder, $MONTAGE_MATTE_MODEL_URL a mirror.
const ModelPack& mattingModel();
bool mattingAvailable();  // built with ONNX Runtime

// How much of each pixel is a person: 1 on them, 0 on the background, between on soft edges
// (premultiplied RGBA in, display-referred 0..1). The model runs with the short side at `size`
// pixels (a multiple of 32). False if the model is missing or fails.
bool estimatePersonMatte(const Image& img, ValueMap& out, int size = 512, std::string* error = nullptr);

// The last few mattes, by what went in (sampled) and the size. Null on failure.
std::shared_ptr<const ValueMap> cachedPersonMatte(const Image& img, int size = 512);

}  // namespace montage
