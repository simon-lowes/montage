#include "Inpaint.h"

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

const ModelPack& inpaintModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "inpaint";
        p.title = "object removal model";
        p.directoryEnv = "MONTAGE_INPAINT_MODEL";
        p.urlEnv = "MONTAGE_INPAINT_MODEL_URL";
        // Carve's ONNX port of big-lama (identical to the original, they report), at 512 x 512.
        p.files = {
            {"lama_fp32.onnx", "https://huggingface.co/Carve/LaMa-ONNX/resolve/c3c0c9e468934d62e79c329e35d82dd09ff8c444/lama_fp32.onnx",
             "1faef5301d78db7dda502fe59966957ec4b79dd64e16f03ed96913c7a4eb68d6", 208044816},
        };
        return p;
    }();
    return pack;
}

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool inpaintAvailable() { return false; }
bool inpaint(const Image& img, const std::vector<float>&, Image& out, std::string* error) {
    out = img;
    if (error) *error = "This build of Montage has no ONNX Runtime";
    return false;
}

#else

bool inpaintAvailable() { return true; }

namespace {

constexpr int kSize = 512;  // the model's fixed input

struct Model {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-inpaint"};
    std::unique_ptr<Ort::Session> session;
};

std::shared_ptr<Model> loadModel(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Model>();  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    const ModelPack& pack = inpaintModel();
    if (!pack.installed()) {
        if (error) *error = "The object removal model is not downloaded";
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
        if (error) *error = std::string("The object removal model could not be loaded: ") + e.what();
        return nullptr;
    }
}

}  // namespace

bool inpaint(const Image& img, const std::vector<float>& mask, Image& out, std::string* error) {
    out = img;
    const int W = img.width, H = img.height;
    if (img.empty() || mask.size() != size_t(W) * size_t(H)) {
        if (error) *error = "The mask does not match the picture";
        return false;
    }
    // The area to fill, and a square round it twice its size (at least 128 px) for context.
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (mask[size_t(y) * size_t(W) + size_t(x)] > 0.5f) x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
    if (x1 < 0) return true;  // nothing to fill
    auto model = loadModel(error);
    if (!model) return false;
    const int side = std::min(std::max(W, H), std::max({128, 2 * (x1 - x0 + 1), 2 * (y1 - y0 + 1)}));
    const int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    const int rw = std::min(W, side), rh = std::min(H, side);
    const int rx = std::clamp(cx - rw / 2, 0, W - rw), ry = std::clamp(cy - rh / 2, 0, H - rh);
    Image region(rw, rh, Image::Uninitialized{});
    for (int y = 0; y < rh; ++y) std::copy_n(img.at(rx, ry + y), size_t(rw) * 4, region.at(0, y));
    const Image small = resizeImage(region, kSize, kSize);
    std::vector<float> input(size_t(3) * kSize * kSize), holes(size_t(kSize) * kSize);
    const size_t plane = size_t(kSize) * kSize;
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x) {
            const float* p = small.at(x, y);
            const float a = p[3] > 1e-6f ? p[3] : 1.0f;
            for (int c = 0; c < 3; ++c) input[size_t(c) * plane + size_t(y) * kSize + size_t(x)] = std::clamp(p[c] / a, 0.0f, 1.0f);
            // The hole, taken generously: any source pixel under it that is over half masked.
            const int sx = std::min(rw - 1, int((x + 0.5) * rw / kSize)), sy = std::min(rh - 1, int((y + 0.5) * rh / kSize));
            holes[size_t(y) * kSize + size_t(x)] = mask[size_t(ry + sy) * size_t(W) + size_t(rx + sx)] > 0.5f ? 1.0f : 0.0f;
        }
    // Grow the hole a pixel at the model's scale, so the object's edge goes too.
    std::vector<float> grown = holes;
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
            if (holes[size_t(y) * kSize + size_t(x)] > 0)
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx >= 0 && ny >= 0 && nx < kSize && ny < kSize) grown[size_t(ny) * kSize + size_t(nx)] = 1;
                    }
    std::vector<float> filled;
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t ishape[4] = {1, 3, kSize, kSize}, mshape[4] = {1, 1, kSize, kSize};
        Ort::Value inputs[2] = {Ort::Value::CreateTensor<float>(cpu, input.data(), input.size(), ishape, 4),
                                Ort::Value::CreateTensor<float>(cpu, grown.data(), grown.size(), mshape, 4)};
        const char* inNames[] = {"image", "mask"};
        const char* outNames[] = {"output"};
        auto result = model->session->Run(Ort::RunOptions{nullptr}, inNames, inputs, 2, outNames, 1);
        const float* o = result[0].GetTensorData<float>();
        filled.assign(o, o + 3 * plane);
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("Object removal failed: ") + e.what();
        return false;
    }
    // Back at the region's size (0..255 out of the model), blended in by the mask.
    Image fill(kSize, kSize, Image::Uninitialized{});
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x) {
            float* q = fill.at(x, y);
            for (int c = 0; c < 3; ++c) q[c] = std::clamp(filled[size_t(c) * plane + size_t(y) * kSize + size_t(x)] / 255.0f, 0.0f, 1.0f);
            q[3] = 1;
        }
    const Image back = resizeImage(fill, rw, rh);
    for (int y = 0; y < rh; ++y)
        for (int x = 0; x < rw; ++x) {
            const float m = std::clamp(mask[size_t(ry + y) * size_t(W) + size_t(rx + x)], 0.0f, 1.0f);
            if (m <= 0) continue;
            float* p = out.at(rx + x, ry + y);
            const float* f = back.at(x, y);
            const float a = p[3];
            for (int c = 0; c < 3; ++c) p[c] += (f[c] * a - p[c]) * m;
        }
    return true;
}

#endif

bool cachedInpaint(const Image& img, const std::vector<float>& mask, Image& out) {
    uint64_t key = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ULL; };
    mix(uint64_t(img.width)), mix(uint64_t(img.height));
    const size_t step = std::max<size_t>(1, img.px.size() / 8192), mstep = std::max<size_t>(1, mask.size() / 8192);
    uint32_t bits;
    for (size_t i = 0; i < img.px.size(); i += step) std::memcpy(&bits, &img.px[i], 4), mix(bits);
    for (size_t i = 0; i < mask.size(); i += mstep) std::memcpy(&bits, &mask[i], 4), mix(bits);
    static std::mutex m;
    static std::deque<std::pair<uint64_t, std::shared_ptr<const Image>>> cache;
    {
        std::lock_guard lock(m);
        for (const auto& [k, im] : cache)
            if (k == key) {
                out = *im;
                return true;
            }
    }
    Image result;
    if (!inpaint(img, mask, result)) {
        out = img;
        return false;
    }
    std::lock_guard lock(m);
    cache.emplace_front(key, std::make_shared<const Image>(result));
    if (cache.size() > 4) cache.pop_back();
    out = std::move(result);
    return true;
}

}  // namespace montage
