// The windowed inference (estimateVideoDepth: windows, key frames, scale and shift alignment, blending) follows
// video_depth_anything/video_depth.py's infer_video_depth at DepthAnything/Video-Depth-Anything, Copyright (2025)
// Bytedance Ltd. and/or its affiliates, licensed under the Apache License, Version 2.0 (LICENSES/Apache-2.0.txt);
// modified: rewritten in C++ on ONNX Runtime for Montage.
#include "VideoDepth.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include "Decoder.h"
#include "ImageSequence.h"
#include "SuperScale.h"
#include "core/Zip.h"

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

// The network in three parts without its weights (src/media/vda-small-*.onnx), compiled in by CMake.
extern const unsigned char kVdaEncoderGraph[];
extern const size_t kVdaEncoderGraphSize;
extern const unsigned char kVdaTemporalGraph[];
extern const size_t kVdaTemporalGraphSize;
extern const unsigned char kVdaDecoderGraph[];
extern const size_t kVdaDecoderGraphSize;

namespace montage {

const ModelPack& videoDepthModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "video-depth";
        p.title = "video depth model";
        p.directoryEnv = "MONTAGE_VIDEO_DEPTH_MODEL";
        p.urlEnv = "MONTAGE_VIDEO_DEPTH_MODEL_URL";
        // The authors' release of the Small model (the only one under Apache-2.0; Base and Large are not for commercial use).
        p.files = {
            {"video_depth_anything_vits.pth",
             "https://huggingface.co/depth-anything/Video-Depth-Anything-Small/resolve/256875362cff76724b920335dfb4b29dd611f66e/"
             "video_depth_anything_vits.pth",
             "13379300b739e659f076a59d52e9801bd8d38c541a7e71f73bbca4dcfb013609", 116440756},
        };
        return p;
    }();
    return pack;
}

namespace {

struct Graph {
    const char* file;
    const unsigned char* bytes;
    size_t size;
};
const Graph kGraphs[3] = {{"vda-small-encoder.onnx", kVdaEncoderGraph, kVdaEncoderGraphSize},
                          {"vda-small-temporal.onnx", kVdaTemporalGraph, kVdaTemporalGraphSize},
                          {"vda-small-decoder.onnx", kVdaDecoderGraph, kVdaDecoderGraphSize}};

bool writeGraph(const QString& path, const Graph& g) {
    const QByteArray bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(g.bytes), qsizetype(g.size));
    QFile f(path);
    if (f.open(QIODevice::ReadOnly) && f.readAll() == bytes) return true;
    f.close();
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == qint64(bytes.size());
}

// The weights the graphs name ("data/<key>"), unpacked from the checkpoint (a zip), and the graphs beside them. Done
// once; a stamp marks it complete. The graphs are compiled in, so a new Montage may bring new ones: the copies on disk
// are rewritten when they differ.
bool unpack(std::string* error) {
    const ModelPack& pack = videoDepthModel();
    const QString dir = QString::fromStdString(pack.directory());
    const QString stamp = dir + "/data/unpacked-" + QString::fromStdString(pack.files[0].sha256.substr(0, 12));
    if (!QFile::exists(stamp)) {
        ZipReader zip;
        if (!zip.openFile(pack.path(pack.files[0]))) {
            if (error) *error = "The video depth download is damaged";
            return false;
        }
        QDir().mkpath(dir + "/data");
        int written = 0;
        for (const ZipReader::Entry& e : zip.entries()) {
            const size_t at = e.name.find("/data/");
            if (at == std::string::npos) continue;
            std::string bytes;
            if (!zip.read(e, bytes, error)) return false;
            QFile f(dir + "/data/" + QString::fromStdString(e.name.substr(at + 6)));
            if (!f.open(QIODevice::WriteOnly) || f.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size())) {
                if (error) *error = "Cannot write " + f.fileName().toStdString();
                return false;
            }
            ++written;
        }
        if (written == 0) {
            if (error) *error = "The video depth download is damaged";
            return false;
        }
    }
    for (const Graph& g : kGraphs)
        if (!writeGraph(dir + "/" + g.file, g)) {
            if (error) *error = "Cannot unpack the video depth model";
            return false;
        }
    if (!QFile::exists(stamp)) {
        QFile s(stamp);
        s.open(QIODevice::WriteOnly);
    }
    return true;
}

