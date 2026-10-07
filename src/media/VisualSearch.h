// Montage — searching footage by what it shows ("a red car at night",
// "two people shaking hands"). Frames sampled through each video and the
// words of a query are embedded in one space by CLIP ViT-B/32 (OpenAI, MIT;
// int8 ONNX export), so the frames nearest a query are the ones it
// describes. Runs locally on ONNX Runtime; the model (155 MB) is downloaded
// on first use.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Image.h"
#include "ModelFiles.h"
#include "core/VisualIndex.h"

namespace montage {

// $MONTAGE_VISUAL_MODEL names another folder, $MONTAGE_VISUAL_MODEL_URL a mirror.
const ModelPack& visualModel();
bool visualSearchAvailable();  // built with ONNX Runtime

// The model, loaded once per process and shared (its sessions are thread safe).
class ClipModel {
public:
    static std::shared_ptr<ClipModel> load(std::string* error = nullptr);
    ~ClipModel();

    // CLIP's byte-level BPE tokens of `text`, with its start and end tokens (at most 77).
    std::vector<int64_t> tokens(const std::string& text) const;
    // Unit-length embeddings (512 values); empty on failure.
    std::vector<float> text(const std::string& query, std::string* error = nullptr) const;
    std::vector<std::vector<float>> images(const std::vector<Frame16Ptr>& frames, std::string* error = nullptr) const;

    struct Impl;

private:
    ClipModel();
    std::unique_ptr<Impl> d_;
};

// Embeds a frame every `step` seconds of a video (0: every 2 s, or every
// second for clips under 20 s), as `out`.
bool indexVideo(const std::string& path, double duration, VisualIndex& out, double step = 0,
                const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                std::string* error = nullptr);

// The size CLIP looks at: frames are scaled so their short side is this, then centre-cropped square.
constexpr int kClipInput = 224;

}  // namespace montage
