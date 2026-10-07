#include "VisualSearch.h"

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "Decoder.h"

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

namespace montage {

const ModelPack& visualModel() {
    static const ModelPack pack = [] {
        const std::string base = "https://huggingface.co/Xenova/clip-vit-base-patch32/resolve/d15189d7028b43f1d3e65039190477f6af591c2a/";
        ModelPack p;
        p.id = "clip-vit-b32";
        p.title = "visual search model";
        p.directoryEnv = "MONTAGE_VISUAL_MODEL";
        p.urlEnv = "MONTAGE_VISUAL_MODEL_URL";
        p.files = {
            // 4-bit weights with float maths (MatMulNBits): closer to the float model than the
            // int8 export, and no integer matrix kernels (ONNX Runtime's AMX one crashed on Windows).
            {"text_model_q4.onnx", base + "onnx/text_model_q4.onnx",
             "e4ccd15d806b8af841a036884b42034535d58e04c52df00f56e73f72d3c166d5", 125742108},
            {"vision_model_q4.onnx", base + "onnx/vision_model_q4.onnx",
             "0769eb1d2f6f68927bbfa6e5330df4c4c3c112f89cd43eae28baafa9e6bd34b4", 63642858},
            {"vocab.json", base + "vocab.json", "5047b556ce86ccaf6aa22b3ffccfc52d391ea4accdab9c2f2407da5b742d4363", 862328},
            {"merges.txt", base + "merges.txt", "9fd691f7c8039210e0fced15865466c65820d09b63988b0174bfe25de299051a", 524619},
        };
        return p;
    }();
    return pack;
}

namespace {

// ---- CLIP's tokenizer ---------------------------------------------------------------
// Byte-level BPE as in GPT-2, with words ending in "</w>": text is normalised
// (NFC, whitespace collapsed, lower case) and split into contractions,
// letter runs, single digits and runs of other symbols.

constexpr int64_t kStart = 49406, kEnd = 49407;
constexpr size_t kContext = 77;

struct Bpe {
    std::unordered_map<std::string, int64_t> vocab;
    std::map<std::pair<std::string, std::string>, int> ranks;
    std::string byteChar[256];  // each byte as the printable character standing for it (UTF-8)
    mutable std::unordered_map<std::string, std::vector<int64_t>> cache;
    mutable std::mutex cacheLock;

    bool load(const QString& vocabPath, const QString& mergesPath) {
        QFile vf(vocabPath), mf(mergesPath);
        if (!vf.open(QIODevice::ReadOnly) || !mf.open(QIODevice::ReadOnly)) return false;
        const QJsonObject v = QJsonDocument::fromJson(vf.readAll()).object();
        for (auto it = v.begin(); it != v.end(); ++it) vocab[it.key().toStdString()] = it.value().toInteger();
        int rank = 0;
        for (const QByteArray& line : mf.readAll().split('\n')) {
            if (line.isEmpty() || line.startsWith("#version")) continue;
            const int sp = int(line.indexOf(' '));
            if (sp <= 0) continue;
            ranks[{line.left(sp).toStdString(), line.mid(sp + 1).trimmed().toStdString()}] = rank++;
        }
        // GPT-2's byte-to-character table: printable bytes stand for themselves, the rest from U+0100 on.
        int extra = 0;
        for (int b = 0; b < 256; ++b) {
            const bool printable = (b >= '!' && b <= '~') || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE && b <= 0xFF);
            const char32_t c = printable ? char32_t(b) : char32_t(256 + extra++);
            byteChar[b] = QString::fromUcs4(&c, 1).toStdString();
        }
        return !vocab.empty() && !ranks.empty();
    }

    // Splits a UTF-8 string into its characters.
    static std::vector<std::string> chars(const std::string& s) {
        std::vector<std::string> out;
        for (size_t i = 0; i < s.size();) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            out.push_back(s.substr(i, n));
            i += n;
        }
        return out;
    }

    std::vector<int64_t> word(const std::string& piece) const {
        {
            std::lock_guard lock(cacheLock);
            auto it = cache.find(piece);
            if (it != cache.end()) return it->second;
        }
        std::string mapped;
        for (unsigned char b : piece) mapped += byteChar[b];
        std::vector<std::string> parts = chars(mapped);
        if (parts.empty()) return {};
        parts.back() += "</w>";
        while (parts.size() > 1) {
            int best = INT_MAX;
            size_t at = 0;
            for (size_t i = 0; i + 1 < parts.size(); ++i) {
                auto r = ranks.find({parts[i], parts[i + 1]});
                if (r != ranks.end() && r->second < best) {
                    best = r->second;
                    at = i;
                }
            }
            if (best == INT_MAX) break;
            // Merge every occurrence of that pair, left to right.
            const std::string a = parts[at], b = parts[at + 1];
            std::vector<std::string> merged;
            for (size_t i = 0; i < parts.size();) {
                if (i + 1 < parts.size() && parts[i] == a && parts[i + 1] == b) {
                    merged.push_back(a + b);
                    i += 2;
                } else {
                    merged.push_back(parts[i++]);
                }
            }
            parts = std::move(merged);
        }
        std::vector<int64_t> ids;
        for (const auto& p : parts) {
            auto it = vocab.find(p);
            if (it != vocab.end()) ids.push_back(it->second);
        }
        std::lock_guard lock(cacheLock);
        cache[piece] = ids;
        return ids;
    }

