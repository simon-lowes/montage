#include "StemSeparation.h"

#include <QDir>
#include <QFile>
#include <QString>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <thread>

#include "BanditSeparation.h"
#include "Decoder.h"
#include "core/Zip.h"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/tx.h>
}

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

// The networks without their weights (src/media/spleeter-<n>stems.onnx), compiled in by CMake.
extern const unsigned char kSpleeter2Graph[], kSpleeter4Graph[], kSpleeter5Graph[];
extern const size_t kSpleeter2GraphSize, kSpleeter4GraphSize, kSpleeter5GraphSize;

namespace montage {

namespace {

constexpr const char* kData = "model.data-00000-of-00001";

struct StemPackInfo {
    int stems;
    const char* sha256;
    int64_t bytes;
    int64_t dataBytes;  // the checkpoint's data file inside it
    const unsigned char* graph;
    const size_t* graphSize;
};

const StemPackInfo* packInfo(int stems) {
    // Deezer's own release (v1.4.0, the models every Spleeter since uses).
    static const StemPackInfo infos[] = {
        {2, "f3a90b39dd2874269e8b05a48a86745df897b848c61f3958efc80a39152bd692", 73109797, 78614080, kSpleeter2Graph, &kSpleeter2GraphSize},
        {4, "3adb4a50ad4eb18c7c4d65fcf4cf2367a07d48408a5eb7d03cd20067429dfaa8", 146308147, 157228152, kSpleeter4Graph, &kSpleeter4GraphSize},
        {5, "25a1e87eb5f75cc72a4d2d5467a0a50ac75f05611f877c278793742513cc7218", 182803368, 196535188, kSpleeter5Graph, &kSpleeter5GraphSize},
    };
    for (const StemPackInfo& i : infos)
        if (i.stems == stems) return &i;
    return nullptr;
}

ModelPack makePack(int stems) {
    const StemPackInfo* info = packInfo(stems);
    ModelPack p;
    p.id = "spleeter-" + std::to_string(stems) + "stems";
    p.title = std::to_string(stems) + "-stem separation model";
    p.parentEnv = "MONTAGE_STEM_MODELS";
    p.urlEnv = "MONTAGE_STEM_MODEL_URL";
    const std::string file = std::to_string(stems) + "stems.tar.gz";
    p.files = {{file, "https://github.com/deezer/spleeter/releases/download/v1.4.0/" + file, info->sha256, info->bytes}};
    return p;
}

// Spleeter's framing: 4096-point Hann frames every 1024 samples, the network seeing 512 frames of the first 1024 bins.
constexpr int kN = 4096, kHop = 1024, kT = 512, kF = 1024, kBins = kN / 2 + 1;
constexpr double kWindowCompensation = 2.0 / 3.0;  // the Hann windows of analysis and synthesis overlap-add to 1.5

}  // namespace

int stemRate(int stems) { return stems == 3 ? kDmeRate : kStemRate; }

const ModelPack& stemModel(int stems) {
    static const ModelPack two = makePack(2), four = makePack(4), five = makePack(5);
    return stems == 3 ? dmeModel() : stems == 2 ? two : stems == 5 ? five : four;
}

const std::vector<std::string>& stemNames(int stems) {
    static const std::vector<std::string> two = {"vocals", "accompaniment"}, three = {"dialogue", "music", "effects"},
                                          four = {"vocals", "drums", "bass", "other"},
                                          five = {"vocals", "piano", "drums", "bass", "other"}, none;
    return stems == 2 ? two : stems == 3 ? three : stems == 4 ? four : stems == 5 ? five : none;
}

std::string stemTitle(const std::string& stem) {
    if (stem.empty()) return stem;
    std::string t = stem;
    t[0] = char(std::toupper(static_cast<unsigned char>(t[0])));
    return t;
}

namespace {

// The checkpoint's data file (which the graph's weights name, with offsets) unpacked from the release's archive, and
// the graph beside it. Done once; a stamp marks it complete.
bool unpack(int stems, std::string& graphPath, std::string* error) {
    const StemPackInfo* info = packInfo(stems);
    const ModelPack& pack = stemModel(stems);
    const QString dir = QString::fromStdString(pack.directory());
    graphPath = (dir + QStringLiteral("/spleeter-%1stems.onnx").arg(stems)).toStdString();
    const QString stamp = dir + "/unpacked-" + QString::fromStdString(pack.files[0].sha256.substr(0, 12));
    const QString data = dir + '/' + kData;
    // The graph is compiled in, so a new Montage may bring a new one: the copy on disk is rewritten when it differs.
    auto writeGraph = [&] {
        QFile graph(QString::fromStdString(graphPath));
        return graph.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               graph.write(reinterpret_cast<const char*>(info->graph), qint64(*info->graphSize)) == qint64(*info->graphSize);
    };
    if (QFile::exists(stamp) && QFile(data).size() == info->dataBytes) {
        QFile graph(QString::fromStdString(graphPath));
        if (graph.open(QIODevice::ReadOnly) &&
            graph.readAll() == QByteArray::fromRawData(reinterpret_cast<const char*>(info->graph), qsizetype(*info->graphSize)))
            return true;
        graph.close();
        if (writeGraph()) return true;
        if (error) *error = "Cannot unpack the separation model into " + dir.toStdString();
        return false;
    }
    QFile archive(QString::fromStdString(pack.path(pack.files[0])));
    std::string tar;
    if (!archive.open(QIODevice::ReadOnly) || !gunzip(archive.readAll().toStdString(), tar, error)) {
        if (error && error->empty()) *error = "The separation model download is damaged";
        return false;
    }
    archive.close();
    const auto entries = tarEntries(tar);
    const auto it = std::find_if(entries.begin(), entries.end(), [](const TarEntry& e) { return e.name == kData; });
    if (it == entries.end() || int64_t(it->size) != info->dataBytes) {
        if (error) *error = "The separation model download is not the one expected";
        return false;
    }
    QFile out(data);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(tar.data() + it->offset, qint64(it->size)) != qint64(it->size) ||
        !writeGraph()) {
        if (error) *error = "Cannot unpack the separation model into " + dir.toStdString();
        return false;
    }
    out.close();
    QFile s(stamp);
    s.open(QIODevice::WriteOnly);
    return true;
}

// av_tx needs SIMD-aligned buffers.
template <typename T>
struct AlignedBuf {
    T* p;
    explicit AlignedBuf(size_t n) : p(static_cast<T*>(av_calloc(n, sizeof(T)))) {}
    ~AlignedBuf() { av_free(p); }
    AlignedBuf(const AlignedBuf&) = delete;
    AlignedBuf& operator=(const AlignedBuf&) = delete;
    T& operator[](size_t i) { return p[i]; }
    T* data() { return p; }
};

struct Tx {
    AVTXContext* ctx = nullptr;
    av_tx_fn fn = nullptr;
    Tx(int n, bool inverse) {
        const float scale = inverse ? 1.0f / float(n) : 1.0f;
        av_tx_init(&ctx, &fn, AV_TX_FLOAT_RDFT, inverse ? 1 : 0, n, &scale, 0);
    }
    ~Tx() { av_tx_uninit(&ctx); }
    Tx(const Tx&) = delete;
    Tx& operator=(const Tx&) = delete;
};

}  // namespace

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool stemSeparationAvailable() { return false; }

