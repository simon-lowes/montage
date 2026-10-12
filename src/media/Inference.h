// Montage — where the on-device models run (Premiere's, Resolve's and Final Cut's GPU and Neural Engine inference):
// every model is opened through ONNX Runtime's own choice of device: the GPU or NPU when there is one it can use
// (CoreML on a Mac, DirectML or Windows ML on Windows, CUDA or OpenVINO where the runtime has them, and any
// execution-provider library registered), the processor otherwise, and the processor again whenever a device will
// not take a model. Tools › AI Models Run On chooses Automatic (the GPU first), Power Saving (an NPU first) or the
// processor only; MONTAGE_INFERENCE=cpu|auto|efficient overrides it for headless runs.
#pragma once

#include <string>
#include <vector>

namespace montage {

enum class InferenceMode { Auto, Efficient, Cpu };
void setInferenceMode(InferenceMode m);
InferenceMode inferenceMode();  // (MONTAGE_INFERENCE, when set, wins)
bool inferenceModeOverridden();  // MONTAGE_INFERENCE is set to a mode
const char* inferenceModeName(InferenceMode m);  // "auto", "efficient", "cpu"
bool parseInferenceMode(const std::string& name, InferenceMode& out);  // (any case)
// What the app and the command line both do first: the execution-provider libraries in `execution-providers` beside
// the program and in Montage's data folder registered (once), and the mode saved by Tools › AI Models Run On taken.
// How many libraries were registered.
int initInference();

struct InferenceDevice {
    std::string provider;  // ONNX Runtime's name for it ("CPUExecutionProvider", "CoreMLExecutionProvider", ...)
    std::string vendor;    // the hardware's maker
    std::string kind;      // CPU, GPU or NPU
};
// The devices ONNX Runtime can run models on here (the processor at least, when it loads).
std::vector<InferenceDevice> inferenceDevices();
// Execution-provider libraries in `folder` (a GPU vendor's or Windows ML's plug-in providers) registered with ONNX
// Runtime, so models can run on what they drive; how many were taken. Links to a library, and a library or a name
// already registered, are skipped; why a library was not taken is in lastInferenceLibraryError().
int registerInferenceLibraries(const std::string& folder);
std::string lastInferenceLibraryError();
// How many models went to the processor because the chosen device would not take or run them, and the last reason.
int inferenceFallbacks();
std::string lastInferenceFallback();
// A tiny model opened and run as the real ones are (with the chosen device); false with the reason if it fails.
bool inferenceCheck(std::string* error = nullptr);

}  // namespace montage
