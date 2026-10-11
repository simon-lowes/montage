#include "DepthMap.h"

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

const ModelPack& depthModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "depth";
        p.title = "depth model";
        p.directoryEnv = "MONTAGE_DEPTH_MODEL";
        p.urlEnv = "MONTAGE_DEPTH_MODEL_URL";
        // The full-precision export: its 4-bit sibling (27 MB) differs by 4 % of the depth range on average.
        p.files = {
            {"depth_anything_v2_small.onnx",
             "https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/4472b7362082ad9968fee890ca0f1e5aca36b93d/"
             "onnx/model.onnx",
             "afb6a5c28f3b6bf1618c6e43f02073ef9dfdc70e937502d51603e57b0a1df10c", 99060839},
        };
        return p;
    }();
    return pack;
}

float ValueMap::at(double u, double v) const {
    if (empty()) return 0;
    const double x = std::clamp(u * width - 0.5, 0.0, double(width - 1)), y = std::clamp(v * height - 0.5, 0.0, double(height - 1));
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    const float fx = float(x - x0), fy = float(y - y0);
    const float* r0 = &values[size_t(y0) * size_t(width)];
    const float* r1 = &values[size_t(y1) * size_t(width)];
    const float a = r0[x0] + (r0[x1] - r0[x0]) * fx, b = r1[x0] + (r1[x1] - r1[x0]) * fx;
    return a + (b - a) * fy;
}

std::vector<float> ValueMap::resized(int w, int h) const {
    std::vector<float> out(size_t(std::max(0, w)) * size_t(std::max(0, h)), 0.0f);
    if (empty()) return out;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) out[size_t(y) * size_t(w) + size_t(x)] = at((x + 0.5) / w, (y + 0.5) / h);
    return out;
}

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool depthAvailable() { return false; }
bool estimateDepth(const Image&, DepthMap&, int, std::string* error) {
    if (error) *error = "This build of Montage has no ONNX Runtime";
    return false;
}

#else

bool depthAvailable() { return true; }

namespace {

struct Model {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-depth"};
    std::unique_ptr<Ort::Session> session;
};

std::shared_ptr<Model> loadModel(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Model>();  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    const ModelPack& pack = depthModel();
    if (!pack.installed()) {
        if (error) *error = "The depth model is not downloaded";
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
        if (error) *error = std::string("The depth model could not be loaded: ") + e.what();
        return nullptr;
    }
}

}  // namespace

bool estimateDepth(const Image& img, DepthMap& out, int size, std::string* error) {
    out = {};
    if (img.empty()) {
        if (error) *error = "No picture";
        return false;
    }
    auto model = loadModel(error);
    if (!model) return false;
    // As the model was trained: the short side at `size`, both sides multiples of 14, ImageNet normalisation.
    size = std::max(14, size / 14 * 14);
    const double k = double(size) / std::min(img.width, img.height);
    const int w = std::max(14, int(std::lround(img.width * k / 14)) * 14), h = std::max(14, int(std::lround(img.height * k / 14)) * 14);
    const Image small = resizeImage(img, w, h);
    const float mean[3] = {0.485f, 0.456f, 0.406f}, stdev[3] = {0.229f, 0.224f, 0.225f};
    std::vector<float> input(size_t(3) * size_t(w) * size_t(h));
    const size_t plane = size_t(w) * size_t(h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float* p = small.at(x, y);
            const float a = p[3] > 1e-6f ? p[3] : 1.0f;
            for (int c = 0; c < 3; ++c)
                input[size_t(c) * plane + size_t(y) * size_t(w) + size_t(x)] = (std::clamp(p[c] / a, 0.0f, 1.0f) - mean[c]) / stdev[c];
        }
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const char* inNames[] = {"pixel_values"};
        const char* outNames[] = {"predicted_depth"};
        const int64_t shape[4] = {1, 3, h, w};
        Ort::Value tensor = Ort::Value::CreateTensor<float>(cpu, input.data(), input.size(), shape, 4);
        auto result = model->session->Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1, outNames, 1);
        const auto dims = result[0].GetTensorTypeAndShapeInfo().GetShape();
        if (dims.size() < 2) {
            if (error) *error = "The depth model gave no map";
            return false;
        }
        const int oh = int(dims[dims.size() - 2]), ow = int(dims[dims.size() - 1]);
        const float* d = result[0].GetTensorData<float>();
        out.width = ow;
        out.height = oh;
        out.values.assign(d, d + size_t(ow) * size_t(oh));
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("Depth estimation failed: ") + e.what();
        return false;
    }
    // Disparity (nearer is larger) stretched between its 1st and 99th percentiles.
    std::vector<float> sorted = out.values;
    const size_t lo = sorted.size() / 100, hi = sorted.size() - 1 - sorted.size() / 100;
    std::nth_element(sorted.begin(), sorted.begin() + std::ptrdiff_t(lo), sorted.end());
    const float dlo = sorted[lo];
    std::nth_element(sorted.begin(), sorted.begin() + std::ptrdiff_t(hi), sorted.end());
    const float dhi = sorted[hi];
    const float span = std::max(1e-6f, dhi - dlo);
    for (float& v : out.values) v = std::clamp((v - dlo) / span, 0.0f, 1.0f);
    return true;
}

#endif

std::shared_ptr<const DepthMap> cachedDepth(const Image& img, int size) {
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
    static std::deque<std::pair<uint64_t, std::shared_ptr<const DepthMap>>> cache;
    {
        std::lock_guard lock(m);
        for (const auto& [k, d] : cache)
            if (k == key) return d;
    }
    auto d = std::make_shared<DepthMap>();
    if (!estimateDepth(img, *d, size)) return nullptr;
    std::lock_guard lock(m);
    cache.emplace_front(key, d);
    if (cache.size() > 8) cache.pop_back();
    return d;
}

}  // namespace montage
