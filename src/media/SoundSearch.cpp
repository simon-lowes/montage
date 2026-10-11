#include "SoundSearch.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <algorithm>
#include <climits>
#include <cmath>
#include <complex>
#include <mutex>
#include <thread>

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

const ModelPack& soundModel() {
    static const ModelPack pack = [] {
        const std::string base = "https://huggingface.co/Xenova/clap-htsat-unfused/resolve/c28f2883575e590e04d3146ff0713c2448d691ba/";
        ModelPack p;
        p.id = "clap-htsat-unfused";
        p.title = "sound search model";
        p.directoryEnv = "MONTAGE_SOUND_MODEL";
        p.urlEnv = "MONTAGE_SOUND_MODEL_URL";
        p.files = {
            // The float audio model (the fastest of the exports on a CPU, and exact) and the half-float text model:
            // the int8 exports' integer matrix kernels are the ones that crashed ONNX Runtime on Windows (AMX).
            {"audio_model.onnx", base + "onnx/audio_model.onnx", "a1c2b43c44f71e0fa841a4b86700886c199bf87699ea45632c4d831bc6c88957", 117528416},
            {"text_model_fp16.onnx", base + "onnx/text_model_fp16.onnx", "c1fd1e4cdd02acbcacaafe7a8e608dcfc8f84a00bfea3e2ca7df710957b4c3a5",
             251029088},
            {"vocab.json", base + "vocab.json", "ed19656ea1707df69134c4af35c8ceda2cc9860bf2c3495026153a133670ab5e", 798293},
            {"merges.txt", base + "merges.txt", "1ce1664773c50f3e0cc8842619a93edc4624525b728b188a9e0be33b7726adc5", 456318},
        };
        return p;
    }();
    return pack;
}

// ---- Features ----------------------------------------------------------------------------
// As transformers' ClapFeatureExtractor makes them for this model (truncation "rand_trunc", padding "repeatpad"):
// the Slaney mel filters with Slaney norm, a periodic Hann window, power spectrum, dB.

