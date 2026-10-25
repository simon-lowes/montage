#include "BandwidthExtension.h"

#include <QFile>
#include <QString>
#include <algorithm>
#include <cmath>
#include <complex>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
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

// The network without its weights (src/media/apbwe.onnx), compiled in by CMake.
extern const unsigned char kApBweGraph[];
extern const size_t kApBweGraphSize;

namespace montage {

namespace {

constexpr const char* kCheckpointFile = "generator.ckpt";  // (the name the graph's external data gives)
constexpr const char* kGraphFile = "apbwe.onnx";
// The model's STFT (the authors' configs: the same for every checkpoint).
constexpr int kFft = 1024, kWin = 320, kHop = 80, kBins = kFft / 2 + 1;
// The network is run on chunks of this many frames, with this many more each side for what it sees around them (its
// convolutions reach 27 frames).
constexpr int kChunkFrames = 1000, kMargin = 32;
constexpr double kCrossfadeHz = 500;

constexpr double kPi = M_PI;

std::string packId(const BweCheckpoint& c) { return "apbwe-" + std::to_string(c.low / 1000) + "k-" + std::to_string(c.high / 1000) + "k"; }

}  // namespace

const std::vector<BweCheckpoint>& bweCheckpoints() {
    static const std::vector<BweCheckpoint> all = {{2000, 16000}, {4000, 16000}, {8000, 16000}, {8000, 48000},
                                                   {12000, 48000}, {16000, 48000}, {24000, 48000}};
    return all;
}

const ModelPack& bweModel(const BweCheckpoint& c) {
    // The authors' generator checkpoints (from their Google Drive folder, MIT), as mirrored on Hugging Face at a pinned
    // revision: each g_<low>kto<high>k file unchanged.
    static const std::map<std::pair<int, int>, std::string> sha = {
        {{2000, 16000}, "a4adc96d9d86c01d7970417a12c79910c726c34989956ed5a8cfd153e137862e"},
        {{4000, 16000}, "f5dd745ede8126075451fbf3f767ce0f506f15d82fe746e6202151d2a77a9ab3"},
        {{8000, 16000}, "84d63ecda61b45260668d3d51b491cbb75e00c7611f2111dd2f17d66da41711c"},
        {{8000, 48000}, "8f0387a4f1cdf1ec5dc557a9e8a303326b45b7706b37843f771f24ee94d2e521"},
        {{12000, 48000}, "defa766ac25ccb5eda300860a7cbad5c7bfa02ee2b5e28d07c1daa470f8e8f39"},
        {{16000, 48000}, "305a05dcab7dc29ffba09d32692d7a34550fc8fbdf338013641ff5d39a3cb285"},
        {{24000, 48000}, "00950c7782a076e218f9c5a9bbc3b59a5beb0c4617b62ccea836df5f616a63dd"}};
    static std::mutex m;
    static std::map<std::pair<int, int>, ModelPack> packs;
    std::lock_guard lock(m);
    auto [it, added] = packs.try_emplace({c.low, c.high});
    if (added) {
        ModelPack& p = it->second;
        p.id = packId(c);
        p.title = "speech bandwidth model (" + std::to_string(c.low / 1000) + " to " + std::to_string(c.high / 1000) + " kHz)";
        p.parentEnv = "MONTAGE_BWE_MODELS";
        p.urlEnv = "MONTAGE_BWE_MODEL_URL";
        const std::string name = std::to_string(c.low / 1000) + "kto" + std::to_string(c.high / 1000) + "k";
        const auto s = sha.find({c.low, c.high});
        p.files = {{kCheckpointFile,
                    "https://huggingface.co/rsxdalv/AP-BWE/resolve/fa3f46d233cbc1d75cec9321188f86db627ba239/weights/" + name + "/g_" + name + ".zip",
                    s != sha.end() ? s->second : std::string(), 119097961}};
    }
    return it->second;
}

bool bandwidthExtensionAvailable() {
#ifdef MONTAGE_WITH_ONNXRUNTIME
    return true;
#else
    return false;
#endif
}

// ---- Resampling ---------------------------------------------------------------

namespace {

double besselI0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 60; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

}  // namespace

std::vector<float> resampleSinc(const std::vector<float>& in, int from, int to) {
    if (from == to || in.empty() || from <= 0 || to <= 0) return in;
    const int64_t g = std::gcd(from, to), L = to / g, M = from / g;
    const int64_t len = int64_t(in.size());
    const size_t n = size_t((len * L + M / 2) / M);
    const double w = 0.97 * std::min(from, to) / 2.0 / from;  // the cut-off, in cycles per input sample
    const int half = int(std::ceil(32 / (2 * w)));
    constexpr double beta = 8.6;
    const double i0beta = besselI0(beta);
    auto tap = [&](double x) {  // x input samples from the output sample
        if (std::abs(x) >= half) return 0.0;
        const double r = x / half, a = 2 * w * x;
        return 2 * w * (a == 0 ? 1.0 : std::sin(kPi * a) / (kPi * a)) * besselI0(beta * std::sqrt(1 - r * r)) / i0beta;
    };
    const int taps = 2 * half + 1;
    // Output i sits p / L past input q (i M = q L + p): one set of taps for each p, when there are not too many.
    const bool table = L * taps <= 4000000;
    std::vector<float> tab;
    if (table) {
        tab.resize(size_t(L * taps));
        for (int64_t p = 0; p < L; ++p)
            for (int k = -half; k <= half; ++k) tab[size_t(p * taps + k + half)] = float(tap(k - double(p) / double(L)));
    }
    std::vector<float> out(n);
    const int threads = int(std::clamp<size_t>(n / 200000, 1, std::max(1u, std::thread::hardware_concurrency())));
    auto run = [&](size_t i0, size_t i1) {
        for (size_t i = i0; i < i1; ++i) {
            const int64_t q = int64_t(i) * M / L, p = int64_t(i) * M % L;
            const int64_t k0 = std::max<int64_t>(-half, -q), k1 = std::min<int64_t>(half, len - 1 - q);
            double acc = 0;
            if (table) {
                const float* t = tab.data() + p * taps + half;
                for (int64_t k = k0; k <= k1; ++k) acc += double(in[size_t(q + k)]) * t[k];
            } else {
                for (int64_t k = k0; k <= k1; ++k) acc += double(in[size_t(q + k)]) * tap(double(k) - double(p) / double(L));
            }
            out[i] = float(acc);
        }
    };
    std::vector<std::thread> pool;
    for (int t = 1; t < threads; ++t) pool.emplace_back(run, n * size_t(t) / size_t(threads), n * size_t(t + 1) / size_t(threads));
    run(0, n / size_t(threads));
    for (auto& t : pool) t.join();
    return out;
}

// ---- The cut-off ----------------------------------------------------------------

namespace {

std::vector<float> hann(int n, int width) {  // periodic, `width` wide, centred in n (as torch.stft pads its window)
    std::vector<float> w(static_cast<size_t>(n), 0.0f);
    const int left = (n - width) / 2;
    for (int i = 0; i < width; ++i) w[size_t(left + i)] = float(0.5 - 0.5 * std::cos(2 * kPi * i / width));
    return w;
}

// Frames of the sound (starting every `hop` from -n), each windowed and transformed.
template <typename F>
void frames(const std::vector<float>& x, int n, int hop, const std::vector<float>& w, F&& each) {
    std::vector<std::complex<float>> buf(static_cast<size_t>(n));
    const int64_t len = int64_t(x.size());
    for (int64_t start = -n, t = 0; start < len; start += hop, ++t) {
        for (int i = 0; i < n; ++i) {
            const int64_t s = start + i;
            buf[size_t(i)] = s >= 0 && s < len ? x[size_t(s)] * w[size_t(i)] : 0.0f;
        }
        fftInPlace(buf, false);
        each(t, start, buf);
    }
}

// The level of each frame (dB), and the gate where speech is: open within 30 dB of the loudest (the 95th percentile),
// closing over the 6 dB below that, held a frame either side.
std::vector<float> speechGate(const std::vector<double>& levels) {
    std::vector<double> sorted = levels;
    std::sort(sorted.begin(), sorted.end());
    const double loud = sorted.empty() ? 0 : sorted[size_t(double(sorted.size() - 1) * 0.95)];
    std::vector<float> g(levels.size());
    for (size_t t = 0; t < levels.size(); ++t) g[t] = float(std::clamp((levels[t] - (loud - 36)) / 6, 0.0, 1.0));
    std::vector<float> held = g;
    for (size_t t = 0; t < g.size(); ++t) {
        if (t > 0) held[t] = std::max(held[t], g[t - 1]);
        if (t + 1 < g.size()) held[t] = std::max(held[t], g[t + 1]);
    }
    return held;
}

}  // namespace

double speechCutoff(const std::vector<float>& x, int rate) {
    const double top = rate / 2.0;
    constexpr int N = 4096;
    if (int64_t(x.size()) < N || rate <= 0) return top;
    const std::vector<float> w = hann(N, N);
    const int bins = N / 2 + 1;
    // The frames' levels first, then the average spectrum of the speech (the frames the gate lets through fully).
    std::vector<double> levels;
    frames(x, N, N / 2, w, [&](int64_t, int64_t, const std::vector<std::complex<float>>& b) {
        double e = 0;
        for (int k = 0; k < bins; ++k) e += std::norm(b[size_t(k)]);
        levels.push_back(10 * std::log10(e + 1e-30));
    });
    const std::vector<float> gate = speechGate(levels);
    std::vector<double> mean(static_cast<size_t>(bins), 0.0);
    int used = 0;
    frames(x, N, N / 2, w, [&](int64_t t, int64_t, const std::vector<std::complex<float>>& b) {
        if (gate[size_t(t)] < 1 || levels[size_t(t)] < -250) return;
        for (int k = 0; k < bins; ++k) mean[size_t(k)] += std::norm(b[size_t(k)]);
        ++used;
    });
    if (!used) return top;
    const double binHz = double(rate) / N;
    auto bin = [&](double hz) { return std::clamp(int(std::lround(hz / binHz)), 0, bins - 1); };
    std::vector<double> db(static_cast<size_t>(bins)), smooth(static_cast<size_t>(bins));
    for (int k = 0; k < bins; ++k) db[size_t(k)] = 10 * std::log10(mean[size_t(k)] / used + 1e-30);
    const int sw = std::max(1, bin(50));
    for (int k = 0; k < bins; ++k) {
        double s = 0;
        int c = 0;
        for (int j = std::max(0, k - sw); j <= std::min(bins - 1, k + sw); ++j) s += db[size_t(j)], ++c;
        smooth[size_t(k)] = s / c;
    }
    // The loudest the spectrum is from each bin to the top (less the last 100 Hz).
    const int last = bin(top - 100);
    std::vector<double> above(size_t(bins) + 1, -1e300);
    for (int k = last; k >= 0; --k) above[size_t(k)] = std::max(above[size_t(k) + 1], smooth[size_t(k)]);
    // The cut-off is where the spectrum falls furthest: from its level over the 1 kHz below to the loudest above.
    const int span = bin(1000), gap = bin(150);
    int best = -1;
    double bestDrop = 0, bestLevel = 0;
    for (int j = bin(500); j + gap <= last; ++j) {
        if (j - span < 0) continue;
        double below = 0;
        for (int k = j - span; k < j; ++k) below += smooth[size_t(k)];
        below /= span;
        const double drop = below - above[size_t(j + gap)];
        if (drop > bestDrop) bestDrop = drop, best = j, bestLevel = below;
    }
    if (best < 0 || bestDrop < 25) return top;
    // Its edge: the highest frequency near there still within 15 dB of that level.
    int edge = best;
    for (int k = std::max(0, best - bin(500)); k <= std::min(last, best + bin(500)); ++k)
        if (smooth[size_t(k)] >= bestLevel - 15) edge = k;
    return std::min(top, (edge + 0.5) * binHz);
}

std::vector<BweCheckpoint> bwePlan(double cutoff, int rate) {
    const double top = std::min(rate / 2.0, 24000.0);
    if (cutoff >= 0.9 * top || cutoff < 800) return {};
    std::vector<BweCheckpoint> plan;
    double have = cutoff;
    // The widest checkpoint whose band the speech fills (allowing 300 Hz for the slope of a filter): to 48 kHz for a
    // wide rate when one takes it, else (speech under 4 kHz, or a rate of 16 kHz or less) to 16 kHz.
    auto widest = [&](int high) {
        const BweCheckpoint* pick = nullptr;
        for (const BweCheckpoint& c : bweCheckpoints())
            if (c.high == high && c.low / 2 <= have + 300 && (!pick || c.low > pick->low)) pick = &c;
        return pick;
    };
    while (have < 0.9 * top) {
        const BweCheckpoint* pick = rate > 16000 ? widest(48000) : nullptr;
        if (!pick) pick = widest(16000);
        if (!pick) return {};
        plan.push_back(*pick);
        have = pick->high / 2.0;
    }
    return plan;
}

// ---- The network on ONNX Runtime ------------------------------------------------

#ifdef MONTAGE_WITH_ONNXRUNTIME
namespace {

Ort::Env& ortEnv() {
    static auto* env = new Ort::Env(ORT_LOGGING_LEVEL_ERROR, "montage-bwe");  // kept for the session, never torn down
    return *env;
}

// The graph beside the checkpoint it reads (rewritten when this Montage brings a different one).
bool writeGraph(const ModelPack& pack, std::string& graphPath, std::string* error) {
    const QString dir = QString::fromStdString(pack.directory());
    graphPath = (dir + '/' + kGraphFile).toStdString();
    const QByteArray want = QByteArray::fromRawData(reinterpret_cast<const char*>(kApBweGraph), qsizetype(kApBweGraphSize));
    QFile graph(QString::fromStdString(graphPath));
    if (graph.open(QIODevice::ReadOnly) && graph.readAll() == want) return true;
    graph.close();
    if (graph.open(QIODevice::WriteOnly | QIODevice::Truncate) && graph.write(want) == want.size()) return true;
    if (error) *error = "Cannot write the speech bandwidth model into " + dir.toStdString();
    return false;
}

std::shared_ptr<OrtModel> loadModel(const BweCheckpoint& c, std::string* error) {
    static std::mutex m;
    static auto* cached = new std::map<std::pair<int, int>, std::shared_ptr<OrtModel>>;
    std::lock_guard lock(m);
    if (auto it = cached->find({c.low, c.high}); it != cached->end()) return it->second;
    if (!ortUsable(error)) return nullptr;
    const ModelPack& pack = bweModel(c);
    if (!pack.installed()) {
        if (error) *error = "The " + pack.title + " is not downloaded";
        return nullptr;
    }
    std::string graph;
    if (!writeGraph(pack, graph, error)) return nullptr;
    try {
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        so.SetIntraOpNumThreads(int(std::max(1u, std::thread::hardware_concurrency())));
#ifdef _WIN32
        const std::wstring path = QString::fromStdString(graph).toStdWString();
#else
        const std::string& path = graph;
#endif
        std::shared_ptr<OrtModel> model = openModel(ortEnv(), path.c_str(), so);
        (*cached)[{c.low, c.high}] = model;
        return model;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The speech bandwidth model could not be loaded: ") + e.what();
        return nullptr;
    }
}

}  // namespace
#endif

BweNetwork bweModelNetwork() {
    return [](const BweCheckpoint& c, int64_t, int frames, const std::vector<float>& logAmp, const std::vector<float>& phase,
              std::vector<float>& logAmpOut, std::vector<float>& phaseOut, std::string* error) -> bool {
#ifdef MONTAGE_WITH_ONNXRUNTIME
        std::shared_ptr<OrtModel> model = loadModel(c, error);
        if (!model) return false;
        const size_t count = size_t(kBins) * size_t(frames);
        try {
            Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const int64_t shape[3] = {1, kBins, frames};
            Ort::Value inputs[2] = {
                Ort::Value::CreateTensor<float>(cpu, const_cast<float*>(logAmp.data()), count, shape, 3),
                Ort::Value::CreateTensor<float>(cpu, const_cast<float*>(phase.data()), count, shape, 3)};
            const char* inNames[2] = {"log_amp", "pha"};
            const char* outNames[3] = {"log_amp_wb", "pha_r", "pha_i"};
            std::vector<Ort::Value> out = model->Run(Ort::RunOptions{}, inNames, inputs, 2, outNames, 3);
            if (out.size() != 3 || out[0].GetTensorTypeAndShapeInfo().GetElementCount() != count) {
                if (error) *error = "The speech bandwidth model gave a result of the wrong size";
                return false;
            }
            const float* a = out[0].GetTensorData<float>();
            const float* r = out[1].GetTensorData<float>();
            const float* i = out[2].GetTensorData<float>();
            logAmpOut.assign(a, a + count);
            phaseOut.resize(count);
            for (size_t k = 0; k < count; ++k) phaseOut[k] = std::atan2(i[k], r[k]);
            return true;
        } catch (const Ort::Exception& e) {
            if (error) *error = std::string("The speech bandwidth model failed: ") + e.what();
            return false;
        }
#else
        (void)c, (void)frames, (void)logAmp, (void)phase, (void)logAmpOut, (void)phaseOut;
        if (error) *error = "This build of Montage has no ONNX Runtime";
        return false;
#endif
    };
}

// ---- Restoring ------------------------------------------------------------------

namespace {

// The sound at index i, reflected at the ends (as torch.stft pads).
float reflected(const std::vector<float>& x, int64_t i) {
    const int64_t len = int64_t(x.size());
    if (len == 1) return x[0];
    while (i < 0 || i >= len) i = i < 0 ? -i : 2 * (len - 1) - i;
    return x[size_t(i)];
}

// One checkpoint on speech at its high rate (band-limited as it was trained: through its low rate): the network's wide
// band, turned back into sound (torch.istft's overlap-add).
bool runCheckpoint(const std::vector<float>& x, const BweCheckpoint& c, const BweNetwork& network, std::vector<float>& y,
                   const std::atomic<bool>* cancel, std::string* error) {
    const int64_t len = int64_t(x.size());
    if (len == 0) {
        y.clear();
        return true;
    }
    const int64_t total = 1 + len / kHop;
    const std::vector<float> w = hann(kFft, kWin);
    std::vector<float> acc(static_cast<size_t>(len), 0.0f), env(size_t(len), 0.0f);
    std::vector<std::complex<float>> buf(kFft);
    for (int64_t a = 0; a < total; a += kChunkFrames) {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return false;
        }
        const int64_t b = std::min(total, a + kChunkFrames);
        const int64_t a0 = std::max<int64_t>(0, a - kMargin), b0 = std::min(total, b + kMargin);
        const int n = int(b0 - a0);
        std::vector<float> logAmp(size_t(kBins) * size_t(n)), phase(logAmp.size());
        for (int j = 0; j < n; ++j) {
            const int64_t t = a0 + j;
            for (int i = 0; i < kFft; ++i)
                buf[size_t(i)] = w[size_t(i)] != 0 ? reflected(x, t * kHop + i - kFft / 2) * w[size_t(i)] : 0.0f;
            fftInPlace(buf, false);
            for (int k = 0; k < kBins; ++k) {
                logAmp[size_t(k) * size_t(n) + size_t(j)] = std::log(std::abs(buf[size_t(k)]) + 1e-4f);
                phase[size_t(k) * size_t(n) + size_t(j)] = std::arg(buf[size_t(k)]);
            }
        }
        std::vector<float> logAmpOut, phaseOut;
        if (!network(c, a0, n, logAmp, phase, logAmpOut, phaseOut, error)) return false;
        if (logAmpOut.size() != logAmp.size() || phaseOut.size() != logAmp.size()) {
            if (error) *error = "The speech bandwidth network gave a result of the wrong size";
            return false;
        }
        for (int64_t t = a; t < b; ++t) {
            const size_t j = size_t(t - a0);
            for (int k = 0; k < kBins; ++k) {
                const float m = std::exp(logAmpOut[size_t(k) * size_t(n) + j]), p = phaseOut[size_t(k) * size_t(n) + j];
                buf[size_t(k)] = k == 0 || k == kBins - 1 ? std::complex<float>(m * std::cos(p), 0) : std::polar(m, p);
            }
            for (int k = kBins; k < kFft; ++k) buf[size_t(k)] = std::conj(buf[size_t(kFft - k)]);
            fftInPlace(buf, true);
            for (int i = 0; i < kFft; ++i) {
                if (w[size_t(i)] == 0) continue;
                const int64_t s = t * kHop + i - kFft / 2;
                if (s < 0 || s >= len) continue;
                acc[size_t(s)] += buf[size_t(i)].real() / kFft * w[size_t(i)];
                env[size_t(s)] += w[size_t(i)] * w[size_t(i)];
            }
        }
    }
    y.resize(size_t(len));
    for (int64_t s = 0; s < len; ++s) y[size_t(s)] = env[size_t(s)] > 1e-11f ? acc[size_t(s)] / env[size_t(s)] : 0.0f;
    return true;
}

}  // namespace