// The picture size the model runs at for a w x h frame, as its authors size it: the short side at `size` (less for
// frames wider than 16:9), both sides multiples of 14 and neither below it.
void inferenceSize(int w, int h, int size, int& iw, int& ih) {
    size = std::max(14, size / 14 * 14);
    const double ratio = double(std::max(w, h)) / std::max(1, std::min(w, h));
    if (ratio > 1.78) size = std::max(14, int(std::lround(int(size * 1.78 / ratio) / 14.0)) * 14);
    const double k = double(size) / std::max(1, std::min(w, h));
    auto fit = [&](double x) {
        int y = int(std::lround(x / 14)) * 14;
        if (y < size) y = int(std::ceil(x / 14)) * 14;
        return std::max(14, y);
    };
    iw = fit(w * k), ih = fit(h * k);
}

}  // namespace

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool videoDepthAvailable() { return false; }
bool estimateVideoDepth(int, const std::function<bool(int, Image&)>&, const std::function<bool(int, const DepthMap&)>&, float&, float&, int,
                        const std::function<bool(double)>&, std::string* error) {
    if (error) *error = "This build of Montage has no ONNX Runtime";
    return false;
}

#else

bool videoDepthAvailable() { return true; }

namespace {

constexpr int kWindow = 32, kOverlap = 10, kInterp = 8, kAlign = kOverlap - kInterp;
constexpr int kKeyframes[kOverlap] = {0, 12, 24, 25, 26, 27, 28, 29, 30, 31};
constexpr int kChannels[4] = {48, 96, 192, 384};

struct Models {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-video-depth"};
    std::unique_ptr<OrtModel> encoder, temporal, decoder;
};

std::shared_ptr<Models> loadModels(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Models>();  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    const ModelPack& pack = videoDepthModel();
    if (!pack.installed()) {
        if (error) *error = "The video depth model is not downloaded";
        return nullptr;
    }
    if (!unpack(error)) return nullptr;
    try {
        auto models = std::make_shared<Models>();
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        std::unique_ptr<OrtModel>* into[3] = {&models->encoder, &models->temporal, &models->decoder};
        for (int i = 0; i < 3; ++i) {
            const QString file = QString::fromStdString(pack.directory()) + "/" + kGraphs[i].file;
#ifdef _WIN32
            const std::wstring path = file.toStdWString();
#else
            const std::string path = file.toStdString();
#endif
            *into[i] = openModel(models->env, path.c_str(), so);
        }
        *cached = models;
        return models;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The video depth model could not be loaded: ") + e.what();
        return nullptr;
    }
}

// One frame's four projected feature maps (48, 96, 192 and 384 channels on the h x w patch grid).
struct Features {
    std::vector<float> f[4];
};

// Least squares s, b with s * pred + b ≈ target (MiDaS's scale and shift).
void scaleAndShift(const std::vector<const std::vector<float>*>& pred, const std::vector<const std::vector<float>*>& target, double& s,
                   double& b) {
    double a00 = 0, a01 = 0, a11 = 0, b0 = 0, b1 = 0;
    for (size_t k = 0; k < pred.size(); ++k)
        for (size_t i = 0; i < pred[k]->size(); ++i) {
            const double p = (*pred[k])[i], t = (*target[k])[i];
            a00 += p * p, a01 += p, a11 += 1, b0 += p * t, b1 += t;
        }
    const double det = a00 * a11 - a01 * a01;
    if (std::fabs(det) < 1e-12) {
        s = 1, b = 0;
        return;
    }
    s = (a11 * b0 - a01 * b1) / det;
    b = (-a01 * b0 + a00 * b1) / det;
}

}  // namespace

