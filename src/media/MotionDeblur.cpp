#include "MotionDeblur.h"

#include <QString>
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

namespace montage {

const ModelPack& deblurModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "deblur";
        p.title = "Motion Deblur model";
        p.directoryEnv = "MONTAGE_DEBLUR_MODEL";
        p.urlEnv = "MONTAGE_DEBLUR_MODEL_URL";
        // OpenCV's model zoo's ONNX export of NAFNet's GoPro model (int8 weights); its output on the zoo's own example
        // matches the zoo's published result (mean difference 0.004, the JPEG's).
        p.files = {
            {"deblurring_nafnet_2025may.onnx",
             "https://huggingface.co/opencv/deblurring_nafnet/resolve/f1f255116cdb628a311d2b5749871189a4639d84/"
             "deblurring_nafnet_2025may.onnx",
             "07263f416febecce10193dd648e950b22e397cf521eedab1a114ef77b2bc9587", 91736251},
        };
        return p;
    }();
    return pack;
}

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool deblurAvailable() { return false; }
bool motionDeblur(const Image&, Image&, double, std::string* error, const std::atomic<bool>*, int) {
    if (error) *error = "This build of Montage cannot use Motion Deblur (it was built without ONNX Runtime)";
    return false;
}

#else

bool deblurAvailable() { return true; }

namespace {

// The smallest picture the model takes on a side (its local averaging spans 384 px); smaller ones are mirrored out to it.
constexpr int kMin = 384;
// Tiles overlap by this much, blended across it.
constexpr int kOverlap = 64;

struct Model {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-deblur"};
    std::unique_ptr<OrtModel> session;
};

std::shared_ptr<Model> loadModel(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Model>();  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    const ModelPack& pack = deblurModel();
    if (!pack.installed()) {
        if (error) *error = "The Motion Deblur model is not downloaded";
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
        model->session = openModel(model->env, path.c_str(), so);
        *cached = model;
        return model;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The Motion Deblur model could not be loaded: ") + e.what();
        return nullptr;
    }
}

// Index i of a run of n mirrored (without repeating the edge) as often as needed.
int mirror(int i, int n) {
    if (n <= 1) return 0;
    const int period = 2 * n - 2;
    i %= period;
    return i < n ? i : period - i;
}

// Where tiles of `t` start along `n` pixels: each a whole tile, the last ending at the edge.
std::vector<int> tileStarts(int n, int t) {
    if (n <= t) return {0};
    std::vector<int> s;
    for (int x = 0;; x += t - kOverlap) {
        if (x + t >= n) {
            s.push_back(n - t);
            break;
        }
        s.push_back(x);
    }
    return s;
}

}  // namespace

bool motionDeblur(const Image& in, Image& out, double strength, std::string* error, const std::atomic<bool>* cancel, int tile) {
    strength = std::clamp(strength, 0.0, 1.0);
    if (in.empty() || strength <= 0) {
        out = in;
        return true;
    }
    auto model = loadModel(error);
    if (!model) return false;
    const int W = in.width, H = in.height;
    tile = std::max(tile, kMin);
    const int tw = std::min(W, tile), th = std::min(H, tile);
    // The model's result, weighted where tiles overlap.
    std::vector<float> acc(size_t(3) * size_t(W) * size_t(H), 0.0f), weight(size_t(W) * size_t(H), 0.0f);
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const char* inNames[] = {"lq"};
        const char* outNames[] = {"output"};
        // Each tile mirrored out to the model's smallest size where the picture is smaller.
        const int pw = std::max(tw, kMin), ph = std::max(th, kMin);
        const size_t plane = size_t(pw) * size_t(ph);
        std::vector<float> input(3 * plane);
        const std::vector<int> xs = tileStarts(W, tw), ys = tileStarts(H, th);
        for (int y0 : ys)
            for (int x0 : xs) {
                if (cancel && cancel->load()) {
                    if (error) *error = "Cancelled";
                    return false;
                }
                for (int y = 0; y < ph; ++y) {
                    const int sy = y0 + mirror(y, th);
                    for (int x = 0; x < pw; ++x) {
                        const float* p = in.at(x0 + mirror(x, tw), sy);
                        const float a = p[3] > 1e-6f ? p[3] : 1.0f;
                        for (int c = 0; c < 3; ++c) input[size_t(c) * plane + size_t(y) * size_t(pw) + size_t(x)] = std::clamp(p[c] / a, 0.0f, 1.0f);
                    }
                }
                const int64_t shape[4] = {1, 3, ph, pw};
                Ort::Value tensor = Ort::Value::CreateTensor<float>(cpu, input.data(), input.size(), shape, 4);
                auto result = model->session->Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1, outNames, 1);
                const float* r = result[0].GetTensorData<float>();
                // Weights ramping up over the overlap from a tile's inner edges, so the tiles blend.
                for (int y = 0; y < th; ++y) {
                    float wy = 1;
                    if (y0 > 0) wy = std::min(wy, (y + 0.5f) / kOverlap);
                    if (y0 + th < H) wy = std::min(wy, (th - y - 0.5f) / kOverlap);
                    for (int x = 0; x < tw; ++x) {
                        float wx = 1;
                        if (x0 > 0) wx = std::min(wx, (x + 0.5f) / kOverlap);
                        if (x0 + tw < W) wx = std::min(wx, (tw - x - 0.5f) / kOverlap);
                        const float w = std::max(1e-4f, wx * wy);
                        const size_t o = size_t(y0 + y) * size_t(W) + size_t(x0 + x);
                        for (int c = 0; c < 3; ++c)
                            acc[size_t(c) * size_t(W) * size_t(H) + o] += w * r[size_t(c) * plane + size_t(y) * size_t(pw) + size_t(x)];
                        weight[o] += w;
                    }
                }
            }
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("Motion Deblur failed: ") + e.what();
        return false;
    }
    out = in;
    const float k = float(strength);
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = out.row(y);
            for (int x = 0; x < W; ++x, p += 4) {
                const float a = p[3];
                if (a <= 0) continue;
                const size_t o = size_t(y) * size_t(W) + size_t(x);
                for (int c = 0; c < 3; ++c) {
                    // (The change the model made, so values outside 0..1, which it never saw, keep what is beyond.)
                    const float before = p[c] / a, made = std::clamp(acc[size_t(c) * size_t(W) * size_t(H) + o] / weight[o], 0.0f, 1.0f);
                    p[c] = (before + (made - std::clamp(before, 0.0f, 1.0f)) * k) * a;
                }
            }
        }
    });
    return true;
}

#endif

}  // namespace montage
