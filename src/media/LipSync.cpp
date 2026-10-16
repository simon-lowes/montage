#include "LipSync.h"

#include <QFile>
#include <QString>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

// The networks without their weights (src/media/lipsync-*.onnx), compiled in by CMake.
extern const unsigned char kLipSyncUnetGraph[], kLipSyncVaeEncoderGraph[], kLipSyncVaeDecoderGraph[], kLipSyncAudioGraph[];
extern const size_t kLipSyncUnetGraphSize, kLipSyncVaeEncoderGraphSize, kLipSyncVaeDecoderGraphSize, kLipSyncAudioGraphSize;

namespace montage {

const ModelPack& lipSyncModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "lipsync";
        p.title = "lip sync model";
        p.directoryEnv = "MONTAGE_LIPSYNC_MODEL";
        p.urlEnv = "MONTAGE_LIPSYNC_MODEL_URL";
        // Each from its authors, at a pinned revision: MuseTalk 1.5's UNet, Stability AI's fine-tuned VAE and OpenAI's
        // whisper-tiny (as Hugging Face's transformers holds it).
        p.files = {
            {"unet.pth", "https://huggingface.co/TMElyralab/MuseTalk/resolve/2bcb936e2fddb4d86db4c62fd45b387d0c061571/musetalkV15/unet.pth",
             "7ebf6c98c181e20838e4c0054e96e944ac60d5d692cc01db42839fe11b787007", 3400074924},
            {"sd-vae-ft-mse.safetensors",
             "https://huggingface.co/stabilityai/sd-vae-ft-mse/resolve/31f26fdeee1355a5c34592e401dd41e45d25a493/diffusion_pytorch_model.safetensors",
             "a1d993488569e928462932c8c38a0760b874d166399b14414135bd9c42df5815", 334643276},
            {"whisper-tiny.safetensors",
             "https://huggingface.co/openai/whisper-tiny/resolve/169d4a4341b33bc18d8881c4b69c2e104e1cc0af/model.safetensors",
             "7ebd0e69e78190ffe1438491fa05cc1f5c1aa3a4c4db3bc1723adbb551ea2395", 151061672},
        };
        return p;
    }();
    return pack;
}

std::vector<float> lipSyncWindow(int64_t index, double fps, const std::function<const float*(int64_t)>& feature) {
    // MuseTalk (audio_processor.py get_whisper_chunk): the encoding padded with silence by two video frames' worth
    // (rounded up to whole encoding frames) at the start, then ten frames from where the video frame falls.
    const double perFrame = kLipSyncFeatureRate / std::max(1.0, fps);
    const int64_t pad = 2 * int64_t(std::ceil(perFrame));
    const int64_t first = int64_t(std::floor(double(index) * perFrame)) - pad;
    constexpr size_t frameSize = size_t(kLipSyncLayers) * kLipSyncWidth;
    std::vector<float> out(size_t(kLipSyncTokens) * kLipSyncWidth, 0.0f);
    for (int k = 0; k < kLipSyncTokens / kLipSyncLayers; ++k)
        if (const int64_t j = first + k; j >= 0)
            if (const float* f = feature(j)) std::copy(f, f + frameSize, out.begin() + ptrdiff_t(size_t(k) * frameSize));
    return out;
}

namespace {

// MediaPipe's face outline, clockwise from the top of the forehead; from 454 (the right of the picture, level with
// the eyes) round the chin (152) to 234 is its lower half.
constexpr int kOval[] = {10,  338, 297, 332, 284, 251, 389, 356, 454, 323, 361, 288, 397, 365, 379, 378, 400, 377,
                         152, 148, 176, 149, 150, 136, 172, 58,  132, 93,  234, 127, 162, 21,  54,  103, 67,  109};
constexpr int kLowerFrom = 8, kLowerTo = 28;  // 454 .. 234
constexpr int kChin = 152;
constexpr int kBridge = 195;  // on the nose's bridge, where the 68-point layout's 29th point is

}  // namespace