struct StemSplitter::Impl {};
StemSplitter::StemSplitter() = default;
StemSplitter::~StemSplitter() = default;
std::unique_ptr<StemSplitter> StemSplitter::create(int, Output, std::string* error) {
    if (error) *error = "This build of Montage has no ONNX Runtime";
    return nullptr;
}
int StemSplitter::stems() const { return 0; }
bool StemSplitter::push(const float*, size_t, std::string*) { return false; }
bool StemSplitter::finish(std::string*) { return false; }
int64_t StemSplitter::done() const { return 0; }

#else

bool stemSeparationAvailable() { return true; }

namespace {

struct Model {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-stems"};
    std::unique_ptr<OrtModel> session;
    int stems = 0;
};

std::shared_ptr<Model> loadModel(int stems, std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Model>[3];  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (!packInfo(stems)) {
        if (error) *error = "Separation into " + std::to_string(stems) + " stems is not offered (2, 4 or 5)";
        return nullptr;
    }
    std::shared_ptr<Model>& slot = cached[stems == 2 ? 0 : stems == 4 ? 1 : 2];
    if (slot) return slot;
    if (!ortUsable(error)) return nullptr;
    if (!stemModel(stems).installed()) {
        if (error) *error = "The " + stemModel(stems).title + " is not downloaded";
        return nullptr;
    }
    std::string graph;
    if (!unpack(stems, graph, error)) return nullptr;
    try {
        auto model = std::make_shared<Model>();
        model->stems = stems;
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        so.SetIntraOpNumThreads(int(std::max(1u, std::thread::hardware_concurrency())));
#ifdef _WIN32
        const std::wstring path = QString::fromStdString(graph).toStdWString();
#else
        const std::string& path = graph;
#endif
        model->session = openModel(model->env, path.c_str(), so);
        slot = model;
        return model;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The separation model could not be loaded: ") + e.what();
        return nullptr;
    }
}

}  // namespace

