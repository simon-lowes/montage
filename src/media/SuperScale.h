// Montage — Super Scale (like Resolve's Super Scale or Topaz): footage and
// stills enlarged up to four times with Real-ESRGAN (realesr-general-x4v3,
// Xintao Wang et al., BSD-3-Clause), which redraws edges and texture instead
// of blurring them as plain scaling does. It runs on ONNX Runtime, in tiles.
#pragma once

#include <atomic>
#include <string>

#include "Image.h"
#include "ModelFiles.h"

namespace montage {

// The model (4.6 MB), downloaded on first use; $MONTAGE_UPSCALE_MODEL names
// another folder, $MONTAGE_UPSCALE_MODEL_URL a mirror.
const ModelPack& upscaleModel();
bool upscalerAvailable();  // built with ONNX Runtime

// Four times the size: the model on the colour (un-premultiplied, 0..1),
// the alpha enlarged smoothly.
bool superScale4x(const Image& in, Image& out, std::string* error = nullptr, const std::atomic<bool>* cancel = nullptr);

// `in` at w x h (up to four times larger): the model's 4x, then averaged
// down to the size. `strength` (0..1) mixes in plain scaling.
bool superScale(const Image& in, int w, int h, Image& out, double strength = 1, std::string* error = nullptr,
                const std::atomic<bool>* cancel = nullptr);

// Box-filtered resize (area averaging down, bilinear up): plain scaling.
Image resizeImage(const Image& in, int w, int h);

}  // namespace montage
