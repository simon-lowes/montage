#include "Segmenter.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#endif

namespace montage {

// ---- Model files ---------------------------------------------------------------

const std::vector<SegmenterFile>& segmenterFiles() {
    static const std::vector<SegmenterFile> files = {
        {"vision_encoder.onnx", "57fe1a2b3d500813fe4987c4209b856920f187ab0b7723d81586b607ea2cffc1", 19826101},
        {"mask_decoder.onnx", "ee24ee1cae6ecc71889912c1893e08b09f645bb63875108ccd180f7218a3647d", 17800677},
        {"memory_attention.onnx", "6477d775639905945949508c08a2c652d01e5933c0eb00a02225ae5f6aa1602c", 20914958},
        {"memory_encoder.onnx", "035b30f8fb1f7c99211ada6eb9d72f0819de819ef94310b14e8147dc97f120ec", 6692568},
    };
    return files;
}

int64_t segmenterDownloadBytes() {
    int64_t n = 0;
    for (const auto& f : segmenterFiles()) n += f.bytes;
    return n;
}

std::string segmenterModelDirectory() {
    if (const char* env = std::getenv("MONTAGE_OBJECT_MODEL"); env && *env) return env;
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) dir = QDir::homePath() + "/.montage";
    return (dir + "/object-models/edgetam-video").toStdString();
}

std::string segmenterFileUrl(const std::string& name) {
    // The EdgeTAM video export (graphs for the memory bank as well as the
    // encoder and decoder), pinned to the revision these hashes describe.
    std::string base = "https://huggingface.co/jax-image-tools/edgetam-video-onnx/resolve/8ca3d3e4169938e65b552cdf14542fde25e8badb";
    if (const char* env = std::getenv("MONTAGE_OBJECT_MODEL_URL"); env && *env) base = env;
    if (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/" + name;
}

bool segmenterModelInstalled() {
    const QString dir = QString::fromStdString(segmenterModelDirectory());
    for (const auto& f : segmenterFiles())
        if (QFileInfo(dir + "/" + QString::fromStdString(f.name)).size() != f.bytes) return false;
    return true;
}

bool segmenterFileVerified(const std::string& path, const SegmenterFile& f) {
    QFile file(QString::fromStdString(path));
    if (file.size() != f.bytes || !file.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&file)) return false;
    return h.result().toHex().toStdString() == f.sha256;
}

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool segmenterAvailable() { return false; }
std::string segmenterRuntimeVersion() { return {}; }

struct ObjectTracker::Impl {};
ObjectTracker::ObjectTracker() = default;
ObjectTracker::~ObjectTracker() = default;
bool ObjectTracker::load(std::string* error) {
    if (error) *error = "This build of Montage cannot pick objects (it was built without ONNX Runtime)";
    return false;
}
void ObjectTracker::reset() {}
bool ObjectTracker::step(const Frame16&, const std::vector<ObjectPoint>&, SegmentResult&, std::string* error) {
    if (error) *error = "This build of Montage cannot pick objects (it was built without ONNX Runtime)";
    return false;
}
int ObjectTracker::framesTracked() const { return 0; }

#else

#include "EdgeTamConstants.inc"

bool segmenterAvailable() { return true; }
std::string segmenterRuntimeVersion() { return Ort::GetVersionString(); }