// The signal Spleeter separates is the input after a frame of silence (kN zeros), framed from 0 every kHop to the end
// (the last frame padded with zeros); positions below are in that padded signal.
struct StemSplitter::Impl {
    std::unique_ptr<BanditSplitter> bandit;  // (dialogue, music and effects: all of the work is its)
    std::shared_ptr<Model> model;
    Output write;
    int stems = 0;
    std::vector<float> window;
    std::vector<float> in;  // padded input from inBase, interleaved stereo
    int64_t inBase = 0;
    int64_t received = 0;   // input frames pushed
    int64_t segment = 0;    // the next segment to separate
    std::vector<std::vector<float>> out;  // each stem's overlap-added output from outBase, interleaved stereo
    int64_t outBase = 0;
    int64_t emitted = 0;    // padded positions handed out (from kN)
    bool stopped = false;

    // Separates the segment's frames whose start is below `frames` (the padded signal's frame count, or -1 while more
    // input may come), then hands out what no later frame can change.
    bool separateSegment(int64_t frames, std::string* error);
    bool handOut(int64_t upTo);
};

StemSplitter::StemSplitter() : d_(std::make_unique<Impl>()) {}
StemSplitter::~StemSplitter() = default;

std::unique_ptr<StemSplitter> StemSplitter::create(int stems, Output write, std::string* error) {
    if (stems == 3) {
        auto bandit = BanditSplitter::create(std::move(write), error);
        if (!bandit) return nullptr;
        std::unique_ptr<StemSplitter> s(new StemSplitter());
        s->d_->bandit = std::move(bandit);
        s->d_->stems = 3;
        return s;
    }
    auto model = loadModel(stems, error);
    if (!model) return nullptr;
    std::unique_ptr<StemSplitter> s(new StemSplitter());
    Impl& d = *s->d_;
    d.model = model;
    d.write = std::move(write);
    d.stems = stems;
    d.window.resize(kN);
    for (int i = 0; i < kN; ++i) d.window[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * i / kN));  // periodic Hann
    d.in.assign(size_t(kN) * 2, 0.0f);  // the leading frame of silence
    d.out.assign(size_t(stems), {});
    d.emitted = kN;
    return s;
}

int StemSplitter::stems() const { return d_->stems; }
int64_t StemSplitter::done() const { return d_->bandit ? d_->bandit->done() : std::max<int64_t>(0, d_->emitted - kN); }

