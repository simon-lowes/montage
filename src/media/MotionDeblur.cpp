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
    static std::string failed;                           // (a model that would not open is not tried again every frame)
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!failed.empty()) {
        if (error) *error = failed;
        return nullptr;
    }
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
        // (Its working memory, a gigabyte or more for a large tile, is handed back after each run rather than kept.)
        so.DisableCpuMemArena();
#ifdef _WIN32
        const std::wstring path = QString::fromStdString(pack.path(pack.files[0])).toStdWString();
#else
        const std::string path = pack.path(pack.files[0]);
#endif
        model->session = openModel(model->env, path.c_str(), so);
        *cached = model;
        return model;
    } catch (const std::exception& e) {
        failed = std::string("The Motion Deblur model could not be loaded: ") + e.what();
        if (error) *error = failed;
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

// Tiles along `n` pixels, none over `t` (nor under the model's smallest where the picture allows): as few as cover it with
// at least kOverlap between neighbours, all one size and evenly spread, so no work is wasted on a sliver at the end.
struct Tiles {
    int size;
    std::vector<int> starts;
};
Tiles tiles(int n, int t) {
    if (n <= t) return {n, {0}};
    const int k = (n - kOverlap + (t - kOverlap) - 1) / (t - kOverlap);
    const int size = std::min(n, std::max(kMin, (n + (k - 1) * kOverlap + k - 1) / k));
    Tiles out{size, {}};
    for (int i = 0; i < k; ++i) out.starts.push_back(int(std::lround(double(i) * (n - size) / std::max(1, k - 1))));
    return out;
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
    // One picture at a time: each run already takes every core, and its memory would multiply.
    static std::mutex running;
    std::lock_guard lock(running);
    const int W = in.width, H = in.height;
    const size_t N = size_t(W) * size_t(H);
    tile = std::max(tile, kMin);
    const Tiles tx = tiles(W, tile), ty = tiles(H, tile);
    const int tw = tx.size, th = ty.size;
    try {
        // The colour un-premultiplied; where it is (nearly) transparent, the nearest visible colour along the row, then the
        // column, so a cut-out's surround does not read as black next to its edge.
        std::vector<float> colour(3 * N);
        std::vector<uint8_t> seen(N);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float* p = in.at(x, y);
                const size_t o = size_t(y) * size_t(W) + size_t(x);
                seen[o] = p[3] >= 0.01f;
                for (int c = 0; c < 3; ++c) colour[size_t(c) * N + o] = seen[o] ? std::clamp(p[c] / p[3], 0.0f, 1.0f) : 0.5f;
            }
        const std::vector<uint8_t> opaque = seen;
        auto fill = [&](size_t count, size_t length, size_t stride, size_t step) {
            std::vector<size_t> filled;
            std::vector<long> before(length);
            for (size_t line = 0; line < count; ++line) {
                const size_t base = line * stride;
                long last = -1;
                for (size_t i = 0; i < length; ++i) {
                    if (seen[base + i * step]) last = long(i);
                    before[i] = last;
                }
                last = -1;
                for (size_t i = length; i-- > 0;) {
                    const size_t o = base + i * step;
                    if (seen[o]) {
                        last = long(i);
                        continue;
                    }
                    long from = before[i];
                    if (last >= 0 && (from < 0 || last - long(i) < long(i) - from)) from = last;
                    if (from < 0) continue;
                    for (int c = 0; c < 3; ++c) colour[size_t(c) * N + o] = colour[size_t(c) * N + base + size_t(from) * step];
                    filled.push_back(o);
                }
            }
            for (size_t o : filled) seen[o] = 1;
        };
        fill(size_t(H), size_t(W), size_t(W), 1);
        fill(size_t(W), size_t(H), 1, size_t(W));

        // The model's result, weighted where tiles overlap.
        std::vector<float> acc(3 * N, 0.0f), weight(N, 0.0f);
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const char* inNames[] = {"lq"};
        const char* outNames[] = {"output"};
        // Each tile mirrored out to the model's smallest size where the picture is smaller.
        const int pw = std::max(tw, kMin), ph = std::max(th, kMin);
        const size_t plane = size_t(pw) * size_t(ph);
        std::vector<float> input(3 * plane);
        for (int y0 : ty.starts)
            for (int x0 : tx.starts) {
                if (cancel && cancel->load()) {
                    if (error) *error = "Cancelled";
                    return false;
                }
                for (int y = 0; y < ph; ++y) {
                    const size_t row = size_t(y0 + mirror(y, th)) * size_t(W);
                    for (int x = 0; x < pw; ++x) {
                        const size_t o = row + size_t(x0 + mirror(x, tw));
                        for (int c = 0; c < 3; ++c) input[size_t(c) * plane + size_t(y) * size_t(pw) + size_t(x)] = colour[size_t(c) * N + o];
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
                        for (int c = 0; c < 3; ++c) acc[size_t(c) * N + o] += w * r[size_t(c) * plane + size_t(y) * size_t(pw) + size_t(x)];
                        weight[o] += w;
                    }
                }
            }
        out = in;
        const float k = float(strength);
        parallelRows(H, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float* p = out.row(y);
                for (int x = 0; x < W; ++x, p += 4) {
                    const size_t o = size_t(y) * size_t(W) + size_t(x);
                    const float a = p[3];
                    if (!opaque[o]) continue;  // (nearly transparent: left as it was)
                    for (int c = 0; c < 3; ++c) {
                        // (The change the model made, so values outside 0..1, which it never saw, keep what is beyond.)
                        const float before = p[c] / a, made = std::clamp(acc[size_t(c) * N + o] / weight[o], 0.0f, 1.0f);
                        p[c] = (before + (made - colour[size_t(c) * N + o]) * k) * a;
                    }
                }
            }
        });
        return true;
    } catch (const std::exception& e) {
        if (error) *error = std::string("Motion Deblur failed: ") + e.what();
        return false;
    }
}

#endif

}  // namespace montage
