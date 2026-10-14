#include "BanditSeparation.h"

#include <QFile>
#include <QString>
#include <algorithm>
#include <cmath>
#include <complex>
#include <mutex>
#include <thread>

#include "audio/RoomResponse.h"

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

// The network without its weights (src/media/bandit-v2-eng.onnx), compiled in by CMake.
extern const unsigned char kBanditEngGraph[];
extern const size_t kBanditEngGraphSize;

namespace montage {

namespace {

constexpr const char* kCheckpoint = "checkpoint-eng.ckpt";
constexpr const char* kGraphFile = "bandit-v2-eng.onnx";

}  // namespace

const ModelPack& dmeModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "bandit-v2-eng";
        p.title = "dialogue, music and effects model";
        p.parentEnv = "MONTAGE_STEM_MODELS";
        p.urlEnv = "MONTAGE_DME_MODEL_URL";
        // The authors' English model (Zenodo record 12701995, version 1).
        p.files = {{kCheckpoint, std::string("https://zenodo.org/records/12701995/files/") + kCheckpoint,
                    "3d14c48399ddb42be6fca6c316adc1b7d67d0e648e4e937359811aa6fa8b8cd2", 446680129}};
        return p;
    }();
    return pack;
}

#ifndef MONTAGE_WITH_ONNXRUNTIME

struct BanditSplitter::Impl {};
BanditSplitter::BanditSplitter() = default;
BanditSplitter::~BanditSplitter() = default;
std::unique_ptr<BanditSplitter> BanditSplitter::create(StemSplitter::Output, std::string* error) {
    if (error) *error = "This build of Montage has no ONNX Runtime";
    return nullptr;
}
bool BanditSplitter::push(const float*, size_t, std::string*) { return false; }
bool BanditSplitter::finish(std::string*) { return false; }
int64_t BanditSplitter::done() const { return 0; }

#else

namespace {

// The chunks: the authors' 8 s, but every 6 s rather than every 1 s (a sixth of their work: the network is heavy),
// crossfaded over the 2 s they share, the first chunk from the start and the last to the end at full weight. The
// spectrogram inside each is torchaudio's (centred with reflection, normalised).
constexpr int kChunk = 8 * kDmeRate, kHop = 6 * kDmeRate, kFade = kChunk - kHop;
constexpr int kN = 2048, kStep = 512, kBins = kN / 2 + 1, kFrames = kChunk / kStep + 1, kStems = 3;

struct Model {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-dme"};
    std::unique_ptr<OrtModel> session;
};

// The graph beside the checkpoint it names (rewritten when this Montage brings a different one).
bool writeGraph(std::string& graphPath, std::string* error) {
    const QString dir = QString::fromStdString(dmeModel().directory());
    graphPath = (dir + '/' + kGraphFile).toStdString();
    const QByteArray want = QByteArray::fromRawData(reinterpret_cast<const char*>(kBanditEngGraph), qsizetype(kBanditEngGraphSize));
    QFile graph(QString::fromStdString(graphPath));
    if (graph.open(QIODevice::ReadOnly) && graph.readAll() == want) return true;
    graph.close();
    if (graph.open(QIODevice::WriteOnly | QIODevice::Truncate) && graph.write(want) == want.size()) return true;
    if (error) *error = "Cannot write the separation model into " + dir.toStdString();
    return false;
}

std::shared_ptr<Model> loadModel(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Model>;  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    if (!dmeModel().installed()) {
        if (error) *error = "The " + dmeModel().title + " is not downloaded";
        return nullptr;
    }
    std::string graph;
    if (!writeGraph(graph, error)) return nullptr;
    try {
        auto model = std::make_shared<Model>();
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        so.SetIntraOpNumThreads(int(std::max(1u, std::thread::hardware_concurrency())));
#ifdef _WIN32
        const std::wstring path = QString::fromStdString(graph).toStdWString();
#else
        const std::string& path = graph;
#endif
        model->session = openModel(model->env, path.c_str(), so);
        *cached = model;
        return model;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The separation model could not be loaded: ") + e.what();
        return nullptr;
    }
}

}  // namespace

struct BanditSplitter::Impl {
    std::shared_ptr<Model> model;
    StemSplitter::Output write;
    std::vector<float> window, fadeIn, envelope;
    float norm = 1;          // the spectrogram's normalisation (the window's energy, square-rooted)
    std::vector<float> in;   // interleaved stereo from inBase
    int64_t inBase = 0;
    int64_t received = 0;
    int64_t chunk = 0;       // the next chunk to separate
    std::vector<std::vector<float>> out;  // each stem, interleaved stereo from outBase
    int64_t outBase = 0;
    int64_t emitted = 0;
    bool stopped = false;

