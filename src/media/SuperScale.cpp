#include "SuperScale.h"

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

const ModelPack& upscaleModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "upscale";
        p.title = "Super Scale model";
        p.directoryEnv = "MONTAGE_UPSCALE_MODEL";
        p.urlEnv = "MONTAGE_UPSCALE_MODEL_URL";
        // An ONNX export of the official realesr-general-x4v3.pth (Real-ESRGAN v0.2.5.0); its output was checked
        // against the checkpoint run directly (largest difference 4e-6).
        p.files = {
            {"realesr-general-x4v3.onnx",
             "https://huggingface.co/jonathanst29/tinier-upscale-models/resolve/899dc1e4b22bbf1955c2f1739c085edc080cb366/"
             "realesr-general-x4v3.onnx",
             "924ebad6532777303582d4ce7811849b88869231a3ae7093e0f21200249df8d5", 4866396},
        };
        return p;
    }();
    return pack;
}

namespace {

// One axis of a resize: for each output index, the source indices and weights.
struct Taps {
    std::vector<int> first, count;
    std::vector<float> weights;  // count[i] per output, packed
    std::vector<int> offset;
};

Taps taps(int src, int dst) {
    Taps t;
    t.first.resize(size_t(dst));
    t.count.resize(size_t(dst));
    t.offset.resize(size_t(dst));
    const double s = double(src) / dst;
    for (int o = 0; o < dst; ++o) {
        t.offset[size_t(o)] = int(t.weights.size());
        if (dst <= src) {
            // Area: every source pixel the output pixel covers, by how much.
            const double a = o * s, b = (o + 1) * s;
            const int i0 = int(std::floor(a)), i1 = std::min(src, int(std::ceil(b)));
            t.first[size_t(o)] = i0;
            t.count[size_t(o)] = i1 - i0;
            for (int i = i0; i < i1; ++i) t.weights.push_back(float((std::min(b, double(i + 1)) - std::max(a, double(i))) / s));
        } else {
            // Bilinear between pixel centres.
            const double x = std::clamp((o + 0.5) * s - 0.5, 0.0, double(src - 1));
            const int i0 = int(x), i1 = std::min(src - 1, i0 + 1);
            const float f = float(x - i0);
            t.first[size_t(o)] = i0;
            t.count[size_t(o)] = i1 > i0 ? 2 : 1;
            t.weights.push_back(i1 > i0 ? 1 - f : 1.0f);
            if (i1 > i0) t.weights.push_back(f);
        }
    }
    return t;
}

}  // namespace

Image resizeImage(const Image& in, int w, int h) {
    if (in.empty() || w <= 0 || h <= 0) return {};
    if (w == in.width && h == in.height) return in;
    const Taps tx = taps(in.width, w), ty = taps(in.height, h);
    Image mid(w, in.height, Image::Uninitialized{});
    parallelRows(in.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                float acc[4] = {0, 0, 0, 0};
                const float* wt = &tx.weights[size_t(tx.offset[size_t(x)])];
                for (int k = 0; k < tx.count[size_t(x)]; ++k) {
                    const float* p = in.at(tx.first[size_t(x)] + k, y);
                    for (int c = 0; c < 4; ++c) acc[c] += p[c] * wt[k];
                }
                std::copy_n(acc, 4, mid.at(x, y));
            }
    });
    Image out(w, h, Image::Uninitialized{});
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* o = out.row(y);
            std::fill(o, o + size_t(w) * 4, 0.0f);
            const float* wt = &ty.weights[size_t(ty.offset[size_t(y)])];
            for (int k = 0; k < ty.count[size_t(y)]; ++k) {
                const float* r = mid.row(ty.first[size_t(y)] + k);
                for (int i = 0; i < w * 4; ++i) o[i] += r[i] * wt[k];
            }
        }
    });
    return out;
}

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool upscalerAvailable() { return false; }
bool superScale4x(const Image&, Image&, std::string* error, const std::atomic<bool>*) {
    if (error) *error = "This build of Montage cannot use Super Scale (it was built without ONNX Runtime)";
    return false;
}

#else

bool upscalerAvailable() { return true; }