    std::vector<int64_t> encode(const std::string& text) const {
        static const QRegularExpression split(QStringLiteral(R"('s|'t|'re|'ve|'m|'ll|'d|[\p{L}]+|[\p{N}]|[^\s\p{L}\p{N}]+)"),
                                              QRegularExpression::UseUnicodePropertiesOption);
        const QString clean = QString::fromStdString(text).normalized(QString::NormalizationForm_C).simplified().toLower();
        std::vector<int64_t> ids{kStart};
        auto it = split.globalMatch(clean);
        while (it.hasNext() && ids.size() < kContext - 1) {
            for (int64_t id : word(it.next().captured(0).toStdString())) {
                if (ids.size() >= kContext - 1) break;
                ids.push_back(id);
            }
        }
        ids.push_back(kEnd);
        return ids;
    }
};

std::vector<float> unit(const float* v, size_t n) {
    double len = 0;
    for (size_t i = 0; i < n; ++i) len += double(v[i]) * v[i];
    len = std::sqrt(std::max(len, 1e-20));
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = float(v[i] / len);
    return out;
}

// CLIP's input: the frame's short side at 224 (already, from the decoder), centre-cropped, normalised.
void clipPixels(const Frame16& f, float* out) {
    static const float mean[3] = {0.48145466f, 0.4578275f, 0.40821073f}, stdev[3] = {0.26862954f, 0.26130258f, 0.27577711f};
    constexpr int S = kClipInput;
    const int x0 = std::max(0, (f.width - S) / 2), y0 = std::max(0, (f.height - S) / 2);
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            const int sx = std::min(f.width - 1, x0 + x), sy = std::min(f.height - 1, y0 + y);
            const uint16_t* p = &f.px[(size_t(sy) * size_t(f.width) + size_t(sx)) * 4];
            for (int c = 0; c < 3; ++c) out[size_t(c) * S * S + size_t(y) * S + size_t(x)] = (p[c] / 65535.f - mean[c]) / stdev[c];
        }
}

}  // namespace

// ---- The model ----------------------------------------------------------------------------

#ifdef MONTAGE_WITH_ONNXRUNTIME

bool visualSearchAvailable() { return true; }

struct ClipModel::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-visual"};
    std::unique_ptr<Ort::Session> text, vision;
    Bpe bpe;
};

#else

bool visualSearchAvailable() { return false; }
struct ClipModel::Impl {
    Bpe bpe;
};

#endif

ClipModel::ClipModel() : d_(std::make_unique<Impl>()) {}
ClipModel::~ClipModel() = default;

std::shared_ptr<ClipModel> ClipModel::load(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<ClipModel>();  // kept for the session
    std::lock_guard lock(m);
    if (*cached) return *cached;
    const ModelPack& pack = visualModel();
    if (!pack.installed()) {
        if (error) *error = "The visual search model is not downloaded";
        return nullptr;
    }
    std::shared_ptr<ClipModel> model(new ClipModel);
    if (!model->d_->bpe.load(QString::fromStdString(pack.path(pack.files[2])), QString::fromStdString(pack.path(pack.files[3])))) {
        if (error) *error = "The visual search model's vocabulary could not be read";
        return nullptr;
    }
#ifdef MONTAGE_WITH_ONNXRUNTIME
    if (!ortUsable(error)) return nullptr;
    try {
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(int(std::max(1u, std::thread::hardware_concurrency())));
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        auto open = [&](const ModelFile& f) {
#ifdef _WIN32
            const std::wstring path = QString::fromStdString(pack.path(f)).toStdWString();
#else
            const std::string path = pack.path(f);
#endif
            return std::make_unique<Ort::Session>(model->d_->env, path.c_str(), so);
        };
        model->d_->text = open(pack.files[0]);
        model->d_->vision = open(pack.files[1]);
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The visual search model could not be loaded: ") + e.what();
        return nullptr;
    }
#else
    if (error) *error = "This build of Montage cannot search footage by description (it was built without ONNX Runtime)";
    return nullptr;
#endif
    *cached = model;
    return model;
}

std::vector<int64_t> ClipModel::tokens(const std::string& text) const { return d_->bpe.encode(text); }