    // `last`: no chunk follows, so this one is heard at full weight to its end.
    bool separateChunk(bool last, std::string* error);
    bool handOut(int64_t upTo);
};

BanditSplitter::BanditSplitter() : d_(std::make_unique<Impl>()) {}
BanditSplitter::~BanditSplitter() = default;

std::unique_ptr<BanditSplitter> BanditSplitter::create(StemSplitter::Output write, std::string* error) {
    auto model = loadModel(error);
    if (!model) return nullptr;
    std::unique_ptr<BanditSplitter> s(new BanditSplitter());
    Impl& d = *s->d_;
    d.model = model;
    d.write = std::move(write);
    // Periodic Hann windows, as torch.hann_window makes them.
    d.window.resize(kN);
    double energy = 0;
    for (int i = 0; i < kN; ++i) {
        d.window[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * i / kN));
        energy += double(d.window[size_t(i)]) * d.window[size_t(i)];
    }
    d.norm = float(std::sqrt(energy));
    d.fadeIn.resize(kFade);  // (and the fade out its complement: they add up to one)
    for (int i = 0; i < kFade; ++i) d.fadeIn[size_t(i)] = float(std::pow(std::sin(M_PI / 2 * (i + 0.5) / kFade), 2));
    // What the windows' squares add up to over the centred frames (torch.istft divides by it).
    d.envelope.assign(size_t(kChunk + kN), 0.0f);
    for (int f = 0; f < kFrames; ++f)
        for (int i = 0; i < kN; ++i) d.envelope[size_t(f * kStep + i)] += d.window[size_t(i)] * d.window[size_t(i)];
    d.out.assign(kStems, {});
    return s;
}

int64_t BanditSplitter::done() const { return d_->emitted; }

bool BanditSplitter::Impl::handOut(int64_t upTo) {
    if (upTo <= emitted) return true;
    std::vector<std::vector<float>> blocks(kStems);
    const size_t from = size_t(emitted - outBase) * 2, to = size_t(upTo - outBase) * 2;
    for (int k = 0; k < kStems; ++k) {
        std::vector<float>& o = out[size_t(k)];
        if (o.size() < to) o.resize(to, 0.0f);
        blocks[size_t(k)].assign(o.begin() + std::ptrdiff_t(from), o.begin() + std::ptrdiff_t(to));
        o.erase(o.begin(), o.begin() + std::ptrdiff_t(to));
    }
    outBase = upTo;
    emitted = upTo;
    if (!write(blocks)) stopped = true;
    return !stopped;
}

