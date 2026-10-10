#include "SpeechEnhance.h"

#include <QString>
#include <algorithm>
#include <memory>
#include <mutex>

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

namespace montage {

const ModelPack& speechModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "speech-enhance";
        p.title = "speech enhancement model";
        p.directoryEnv = "MONTAGE_SPEECH_MODEL";
        p.urlEnv = "MONTAGE_SPEECH_MODEL_URL";
        p.files = {
            {"deepfilternet3.onnx",
             "https://huggingface.co/kimtos-labs/denoiser-dfn3/resolve/888f33c41851d7168ae31c3637c323b9e2f3c2a4/"
             "denoiser_model.onnx",
             "fe5eb64fa2e4154c83f8e4935e82871c850c154387ee892e0ab65fe179e7d8c9", 16104687},
        };
        return p;
    }();
    return pack;
}

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool speechEnhancerAvailable() { return false; }
bool enhanceSpeech48k(const std::vector<float>&, std::vector<float>&, const std::atomic<bool>*, std::string* error) {
    if (error) *error = "This build of Montage cannot enhance speech (it was built without ONNX Runtime)";
    return false;
}

#else

bool speechEnhancerAvailable() { return true; }

namespace {

constexpr int kHop = 480;         // 10 ms at 48 kHz
constexpr int kStateSize = 45304;  // the export's flattened recurrent and buffer state

struct Model {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-speech"};
    std::unique_ptr<Ort::Session> session;
};

std::shared_ptr<Model> loadModel(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Model>();  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    const ModelPack& pack = speechModel();
    if (!pack.installed()) {
        if (error) *error = "The speech enhancement model is not downloaded";
        return nullptr;
    }
    try {
        auto model = std::make_shared<Model>();
        Ort::SessionOptions so;
        // One frame at a time is small work: threads would cost more than they give.
        // Channels run in parallel instead.
        so.SetIntraOpNumThreads(1);
        so.SetInterOpNumThreads(1);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        const std::wstring path = QString::fromStdString(pack.path(pack.files[0])).toStdWString();
#else
        const std::string path = pack.path(pack.files[0]);
#endif
        model->session = std::make_unique<Ort::Session>(model->env, path.c_str(), so);
        *cached = model;
        return model;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The speech enhancement model could not be loaded: ") + e.what();
        return nullptr;
    }
}

}  // namespace

bool enhanceSpeech48k(const std::vector<float>& in, std::vector<float>& out, const std::atomic<bool>* cancel, std::string* error) {
    auto model = loadModel(error);
    if (!model) return false;
    out.assign(in.size(), 0.0f);
    if (in.empty()) return true;
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<float> state(kStateSize, 0.0f), frame(kHop, 0.0f);
        float limit = 0;  // no cap here: the caller mixes with the original, lined up
        const int64_t frameShape[1] = {kHop}, stateShape[1] = {kStateSize};
        const char* inNames[] = {"input_frame", "states", "atten_lim_db"};
        const char* outNames[] = {"enhanced_audio_frame", "new_states"};
        // Enough frames to flush the delay out.
        const size_t total = in.size() + size_t(kSpeechEnhanceDelay);
        for (size_t pos = 0; pos < total; pos += kHop) {
            if (cancel && cancel->load()) return false;
            for (int i = 0; i < kHop; ++i) frame[size_t(i)] = pos + size_t(i) < in.size() ? in[pos + size_t(i)] : 0.0f;
            Ort::Value inputs[3] = {Ort::Value::CreateTensor<float>(cpu, frame.data(), frame.size(), frameShape, 1),
                                    Ort::Value::CreateTensor<float>(cpu, state.data(), state.size(), stateShape, 1),
                                    Ort::Value::CreateTensor<float>(cpu, &limit, 1, nullptr, 0)};
            auto outputs = model->session->Run(Ort::RunOptions{nullptr}, inNames, inputs, 3, outNames, 2);
            const float* y = outputs[0].GetTensorData<float>();
            const float* ns = outputs[1].GetTensorData<float>();
            std::copy(ns, ns + kStateSize, state.begin());
            for (int i = 0; i < kHop; ++i) {
                const int64_t at = int64_t(pos) + i - kSpeechEnhanceDelay;
                if (at >= 0 && at < int64_t(out.size())) out[size_t(at)] = y[i];
            }
        }
        return true;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("Speech enhancement failed: ") + e.what();
        return false;
    }
}

#endif

}  // namespace montage