bool estimateVideoDepth(int count, const std::function<bool(int, Image&)>& frameAt, const std::function<bool(int, const DepthMap&)>& sink,
                        float& lo, float& hi, int size, const std::function<bool(double)>& progress, std::string* error) {
    lo = 0, hi = 1;
    if (count <= 0) {
        if (error) *error = "No frames";
        return false;
    }
    auto models = loadModels(error);
    if (!models) return false;
    int W = 0, H = 0, ph = 0, pw = 0;
    const float mean[3] = {0.485f, 0.456f, 0.406f}, stdev[3] = {0.229f, 0.224f, 0.225f};
    Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto cancelled = [&] {
        if (error) *error = "Cancelled";
        return false;
    };
    // Frame i (the last one again past the end, as the authors pad) through the encoder.
    auto encode = [&](int i, Features& out) {
        Image img;
        if (!frameAt(std::clamp(i, 0, count - 1), img) || img.empty()) {
            if (error && error->empty()) *error = "A frame could not be read";
            return false;
        }
        if (!W) {
            inferenceSize(img.width, img.height, size, W, H);
            ph = H / 14, pw = W / 14;
        }
        const Image small = img.width == W && img.height == H ? img : resizeImage(img, W, H);
        const size_t plane = size_t(W) * size_t(H);
        std::vector<float> input(plane * 3);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float* p = small.at(x, y);
                const float a = p[3] > 1e-6f ? p[3] : 1.0f;
                for (int c = 0; c < 3; ++c)
                    input[size_t(c) * plane + size_t(y) * size_t(W) + size_t(x)] = (std::clamp(p[c] / a, 0.0f, 1.0f) - mean[c]) / stdev[c];
            }
        const int64_t shape[4] = {1, 3, H, W};
        Ort::Value tensor = Ort::Value::CreateTensor<float>(cpu, input.data(), input.size(), shape, 4);
        const char* inNames[] = {"frames"};
        const char* outNames[] = {"layer1", "layer2", "layer3", "layer4"};
        auto result = models->encoder->Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1, outNames, 4);
        for (int k = 0; k < 4; ++k) {
            const float* d = result[size_t(k)].GetTensorData<float>();
            out.f[k].assign(d, d + size_t(kChannels[k]) * size_t(ph) * size_t(pw));
        }
        return true;
    };
    const int step = kWindow - kOverlap;
    const int windows = (count + step - 1) / step;
    std::vector<Features> previous;  // the last window's key frames (kKeyframes), which start the next
    std::deque<std::pair<int, DepthMap>> tail;  // aligned frames not yet given out: the last eight are blended again
    std::vector<float> ref[kAlign];  // what each window is aligned to: the first frame's depth, and the last window's twelfth
    std::vector<float> samples;      // of every frame given out, for the run's percentiles (thinned to stay small)
    size_t thin = 1;                 // (every thin-th sample of a frame's 4096 kept)
    constexpr size_t kMostSamples = size_t(1) << 21;
    int given = 0;
    auto giveOut = [&](int upTo) {
        while (!tail.empty() && tail.front().first < upTo) {
            const auto& [index, map] = tail.front();
            if (index < count) {
                if (!sink(index, map)) return cancelled();
                const size_t stride = std::max<size_t>(1, map.values.size() / 4096) * thin;
                for (size_t i = 0; i < map.values.size(); i += stride) samples.push_back(map.values[i]);
                if (samples.size() > kMostSamples) {  // (a long clip: every other kept, and half as many from now on)
                    for (size_t i = 0; 2 * i < samples.size(); ++i) samples[i] = samples[2 * i];
                    samples.resize((samples.size() + 1) / 2);
                    thin *= 2;
                }
                given = index + 1;
            }
            tail.pop_front();
        }
        return true;
    };
    try {
        for (int w = 0; w < windows; ++w) {
            if (progress && !progress(double(w) / windows)) return cancelled();
            const int start = w * step;
            std::vector<Features> inputs(kWindow);
            for (int i = 0; i < kWindow; ++i) {
                if (w > 0 && i < kOverlap) inputs[size_t(i)] = std::move(previous[size_t(i)]);
                else if (!encode(start + i, inputs[size_t(i)])) return false;
            }
            // The window's temporal pass.
            const size_t grid = size_t(ph) * size_t(pw);
            std::vector<float> l3(size_t(kWindow) * kChannels[2] * grid), l4(size_t(kWindow) * kChannels[3] * grid);
            for (int i = 0; i < kWindow; ++i) {
                std::copy(inputs[size_t(i)].f[2].begin(), inputs[size_t(i)].f[2].end(), l3.begin() + std::ptrdiff_t(size_t(i) * kChannels[2] * grid));
                std::copy(inputs[size_t(i)].f[3].begin(), inputs[size_t(i)].f[3].end(), l4.begin() + std::ptrdiff_t(size_t(i) * kChannels[3] * grid));
            }
            const int64_t s3[4] = {kWindow, kChannels[2], ph, pw}, s4[4] = {kWindow, kChannels[3], ph, pw};
            Ort::Value tin[2] = {Ort::Value::CreateTensor<float>(cpu, l3.data(), l3.size(), s3, 4),
                                 Ort::Value::CreateTensor<float>(cpu, l4.data(), l4.size(), s4, 4)};
            const char* tinNames[] = {"layer3", "layer4"};
            const char* toutNames[] = {"path3"};
            auto path3 = models->temporal->Run(Ort::RunOptions{nullptr}, tinNames, tin, 2, toutNames, 1);
            const auto p3dims = path3[0].GetTensorTypeAndShapeInfo().GetShape();
            const size_t p3frame = size_t(p3dims[1] * p3dims[2] * p3dims[3]);
            float* p3 = path3[0].GetTensorMutableData<float>();
            // Each frame's depth.
            std::vector<DepthMap> depth(kWindow);
            for (int i = 0; i < kWindow; ++i) {
                const int64_t sp[4] = {1, p3dims[1], p3dims[2], p3dims[3]};
                const int64_t s1[4] = {1, kChannels[0], ph, pw}, s2[4] = {1, kChannels[1], ph, pw};
                Features& f = inputs[size_t(i)];
                Ort::Value din[3] = {Ort::Value::CreateTensor<float>(cpu, p3 + size_t(i) * p3frame, p3frame, sp, 4),
                                     Ort::Value::CreateTensor<float>(cpu, f.f[1].data(), f.f[1].size(), s2, 4),
                                     Ort::Value::CreateTensor<float>(cpu, f.f[0].data(), f.f[0].size(), s1, 4)};
                const char* dinNames[] = {"path3", "layer2", "layer1"};
                const char* doutNames[] = {"depth"};
                auto out = models->decoder->Run(Ort::RunOptions{nullptr}, dinNames, din, 3, doutNames, 1);
                const float* d = out[0].GetTensorData<float>();
                depth[size_t(i)].width = W, depth[size_t(i)].height = H;
                depth[size_t(i)].values.assign(d, d + size_t(W) * size_t(H));
            }
            // Aligned to what came before, as the authors align windows.
            if (w == 0) {
                for (int i = 0; i < kWindow; ++i) tail.emplace_back(i, depth[size_t(i)]);
                ref[0] = depth[0].values;
                ref[1] = depth[size_t(kKeyframes[1])].values;
            } else {
                double s, b;
                scaleAndShift({&depth[0].values, &depth[1].values}, {&ref[0], &ref[1]}, s, b);
                auto fitted = [&](DepthMap& m) {
                    for (float& v : m.values) v = std::max(0.0f, float(v * s + b));
                };
                // The last eight aligned frames are these windows' inputs 2 to 9 too: blended from the old to the new.
                for (int k = 0; k < kInterp; ++k) {
                    DepthMap& post = depth[size_t(kAlign + k)];
                    fitted(post);
                    DepthMap& pre = tail[tail.size() - size_t(kInterp) + size_t(k)].second;
                    const float wt = float(k) / float(kInterp - 1);
                    for (size_t i = 0; i < pre.values.size() && i < post.values.size(); ++i)
                        pre.values[i] = pre.values[i] * (1 - wt) + post.values[i] * wt;
                }
                for (int i = kOverlap; i < kWindow; ++i) {
                    fitted(depth[size_t(i)]);
                    tail.emplace_back(start + i, depth[size_t(i)]);
                }
                ref[1] = depth[size_t(kKeyframes[1])].values;
            }
            previous.assign(size_t(kOverlap), Features{});
            for (int i = 0; i < kOverlap; ++i) previous[size_t(i)] = std::move(inputs[size_t(kKeyframes[i])]);
            // Every frame but the last eight is final.
            if (!giveOut(start + kWindow - kInterp)) return false;
        }
        if (!giveOut(count)) return false;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("Video depth estimation failed: ") + e.what();
        return false;
    } catch (const std::bad_alloc&) {
        if (error) *error = "Not enough memory for the video depth analysis: try a smaller size";
        return false;
    }
    if (given < count) {
        if (error) *error = "Video depth estimation gave too few frames";
        return false;
    }
    if (!samples.empty()) {
        const size_t a = samples.size() / 100, z = samples.size() - 1 - samples.size() / 100;
        std::nth_element(samples.begin(), samples.begin() + std::ptrdiff_t(a), samples.end());
        lo = samples[a];
        std::nth_element(samples.begin(), samples.begin() + std::ptrdiff_t(z), samples.end());
        hi = std::max(samples[z], lo + 1e-6f);
    }
    if (progress) progress(1.0);
    return true;
}

