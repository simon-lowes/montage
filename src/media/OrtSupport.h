// Montage — what the ONNX Runtime users share: the runtime check, and models opened and run on the chosen device.
// Include after onnxruntime_cxx_api.h.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace montage {

// The ONNX Runtime library that was loaded supports the API these headers
// use. (On Windows an older onnxruntime.dll in System32 can be picked up
// instead of Montage's own; the C++ API would then crash on first use.)
inline bool ortUsable(std::string* error) {
    const OrtApiBase* base = OrtGetApiBase();
    if (base && base->GetApi(ORT_API_VERSION)) return true;
    if (error)
        *error = std::string("The ONNX Runtime library that was loaded (") + (base ? base->GetVersionString() : "unknown") +
                 ") is older than the one Montage was built for (API " + std::to_string(ORT_API_VERSION) + ")";
    return false;
}

// Where a model may run: anywhere (the device media/Inference.h chooses), or on the processor only, for models run
// step by step (a frame or a token at a time), whose state would be copied to and from a device at every step.
enum class ModelRuns { Anywhere, OnProcessor };

// A model as the loaders hold it: an ONNX Runtime session on the device chosen when it opened. When that device
// fails while running it (out of memory, a driver error), the model opens again on the processor and the run is
// repeated, and it stays there; when Tools › AI Models Run On changes, it opens again on the next run. Runs on a GPU or
// NPU take turns (not every device takes two at once); runs on the processor do not need to.
class OrtModel {
public:
    // Throws as Ort::Session does when even the processor cannot open it.
    OrtModel(Ort::Env& env, const ORTCHAR_T* path, const Ort::SessionOptions& options, ModelRuns where);
    OrtModel(Ort::Env& env, std::vector<unsigned char> bytes, const Ort::SessionOptions& options, ModelRuns where);
    std::vector<Ort::Value> Run(const Ort::RunOptions& run, const char* const* inputNames, const Ort::Value* inputs, size_t inputCount,
                                const char* const* outputNames, size_t outputCount);
    size_t GetInputCount() const;
    size_t GetOutputCount() const;
    Ort::AllocatedStringPtr GetInputNameAllocated(size_t i, OrtAllocator* allocator) const;
    Ort::AllocatedStringPtr GetOutputNameAllocated(size_t i, OrtAllocator* allocator) const;
    bool accelerated() const;  // on a GPU or NPU (as far as ONNX Runtime was asked)

private:
    std::shared_ptr<Ort::Session> open(bool device);  // (with mutex_ held, or before the model is shared)
    Ort::Env& env_;
    std::basic_string<ORTCHAR_T> path_;
    std::vector<unsigned char> bytes_;
    Ort::SessionOptions options_;
    ModelRuns where_;
    mutable std::mutex mutex_;
    std::shared_ptr<Ort::Session> session_;
    bool accelerated_ = false;
    int generation_ = 0;  // the mode it was opened for
};

// A model opened on the device media/Inference.h chooses (the processor when that device will not take it, or when
// `where` says so), with `options` otherwise as given.
std::unique_ptr<OrtModel> openModel(Ort::Env& env, const ORTCHAR_T* path, const Ort::SessionOptions& options,
                                    ModelRuns where = ModelRuns::Anywhere);

}  // namespace montage