bool StemSplitter::Impl::handOut(int64_t upTo) {
    if (upTo <= emitted) return true;
    std::vector<std::vector<float>> blocks(static_cast<size_t>(stems));
    const size_t from = size_t(emitted - outBase) * 2, to = size_t(upTo - outBase) * 2;
    for (int k = 0; k < stems; ++k) {
        std::vector<float>& o = out[size_t(k)];
        if (o.size() < to) o.resize(to, 0.0f);
        blocks[size_t(k)].assign(o.begin() + std::ptrdiff_t(from), o.begin() + std::ptrdiff_t(to));
        for (float& v : blocks[size_t(k)]) v = float(v * kWindowCompensation);
        o.erase(o.begin(), o.begin() + std::ptrdiff_t(to));
    }
    outBase = upTo;
    emitted = upTo;
    if (!write(blocks)) stopped = true;
    return !stopped;
}

bool StemSplitter::Impl::separateSegment(int64_t frames, std::string* error) {
    const int64_t first = segment * kT;  // the segment's first frame
    const int count = frames < 0 ? kT : int(std::clamp<int64_t>(frames - first, 0, kT));
    Tx fwd(kN, false), inv(kN, true);
    if (!fwd.ctx || !inv.ctx) {
        if (error) *error = "No FFT for the separation";
        return false;
    }
    // The mix's spectra (both channels) and magnitudes [1, 2, T, F].
    std::vector<AVComplexFloat> spec(size_t(count) * 2 * kBins);
    std::vector<float> magnitude(size_t(2) * kT * kF, 0.0f);
    AlignedBuf<float> frame(kN);
    AlignedBuf<AVComplexFloat> bins(size_t(kBins) + 1);
    for (int f = 0; f < count; ++f) {
        const int64_t at = (first + f) * kHop - inBase;
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < kN; ++i) {
                const size_t idx = size_t(at + i) * 2 + size_t(c);
                frame[size_t(i)] = (idx < in.size() ? in[idx] : 0.0f) * window[size_t(i)];
            }
            fwd.fn(fwd.ctx, bins.data(), frame.data(), sizeof(float));
            AVComplexFloat* dst = &spec[(size_t(f) * 2 + size_t(c)) * kBins];
            std::copy(bins.data(), bins.data() + kBins, dst);
            float* mag = &magnitude[(size_t(c) * kT + size_t(f)) * kF];
            for (int b = 0; b < kF; ++b) mag[b] = std::hypot(dst[b].re, dst[b].im);
        }
    }
    // The masks [1, S, 2, T, F].
    std::vector<float> masks;
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t shape[4] = {1, 2, kT, kF};
        Ort::Value input = Ort::Value::CreateTensor<float>(cpu, magnitude.data(), magnitude.size(), shape, 4);
        const char* inName = "magnitude";
        const char* outName = "masks";
        auto result = model->session->Run(Ort::RunOptions{nullptr}, &inName, &input, 1, &outName, 1);
        const size_t want = size_t(stems) * 2 * kT * kF;
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
    // Each stem: its masked spectra (nothing above the network's bins, as Spleeter extends its masks with zeros),
    // windowed and overlap-added.
    const int64_t end = (first + count - 1) * kHop + kN;
    for (int k = 0; k < stems; ++k) {
        std::vector<float>& o = out[size_t(k)];
        if (count > 0 && o.size() < size_t(end - outBase) * 2) o.resize(size_t(end - outBase) * 2, 0.0f);
        for (int f = 0; f < count; ++f)
            for (int c = 0; c < 2; ++c) {
                const AVComplexFloat* x = &spec[(size_t(f) * 2 + size_t(c)) * kBins];
                const float* m = &masks[((size_t(k) * 2 + size_t(c)) * kT + size_t(f)) * kF];
                for (int b = 0; b < kF; ++b) bins[size_t(b)] = {x[b].re * m[b], x[b].im * m[b]};
                for (int b = kF; b < kBins; ++b) bins[size_t(b)] = {0, 0};
                inv.fn(inv.ctx, frame.data(), bins.data(), sizeof(AVComplexFloat));
                float* dst = &o[size_t((first + f) * kHop - outBase) * 2 + size_t(c)];
                for (int i = 0; i < kN; ++i) dst[size_t(i) * 2] += frame[size_t(i)] * window[size_t(i)];
            }
    }
    ++segment;
    // Input before the next segment's first frame is no longer needed.
    const int64_t keepFrom = segment * kT * kHop;
    if (keepFrom > inBase) {
        const size_t drop = std::min(in.size(), size_t(keepFrom - inBase) * 2);
        in.erase(in.begin(), in.begin() + std::ptrdiff_t(drop));
        inBase = keepFrom;
    }
    // Output before the next segment's first frame is complete (later frames start there), up to the input's end.
    const int64_t complete = frames < 0 ? keepFrom : std::min<int64_t>(keepFrom, kN + received);
    return handOut(complete);
}