#endif

// ---- Kept analyses ------------------------------------------------------------------------------------------------
// A file per analysed media file, named by a hash of its path:
//   "MDEP" version(u32=2) mediaSize(i64) mediaModified(i64 ms) start(f64) step(f64) frame(f64: the media's frame, s)
//   count(i32) width(i32) height(i32)
//   lo(f32) hi(f32) pathLength(u32) path offsets[count + 1](u64), then each frame: its least and greatest disparity
//   (f32, f32) and its values as 12 bits between them (in 16), each row's differences from the left, deflated
//   (qCompress). (Twelve bits are 4096 steps across the frame's depth, finer than its effects tell; 16 kept the model's
//   noise and took over twice the room.) The folder is kept under a size: the analyses longest unchanged go first.

std::string videoDepthCacheDirectory() {
    const QByteArray env = qgetenv("MONTAGE_DEPTH_CACHE");
    if (!env.isEmpty()) return QString::fromLocal8Bit(env).toStdString();
    return (QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/depth").toStdString();
}

namespace {

constexpr uint32_t kKeptVersion = 2;

QString keptPath(const std::string& media) {
    const QString abs = QFileInfo(QString::fromStdString(media)).absoluteFilePath();
    const QByteArray hash = QCryptographicHash::hash(abs.toUtf8(), QCryptographicHash::Sha1).toHex().left(24);
    return QString::fromStdString(videoDepthCacheDirectory()) + "/" + QString::fromLatin1(hash) + ".mdepth";
}

struct Kept {
    int64_t mediaSize = 0, mediaModified = 0;
    double start = 0, step = 0, frame = 0;
    int count = 0, width = 0, height = 0;
    float lo = 0, hi = 1;
    std::string path;
    std::vector<uint64_t> offsets;
};

template <typename T>
bool readValue(QFile& f, T& v) {
    return f.read(reinterpret_cast<char*>(&v), sizeof v) == qint64(sizeof v);
}
template <typename T>
void writeValue(QByteArray& out, const T& v) {
    out.append(reinterpret_cast<const char*>(&v), qsizetype(sizeof v));
}

// The file's size and modification time (of the file on disk: a path read with Interpret Footage settings is its file's).
bool mediaStamp(const std::string& media, int64_t& size, int64_t& modified) {
    const QFileInfo fi(QString::fromStdString(mediaFileOnDisk(media)));
    if (!fi.exists()) return false;
    size = fi.size();
    modified = fi.lastModified().toMSecsSinceEpoch();
    return true;
}

bool readKept(const QString& file, Kept& k) {
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;
    char magic[4];
    uint32_t version = 0, length = 0;
    if (f.read(magic, 4) != 4 || std::memcmp(magic, "MDEP", 4) != 0 || !readValue(f, version) || version != kKeptVersion) return false;
    if (!readValue(f, k.mediaSize) || !readValue(f, k.mediaModified) || !readValue(f, k.start) || !readValue(f, k.step) ||
        !readValue(f, k.frame) || !readValue(f, k.count) || !readValue(f, k.width) || !readValue(f, k.height) || !readValue(f, k.lo) || !readValue(f, k.hi) ||
        !readValue(f, length) || length > 65536 || k.count <= 0 || k.width <= 0 || k.height <= 0 || !(k.frame > 0) || !(k.step > 0))
        return false;
    const QByteArray path = f.read(length);
    if (path.size() != qsizetype(length)) return false;
    k.path = path.toStdString();
    k.offsets.resize(size_t(k.count) + 1);
    const qint64 bytes = qint64(k.offsets.size() * sizeof(uint64_t));
    return f.read(reinterpret_cast<char*>(k.offsets.data()), bytes) == bytes && k.offsets.back() <= uint64_t(f.size());
}

QByteArray packFrame(const DepthMap& m) {
    float a = m.values.empty() ? 0.0f : m.values[0], z = a;
    for (float v : m.values) a = std::min(a, v), z = std::max(z, v);
    const float span = std::max(1e-12f, z - a);
    QByteArray raw(qsizetype(m.values.size() * 2), Qt::Uninitialized);
    auto* q = reinterpret_cast<uint16_t*>(raw.data());
    for (int y = 0; y < m.height; ++y) {
        uint16_t left = 0;
        for (int x = 0; x < m.width; ++x) {
            const size_t i = size_t(y) * size_t(m.width) + size_t(x);
            const auto v = uint16_t(std::lround(std::clamp((m.values[i] - a) / span, 0.0f, 1.0f) * 4095));
            q[i] = uint16_t(v - left);
            left = v;
        }
    }
    QByteArray out;
    writeValue(out, a);
    writeValue(out, z);
    out.append(qCompress(raw, 6));
    return out;
}

bool unpackFrame(const QByteArray& blob, const Kept& k, DepthMap& out) {
    if (blob.size() < 8) return false;
    float a, z;
    std::memcpy(&a, blob.constData(), 4);
    std::memcpy(&z, blob.constData() + 4, 4);
    const QByteArray raw = qUncompress(reinterpret_cast<const uchar*>(blob.constData() + 8), qsizetype(blob.size() - 8));
    const size_t n = size_t(k.width) * size_t(k.height);
    if (size_t(raw.size()) != n * 2) return false;
    const auto* q = reinterpret_cast<const uint16_t*>(raw.constData());
    out.width = k.width, out.height = k.height;
    out.values.resize(n);
    // Stretched as the whole analysis is: 0 at its 1st percentile, 1 at its 99th.
    const float span = std::max(1e-6f, k.hi - k.lo);
    for (int y = 0; y < k.height; ++y) {
        uint16_t v = 0;
        for (int x = 0; x < k.width; ++x) {
            const size_t i = size_t(y) * size_t(k.width) + size_t(x);
            v = uint16_t(v + q[i]);
            const float d = a + (z - a) * (v / 4095.0f);
            out.values[i] = std::clamp((d - k.lo) / span, 0.0f, 1.0f);
        }
    }
    return true;
}

std::mutex gKeptMutex;
struct KeptEntry {
    QString file;
    int64_t stamp = 0;  // the kept file's modification time and size, as read (0: none there)
    std::shared_ptr<const Kept> kept;  // (null: none for the file as it is)
};
std::map<std::string, KeptEntry> gKept;  // by media path
std::deque<std::pair<std::string, std::shared_ptr<const DepthMap>>> gFrames;  // recently read frames, by "<file>|<stamp>|<index>"

int64_t keptStamp(const QString& file) {
    const QFileInfo fi(file);
    return fi.exists() ? fi.lastModified().toMSecsSinceEpoch() * 4096 + (fi.size() & 4095) : 0;
}

// The kept analysis of `media` as the file is now, or null; read again when it or the kept file changes (another
// process may analyse it).
std::shared_ptr<const Kept> keptFor(const std::string& media, QString& file, int64_t& stamp) {
    int64_t size, modified;
    if (!mediaStamp(media, size, modified)) return nullptr;
    const QString path = keptPath(media);
    const int64_t now = keptStamp(path);
    std::lock_guard lock(gKeptMutex);
    auto it = gKept.find(media);
    if (it == gKept.end() || it->second.stamp != now ||
        (it->second.kept && (it->second.kept->mediaSize != size || it->second.kept->mediaModified != modified))) {
        auto k = std::make_shared<Kept>();
        if (!now || !readKept(path, *k) || k->mediaSize != size || k->mediaModified != modified) k.reset();
        if (gKept.size() > 64) gKept.clear();
        it = gKept.insert_or_assign(media, KeptEntry{path, now, std::shared_ptr<const Kept>(k)}).first;
    }
    file = it->second.file;
    stamp = it->second.stamp;
    return it->second.kept;
}

std::shared_ptr<const DepthMap> keptFrame(const QString& file, int64_t stamp, const Kept& k, int index) {
    const std::string key = file.toStdString() + "|" + std::to_string(stamp) + "|" + std::to_string(index);
    {
        std::lock_guard lock(gKeptMutex);
        for (const auto& [name, map] : gFrames)
            if (name == key) return map;
    }
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(qint64(k.offsets[size_t(index)]))) return nullptr;
    const QByteArray blob = f.read(qint64(k.offsets[size_t(index) + 1] - k.offsets[size_t(index)]));
    f.close();
    if (keptStamp(file) != stamp) return nullptr;  // (analysed again meanwhile: not this header's frame)
    auto map = std::make_shared<DepthMap>();
    if (!unpackFrame(blob, k, *map)) return nullptr;
    std::lock_guard lock(gKeptMutex);
    gFrames.emplace_front(key, map);
    if (gFrames.size() > 8) gFrames.pop_back();
    return map;
}

}  // namespace