bool BanditSplitter::Impl::separateChunk(bool last, std::string* error) {
    // Chunk c starts at c kHop, except that the last one ends where the input does (the network hears trailing
    // silence as nothing at all), and input shorter than a chunk is looped to fill one (played backwards as well, speech
    // can come out as effects).
    const int64_t from = chunk * kHop;
    const int64_t start = last ? std::max<int64_t>(0, std::min<int64_t>(from, received - kChunk)) : from;
    auto sample = [&](int64_t i, int c) {  // the chunk's i-th sample
        int64_t p = start + i;
        if (p >= received) p %= received;
        const size_t idx = size_t(p - inBase) * 2 + size_t(c);
        return idx < in.size() ? in[idx] : 0.0f;
    };
    // A chunk whose channels are the same (a mono recording) is separated once.
    bool same = true;
    for (int i = 0; i < kChunk && same; ++i) same = sample(i, 0) == sample(i, 1);
    const int channels = same ? 1 : 2;
    // Each channel's spectrogram [channels, T, F, 2], from the chunk reflected at its ends by half a frame.
    std::vector<float> spec(size_t(channels) * kFrames * kBins * 2);
    std::vector<std::complex<float>> buf(kN);
    std::vector<float> x(size_t(kChunk) + kN);
    for (int c = 0; c < channels; ++c) {
        for (int i = 0; i < kChunk + kN; ++i) {
            int j = i - kN / 2;
            if (j < 0) j = -j;
            if (j >= kChunk) j = 2 * (kChunk - 1) - j;
            x[size_t(i)] = sample(j, c);
        }
        for (int f = 0; f < kFrames; ++f) {
            for (int i = 0; i < kN; ++i) buf[size_t(i)] = x[size_t(f * kStep + i)] * window[size_t(i)];
            fftInPlace(buf, false);
            float* dst = &spec[(size_t(c) * kFrames + size_t(f)) * kBins * 2];
            for (int b = 0; b < kBins; ++b) dst[b * 2] = buf[size_t(b)].real() / norm, dst[b * 2 + 1] = buf[size_t(b)].imag() / norm;
        }
    }
    // The masks [2, S, T, F, 2].
    std::vector<float> masks;
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t shape[4] = {channels, kFrames, kBins, 2};
        Ort::Value input = Ort::Value::CreateTensor<float>(cpu, spec.data(), spec.size(), shape, 4);
        const char* inName = "spectrogram";
        const char* outName = "masks";
        auto result = model->session->Run(Ort::RunOptions{nullptr}, &inName, &input, 1, &outName, 1);
        const size_t want = size_t(channels) * kStems * kFrames * kBins * 2;
        if (result.empty() || result[0].GetTensorTypeAndShapeInfo().GetElementCount() != want) {
            if (error) *error = "The separation model gave masks of the wrong size";
            return false;
        }
        const float* m = result[0].GetTensorData<float>();
        masks.assign(m, m + want);
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The separation failed: ") + e.what();
        return false;
    }
    // Each stem: the masked spectrogram turned back into the chunk's sound, crossfaded with the chunks either side:
    // faded in over the kFade from `from` (before which the chunk before is heard alone), faded out over the kFade
    // where the next chunk begins.
    std::vector<float> y(size_t(kChunk) + kN);
    auto weight = [&](int64_t p) {
        if (chunk > 0 && p < from + kFade) return p < from ? 0.0f : fadeIn[size_t(p - from)];
        if (!last && p >= from + kHop) return 1.0f - fadeIn[size_t(p - from - kHop)];
        return 1.0f;
    };
    const int64_t first = std::max(start, outBase);  // (what comes before is out already, the chunk before's alone)
    const int64_t end = std::min<int64_t>(start + kChunk, received);
    for (int k = 0; k < kStems; ++k) {
        std::vector<float>& o = out[size_t(k)];
        const size_t need = size_t(end - outBase) * 2;
        if (o.size() < need) o.resize(need, 0.0f);
        for (int c = 0; c < channels; ++c) {
            std::fill(y.begin(), y.end(), 0.0f);
            for (int f = 0; f < kFrames; ++f) {
                const float* s = &spec[(size_t(c) * kFrames + size_t(f)) * kBins * 2];
                const float* m = &masks[(((size_t(c) * kStems + size_t(k)) * kFrames) + size_t(f)) * kBins * 2];
                for (int b = 0; b < kBins; ++b) {
                    const std::complex<float> v = std::complex<float>(s[b * 2], s[b * 2 + 1]) * std::complex<float>(m[b * 2], m[b * 2 + 1]) * norm;
                    buf[size_t(b)] = v;
                    if (b > 0 && b < kN / 2) buf[size_t(kN - b)] = std::conj(v);
                }
                buf[0] = buf[0].real(), buf[kN / 2] = buf[kN / 2].real();  // (a real signal's)
                fftInPlace(buf, true);
                for (int i = 0; i < kN; ++i) y[size_t(f * kStep + i)] += buf[size_t(i)].real() / kN * window[size_t(i)];
            }
            for (int64_t p = first; p < end; ++p) {
                const size_t at = size_t(p - start + kN / 2);
                const float v = y[at] / envelope[at] * weight(p);
                float* dst = &o[size_t(p - outBase) * 2];
                if (same) dst[0] += v, dst[1] += v;
                else dst[c] += v;
            }
        }
    }
    ++chunk;
    // Input before this chunk's start is no longer needed (the last chunk may begin before the next one would);
    // output before the next chunk is complete (up to the input's end).
    if (from > inBase) {
        const size_t drop = std::min(in.size(), size_t(from - inBase) * 2);
        in.erase(in.begin(), in.begin() + std::ptrdiff_t(drop));
        inBase = from;
    }
    return handOut(last ? end : std::min<int64_t>(chunk * kHop, received));
}

bool BanditSplitter::push(const float* stereo, size_t frames, std::string* error) {
    Impl& d = *d_;
    if (d.stopped) return false;
    d.in.insert(d.in.end(), stereo, stereo + frames * 2);
    d.received += int64_t(frames);
    // (a chunk is separated once the one after it has begun, so that one is known to follow)
    while (d.inBase + int64_t(d.in.size() / 2) > d.chunk * kHop + kChunk)
        if (!d.separateChunk(false, error)) return false;
    return true;
}

bool BanditSplitter::finish(std::string* error) {
    Impl& d = *d_;
    if (d.stopped) return false;
    const int64_t length = d.received;
    if (length == 0) return true;
    while (d.chunk == 0 || d.chunk * kHop < length) {
        const bool last = d.chunk * kHop + kChunk >= length;
        if (!d.separateChunk(last, error)) return false;
        if (last) break;
    }
    return d.handOut(length);
}

#endif

}  // namespace montage