namespace {

constexpr int kTile = 320;  // input pixels per tile side
// Context round each tile, cropped away afterwards: the network's 34 3x3 layers see 34 px, so the tiles
// join without seams (the same as one pass over the whole picture).
constexpr int kPad = 34;

struct Model {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-upscale"};
    std::unique_ptr<Ort::Session> session;
};

std::shared_ptr<Model> loadModel(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Model>();  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    const ModelPack& pack = upscaleModel();
    if (!pack.installed()) {
        if (error) *error = "The Super Scale model is not downloaded";
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
        if (error) *error = std::string("The Super Scale model could not be loaded: ") + e.what();
        return nullptr;
    }
}

}  // namespace

bool superScale4x(const Image& in, Image& out, std::string* error, const std::atomic<bool>* cancel) {
    if (in.empty()) {
        out = {};
        return true;
    }
    auto model = loadModel(error);
    if (!model) return false;
    const int W = in.width, H = in.height;
    out = Image(W * 4, H * 4, Image::Uninitialized{});
    // The alpha, enlarged smoothly; the colour comes from the model.
    const Image smooth = resizeImage(in, W * 4, H * 4);
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const char* inNames[] = {"input"};
        const char* outNames[] = {"output"};
        std::vector<float> tile;
        for (int ty = 0; ty < H; ty += kTile)
            for (int tx = 0; tx < W; tx += kTile) {
                if (cancel && cancel->load()) {
                    if (error) *error = "Cancelled";
                    return false;
                }
                // The tile with its context, clamped to the picture.
                const int x0 = std::max(0, tx - kPad), y0 = std::max(0, ty - kPad);
                const int x1 = std::min(W, tx + kTile + kPad), y1 = std::min(H, ty + kTile + kPad);
                const int tw = x1 - x0, th = y1 - y0;
                tile.assign(size_t(3) * size_t(tw) * size_t(th), 0.0f);
                for (int y = 0; y < th; ++y)
                    for (int x = 0; x < tw; ++x) {
                        const float* p = in.at(x0 + x, y0 + y);
                        const float a = p[3] > 1e-6f ? p[3] : 1.0f;
                        for (int c = 0; c < 3; ++c)
                            tile[size_t(c) * size_t(tw) * size_t(th) + size_t(y) * size_t(tw) + size_t(x)] = std::clamp(p[c] / a, 0.0f, 1.0f);
                    }
                const int64_t shape[4] = {1, 3, th, tw};
                Ort::Value input = Ort::Value::CreateTensor<float>(cpu, tile.data(), tile.size(), shape, 4);
                auto result = model->session->Run(Ort::RunOptions{nullptr}, inNames, &input, 1, outNames, 1);
                const float* y4 = result[0].GetTensorData<float>();
                const size_t plane = size_t(tw) * 4 * size_t(th) * 4;
                // The tile's own part, without its context, into place.
                const int ox0 = (tx - x0) * 4, oy0 = (ty - y0) * 4;
                const int ow = std::min(kTile, W - tx) * 4, oh = std::min(kTile, H - ty) * 4;
                for (int y = 0; y < oh; ++y) {
                    float* o = out.at(tx * 4, ty * 4 + y);
                    const float* s = smooth.at(tx * 4, ty * 4 + y);
                    const size_t row = size_t(oy0 + y) * size_t(tw) * 4;
                    for (int x = 0; x < ow; ++x, o += 4, s += 4) {
                        const float a = std::clamp(s[3], 0.0f, 1.0f);
                        for (int c = 0; c < 3; ++c) o[c] = std::clamp(y4[size_t(c) * plane + row + size_t(ox0 + x)], 0.0f, 1.0f) * a;
                        o[3] = a;
                    }
                }
            }
        return true;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("Super Scale failed: ") + e.what();
        return false;
    }
}

#endif

bool superScale(const Image& in, int w, int h, Image& out, double strength, std::string* error, const std::atomic<bool>* cancel) {
    strength = std::clamp(strength, 0.0, 1.0);
    if (strength <= 0 || w <= in.width || h <= in.height) {
        out = resizeImage(in, w, h);
        return true;
    }
    Image big;
    if (!superScale4x(in, big, error, cancel)) return false;
    out = resizeImage(big, w, h);
    if (strength < 1) {
        const Image plain = resizeImage(in, w, h);
        const float k = float(strength);
        for (size_t i = 0; i < out.px.size(); ++i) out.px[i] = plain.px[i] + (out.px[i] - plain.px[i]) * k;
    }
    return true;
}

}  // namespace montage