LipSyncBox lipSyncBox(const FaceMesh& face, int width, int height) {
    LipSyncBox b;
    if (!face.valid() || width <= 0 || height <= 0) return b;
    // (MuseTalk's preprocessing: across the landmarks, from the bridge up by as much as the chin is below it, to the
    // chin; 1.5 adds 10 pixels below for a face of about 256, which is kept in proportion here.)
    float x0 = face.x(kChin), x1 = x0, chin = face.y(kChin);
    for (int i = kLowerFrom; i <= kLowerTo; ++i) {
        x0 = std::min(x0, face.x(kOval[i])), x1 = std::max(x1, face.x(kOval[i]));
        chin = std::max(chin, face.y(kOval[i]));
    }
    const float bridge = face.y(kBridge);
    const float top = std::max(0.0f, bridge - (chin - bridge));
    const float margin = (chin - top) * 10.0f / 256.0f;
    b.x0 = std::clamp(int(std::floor(x0)), 0, width);
    b.x1 = std::clamp(int(std::ceil(x1)), 0, width);
    b.y0 = std::clamp(int(std::floor(top)), 0, height);
    b.y1 = std::clamp(int(std::ceil(chin + margin)), 0, height);
    if (chin <= bridge) b = {};
    return b;
}

namespace {

// Separable Gaussian blur of a single-channel map (in place).
void blurMap(std::vector<float>& m, int w, int h, double sigma) {
    if (sigma < 0.3) return;
    const int r = int(std::ceil(sigma * 3));
    std::vector<float> k(size_t(2 * r + 1));
    double sum = 0;
    for (int i = -r; i <= r; ++i) sum += k[size_t(i + r)] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
    for (float& v : k) v = float(v / sum);
    std::vector<float> tmp(m.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int i = -r; i <= r; ++i) s += k[size_t(i + r)] * m[size_t(y) * size_t(w) + size_t(std::clamp(x + i, 0, w - 1))];
            tmp[size_t(y) * size_t(w) + size_t(x)] = s;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float s = 0;
            for (int i = -r; i <= r; ++i) s += k[size_t(i + r)] * tmp[size_t(std::clamp(y + i, 0, h - 1)) * size_t(w) + size_t(x)];
            m[size_t(y) * size_t(w) + size_t(x)] = s;
        }
}

}  // namespace

std::vector<float> lipSyncMask(const FaceMesh& face, const LipSyncBox& box, int width, int height) {
    std::vector<float> mask(size_t(width) * size_t(height), 0.0f);
    if (box.empty() || !face.valid()) return mask;
    // The outline's lower half, its chin end pushed down (to the box's bottom at most) so a jaw that opens further than
    // the picture's stays inside; filled by scanlines from the bridge's height down.
    std::vector<std::pair<float, float>> poly;
    const float chinY = face.y(kChin), bridge = face.y(kBridge);
    const float drop = (float(box.y1) - chinY);
    for (int i = kLowerFrom; i <= kLowerTo; ++i) {
        const float x = face.x(kOval[i]), y = face.y(kOval[i]);
        // (more the nearer the chin)
        const float t = std::clamp((y - bridge) / std::max(1.0f, chinY - bridge), 0.0f, 1.0f);
        poly.emplace_back(x, y + drop * t * t);
    }
    const int top = std::max(box.y0, int(std::ceil(bridge)));
    // Work in the region round the box (with room for the feather) rather than the whole picture.
    const int side = std::max(box.x1 - box.x0, box.y1 - box.y0);
    const int pad = side / 4 + 2;
    const int rx0 = std::max(0, box.x0 - pad), ry0 = std::max(0, box.y0 - pad);
    const int rx1 = std::min(width, box.x1 + pad), ry1 = std::min(height, box.y1 + pad);
    const int rw = rx1 - rx0, rh = ry1 - ry0;
    if (rw <= 0 || rh <= 0) return mask;
    std::vector<float> local(size_t(rw) * size_t(rh), 0.0f);
    for (int y = std::max(top, ry0); y < std::min(box.y1, ry1); ++y) {
        const float cy = float(y) + 0.5f;
        std::vector<float> xs;
        for (size_t i = 0; i < poly.size(); ++i) {
            const auto [ax, ay] = poly[i];
            const auto [bx, by] = poly[(i + 1) % poly.size()];
            if ((ay <= cy) != (by <= cy)) xs.push_back(ax + (cy - ay) / (by - ay) * (bx - ax));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2) {
            const int a = std::max({box.x0, rx0, int(std::lround(xs[i]))}), b = std::min({box.x1, rx1, int(std::lround(xs[i + 1]))});
            for (int x = a; x < b; ++x) local[size_t(y - ry0) * size_t(rw) + size_t(x - rx0)] = 1.0f;
        }
    }
    // Feathered as MuseTalk's blending does it: a Gaussian of a twentieth of the crop it blends in (1.5 times the box).
    const int k = int(0.05 * 1.5 * side / 2) * 2 + 1;
    blurMap(local, rw, rh, 0.3 * ((k - 1) * 0.5 - 1) + 0.8);
    for (int y = 0; y < rh; ++y)
        std::copy(local.begin() + ptrdiff_t(size_t(y) * size_t(rw)), local.begin() + ptrdiff_t(size_t(y + 1) * size_t(rw)),
                  mask.begin() + ptrdiff_t(size_t(y + ry0) * size_t(width) + size_t(rx0)));
    return mask;
}