namespace {

constexpr int kFft = 1024, kHop = 480;

void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * M_PI / double(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Slaney's mel scale: linear below 1 kHz, logarithmic above.
double hzToMel(double hz) { return hz < 1000 ? 3.0 * hz / 200.0 : 15.0 + std::log(hz / 1000.0) * (27.0 / std::log(6.4)); }
double melToHz(double mel) { return mel < 15 ? 200.0 * mel / 3.0 : 1000.0 * std::exp((mel - 15.0) * (std::log(6.4) / 27.0)); }

// Triangular filters, (kFft / 2 + 1) x kClapMels, each scaled to unit area (Slaney's norm).
const std::vector<double>& melFilters() {
    static const std::vector<double> filters = [] {
        const int bins = kFft / 2 + 1;
        std::vector<double> edges(kClapMels + 2);
        const double lo = hzToMel(50), hi = hzToMel(14000);
        for (int i = 0; i < kClapMels + 2; ++i) edges[size_t(i)] = melToHz(lo + (hi - lo) * i / (kClapMels + 1));
        std::vector<double> f(size_t(bins) * kClapMels, 0.0);
        for (int k = 0; k < bins; ++k) {
            const double hz = double(kClapRate / 2) * k / (bins - 1);
            for (int m = 0; m < kClapMels; ++m) {
                const double down = (hz - edges[size_t(m)]) / (edges[size_t(m) + 1] - edges[size_t(m)]);
                const double up = (edges[size_t(m) + 2] - hz) / (edges[size_t(m) + 2] - edges[size_t(m) + 1]);
                f[size_t(k) * kClapMels + size_t(m)] = std::max(0.0, std::min(down, up)) * 2.0 / (edges[size_t(m) + 2] - edges[size_t(m)]);
            }
        }
        return f;
    }();
    return filters;
}

}  // namespace

std::vector<float> clapFeatures(const std::vector<float>& mono) {
    // Ten seconds: a shorter recording repeated as many whole times as fit, the rest silence.
    std::vector<float> x(size_t(kClapWindow), 0.0f);
    if (!mono.empty()) {
        if (mono.size() >= x.size()) {
            std::copy(mono.begin(), mono.begin() + std::ptrdiff_t(x.size()), x.begin());
        } else {
            const size_t times = x.size() / mono.size();
            for (size_t r = 0; r < times; ++r) std::copy(mono.begin(), mono.end(), x.begin() + std::ptrdiff_t(r * mono.size()));
        }
    }
    // Centred frames: reflected at the edges by half a window.
    const size_t pad = kFft / 2, n = x.size();
    std::vector<float> padded(n + 2 * pad);
    std::copy(x.begin(), x.end(), padded.begin() + std::ptrdiff_t(pad));
    for (size_t k = 0; k < pad; ++k) {
        padded[pad - 1 - k] = x[1 + k];
        padded[pad + n + k] = x[n - 2 - k];
    }
    static const std::vector<double> window = [] {
        std::vector<double> w(kFft);
        for (int i = 0; i < kFft; ++i) w[size_t(i)] = 0.5 - 0.5 * std::cos(2 * M_PI * i / kFft);  // periodic
        return w;
    }();
    const std::vector<double>& filters = melFilters();
    std::vector<float> out(size_t(kClapFrames) * kClapMels);
    std::vector<std::complex<double>> spec(kFft);
    std::vector<double> power(kFft / 2 + 1);
    for (int t = 0; t < kClapFrames; ++t) {
        const size_t at = size_t(t) * kHop;
        for (int i = 0; i < kFft; ++i) spec[size_t(i)] = padded[at + size_t(i)] * window[size_t(i)];
        fft(spec);
        for (int k = 0; k <= kFft / 2; ++k) power[size_t(k)] = std::norm(spec[size_t(k)]);
        for (int m = 0; m < kClapMels; ++m) {
            double e = 0;
            for (int k = 0; k <= kFft / 2; ++k) e += power[size_t(k)] * filters[size_t(k) * kClapMels + size_t(m)];
            out[size_t(t) * kClapMels + size_t(m)] = float(10.0 * std::log10(std::max(e, 1e-10)));
        }
    }
    return out;
}

// ---- RoBERTa's tokenizer -----------------------------------------------------------------

namespace {
constexpr int64_t kBos = 0, kEos = 2;
constexpr size_t kMaxTokens = 128;

std::vector<std::string> utf8Chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}
}  // namespace

bool ClapTokenizer::load(const std::string& vocabJson, const std::string& mergesTxt) {
    QFile vf(QString::fromStdString(vocabJson)), mf(QString::fromStdString(mergesTxt));
    if (!vf.open(QIODevice::ReadOnly) || !mf.open(QIODevice::ReadOnly)) return false;
    const QJsonObject v = QJsonDocument::fromJson(vf.readAll()).object();
    for (auto it = v.begin(); it != v.end(); ++it) vocab_[it.key().toStdString()] = it.value().toInteger();
    int rank = 0;
    for (const QByteArray& line : mf.readAll().split('\n')) {
        if (line.isEmpty() || line.startsWith("#version")) continue;
        const int sp = int(line.indexOf(' '));
        if (sp <= 0) continue;
        ranks_[{line.left(sp).toStdString(), line.mid(sp + 1).trimmed().toStdString()}] = rank++;
    }
    // GPT-2's byte-to-character table: printable bytes stand for themselves, the rest from U+0100 on.
    int extra = 0;
    for (int b = 0; b < 256; ++b) {
        const bool printable = (b >= '!' && b <= '~') || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE && b <= 0xFF);
        const char32_t c = printable ? char32_t(b) : char32_t(256 + extra++);
        byteChar_[b] = QString::fromUcs4(&c, 1).toStdString();
    }
    return !vocab_.empty() && !ranks_.empty();
}

