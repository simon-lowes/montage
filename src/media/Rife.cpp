#include "Rife.h"

#include <QDir>
#include <QFile>
#include <QString>
#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "core/Zip.h"

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

// The network without its weights (src/media/rife-4.26.onnx), compiled in by CMake.
extern const unsigned char kRifeGraph[];
extern const size_t kRifeGraphSize;

namespace montage {

const ModelPack& rifeModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "rife";
        p.title = "RIFE model";
        p.directoryEnv = "MONTAGE_RIFE_MODEL";
        p.urlEnv = "MONTAGE_RIFE_MODEL_URL";
        // The authors' release, from Zhewei Huang's own Hugging Face account.
        p.files = {
            {"RIFEv4.26_0921.zip",
             "https://huggingface.co/hzwer/RIFE/resolve/01fdc7e97404120c243c3ea7b427046e5dc7643e/RIFEv4.26_0921.zip",
             "1fa9b9cda3d9b8c3e301359e2595960902f97bf926c08598b0e9957a3f3f760e", 22869906},
        };
        return p;
    }();
    return pack;
}

namespace {

// The weights the graph names ("data/<key>"), unpacked from the release's flownet.pkl (itself a zip),
// and the graph beside them. Done once; a stamp marks it complete.
bool unpack(std::string& graphPath, std::string* error) {
    const ModelPack& pack = rifeModel();
    const QString dir = QString::fromStdString(pack.directory());
    graphPath = (dir + "/rife-4.26.onnx").toStdString();
    const QString stamp = dir + "/data/unpacked-" + QString::fromStdString(pack.files[0].sha256.substr(0, 12));
    if (QFile::exists(stamp) && QFile::exists(QString::fromStdString(graphPath))) return true;
    ZipReader outer;
    std::string pkl;
    if (!outer.openFile(pack.path(pack.files[0])) || !outer.read("RIFEv4.26_0921/flownet.pkl", pkl, error)) {
        if (error && error->empty()) *error = "The RIFE download is damaged";
        return false;
    }
    ZipReader inner;
    if (!inner.open(std::move(pkl))) {
        if (error) *error = "The RIFE weights are damaged";
        return false;
    }
    QDir().mkpath(dir + "/data");
    int written = 0;
    for (const ZipReader::Entry& e : inner.entries()) {
        const size_t at = e.name.find("/data/");
        if (at == std::string::npos) continue;
        std::string bytes;
        if (!inner.read(e, bytes, error)) return false;
        QFile f(dir + "/data/" + QString::fromStdString(e.name.substr(at + 6)));
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size())) {
            if (error) *error = "Cannot write " + f.fileName().toStdString();
            return false;
        }
        ++written;
    }
    QFile graph(QString::fromStdString(graphPath));
    if (written == 0 || !graph.open(QIODevice::WriteOnly) ||
        graph.write(reinterpret_cast<const char*>(kRifeGraph), qint64(kRifeGraphSize)) != qint64(kRifeGraphSize)) {
        if (error) *error = "Cannot unpack the RIFE model";
        return false;
    }
    graph.close();
    QFile s(stamp);
    s.open(QIODevice::WriteOnly);
    return true;
}

}  // namespace

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool rifeAvailable() { return false; }
bool rifeInterpolate(const Image&, const Image&, double, Image&, std::string* error) {
    if (error) *error = "This build of Montage has no ONNX Runtime";
    return false;
}

#else

bool rifeAvailable() { return true; }

namespace {

struct Model {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-rife"};
    std::unique_ptr<Ort::Session> session;
};

std::shared_ptr<Model> loadModel(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Model>();  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    if (!rifeModel().installed()) {
        if (error) *error = "The RIFE model is not downloaded";
        return nullptr;
    }
    std::string graph;
    if (!unpack(graph, error)) return nullptr;
    try {
        auto model = std::make_shared<Model>();
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef _WIN32
        const std::wstring path = QString::fromStdString(graph).toStdWString();
#else
        const std::string& path = graph;
#endif
        model->session = std::make_unique<Ort::Session>(model->env, path.c_str(), so);
        *cached = model;
        return model;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The RIFE model could not be loaded: ") + e.what();
        return nullptr;
    }
}

}  // namespace

bool rifeInterpolate(const Image& a, const Image& b, double t, Image& out, std::string* error) {
    if (a.empty() || a.width != b.width || a.height != b.height) {
        if (error) *error = "The two frames differ in size";
        return false;
    }
    auto model = loadModel(error);
    if (!model) return false;
    // Padded to multiples of 64 (the coarsest level works at 1/64), with zeros as the authors pad.
    const int W = a.width, H = a.height, PW = (W + 63) / 64 * 64, PH = (H + 63) / 64 * 64;
    const size_t plane = size_t(PW) * size_t(PH);
    auto planar = [&](const Image& im) {
        std::vector<float> v(plane * 3, 0.0f);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float* p = im.at(x, y);
                const float al = p[3] > 1e-6f ? p[3] : 1.0f;
                for (int c = 0; c < 3; ++c) v[size_t(c) * plane + size_t(y) * size_t(PW) + size_t(x)] = std::clamp(p[c] / al, 0.0f, 1.0f);
            }
        return v;
    };
    std::vector<float> va = planar(a), vb = planar(b);
    float ts = float(std::clamp(t, 0.0, 1.0));
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t shape[4] = {1, 3, PH, PW}, tshape[4] = {1, 1, 1, 1};
        Ort::Value inputs[3] = {Ort::Value::CreateTensor<float>(cpu, va.data(), va.size(), shape, 4),
                                Ort::Value::CreateTensor<float>(cpu, vb.data(), vb.size(), shape, 4),
                                Ort::Value::CreateTensor<float>(cpu, &ts, 1, tshape, 4)};
        const char* inNames[] = {"img0", "img1", "timestep"};
        const char* outNames[] = {"frame"};
        auto result = model->session->Run(Ort::RunOptions{nullptr}, inNames, inputs, 3, outNames, 1);
        const float* f = result[0].GetTensorData<float>();
        out = Image(W, H, Image::Uninitialized{});
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float* o = out.at(x, y);
                const float al = a.at(x, y)[3] * (1 - ts) + b.at(x, y)[3] * ts;
                for (int c = 0; c < 3; ++c) o[c] = std::clamp(f[size_t(c) * plane + size_t(y) * size_t(PW) + size_t(x)], 0.0f, 1.0f) * al;
                o[3] = al;
            }
        return true;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("RIFE failed: ") + e.what();
        return false;
    }
}

#endif

bool cachedRife(const Image& a, const Image& b, double t, const std::string& cacheKey, Image& out) {
    static std::mutex m;
    static std::deque<std::pair<std::string, std::shared_ptr<const Image>>> cache;
    char tk[32];
    std::snprintf(tk, sizeof tk, "|%.4f|%dx%d", t, a.width, a.height);
    const std::string key = cacheKey.empty() ? std::string() : cacheKey + tk;
    if (!key.empty()) {
        std::lock_guard lock(m);
        for (const auto& [k, img] : cache)
            if (k == key) {
                out = *img;
                return true;
            }
    }
    Image result;
    if (!rifeInterpolate(a, b, t, result)) return false;
    if (!key.empty()) {
        std::lock_guard lock(m);
        cache.emplace_front(key, std::make_shared<const Image>(result));
        if (cache.size() > 6) cache.pop_back();
    }
    out = std::move(result);
    return true;
}

}  // namespace montage