namespace {

// Resampling with a Lanczos-3 filter (widened when shrinking), one axis at a time, `channels` interleaved floats.
std::vector<float> resampleAxis(const std::vector<float>& in, int w, int h, int channels, int outLen, bool horizontal, double from0,
                                double from1) {
    const int inLen = horizontal ? w : h;
    const int across = horizontal ? h : w;
    const double scale = (from1 - from0) / outLen;
    const double support = 3.0 * std::max(1.0, scale), widen = std::max(1.0, scale);
    std::vector<float> out(size_t(outLen) * size_t(across) * size_t(channels), 0.0f);
    auto lanczos = [](double x) {
        x = std::abs(x);
        if (x < 1e-9) return 1.0;
        if (x >= 3) return 0.0;
        const double px = M_PI * x;
        return 3 * std::sin(px) * std::sin(px / 3) / (px * px);
    };
    for (int o = 0; o < outLen; ++o) {
        const double centre = from0 + (o + 0.5) * scale - 0.5;
        const int a = int(std::floor(centre - support)), b = int(std::ceil(centre + support));
        std::vector<std::pair<int, double>> taps;
        double sum = 0;
        for (int i = a; i <= b; ++i) {
            const double wgt = lanczos((i - centre) / widen);
            if (wgt == 0) continue;
            taps.emplace_back(std::clamp(i, 0, inLen - 1), wgt);
            sum += wgt;
        }
        for (auto& t : taps) t.second /= sum;
        for (int j = 0; j < across; ++j)
            for (int c = 0; c < channels; ++c) {
                double v = 0;
                for (const auto& [i, wgt] : taps) {
                    const size_t idx = horizontal ? (size_t(j) * size_t(w) + size_t(i)) : (size_t(i) * size_t(w) + size_t(j));
                    v += wgt * in[idx * size_t(channels) + size_t(c)];
                }
                const size_t at = horizontal ? (size_t(j) * size_t(outLen) + size_t(o)) : (size_t(o) * size_t(across) + size_t(j));
                out[at * size_t(channels) + size_t(c)] = float(v);
            }
    }
    return out;
}

// The region x0..x1, y0..y1 of an RGB image (w x h) at outW x outH.
std::vector<float> resampleRegion(const std::vector<float>& rgb, int w, int h, double x0, double y0, double x1, double y1, int outW, int outH) {
    const std::vector<float> across = resampleAxis(rgb, w, h, 3, outW, true, x0, x1);
    return resampleAxis(across, outW, h, 3, outH, false, y0, y1);
}

}  // namespace

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool lipSyncAvailable() { return false; }
bool lipSyncEncodeSound(const std::vector<float>&, std::vector<float>&, std::string* error) {
    if (error) *error = "This build of Montage has no ONNX Runtime";
    return false;
}
bool lipSyncFace(Image&, const FaceMesh&, const std::vector<float>&, double, std::string* error, const std::atomic<bool>*) {
    if (error) *error = "This build of Montage has no ONNX Runtime";
    return false;
}

#else

bool lipSyncAvailable() { return true; }

