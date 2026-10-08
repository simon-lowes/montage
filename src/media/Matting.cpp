#include "Matting.h"

#include <QString>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

#include "SuperScale.h"

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

namespace montage {

const ModelPack& mattingModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "matte";
        p.title = "background removal model";
        p.directoryEnv = "MONTAGE_MATTE_MODEL";
        p.urlEnv = "MONTAGE_MATTE_MODEL_URL";
        // Xenova's ONNX export of MODNet's photographic portrait model.
        p.files = {
            {"modnet.onnx",
             "https://huggingface.co/Xenova/modnet/resolve/fa2fa546052fba4c08921230a26cc69a333fca12/onnx/model.onnx",
             "07c308cf0fc7e6e8b2065a12ed7fc07e1de8febb7dc7839d7b7f15dd66584df9", 25888640},
        };
        return p;
    }();
    return pack;
}

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool mattingAvailable() { return false; }
bool estimatePersonMatte(const Image&, ValueMap&, int, std::string* error) {
    if (error) *error = "This build of Montage has no ONNX Runtime";
    return false;
}

#else

bool mattingAvailable() { return true; }

namespace {

struct Model {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-matte"};
    std::unique_ptr<Ort::Session> session;
};

std::shared_ptr<Model> loadModel(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Model>();  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    const ModelPack& pack = mattingModel();
    if (!pack.installed()) {
        if (error) *error = "The background removal model is not downloaded";
        return nullptr;
    }
    try {
        auto model = std::make_shared<Model>();
        Ort::SessionOptions so;
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
        if (error) *error = std::string("The background removal model could not be loaded: ") + e.what();
        return nullptr;
    }
}

}  // namespace

bool estimatePersonMatte(const Image& img, ValueMap& out, int size, std::string* error) {
    out = {};
    if (img.empty()) {
        if (error) *error = "No picture";
        return false;
    }
    auto model = loadModel(error);
    if (!model) return false;
    // As the model was trained: the short side at `size`, both sides multiples of 32, values in -1..1.
    size = std::max(32, size / 32 * 32);
    const double k = double(size) / std::min(img.width, img.height);
    const int w = std::max(32, int(std::lround(img.width * k / 32)) * 32), h = std::max(32, int(std::lround(img.height * k / 32)) * 32);
    const Image small = resizeImage(img, w, h);
    std::vector<float> input(size_t(3) * size_t(w) * size_t(h));
    const size_t plane = size_t(w) * size_t(h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float* p = small.at(x, y);
            const float a = p[3] > 1e-6f ? p[3] : 1.0f;
            for (int c = 0; c < 3; ++c) input[size_t(c) * plane + size_t(y) * size_t(w) + size_t(x)] = std::clamp(p[c] / a, 0.0f, 1.0f) * 2 - 1;
        }
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const char* inNames[] = {"input"};
        const char* outNames[] = {"output"};
        const int64_t shape[4] = {1, 3, h, w};
        Ort::Value tensor = Ort::Value::CreateTensor<float>(cpu, input.data(), input.size(), shape, 4);
        auto result = model->session->Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1, outNames, 1);
        const auto dims = result[0].GetTensorTypeAndShapeInfo().GetShape();
        if (dims.size() < 2) {
            if (error) *error = "The background removal model gave no matte";
            return false;
        }
        const int oh = int(dims[dims.size() - 2]), ow = int(dims[dims.size() - 1]);
        const float* d = result[0].GetTensorData<float>();
        out.width = ow;
        out.height = oh;
        out.values.resize(size_t(ow) * size_t(oh));
        for (size_t i = 0; i < out.values.size(); ++i) out.values[i] = std::clamp(d[i], 0.0f, 1.0f);
        // The model sees the picture without its transparent parts: they are no one.
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x) {
                const float* p = small.at(std::min(x, w - 1), std::min(y, h - 1));
                out.values[size_t(y) * size_t(ow) + size_t(x)] *= std::clamp(p[3], 0.0f, 1.0f);
            }
        return true;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("Background removal failed: ") + e.what();
        return false;
    }
}

#endif

std::shared_ptr<const ValueMap> cachedPersonMatte(const Image& img, int size) {
    uint64_t key = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ULL; };
    mix(uint64_t(img.width)), mix(uint64_t(img.height)), mix(uint64_t(size));
    const size_t step = std::max<size_t>(1, img.px.size() / 8192);
    for (size_t i = 0; i < img.px.size(); i += step) {
        uint32_t bits;
        std::memcpy(&bits, &img.px[i], 4);
        mix(bits);
    }
    static std::mutex m;
    static std::deque<std::pair<uint64_t, std::shared_ptr<const ValueMap>>> cache;
    {
        std::lock_guard lock(m);
        for (const auto& [k, d] : cache)
            if (k == key) return d;
    }
    auto d = std::make_shared<ValueMap>();
    if (!estimatePersonMatte(img, *d, size)) return nullptr;
    std::lock_guard lock(m);
    cache.emplace_front(key, d);
    if (cache.size() > 8) cache.pop_back();
    return d;
}

}  // namespace montage
