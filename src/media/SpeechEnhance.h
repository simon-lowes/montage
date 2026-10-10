// Montage — speech enhancement (like Premiere's Enhance Speech or Descript's
// Studio Sound): DeepFilterNet3 (Hendrik Schröter et al., MIT or
// Apache-2.0) takes noise and room out of a voice. It runs on ONNX Runtime
// as one streaming graph (torchDF's export), 10 ms at a time at 48 kHz.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "ModelFiles.h"

namespace montage {

// The model (16 MB), downloaded on first use; $MONTAGE_SPEECH_MODEL names
// another folder, $MONTAGE_SPEECH_MODEL_URL a mirror.
const ModelPack& speechModel();
bool speechEnhancerAvailable();  // built with ONNX Runtime

// The model's output lags its input by this many samples (three 10 ms frames).
constexpr int kSpeechEnhanceDelay = 1440;

// Enhances 48 kHz mono speech. `out` lines up with `in` and has its length.
bool enhanceSpeech48k(const std::vector<float>& in, std::vector<float>& out, const std::atomic<bool>* cancel = nullptr,
                      std::string* error = nullptr);

}  // namespace montage