namespace {

constexpr int kSide = 256, kLatent = 32;

struct Models {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-lipsync"};
    std::unique_ptr<OrtModel> unet, encoder, decoder, audio;
};

// Each graph beside the weights it names (rewritten when this Montage brings a different one).
bool writeGraph(const char* name, const unsigned char* bytes, size_t size, std::string& path, std::string* error) {
    const QString dir = QString::fromStdString(lipSyncModel().directory());
    path = (dir + '/' + name).toStdString();
    const QByteArray want = QByteArray::fromRawData(reinterpret_cast<const char*>(bytes), qsizetype(size));
    QFile graph(QString::fromStdString(path));
    if (graph.open(QIODevice::ReadOnly) && graph.readAll() == want) return true;
    graph.close();
    if (graph.open(QIODevice::WriteOnly | QIODevice::Truncate) && graph.write(want) == want.size()) return true;
    if (error) *error = "Cannot write the lip sync model into " + dir.toStdString();
    return false;
}

std::shared_ptr<Models> loadModels(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Models>;  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    if (!lipSyncModel().installed()) {
        if (error) *error = "The " + lipSyncModel().title + " is not downloaded";
        return nullptr;
    }
    struct Graph {
        const char* name;
        const unsigned char* bytes;
        size_t size;
        std::unique_ptr<OrtModel> Models::*into;
        ModelRuns where;
    };
    const Graph graphs[] = {{"lipsync-unet.onnx", kLipSyncUnetGraph, kLipSyncUnetGraphSize, &Models::unet, ModelRuns::Anywhere},
                            {"lipsync-vae-encoder.onnx", kLipSyncVaeEncoderGraph, kLipSyncVaeEncoderGraphSize, &Models::encoder, ModelRuns::Anywhere},
                            {"lipsync-vae-decoder.onnx", kLipSyncVaeDecoderGraph, kLipSyncVaeDecoderGraphSize, &Models::decoder, ModelRuns::Anywhere},
                            {"lipsync-audio.onnx", kLipSyncAudioGraph, kLipSyncAudioGraphSize, &Models::audio, ModelRuns::Anywhere}};
    try {
        auto models = std::make_shared<Models>();
        for (const Graph& g : graphs) {
            std::string path;
            if (!writeGraph(g.name, g.bytes, g.size, path, error)) return nullptr;
            Ort::SessionOptions so;
            so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            so.SetIntraOpNumThreads(int(std::max(1u, std::thread::hardware_concurrency())));
#ifdef _WIN32
            const std::wstring p = QString::fromStdString(path).toStdWString();
#else
            const std::string& p = path;
#endif
            (*models).*(g.into) = openModel(models->env, p.c_str(), so, g.where);
        }
        *cached = models;
        return models;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The lip sync model could not be loaded: ") + e.what();
        return nullptr;
    }
}

Ort::Value tensor(std::vector<float>& data, std::initializer_list<int64_t> shape) {
    static const Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const std::vector<int64_t> dims(shape);
    return Ort::Value::CreateTensor<float>(mem, data.data(), data.size(), dims.data(), dims.size());
}

std::vector<float> run(OrtModel& model, const std::vector<const char*>& names, std::vector<Ort::Value>& inputs, const char* output,
                       size_t expect) {
    auto out = model.Run(Ort::RunOptions{nullptr}, names.data(), inputs.data(), inputs.size(), &output, 1);
    const float* d = out[0].GetTensorData<float>();
    const size_t n = out[0].GetTensorTypeAndShapeInfo().GetElementCount();
    if (n != expect) throw std::runtime_error("the lip sync model gave " + std::to_string(n) + " values, not " + std::to_string(expect));
    return {d, d + n};
}

}  // namespace

bool lipSyncEncodeSound(const std::vector<float>& segment, std::vector<float>& features, std::string* error) {
    std::shared_ptr<Models> models = loadModels(error);
    if (!models) return false;
    std::vector<float> pcm(size_t(kLipSyncSegment), 0.0f);
    std::copy_n(segment.begin(), std::min(segment.size(), pcm.size()), pcm.begin());
    try {
        std::vector<Ort::Value> in;
        in.push_back(tensor(pcm, {1, kLipSyncSegment}));
        features = run(*models->audio, {"pcm"}, in, "features", size_t(kLipSyncSegment / 320) * kLipSyncLayers * kLipSyncWidth);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = std::string("Lip sync could not hear the sound: ") + e.what();
        return false;
    }
}