namespace {

constexpr int kHidden = 256;          // decoder channels
constexpr int kMemDim = 64;           // memory channels
constexpr int kFeat = 64;             // 64 x 64 top-level feature map
constexpr int kTokens = kFeat * kFeat;
constexpr int kMemTokens = 512;       // spatial tokens per remembered frame (EdgeTAM's perceiver)
constexpr int kMaskMem = 7;           // memory slots: the conditioning frames plus the last 6 tracked
constexpr int kMaxPointers = 16;      // object pointers attended to
constexpr float kNoObject = -1024.f;  // SAM 2's logit for "no object here"

struct Models {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage"};
    std::unique_ptr<Ort::Session> encoder, decoder, attention, memory;
};

std::basic_string<ORTCHAR_T> ortPath(const QString& path) {
#ifdef _WIN32
    return path.toStdWString();
#else
    return path.toStdString();
#endif
}

// Loaded once and kept for the session (clicks are then answered at once);
// ORT sessions are safe to run from several threads. Never freed, so no
// ONNX Runtime teardown races the library's own at exit.
std::shared_ptr<Models> loadModels(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Models>();
    std::lock_guard lock(m);
    if (*cached) return *cached;
    const QString dir = QString::fromStdString(segmenterModelDirectory());
    for (const auto& f : segmenterFiles())
        if (!QFileInfo::exists(dir + "/" + QString::fromStdString(f.name))) {
            if (error) *error = "The object model is not downloaded";
            return nullptr;
        }
    try {
        auto models = std::make_shared<Models>();
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(int(std::max(1u, std::thread::hardware_concurrency())));
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        auto open = [&](const char* name) {
            return std::make_unique<Ort::Session>(models->env, ortPath(dir + "/" + name).c_str(), so);
        };
        models->encoder = open("vision_encoder.onnx");
        models->decoder = open("mask_decoder.onnx");
        models->attention = open("memory_attention.onnx");
        models->memory = open("memory_encoder.onnx");
        *cached = models;
        return models;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The object model could not be loaded: ") + e.what();
        return nullptr;
    }
}

Ort::MemoryInfo& cpu() {
    static Ort::MemoryInfo info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    return info;
}

template <class T>
Ort::Value tensor(std::vector<T>& data, std::initializer_list<int64_t> shape) {
    return Ort::Value::CreateTensor<T>(cpu(), data.data(), data.size(), shape.begin(), shape.size());
}

std::vector<float> copyOut(Ort::Value& v) {
    const size_t n = v.GetTensorTypeAndShapeInfo().GetElementCount();
    const float* p = v.GetTensorData<float>();
    return std::vector<float>(p, p + n);
}

// One remembered frame: its spatial memory and the object pointer.
struct Memory {
    std::vector<float> tokens, pos;  // kMemTokens x kMemDim
    std::vector<float> pointer;      // kHidden
};

// RGBA16 to the encoder's normalised CHW input at kSegmenterInput², resampling if needed.
std::vector<float> encoderInput(const Frame16& f) {
    constexpr int S = kSegmenterInput;
    static const float mean[3] = {0.485f, 0.456f, 0.406f}, stdev[3] = {0.229f, 0.224f, 0.225f};
    std::vector<float> out(size_t(3) * S * S);
    const bool same = f.width == S && f.height == S;
    parallelRows(S, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < S; ++x) {
                float rgb[3];
                if (same) {
                    const uint16_t* p = &f.px[(size_t(y) * S + size_t(x)) * 4];
                    for (int c = 0; c < 3; ++c) rgb[c] = p[c] / 65535.f;
                } else {
                    const double sx = std::clamp((x + 0.5) * f.width / S - 0.5, 0.0, f.width - 1.0);
                    const double sy = std::clamp((y + 0.5) * f.height / S - 0.5, 0.0, f.height - 1.0);
                    const int x0 = int(sx), y0i = int(sy), x1 = std::min(x0 + 1, f.width - 1), y1i = std::min(y0i + 1, f.height - 1);
                    const float fx = float(sx - x0), fy = float(sy - y0i);
                    auto px = [&](int xx, int yy, int c) { return f.px[(size_t(yy) * size_t(f.width) + size_t(xx)) * 4 + size_t(c)] / 65535.f; };
                    for (int c = 0; c < 3; ++c) {
                        const float a = px(x0, y0i, c) + (px(x1, y0i, c) - px(x0, y0i, c)) * fx;
                        const float b = px(x0, y1i, c) + (px(x1, y1i, c) - px(x0, y1i, c)) * fx;
                        rgb[c] = a + (b - a) * fy;
                    }
                }
                for (int c = 0; c < 3; ++c) out[size_t(c) * S * S + size_t(y) * S + size_t(x)] = (rgb[c] - mean[c]) / stdev[c];
            }
        }
    });
    return out;
}

// (1, C, H, W) <-> (H*W, 1, C).
std::vector<float> toTokens(const float* chw, int channels, int tokens) {
    std::vector<float> out(size_t(channels) * size_t(tokens));
    for (int c = 0; c < channels; ++c)
        for (int t = 0; t < tokens; ++t) out[size_t(t) * size_t(channels) + size_t(c)] = chw[size_t(c) * size_t(tokens) + size_t(t)];
    return out;
}
std::vector<float> fromTokens(const float* hwc, int channels, int tokens) {
    std::vector<float> out(size_t(channels) * size_t(tokens));
    for (int t = 0; t < tokens; ++t)
        for (int c = 0; c < channels; ++c) out[size_t(c) * size_t(tokens) + size_t(t)] = hwc[size_t(t) * size_t(channels) + size_t(c)];
    return out;
}

}  // namespace