bool analysedDepthRange(const std::string& path, double& start, double& end, double* step) {
    QString file;
    int64_t stamp;
    auto k = keptFor(path, file, stamp);
    if (!k) return false;
    start = k->start;
    end = k->start + k->step * (k->count - 1);
    if (step) *step = k->step;
    return true;
}

std::string analysedDepthStamp(const std::string& path) {
    QString file;
    int64_t stamp;
    return keptFor(path, file, stamp) ? std::to_string(stamp) : std::string();
}

std::shared_ptr<const DepthMap> analysedDepth(const std::string& path, double seconds) {
    QString file;
    int64_t stamp;
    auto k = keptFor(path, file, stamp);
    if (!k) return nullptr;
    // The frame shown at that time (the one starting at or before it), not a mix of it and the next.
    const double shown = std::floor(seconds / k->frame + 1e-4) * k->frame;
    const double pos = (shown - k->start) / k->step;
    if (pos < -1 || pos > k->count) return nullptr;
    const double at = std::clamp(pos, 0.0, double(k->count - 1));
    const int i0 = int(std::floor(at + 1e-3));
    const double frac = std::max(0.0, at - i0);
    if (frac < 1e-3 || i0 + 1 >= k->count) return keptFrame(file, stamp, *k, std::min(i0, k->count - 1));
    if (frac > 1 - 1e-3) return keptFrame(file, stamp, *k, i0 + 1);
    // Between two analysed frames (footage above 30 frames a second, analysed every few): the two mixed.
    auto a = keptFrame(file, stamp, *k, i0), b = keptFrame(file, stamp, *k, i0 + 1);
    if (!a || !b) return a ? a : b;
    auto mixed = std::make_shared<DepthMap>(*a);
    const float t = float(frac);
    for (size_t i = 0; i < mixed->values.size() && i < b->values.size(); ++i) mixed->values[i] += (b->values[i] - mixed->values[i]) * t;
    return mixed;
}