bool StemSplitter::push(const float* stereo, size_t frames, std::string* error) {
    Impl& d = *d_;
    if (d.bandit) return d.bandit->push(stereo, frames, error);
    if (d.stopped) return false;
    d.in.insert(d.in.end(), stereo, stereo + frames * 2);
    d.received += int64_t(frames);
    // A segment can be separated once every frame it holds is wholly in.
    for (;;) {
        const int64_t need = (d.segment * kT + kT - 1) * kHop + kN;
        if (d.inBase + int64_t(d.in.size() / 2) < need) return true;
        if (!d.separateSegment(-1, error)) return false;
    }
}

bool StemSplitter::finish(std::string* error) {
    Impl& d = *d_;
    if (d.bandit) return d.bandit->finish(error);
    if (d.stopped) return false;
    const int64_t length = kN + d.received;
    const int64_t frames = (length + kHop - 1) / kHop;  // TensorFlow's stft with pad_end
    while (d.segment * kT < frames)
        if (!d.separateSegment(frames, error)) return false;
    return d.handOut(length);
}

#endif

namespace {

// 32-bit float stereo WAV, written as it goes; sizes completed on close. Room is kept (a JUNK chunk) for the ds64 chunk
// of RF64 (EBU Tech 3306), which a file past 4 GB (about 3 h 20 min) becomes.
struct FloatWav {
    static constexpr qint64 kHeader = 80;
    QFile file;
    qint64 frames = 0;
    uint32_t rate = kStemRate;
    bool open(const QString& path) {
        file.setFileName(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        return file.write(header()) == kHeader;
    }
    QByteArray header() const {
        QByteArray b;
        auto u64 = [&](uint64_t v) { b.append(reinterpret_cast<const char*>(&v), 8); };
        auto u32 = [&](uint32_t v) { b.append(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&](uint16_t v) { b.append(reinterpret_cast<const char*>(&v), 2); };
        const uint64_t bytes = uint64_t(frames) * 8, riff = bytes + uint64_t(kHeader) - 8;
        const bool large = riff > 0xFFFFFFFFULL;
        b.append(large ? "RF64" : "RIFF", 4), u32(large ? 0xFFFFFFFFU : uint32_t(riff)), b.append("WAVE", 4);
        b.append(large ? "ds64" : "JUNK", 4), u32(28), u64(large ? riff : 0), u64(large ? bytes : 0), u64(large ? uint64_t(frames) : 0), u32(0);
        b.append("fmt ", 4), u32(16), u16(3), u16(2), u32(rate), u32(rate * 8), u16(8), u16(32);
        b.append("data", 4), u32(large ? 0xFFFFFFFFU : uint32_t(bytes));
        return b;
    }
    bool write(const std::vector<float>& stereo) {
        frames += qint64(stereo.size() / 2);
        const qint64 n = qint64(stereo.size() * sizeof(float));
        return file.write(reinterpret_cast<const char*>(stereo.data()), n) == n;
    }
    bool close() {
        if (!file.seek(0) || file.write(header()) != kHeader) return false;
        file.close();
        return true;
    }
};

}  // namespace

bool separateStemsToFiles(const std::string& path, double from, double to, const std::vector<int>& channels, int stems,
                          const std::vector<std::string>& outPaths, const std::function<void(double)>& progress,
                          const std::atomic<bool>* cancel, std::string* error) {
    if (stemNames(stems).empty() || outPaths.size() != stemNames(stems).size()) {
        if (error) *error = "Separation is into 2, 3, 4 or 5 stems, one file each";
        return false;
    }
    if (to > 0 && to <= from) {
        if (error) *error = "The stretch to separate ends before it starts";
        return false;
    }
    std::vector<std::unique_ptr<FloatWav>> files;
    auto removeAll = [&] {
        for (auto& f : files) f->file.close();
        for (const std::string& p : outPaths) QFile::remove(QString::fromStdString(p));
    };
    const int rate = stemRate(stems);
    for (const std::string& p : outPaths) {
        files.push_back(std::make_unique<FloatWav>());
        files.back()->rate = uint32_t(rate);
        if (!files.back()->open(QString::fromStdString(p))) {
            if (error) *error = "Cannot write " + p;
            removeAll();
            return false;
        }
    }
    bool writeFailed = false;
    auto splitter = StemSplitter::create(stems, [&](const std::vector<std::vector<float>>& blocks) {
        for (size_t k = 0; k < blocks.size(); ++k) writeFailed |= !files[k]->write(blocks[k]);
        return !writeFailed;
    }, error);
    if (!splitter) {
        removeAll();
        return false;
    }
    const int64_t a = std::max<int64_t>(0, int64_t(std::llround(from * rate)));
    const int64_t b = to > 0 ? std::max(a, int64_t(std::llround(to * rate))) : -1;
    int64_t at = a;  // the media frame the next block starts at
    std::string err;
    const bool decoded = decodeAudioStereoBlocks(
        path, rate, size_t(rate),
        [&](const std::vector<float>& stereo) {
            const int64_t n = int64_t(stereo.size() / 2), lo = std::max(a, at), hi = b < 0 ? at + n : std::min(b, at + n);
            if (hi > lo && !splitter->push(stereo.data() + (lo - at) * 2, size_t(hi - lo), &err)) return false;
            at += n;
            if (progress && b > a) progress(std::clamp(double(splitter->done()) / double(b - a), 0.0, 1.0));
            return (b < 0 || at < b) && !(cancel && cancel->load());
        },
        &err, cancel, channels, double(a) / rate);
    if (cancel && cancel->load()) err = "Cancelled";
    else if (decoded && err.empty() && at <= a) err = "There is no sound there to separate";
    if (!decoded || !err.empty() || !splitter->finish(&err) || writeFailed) {
        if (error) *error = !err.empty() ? err : writeFailed ? "Cannot write the stems" : "The separation failed";
        removeAll();
        return false;
    }
    for (auto& f : files)
        if (!f->close()) {
            if (error) *error = "Cannot write " + f->file.fileName().toStdString();
            removeAll();
            return false;
        }
    if (progress) progress(1.0);
    return true;
}

bool stemSource(const Project& p, const Sequence& s, Id clip, double handles, double& from, double& to, std::string* error) {
    const Clip* c = edit::clipById(const_cast<Sequence&>(s), clip);
    const MediaItem* m = c && c->mediaId ? p.findMedia(c->mediaId) : nullptr;
    const auto at = c ? edit::locate(s, clip) : std::nullopt;
    if (!m || !at || at->track.kind != TrackKind::Audio || !m->hasAudio || m->path.empty() || c->isGenerator()) {
        if (error) *error = "That is not an audio clip of a recording";
        return false;
    }
    // (A frame's worth more each side: a reversed clip's last frame reaches a frame past where sourceAt puts it.)
    const double fps = s.fpsValue(), a = c->sourceAt(0) / fps, b = c->sourceAt(double(c->duration)) / fps;
    const double margin = std::max(1.0, c->speed) / fps;
    from = std::max(0.0, std::min(a, b) - handles - margin);
    to = std::max(a, b) + handles + margin;
    if (m->duration > 0) to = std::min(to, m->duration);
    if (to <= from) {
        if (error) *error = "That clip plays no sound of its recording";
        return false;
    }
    return true;
}

edit::Result placeStems(Project& p, Sequence& s, Id clip, const std::vector<Id>& stemMedia, const std::vector<std::string>& titles,
                        double from, bool muteOriginal) {
    Clip* orig = edit::clipById(s, clip);
    if (!orig || stemMedia.size() != titles.size() || stemMedia.empty()) return edit::Result::fail("Nothing to place");
    const auto loc = edit::locate(s, clip);
    if (!loc || loc->track.kind != TrackKind::Audio) return edit::Result::fail("That is not an audio clip");
    if (orig->linkGroup == 0) orig->linkGroup = p.newId();
    const Clip base = *orig;
    // The stems replace the clip, so they are mixed as its track mixes it: its level, pan, inserts, sends, routing,
    // surround position and automation.
    const Track source = s.audioTracks[size_t(loc->track.index)];
    auto sameMix = [&](const Track& t) {
        if (t.output != source.output || t.volumeDb != source.volumeDb || t.pan != source.pan || !(t.sends == source.sends) ||
            t.effects.size() != source.effects.size() || !(t.volumeAuto == source.volumeAuto) || !(t.panAuto == source.panAuto))
            return false;
        for (size_t k = 0; k < t.effects.size(); ++k)
            if (t.effects[k].type != source.effects[k].type || t.effects[k].params != source.effects[k].params) return false;
        return true;
    };
    edit::Result all;
    for (size_t i = 0; i < stemMedia.size(); ++i) {
        // A track for this stem that is free over the clip and mixes as the clip's does, else a new one below the others.
        int index = -1;
        for (int t = 0; t < int(s.audioTracks.size()) && index < 0; ++t) {
            const Track& tr = s.audioTracks[size_t(t)];
            if (tr.name != titles[i] || tr.locked || !sameMix(tr)) continue;
            if (std::none_of(tr.clips.begin(), tr.clips.end(), [&](const Clip& o) { return o.start < base.end() && base.start < o.end(); }))
                index = t;
        }
        if (index < 0) {
            index = edit::addTrack(p, s, TrackKind::Audio).index;
            Track& made = s.audioTracks[size_t(index)];
            made.name = titles[i];
            made.volumeDb = source.volumeDb;
            made.pan = source.pan;
            made.output = source.output;
            made.sends = source.sends;
            made.surround = source.surround;
            made.volumeAuto = source.volumeAuto;
            made.panAuto = source.panAuto;
            made.muteAuto = source.muteAuto;
            made.surroundXAuto = source.surroundXAuto;
            made.surroundYAuto = source.surroundYAuto;
            made.surroundZAuto = source.surroundZAuto;
            made.automation = source.automation;
            made.effects = source.effects;
            for (Effect& e : made.effects) e.id = p.newId();
        }
        Clip stem = base;
        stem.id = p.newId();
        stem.mediaId = stemMedia[i];
        stem.name = (base.name.empty() ? std::string("Clip") : base.name) + " (" + titles[i] + ")";
        stem.sourceIn = base.sourceIn - from * s.fpsValue();
        stem.enabled = true;
        stem.effects.clear();
        stem.channels.clear();
        stem.markers.clear();
        stem.takes.clear();
        stem.take = 0;
        stem.unrendered.clear();
        stem.audioAngle = -1;
        if (titles[i] == "Dialogue" || titles[i] == "Music" || titles[i] == "Effects") stem.role = titles[i];
        const edit::Result r = edit::overwrite(p, s, {TrackKind::Audio, index}, stem);
        if (!r.ok) return r;
        all.created.insert(all.created.end(), r.created.begin(), r.created.end());
    }
    if (muteOriginal)
        if (Clip* o = edit::clipById(s, clip)) o->enabled = false;
    all.ok = true;
    return all;
}

}  // namespace montage