bool extendBandwidth(const std::vector<float>& in, int rate, std::vector<float>& out, double amount, double cutoff,
                     const BweNetwork& network, const std::atomic<bool>* cancel, std::string* error, BweResult* result) {
    BweResult r;
    if (in.empty() || rate <= 0) {
        out = in;
        return true;
    }
    r.cutoff = cutoff > 0 ? std::min(cutoff, rate / 2.0) : speechCutoff(in, rate);
    r.plan = bwePlan(r.cutoff, rate);
    if (result) *result = r;
    const double a = std::clamp(amount, 0.0, 100.0) / 100.0;
    if (r.plan.empty() || a <= 0) {
        out = in;
        return true;
    }
    const BweNetwork net = network ? network : bweModelNetwork();
    std::vector<float> cur = in;
    int curRate = rate;
    for (const BweCheckpoint& c : r.plan) {
        std::vector<float> y;
        if (!runCheckpoint(resampleSinc(resampleSinc(cur, curRate, c.low), c.low, c.high), c, net, y, cancel, error)) return false;
        cur = std::move(y);
        curRate = c.high;
    }
    std::vector<float> wide = resampleSinc(cur, curRate, rate);
    wide.resize(in.size(), 0.0f);
    std::vector<float>().swap(cur);

    // The original kept below the cut-off; above it the new band crossfaded in with equal power over 500 Hz (scaled by
    // the amount), only where the speech is. What is added is the difference, so below the cut-off nothing changes.
    constexpr int N = 2048, H = N / 4;
    const std::vector<float> w = hann(N, N);
    const int bins = N / 2 + 1;
    const double binHz = double(rate) / N;
    std::vector<float> keep(static_cast<size_t>(bins)), add(static_cast<size_t>(bins));
    // (At rates above 48 kHz, above what the models make the original stays.)
    const double made = 0.97 * std::min(rate, 48000) / 2.0;
    for (int k = 0; k < bins; ++k) {
        if (k * binHz > made && rate > 48000) {
            add[size_t(k)] = 0;
            keep[size_t(k)] = 1;
            continue;
        }
        const double ramp = std::clamp((k * binHz - r.cutoff) / kCrossfadeHz, 0.0, 1.0);
        const double s = a * std::sin(kPi / 2 * ramp);
        add[size_t(k)] = float(s);
        keep[size_t(k)] = float(std::sqrt(std::max(0.0, 1 - s * s)));
    }
    std::vector<double> levels;
    frames(in, N, H, w, [&](int64_t, int64_t, const std::vector<std::complex<float>>& b) {
        double e = 0;
        for (int k = 0; k < bins; ++k) e += std::norm(b[size_t(k)]);
        levels.push_back(10 * std::log10(e + 1e-30));
    });
    const std::vector<float> gate = speechGate(levels);
    const int64_t len = int64_t(in.size());
    std::vector<float> acc(static_cast<size_t>(len), 0.0f), env(size_t(len), 0.0f);
    std::vector<std::complex<float>> x(static_cast<size_t>(N)), y(static_cast<size_t>(N));
    // The original's frames and the restored sound's, in step.
    for (int64_t start = -N, t = 0; start < len; start += H, ++t) {
        if ((t & 255) == 0 && cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return false;
        }
        for (int i = 0; i < N; ++i) {
            const int64_t s = start + i;
            if (s >= 0 && s < len) env[size_t(s)] += w[size_t(i)] * w[size_t(i)];
        }
        const float g = gate[size_t(t)];
        if (g <= 0) continue;
        for (int i = 0; i < N; ++i) {
            const int64_t s = start + i;
            const bool inside = s >= 0 && s < len;
            x[size_t(i)] = inside ? in[size_t(s)] * w[size_t(i)] : 0.0f;
            y[size_t(i)] = inside ? wide[size_t(s)] * w[size_t(i)] : 0.0f;
        }
        fftInPlace(x, false);
        fftInPlace(y, false);
        for (int k = 0; k < bins; ++k)
            y[size_t(k)] = add[size_t(k)] > 0 ? g * ((keep[size_t(k)] - 1) * x[size_t(k)] + add[size_t(k)] * y[size_t(k)]) : 0.0f;
        y[0] = y[0].real();
        y[size_t(bins - 1)] = y[size_t(bins - 1)].real();
        for (int k = bins; k < N; ++k) y[size_t(k)] = std::conj(y[size_t(N - k)]);
        fftInPlace(y, true);
        for (int i = 0; i < N; ++i) {
            const int64_t s = start + i;
            if (s >= 0 && s < len) acc[size_t(s)] += y[size_t(i)].real() / N * w[size_t(i)];
        }
    }
    out.resize(in.size());
    for (int64_t s = 0; s < len; ++s) out[size_t(s)] = in[size_t(s)] + (env[size_t(s)] > 1e-11f ? acc[size_t(s)] / env[size_t(s)] : 0.0f);
    return true;
}

}  // namespace montage
