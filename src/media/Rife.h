// Montage — AI in-between frames for slow motion and speed ramps with RIFE
// 4.26 (Practical-RIFE, Zhewei Huang et al., MIT), like Resolve's Speed
// Warp or Topaz. The weights are the authors' own release, downloaded on
// first use and unpacked beside the network's graph, which is compiled in
// (scripts/gen-rife-graph.py). It runs on ONNX Runtime.
#pragma once

#include <string>

#include "Image.h"
#include "ModelFiles.h"

namespace montage {

// The official RIFEv4.26_0921.zip (23 MB); $MONTAGE_RIFE_MODEL names another
// folder, $MONTAGE_RIFE_MODEL_URL a mirror.
const ModelPack& rifeModel();
bool rifeAvailable();  // built with ONNX Runtime

// The picture at fraction t (0..1) between a and b (premultiplied, the same
// size): the colour from RIFE, the alpha blended. False if the model is
// missing or fails.
bool rifeInterpolate(const Image& a, const Image& b, double t, Image& out, std::string* error = nullptr);

// The same, remembering the last few results by `cacheKey` and t (a paused
// or repainted frame is not run again); falls back to nothing on failure.
bool cachedRife(const Image& a, const Image& b, double t, const std::string& cacheKey, Image& out);

}  // namespace montage
