// Montage — checks shared by the ONNX Runtime users (Segmenter, Diarizer).
// Include after onnxruntime_cxx_api.h.
#pragma once

#include <string>

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

}  // namespace montage
