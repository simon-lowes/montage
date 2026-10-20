// Montage — pictures put into words, on this computer: a caption of what a frame shows, for logging and search
// (each shot's description) and for drafting audio description. Florence-2 large (Microsoft, MIT; ONNX export by the
// ONNX community, 4-bit weights with float maths) run by ONNX Runtime; downloaded on first use (790 MB). Decoding is
// greedy, so a picture is described the same way every time.
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "Image.h"
#include "ModelFiles.h"

namespace montage {

// $MONTAGE_DESCRIBE_MODEL names another folder, $MONTAGE_DESCRIBE_MODEL_URL a mirror.
const ModelPack& describeModel();
bool describeAvailable();  // built with ONNX Runtime

enum class PictureDetail {
    Brief,     // a line ("a man in a suit and tie")
    Detailed,  // a few sentences: who and what, where, the light and mood
};

// Florence-2's picture size (the frame is scaled to it, square).
constexpr int kDescribeInput = 768;

// The model, loaded once per process and shared.
class PictureDescriber {
public:
    static std::shared_ptr<PictureDescriber> load(std::string* error = nullptr);
    ~PictureDescriber();
    // What `picture` (any size; display-referred, over black) shows; empty with `error` on failure or cancel.
    std::string describe(const Image& picture, PictureDetail detail = PictureDetail::Detailed, std::string* error = nullptr,
                         const std::atomic<bool>* cancel = nullptr) const;
    struct Impl;

private:
    PictureDescriber();
    std::unique_ptr<Impl> d_;
};

// The frame of the file at `seconds` described.
std::string describeFrame(const std::string& path, double seconds, PictureDetail detail = PictureDetail::Detailed,
                          std::string* error = nullptr, const std::atomic<bool>* cancel = nullptr);

}  // namespace montage