std::vector<int64_t> ClapTokenizer::word(const std::string& piece) const {
    std::string mapped;
    for (const unsigned char b : piece) mapped += byteChar_[b];
    std::vector<std::string> parts = utf8Chars(mapped);
    while (parts.size() > 1) {
        int best = INT_MAX;
        size_t at = 0;
        for (size_t i = 0; i + 1 < parts.size(); ++i)
            if (const auto r = ranks_.find({parts[i], parts[i + 1]}); r != ranks_.end() && r->second < best) best = r->second, at = i;
        if (best == INT_MAX) break;
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
    for (const std::string& p : parts)
        if (const auto it = vocab_.find(p); it != vocab_.end()) ids.push_back(it->second);
    return ids;
}

std::vector<int64_t> ClapTokenizer::encode(const std::string& text) const {
    // GPT-2's pre-tokenizer: contractions, letter runs, number runs and symbol runs, each with the space before it.
    static const QRegularExpression split(QStringLiteral(R"('s|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+)"),
                                          QRegularExpression::UseUnicodePropertiesOption);
    std::vector<int64_t> ids{kBos};
    auto it = split.globalMatch(QString::fromStdString(text).trimmed());
    while (it.hasNext() && ids.size() < kMaxTokens - 1)
        for (const int64_t id : word(it.next().captured(0).toStdString())) {
            if (ids.size() >= kMaxTokens - 1) break;
            ids.push_back(id);
        }
    ids.push_back(kEos);
    return ids;
}

// ---- The model ---------------------------------------------------------------------------

namespace {
std::vector<float> unit(const float* v, size_t n) {
    double len = 0;
    for (size_t i = 0; i < n; ++i) len += double(v[i]) * v[i];
    len = std::sqrt(std::max(len, 1e-20));
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = float(v[i] / len);
    return out;
}
}  // namespace

#ifdef MONTAGE_WITH_ONNXRUNTIME

bool soundSearchAvailable() { return true; }

struct SoundModel::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-sound"};
    std::unique_ptr<OrtModel> text, audio;
    ClapTokenizer tokenizer;
};

#else

bool soundSearchAvailable() { return false; }
struct SoundModel::Impl {
    ClapTokenizer tokenizer;
};

#endif

SoundModel::SoundModel() : d_(std::make_unique<Impl>()) {}
SoundModel::~SoundModel() = default;
const ClapTokenizer& SoundModel::tokenizer() const { return d_->tokenizer; }

std::shared_ptr<SoundModel> SoundModel::load(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<SoundModel>();  // kept for the session
    std::lock_guard lock(m);
    if (*cached) return *cached;
    const ModelPack& pack = soundModel();
    if (!pack.installed()) {
        if (error) *error = "The sound search model is not downloaded";
        return nullptr;
    }
    std::shared_ptr<SoundModel> model(new SoundModel);
    if (!model->d_->tokenizer.load(pack.path(pack.files[2]), pack.path(pack.files[3]))) {
        if (error) *error = "The sound search model's vocabulary could not be read";
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
            return openModel(model->d_->env, path.c_str(), so);
        };
        model->d_->audio = open(pack.files[0]);
        model->d_->text = open(pack.files[1]);
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The sound search model could not be loaded: ") + e.what();
        return nullptr;
    }
#else
    if (error) *error = "This build of Montage cannot search sound: it was built without ONNX Runtime";
    return nullptr;
#endif
    *cached = model;
    return model;
}

std::vector<float> SoundModel::text(const std::string& query, std::string* error) const {
#ifdef MONTAGE_WITH_ONNXRUNTIME
    try {
        std::vector<int64_t> ids = d_->tokenizer.encode(query);
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t shape[2] = {1, int64_t(ids.size())};
        Ort::Value in = Ort::Value::CreateTensor<int64_t>(cpu, ids.data(), ids.size(), shape, 2);
        const char* inName[] = {"input_ids"};
        const char* outName[] = {"text_embeds"};
        auto out = d_->text->Run(Ort::RunOptions{nullptr}, inName, &in, 1, outName, 1);
        return unit(out[0].GetTensorData<float>(), out[0].GetTensorTypeAndShapeInfo().GetElementCount());
    } catch (const Ort::Exception& e) {
        if (error) *error = e.what();
    }
#else
    (void)query;
    if (error) *error = "This build of Montage cannot search sound";
#endif
    return {};
}