struct ObjectTracker::Impl {
    std::shared_ptr<Models> models;
    std::vector<Memory> conditioning;           // frames with clicks, in the order they were seen
    std::deque<std::pair<int, Memory>> recent;  // the last tracked frames, by step
    int steps = 0;
};

ObjectTracker::ObjectTracker() : d_(std::make_unique<Impl>()) {}
ObjectTracker::~ObjectTracker() = default;

bool ObjectTracker::load(std::string* error) {
    if (!d_->models) d_->models = loadModels(error);
    return d_->models != nullptr;
}

void ObjectTracker::reset() {
    d_->conditioning.clear();
    d_->recent.clear();
    d_->steps = 0;
}

int ObjectTracker::framesTracked() const { return d_->steps; }

bool ObjectTracker::step(const Frame16& frame, const std::vector<ObjectPoint>& points, SegmentResult& out,
                         std::string* error) {
    if (!load(error)) return false;
    if (d_->conditioning.empty() && points.empty()) {
        if (error) *error = "Click the object first";
        return false;
    }
    if (frame.width <= 0 || frame.height <= 0) {
        if (error) *error = "No picture to segment";
        return false;
    }
    Models& m = *d_->models;
    try {
        // Image features at three scales, and the top level's position encoding.
        std::vector<float> pixels = encoderInput(frame);
        Ort::Value in = tensor(pixels, {1, 3, kSegmenterInput, kSegmenterInput});
        const char* encIn[] = {"pixel_values"};
        const char* encOut[] = {"feats0", "feats1", "feats2", "pos2"};
        auto enc = m.encoder->Run(Ort::RunOptions{nullptr}, encIn, &in, 1, encOut, 4);
        const float* feats2 = enc[2].GetTensorData<float>();

        // The features the decoder reads: with no memory on the first frame,
        // otherwise conditioned on the memory bank.
        std::vector<float> conditioned;
        if (d_->conditioning.empty()) {
            conditioned.assign(feats2, feats2 + size_t(kHidden) * kTokens);
            for (int c = 0; c < kHidden; ++c)
                for (int t = 0; t < kTokens; ++t) conditioned[size_t(c) * kTokens + size_t(t)] += kNoMemoryEmbedding[c];
        } else {
            // Spatial memory: conditioning frames (temporal slot 0, which is the
            // encoding's last entry), then the tracked frames 6..1 steps back.
            std::vector<float> spatial, spatialPos;
            auto addSpatial = [&](const Memory& mem, int slot) {
                spatial.insert(spatial.end(), mem.tokens.begin(), mem.tokens.end());
                const float* tpos = kMemoryTemporalPosition + size_t(slot) * kMemDim;
                for (int t = 0; t < kMemTokens; ++t)
                    for (int c = 0; c < kMemDim; ++c) spatialPos.push_back(mem.pos[size_t(t) * kMemDim + size_t(c)] + tpos[c]);
            };
            auto recentAt = [&](int back) -> const Memory* {
                for (const auto& [s, mem] : d_->recent)
                    if (s == d_->steps - back) return &mem;
                return nullptr;
            };
            for (const Memory& mem : d_->conditioning) addSpatial(mem, kMaskMem - 1);
            for (int back = kMaskMem - 1; back >= 1; --back)
                if (const Memory* mem = recentAt(back)) addSpatial(*mem, back - 1);
            // Object pointers: conditioning frames, then tracked frames 1..15 back; each is four 64-wide tokens.
            std::vector<float> pointers;
            for (const Memory& mem : d_->conditioning) pointers.insert(pointers.end(), mem.pointer.begin(), mem.pointer.end());
            for (int back = 1; back < kMaxPointers; ++back)
                if (const Memory* mem = recentAt(back)) pointers.insert(pointers.end(), mem->pointer.begin(), mem->pointer.end());
            std::vector<float> pointerPos(pointers.size(), 0.f);  // EdgeTAM has no temporal encoding for pointers

            std::vector<float> current = toTokens(feats2, kHidden, kTokens);
            std::vector<float> currentPos = toTokens(enc[3].GetTensorData<float>(), kHidden, kTokens);
            const int64_t nSpatial = int64_t(spatial.size() / kMemDim), nPointer = int64_t(pointers.size() / kMemDim);
            Ort::Value att[] = {tensor(current, {kTokens, 1, kHidden}),       tensor(currentPos, {kTokens, 1, kHidden}),
                                tensor(spatial, {nSpatial, 1, kMemDim}),      tensor(spatialPos, {nSpatial, 1, kMemDim}),
                                tensor(pointers, {nPointer, 1, kMemDim}),     tensor(pointerPos, {nPointer, 1, kMemDim})};
            const char* attIn[] = {"current_vision_features", "current_vision_position_embeddings", "spatial_memory",
                                   "spatial_memory_position_embeddings", "pointer_memory", "pointer_memory_position_embeddings"};
            const char* attOut[] = {"conditioned_features"};
            auto res = m.attention->Run(Ort::RunOptions{nullptr}, attIn, att, 6, attOut, 1);
            conditioned = fromTokens(res[0].GetTensorData<float>(), kHidden, kTokens);
        }

        // Decode the mask. Without clicks SAM 2 passes one padding point (label -1).
        std::vector<float> coords;
        std::vector<int32_t> labels;
        for (const ObjectPoint& p : points) {
            coords.push_back(float(p.x * kSegmenterInput));
            coords.push_back(float(p.y * kSegmenterInput));
            labels.push_back(p.label);
        }
        if (points.empty()) {
            coords = {0.f, 0.f};
            labels = {-1};
        }
        const int64_t np = int64_t(labels.size());
        Ort::Value dec[] = {std::move(enc[0]), std::move(enc[1]), tensor(conditioned, {1, kHidden, kFeat, kFeat}),
                            tensor(coords, {1, 1, np, 2}), tensor(labels, {1, 1, np})};
        const char* decIn[] = {"feats0", "feats1", "feats2", "input_points", "input_labels"};
        const char* decOut[] = {"pred_masks", "high_res_masks", "object_pointer", "object_score_logits"};
        auto res = m.decoder->Run(Ort::RunOptions{nullptr}, decIn, dec, 5, decOut, 4);
        out.logits = copyOut(res[0]);
        std::vector<float> highRes = copyOut(res[1]);
        std::vector<float> pointer = copyOut(res[2]);
        out.objectScore = res[3].GetTensorData<float>()[0];
        if (out.logits.size() != size_t(kObjectGrid) * kObjectGrid || highRes.size() != size_t(kSegmenterInput) * kSegmenterInput ||
            pointer.size() != size_t(kHidden)) {
            if (error) *error = "The object model gave an unexpected result";
            return false;
        }
        if (out.objectScore <= 0) {
            // The object is not in this frame: remembered as absent.
            std::fill(out.logits.begin(), out.logits.end(), kNoObject);
            std::fill(highRes.begin(), highRes.end(), kNoObject);
        }

        // Remember this frame for the ones that follow.
        std::vector<float> raw(feats2, feats2 + size_t(kHidden) * kTokens);
        bool fromClicks[1] = {!points.empty()};
        Ort::Value mem[] = {tensor(raw, {1, kHidden, kFeat, kFeat}), tensor(highRes, {1, 1, kSegmenterInput, kSegmenterInput}),
                            Ort::Value::CreateTensor<bool>(cpu(), fromClicks, 1, std::array<int64_t, 1>{1}.data(), 1)};
        const char* memIn[] = {"vision_features", "pred_masks_high_res", "is_mask_from_pts"};
        const char* memOut[] = {"memory_tokens", "memory_pos_enc"};
        auto enc2 = m.memory->Run(Ort::RunOptions{nullptr}, memIn, mem, 3, memOut, 2);
        Memory remembered{copyOut(enc2[0]), copyOut(enc2[1]), std::move(pointer)};
        if (!points.empty()) {
            d_->conditioning.push_back(std::move(remembered));
        } else {
            d_->recent.emplace_back(d_->steps, std::move(remembered));
            while (!d_->recent.empty() && d_->recent.front().first <= d_->steps - kMaxPointers) d_->recent.pop_front();
        }
        ++d_->steps;
        return true;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("Object segmentation failed: ") + e.what();
        return false;
    }
}

#endif

}  // namespace montage