std::vector<float> ClipModel::text(const std::string& query, std::string* error) const {
#ifdef MONTAGE_WITH_ONNXRUNTIME
    try {
        std::vector<int64_t> ids = tokens(query);
        const int64_t shape[2] = {1, int64_t(ids.size())};
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value in = Ort::Value::CreateTensor<int64_t>(mem, ids.data(), ids.size(), shape, 2);
        const char* inName[] = {"input_ids"};
        const char* outName[] = {"text_embeds"};
        auto out = d_->text->Run(Ort::RunOptions{nullptr}, inName, &in, 1, outName, 1);
        return unit(out[0].GetTensorData<float>(), out[0].GetTensorTypeAndShapeInfo().GetElementCount());
    } catch (const Ort::Exception& e) {
        if (error) *error = e.what();
    }
#else
    (void)query;
    if (error) *error = "No ONNX Runtime";
#endif
    return {};
}

std::vector<std::vector<float>> ClipModel::images(const std::vector<Frame16Ptr>& frames, std::string* error) const {
    std::vector<std::vector<float>> out;
#ifdef MONTAGE_WITH_ONNXRUNTIME
    if (frames.empty()) return out;
    try {
        constexpr size_t plane = size_t(3) * kClipInput * kClipInput;
        std::vector<float> pixels(frames.size() * plane);
        for (size_t i = 0; i < frames.size(); ++i) clipPixels(*frames[i], pixels.data() + i * plane);
        const int64_t shape[4] = {int64_t(frames.size()), 3, kClipInput, kClipInput};
        auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value in = Ort::Value::CreateTensor<float>(mem, pixels.data(), pixels.size(), shape, 4);
        const char* inName[] = {"pixel_values"};
        const char* outName[] = {"image_embeds"};
        auto res = d_->vision->Run(Ort::RunOptions{nullptr}, inName, &in, 1, outName, 1);
        const float* p = res[0].GetTensorData<float>();
        const size_t dim = res[0].GetTensorTypeAndShapeInfo().GetElementCount() / frames.size();
        for (size_t i = 0; i < frames.size(); ++i) out.push_back(unit(p + i * dim, dim));
    } catch (const Ort::Exception& e) {
        if (error) *error = e.what();
        out.clear();
    }
#else
    (void)frames;
    if (error) *error = "No ONNX Runtime";
#endif
    return out;
}

LabelEmbeddings ClipModel::labels(std::string* error) const {
    std::lock_guard lock(labelsMutex_);
    if (!labels_.empty()) return labels_;
    LabelEmbeddings out;
    for (const TagCategory& c : tagCategories()) {
        out.emplace_back();
        for (const TagLabel& l : c.labels) {
            std::vector<float> mean;
            for (const std::string& prompt : l.prompts) {
                const std::vector<float> e = text(prompt, error);
                if (e.empty()) return {};
                if (mean.empty()) mean.assign(e.size(), 0.f);
                for (size_t i = 0; i < e.size() && i < mean.size(); ++i) mean[i] += e[i];
            }
            out.back().push_back(unit(mean.data(), mean.size()));
        }
    }
    labels_ = out;
    return out;
}

bool indexVideo(const std::string& path, double duration, VisualIndex& out, double step,
                const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::string* error) {
    std::shared_ptr<ClipModel> model = ClipModel::load(error);
    if (!model) return false;
    VideoDecoder dec;
    if (!dec.open(path, error)) return false;
    if (duration <= 0) duration = dec.duration();
    if (duration <= 0) {
        if (error) *error = "The video has no length";
        return false;
    }
    if (step <= 0) step = duration < 20 ? 1.0 : 2.0;
    // Decoded with the short side at CLIP's size, keeping the shape.
    const int dw = std::max(1, dec.displayWidth()), dh = std::max(1, dec.displayHeight());
    const double k = double(kClipInput) / std::min(dw, dh);
    const int w = std::max(kClipInput, int(std::lround(dw * k))), h = std::max(kClipInput, int(std::lround(dh * k)));
    VisualIndex index;
    index.model = visualModel().id;
    index.step = step;
    const int count = std::max(1, int(std::floor(duration / step)));
    std::vector<Frame16Ptr> batch;
    std::vector<double> times;
    auto flush = [&]() {
        if (batch.empty()) return true;
        const auto embeds = model->images(batch, error);
        if (embeds.size() != batch.size()) return false;
        for (size_t i = 0; i < embeds.size(); ++i) index.add(times[i], embeds[i]);
        batch.clear();
        times.clear();
        return true;
    };
    for (int i = 0; i < count; ++i) {
        if (cancel && cancel->load()) {
            if (error) error->clear();
            return false;
        }
        const double t = std::min(duration - 0.01, (i + 0.5) * step);
        Frame16Ptr f = dec.frameAt(t, w, h, true);
        if (!f) continue;
        batch.push_back(f);
        times.push_back(t);
        if (batch.size() == 8 && !flush()) return false;
        if (progress) progress(double(i + 1) / count);
    }
    if (!flush()) return false;
    if (index.samples.empty()) {
        if (error) *error = "No frames could be read";
        return false;
    }
    out = std::move(index);
    return true;
}

}  // namespace montage