std::vector<float> SoundModel::audio(const std::vector<float>& features, std::string* error) const {
#ifdef MONTAGE_WITH_ONNXRUNTIME
    if (features.size() != size_t(kClapFrames) * kClapMels) {
        if (error) *error = "Wrong feature size";
        return {};
    }
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t shape[4] = {1, 1, kClapFrames, kClapMels};
        Ort::Value in = Ort::Value::CreateTensor<float>(cpu, const_cast<float*>(features.data()), features.size(), shape, 4);
        const char* inName[] = {"input_features"};
        const char* outName[] = {"audio_embeds"};
        auto out = d_->audio->Run(Ort::RunOptions{nullptr}, inName, &in, 1, outName, 1);
        return unit(out[0].GetTensorData<float>(), out[0].GetTensorTypeAndShapeInfo().GetElementCount());
    } catch (const Ort::Exception& e) {
        if (error) *error = e.what();
    }
#else
    (void)features;
    if (error) *error = "This build of Montage cannot search sound";
#endif
    return {};
}

// ---- Indexing ----------------------------------------------------------------------------

bool indexSound(const std::string& path, double duration, VisualIndex& out, double step, const std::function<void(double)>& progress,
                const std::atomic<bool>* cancel, std::string* error) {
    std::shared_ptr<SoundModel> model = SoundModel::load(error);
    if (!model) return false;
    if (step <= 0) step = duration > 0 && duration < 30 ? 2.0 : 5.0;
    const size_t hop = size_t(std::lround(step * kClapRate)), window = size_t(kClapWindow);
    out = VisualIndex{};
    out.model = soundModel().id;
    out.step = step;
    out.span = double(kClapWindow) / kClapRate;
    // The sound a block at a time; a window is embedded as soon as all of it is in.
    std::vector<float> buffer;
    size_t bufferStart = 0, next = 0, total = 0;  // in samples from the start
    std::string err;
    bool failed = false;
    auto embed = [&](size_t start, size_t length) {
        const std::vector<float> piece(buffer.begin() + std::ptrdiff_t(start - bufferStart),
                                       buffer.begin() + std::ptrdiff_t(start - bufferStart + length));
        const std::vector<float> e = model->audio(clapFeatures(piece), &err);
        if (e.empty()) return !(failed = true);
        out.add((double(start) + double(length) / 2) / kClapRate, e);
        return true;
    };
    const bool read = decodeAudioBlocks(
        path, kClapRate, size_t(kClapRate) * 5,
        [&](const std::vector<float>& block) {
            buffer.insert(buffer.end(), block.begin(), block.end());
            total += block.size();
            while (next + window <= total) {
                if (cancel && cancel->load()) return false;
                if (!embed(next, window)) return false;
                next += hop;
            }
            // What later windows still need, and always the last window's worth (the end may need it).
            const size_t keep = std::min(next, total > window ? total - window : 0);
            if (keep > bufferStart) {
                buffer.erase(buffer.begin(), buffer.begin() + std::ptrdiff_t(keep - bufferStart));
                bufferStart = keep;
            }
            if (progress && duration > 0) progress(std::min(0.99, double(total) / kClapRate / duration));
            return true;
        },
        &err, cancel);
    if (cancel && cancel->load()) {
        if (error) *error = "Cancelled";
        return false;
    }
    if (!read || failed) {
        if (error) *error = err.empty() ? "Cannot read the sound of " + path : err;
        return false;
    }
    if (total == 0) {
        if (error) *error = "No sound in " + path;
        return false;
    }
    // The end: a recording shorter than a window as one (repeated to fill it), or the last window to its end.
    if (total < window) {
        if (!embed(0, total)) {
            if (error) *error = err;
            return false;
        }
    } else if (total - (next - hop + window) > size_t(kClapRate)) {
        if (!embed(total - window, window)) {
            if (error) *error = err;
            return false;
        }
    }
    if (progress) progress(1.0);
    return true;
}

}  // namespace montage