std::shared_ptr<const DepthMap> frameDepth(const Image& img, const std::string& path, double seconds) {
    if (!path.empty() && seconds >= 0)
        if (auto d = analysedDepth(path, seconds)) return d;
    if (depthAvailable() && depthModel().installed()) return cachedDepth(img);
    return nullptr;
}

bool analyzeVideoDepth(const std::string& path, double start, double end, int size, const std::function<bool(double)>& progress,
                       std::string* error) {
    int64_t mediaSize, modified;
    if (!mediaStamp(path, mediaSize, modified)) {
        if (error) *error = "The file is not there";
        return false;
    }
    VideoDecoder dec;
    if (!dec.open(path, error)) return false;
    if (dec.isStill() || dec.duration() <= 0) {
        if (error) *error = "Steady depth is for video; a still's depth is worked out as it is shown";
        return false;
    }
    const double fps = dec.fps() > 0 ? dec.fps() : 25;
    const int stride = std::max(1, int(std::lround(fps / 30)));
    const double last = std::max(0.0, dec.duration() - 0.5 / fps);
    double a = std::clamp(std::min(start, end), 0.0, last), z = std::clamp(std::max(start, end), a, last);
    // What was analysed before stays analysed: the file's analysis covers both (one stretch of depth for both).
    {
        QString was;
        int64_t stamp;
        if (auto old = keptFor(path, was, stamp); old && std::fabs(old->frame - 1 / fps) < 1e-9) {
            a = std::clamp(std::min(a, old->start), 0.0, last);
            z = std::clamp(std::max(z, old->start + old->step * (old->count - 1)), a, last);
        }
    }
    const long first = long(std::floor(a * fps + 1e-6)), lastFrame = long(std::floor(z * fps + 1e-6));
    const int count = int(std::max(1L, (lastFrame - first) / stride + 1));
    // Decoded straight to the size the model runs at.
    const int dw = std::max(1, dec.displayWidth()), dh = std::max(1, dec.displayHeight());
    int iw = 0, ih = 0;
    inferenceSize(dw, dh, size, iw, ih);
    Image previous;
    auto frameAt = [&](int i, Image& img) {
        Frame16Ptr f = dec.frameAt((double(first + long(i) * stride) + 0.25) / fps, iw, ih, true);
        if (f) previous = toImage(*f);
        img = previous;  // (a frame that will not decode keeps the one before)
        return !img.empty();
    };
    QDir().mkpath(QString::fromStdString(videoDepthCacheDirectory()));
    const QString file = keptPath(path);
    static std::atomic<int> runs{0};  // (analyses of one file at once each write their own)
    QFile out(file + QStringLiteral(".%1-%2.part").arg(QCoreApplication::applicationPid()).arg(++runs));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = "Cannot write " + out.fileName().toStdString();
        return false;
    }
    const QByteArray absolute = QFileInfo(QString::fromStdString(path)).absoluteFilePath().toUtf8();
    Kept k;
    k.mediaSize = mediaSize, k.mediaModified = modified;
    k.start = double(first) / fps, k.step = stride / fps, k.frame = 1 / fps, k.count = count;
    k.offsets.assign(size_t(count) + 1, 0);
    auto header = [&] {
        QByteArray h("MDEP", 4);
        writeValue(h, kKeptVersion);
        writeValue(h, k.mediaSize);
        writeValue(h, k.mediaModified);
        writeValue(h, k.start);
        writeValue(h, k.step);
        writeValue(h, k.frame);
        writeValue(h, k.count);
        writeValue(h, k.width);
        writeValue(h, k.height);
        writeValue(h, k.lo);
        writeValue(h, k.hi);
        writeValue(h, uint32_t(absolute.size()));
        h.append(absolute);
        h.append(reinterpret_cast<const char*>(k.offsets.data()), qsizetype(k.offsets.size() * sizeof(uint64_t)));
        return h;
    };
    bool written = out.write(header()) > 0;  // (written again at the end, with the offsets)
    auto sink = [&](int i, const DepthMap& m) {
        k.width = m.width, k.height = m.height;
        k.offsets[size_t(i)] = uint64_t(out.pos());
        const QByteArray blob = packFrame(m);
        written = written && out.write(blob) == qint64(blob.size());
        return written;
    };
    float lo = 0, hi = 1;
    if (!estimateVideoDepth(count, frameAt, sink, lo, hi, size, progress, error) || !written) {
        out.close();
        out.remove();
        if (error && error->empty()) *error = "Cannot write " + out.fileName().toStdString();
        return false;
    }
    k.offsets[size_t(count)] = uint64_t(out.pos());
    k.lo = lo, k.hi = hi;
    const QByteArray h = header();
    out.seek(0);
    if (out.write(h) != h.size() || !out.flush()) {
        out.close();
        out.remove();
        if (error) *error = "Cannot write " + out.fileName().toStdString();
        return false;
    }
    out.close();
    // (Windows will not replace a file another thread is reading from: it reads a frame at a time, so try again.)
    bool moved = false;
    for (int tries = 0; tries < 40 && !moved; ++tries) {
        if (tries) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        QFile::remove(file);
        moved = QFile::rename(out.fileName(), file);
    }
    if (!moved) {
        out.remove();
        if (error) *error = "Cannot write " + file.toStdString();
        return false;
    }
    // The folder kept under its size ($MONTAGE_DEPTH_CACHE_GB, 20 by default): the others longest unchanged removed.
    {
        bool given = false;
        const double gb = qEnvironmentVariableIntValue("MONTAGE_DEPTH_CACHE_GB", &given);
        const qint64 most = qint64((given && gb > 0 ? gb : 20) * 1024 * 1024 * 1024);
        QFileInfoList kept = QDir(QString::fromStdString(videoDepthCacheDirectory())).entryInfoList({"*.mdepth"}, QDir::Files, QDir::Time);
        qint64 total = 0;
        for (const QFileInfo& fi : kept) total += fi.size();
        for (qsizetype i = kept.size(); i-- > 0 && total > most;)
            if (kept[i].absoluteFilePath() != QFileInfo(file).absoluteFilePath() && QFile::remove(kept[i].absoluteFilePath())) total -= kept[i].size();
    }
    std::lock_guard lock(gKeptMutex);
    gKept.erase(path);
    gFrames.clear();
    return true;
}

}  // namespace montage
