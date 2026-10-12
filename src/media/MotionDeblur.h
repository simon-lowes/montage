// Montage — Motion Deblur (like Resolve 21's AI Motion Deblur): the smear a moving camera or subject leaves in a frame
// taken out by NAFNet (Liangyu Chen et al., "Simple Baselines for Image Restoration", MIT; its GoPro model, trained on
// the GoPro dataset of Seungjun Nah et al., CC BY 4.0), as OpenCV's model zoo publishes it in ONNX (MIT). It runs on ONNX
// Runtime in overlapping tiles. It is slow (seconds a frame at preview sizes, more at full size), so long stretches are
// best rendered ahead (Render In to Out) or baked in (Render and Replace).
#pragma once

#include <atomic>
#include <string>

#include "Image.h"
#include "ModelFiles.h"

namespace montage {

// The model (92 MB), downloaded on first use; $MONTAGE_DEBLUR_MODEL names another folder, $MONTAGE_DEBLUR_MODEL_URL a
// mirror.
const ModelPack& deblurModel();
bool deblurAvailable();  // built with ONNX Runtime

// `in` (premultiplied) with its motion blur taken out: the model on the colour (un-premultiplied, 0..1; the code values as
// they are, which is what it was trained on), alpha kept. `strength` (0..1) mixes the result over the original. Pictures
// larger than `tile` on a side go through in overlapping tiles, blended where they meet.
bool motionDeblur(const Image& in, Image& out, double strength = 1, std::string* error = nullptr,
                  const std::atomic<bool>* cancel = nullptr, int tile = 768);

}  // namespace montage