bool lipSyncFace(Image& img, const FaceMesh& face, const std::vector<float>& window, double amount, std::string* error,
                 const std::atomic<bool>* cancel) {
    if (img.empty() || amount <= 0) return true;
    if (window.size() != size_t(kLipSyncTokens) * kLipSyncWidth) {
        if (error) *error = "Lip sync was given the wrong amount of sound";
        return false;
    }
    const LipSyncBox box = lipSyncBox(face, img.width, img.height);
    if (box.x1 - box.x0 < 8 || box.y1 - box.y0 < 8) {
        if (error) *error = "The face is too small to lip-sync";
        return false;
    }
    std::shared_ptr<Models> models = loadModels(error);
    if (!models) return false;
    // The box's colour (un-premultiplied, 0..1), at 256 x 256.
    const int bw = box.x1 - box.x0, bh = box.y1 - box.y0;
    std::vector<float> region(size_t(bw) * size_t(bh) * 3);
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const float* p = img.at(box.x0 + x, box.y0 + y);
            const float a = p[3] > 1e-6f ? p[3] : 1.0f;
            for (int c = 0; c < 3; ++c) region[(size_t(y) * size_t(bw) + size_t(x)) * 3 + size_t(c)] = std::clamp(p[c] / a, 0.0f, 1.0f);
        }
    const std::vector<float> crop = resampleRegion(region, bw, bh, 0, 0, bw, bh, kSide, kSide);
    // The VAE's two inputs, planar and from -1 to 1: the crop with its lower half blacked out, and the crop.
    constexpr size_t plane = size_t(kSide) * kSide;
    std::vector<float> images(2 * 3 * plane);
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x)
            for (int c = 0; c < 3; ++c) {
                const float v = crop[(size_t(y) * kSide + size_t(x)) * 3 + size_t(c)];
                const size_t at = size_t(c) * plane + size_t(y) * kSide + size_t(x);
                images[at] = y < kSide / 2 ? v * 2 - 1 : -1.0f;
                images[3 * plane + at] = v * 2 - 1;
            }
    std::vector<float> drawn;
    try {
        std::vector<Ort::Value> in;
        in.push_back(tensor(images, {2, 3, kSide, kSide}));
        std::vector<float> latents = run(*models->encoder, {"image"}, in, "latents", 2 * 4 * size_t(kLatent) * kLatent);
        if (cancel && cancel->load()) return false;
        // (masked first, then the reference: [1, 8, 32, 32])
        std::vector<float> sound = window;
        in.clear();
        in.push_back(tensor(latents, {1, 8, kLatent, kLatent}));
        in.push_back(tensor(sound, {1, kLipSyncTokens, kLipSyncWidth}));
        std::vector<float> made = run(*models->unet, {"latents", "audio"}, in, "out", 4 * size_t(kLatent) * kLatent);
        if (cancel && cancel->load()) return false;
        in.clear();
        in.push_back(tensor(made, {1, 4, kLatent, kLatent}));
        drawn = run(*models->decoder, {"latents"}, in, "image", 3 * plane);
    } catch (const std::exception& e) {
        if (error) *error = std::string("Lip sync failed: ") + e.what();
        return false;
    }
    // Back to the box's size and blended in through the mask.
    std::vector<float> rgb(3 * plane);
    for (size_t i = 0; i < plane; ++i)
        for (int c = 0; c < 3; ++c) rgb[i * 3 + size_t(c)] = drawn[size_t(c) * plane + i];
    const std::vector<float> back = resampleRegion(rgb, kSide, kSide, 0, 0, kSide, kSide, bw, bh);
    const std::vector<float> mask = lipSyncMask(face, box, img.width, img.height);
    const float k = float(std::clamp(amount, 0.0, 1.0));
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const float m = mask[size_t(box.y0 + y) * size_t(img.width) + size_t(box.x0 + x)] * k;
            if (m <= 0) continue;
            float* p = img.at(box.x0 + x, box.y0 + y);
            for (int c = 0; c < 3; ++c) {
                const float v = std::clamp(back[(size_t(y) * size_t(bw) + size_t(x)) * 3 + size_t(c)], 0.0f, 1.0f) * p[3];
                p[c] += (v - p[c]) * m;
            }
        }
    return true;
}

#endif

}  // namespace montage
