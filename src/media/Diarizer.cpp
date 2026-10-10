#include "Diarizer.h"

#include <QFileInfo>
#include <QString>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>
#include <tuple>

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

namespace montage {

const ModelPack& speakerModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "speakers";
        p.title = "speaker model";
        p.directoryEnv = "MONTAGE_SPEAKER_MODEL";
        p.urlEnv = "MONTAGE_SPEAKER_MODEL_URL";
        p.files = {
            {"pyannote-segmentation-3.0.onnx",
             "https://huggingface.co/csukuangfj/sherpa-onnx-pyannote-segmentation-3-0/resolve/"
             "9403a6902bb58e3d5ae8c7e77c3422de279db2e0/model.onnx",
             "220ad67ca923bef2fa91f2390c786097bf305bceb5e261d4af67b38e938e1079", 5992913},
            {"campplus-voxceleb.onnx",
             "https://huggingface.co/csukuangfj/speaker-embedding-models/resolve/0743f301363dec56491a490f6d6cbc9d67f9a3bf/"
             "3dspeaker_speech_campplus_sv_en_voxceleb_16k.onnx",
             "357a834f702b80161e5b981182c038e18553c1f2ca752ed6cec2052365d4129b", 29596978},
        };
        return p;
    }();
    return pack;
}

// ---- Features --------------------------------------------------------------------

namespace {

constexpr int kRate = 16000;
constexpr int kFrameLength = 400;  // 25 ms
constexpr int kFrameShift = 160;   // 10 ms
constexpr int kFft = 512;
constexpr int kMelBins = 80;

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

double mel(double hz) { return 1127.0 * std::log(1.0 + hz / 700.0); }

// Kaldi's triangular mel filters over the first kFft/2 FFT bins (20 Hz to Nyquist - 400 Hz).
const std::vector<std::vector<std::pair<int, float>>>& melBanks() {
    static const std::vector<std::vector<std::pair<int, float>>> banks = [] {
        std::vector<std::vector<std::pair<int, float>>> b(kMelBins);
        const double lo = mel(20), hi = mel(kRate / 2.0 - 400), delta = (hi - lo) / (kMelBins + 1);
        for (int m = 0; m < kMelBins; ++m) {
            const double left = lo + m * delta, centre = left + delta, right = centre + delta;
            for (int i = 0; i < kFft / 2; ++i) {
                const double v = mel(double(kRate) / kFft * i);
                if (v <= left || v >= right) continue;
                const double w = v <= centre ? (v - left) / (centre - left) : (right - v) / (right - centre);
                b[size_t(m)].push_back({i, float(w)});
            }
        }
        return b;
    }();
    return banks;
}

}  // namespace

std::vector<float> speakerFeatures(const float* samples, size_t n) {
    std::vector<float> out;
    if (n == 0) return out;
    const size_t frames = (n + kFrameShift / 2) / kFrameShift;
    out.resize(frames * kMelBins);
    static const std::vector<double> window = [] {
        std::vector<double> w(kFrameLength);
        for (int i = 0; i < kFrameLength; ++i) w[size_t(i)] = std::pow(0.5 - 0.5 * std::cos(2 * M_PI * i / (kFrameLength - 1)), 0.85);
        return w;
    }();
    const auto& banks = melBanks();
    const int64_t N = int64_t(n);
    std::vector<double> x(kFrameLength);
    std::vector<std::complex<double>> spec(kFft);
    std::vector<double> power(kFft / 2 + 1);
    for (size_t f = 0; f < frames; ++f) {
        // Centred frames, reflected at the edges (Kaldi with snip_edges = false).
        const int64_t begin = int64_t(f) * kFrameShift + kFrameShift / 2 - kFrameLength / 2;
        double mean = 0;
        for (int i = 0; i < kFrameLength; ++i) {
            int64_t s = begin + i;
            while (s < 0 || s >= N) s = s < 0 ? -s - 1 : 2 * N - 1 - s;
            x[size_t(i)] = samples[s];
            mean += x[size_t(i)];
        }
        mean /= kFrameLength;
        for (double& v : x) v -= mean;
        for (int i = kFrameLength - 1; i > 0; --i) x[size_t(i)] -= 0.97 * x[size_t(i - 1)];
        x[0] -= 0.97 * x[0];
        std::fill(spec.begin(), spec.end(), std::complex<double>(0));
        for (int i = 0; i < kFrameLength; ++i) spec[size_t(i)] = x[size_t(i)] * window[size_t(i)];
        fft(spec);
        for (int i = 0; i <= kFft / 2; ++i) power[size_t(i)] = std::norm(spec[size_t(i)]);
        for (int m = 0; m < kMelBins; ++m) {
            double e = 0;
            for (const auto& [i, w] : banks[size_t(m)]) e += w * power[size_t(i)];
            out[f * kMelBins + size_t(m)] = float(std::log(std::max(e, double(std::numeric_limits<float>::epsilon()))));
        }
    }
    return out;
}

// ---- Clustering ------------------------------------------------------------------

std::vector<int> clusterSpeakers(const std::vector<std::vector<float>>& embeddings, int count, double threshold) {
    const int n = int(embeddings.size());
    std::vector<int> labels(size_t(n), 0);
    if (n <= 1) return labels;
    // Cosine distances between the normalised embeddings.
    std::vector<std::vector<float>> unit(embeddings);
    for (auto& v : unit) {
        double len = 0;
        for (float x : v) len += double(x) * x;
        len = std::sqrt(std::max(len, 1e-20));
        for (float& x : v) x = float(x / len);
    }
    std::vector<float> d(size_t(n) * size_t(n), 0.f);
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            double dot = 0;
            for (size_t k = 0; k < unit[size_t(i)].size(); ++k) dot += double(unit[size_t(i)][k]) * unit[size_t(j)][k];
            d[size_t(i) * n + size_t(j)] = d[size_t(j) * n + size_t(i)] = float(std::max(0.0, 1 - dot));
        }
    // Average linkage by nearest-neighbour chains: each merge keeps the slot of
    // one cluster, whose distances become the size-weighted mean of the two.
    std::vector<bool> active(size_t(n), true);
    std::vector<double> size(size_t(n), 1.0);
    std::vector<std::tuple<float, int, int>> merges;  // height, slot kept, slot removed
    std::vector<int> chain;
    for (int remaining = n; remaining > 1;) {
        if (chain.empty())
            for (int i = 0; i < n; ++i)
                if (active[size_t(i)]) {
                    chain.push_back(i);
                    break;
                }
        const int a = chain.back();
        const int prev = chain.size() >= 2 ? chain[chain.size() - 2] : -1;
        int b = prev;
        float best = prev >= 0 ? d[size_t(a) * n + size_t(prev)] : std::numeric_limits<float>::infinity();
        for (int c = 0; c < n; ++c)
            if (active[size_t(c)] && c != a && d[size_t(a) * n + size_t(c)] < best) {
                best = d[size_t(a) * n + size_t(c)];
                b = c;
            }
        if (b != prev) {
            chain.push_back(b);
            continue;
        }
        // a and prev are each other's nearest: merge them.
        chain.pop_back();
        chain.pop_back();
        const int keep = std::min(a, b), drop = std::max(a, b);
        merges.emplace_back(best, keep, drop);
        active[size_t(drop)] = false;
        const double wk = size[size_t(keep)], wd = size[size_t(drop)];
        for (int c = 0; c < n; ++c)
            if (active[size_t(c)] && c != keep) {
                const float v = float((wk * d[size_t(keep) * n + size_t(c)] + wd * d[size_t(drop) * n + size_t(c)]) / (wk + wd));
                d[size_t(keep) * n + size_t(c)] = d[size_t(c) * n + size_t(keep)] = v;
            }
        size[size_t(keep)] += size[size_t(drop)];
        --remaining;
    }
    // Cut the tree: the lowest merges first, until `count` clusters remain or the threshold is passed.
    std::stable_sort(merges.begin(), merges.end(), [](const auto& x, const auto& y) { return std::get<0>(x) < std::get<0>(y); });
    std::vector<int> parent(static_cast<size_t>(n));
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](int i) {
        while (parent[size_t(i)] != i) i = parent[size_t(i)] = parent[size_t(parent[size_t(i)])];
        return i;
    };
    int clusters = n;
    for (const auto& [h, keep, drop] : merges) {
        if (count > 0 ? clusters <= count : h > threshold) break;
        parent[size_t(root(drop))] = root(keep);
        --clusters;
    }
    std::vector<int> group(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) group[size_t(i)] = root(i);
    if (count <= 0) {
        // As in pyannote, a handful of windows is not a person: very small
        // clusters join the nearest real one (by centroid), if there is one.
        const int minSize = std::clamp(n / 20, 2, 12);
        std::map<int, std::vector<double>> centre;
        std::map<int, int> members;
        for (int i = 0; i < n; ++i) ++members[group[size_t(i)]];
        for (int i = 0; i < n; ++i) {
            if (members[group[size_t(i)]] < minSize) continue;
            auto& c = centre[group[size_t(i)]];
            c.resize(unit[size_t(i)].size(), 0.0);
            for (size_t k = 0; k < c.size(); ++k) c[k] += unit[size_t(i)][k];
        }
        if (!centre.empty())
            for (int i = 0; i < n; ++i) {
                if (members[group[size_t(i)]] >= minSize) continue;
                double best = -2;
                for (const auto& [g, c] : centre) {
                    double dot = 0, len = 0;
                    for (size_t k = 0; k < c.size(); ++k) {
                        dot += c[k] * unit[size_t(i)][k];
                        len += c[k] * c[k];
                    }
                    const double cosine = dot / std::sqrt(std::max(len, 1e-20));
                    if (cosine > best) {
                        best = cosine;
                        group[size_t(i)] = g;
                    }
                }
            }
    }
    std::map<int, int> number;
    for (int i = 0; i < n; ++i) {
        auto it = number.find(group[size_t(i)]);
        if (it == number.end()) it = number.emplace(group[size_t(i)], int(number.size())).first;
        labels[size_t(i)] = it->second;
    }
    return labels;
}

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool diarizerAvailable() { return false; }
bool diarize(const std::vector<float>&, const DiarizeOptions&, std::vector<SpeakerTurn>&, const std::function<void(double)>&,
             const std::atomic<bool>*, std::string* error) {
    if (error) *error = "This build of Montage cannot tell speakers apart (it was built without ONNX Runtime)";
    return false;
}

#else

bool diarizerAvailable() { return true; }

namespace {

// pyannote segmentation 3.0: 10 s windows, an output frame every 270
// samples (991 samples wide), and seven "powerset" classes: nobody, one of
// three speakers, or one of the three pairs.
constexpr int kWindow = 160000;
constexpr int kFieldShift = 270;
constexpr int kFieldSize = 991;
constexpr int kLocal = 3;
constexpr int kPowerset[7][kLocal] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {1, 1, 0}, {1, 0, 1}, {0, 1, 1}};

struct Models {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-speakers"};
    std::unique_ptr<Ort::Session> segmentation, embedding;
};

std::shared_ptr<Models> loadModels(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Models>();  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    const ModelPack& pack = speakerModel();
    if (!pack.installed()) {
        if (error) *error = "The speaker model is not downloaded";
        return nullptr;
    }
    try {
        auto models = std::make_shared<Models>();
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(int(std::max(1u, std::thread::hardware_concurrency())));
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        auto open = [&](const ModelFile& f) {
#ifdef _WIN32
            const std::wstring path = QString::fromStdString(pack.path(f)).toStdWString();
#else
            const std::string path = pack.path(f);
#endif
            return std::make_unique<Ort::Session>(models->env, path.c_str(), so);
        };
        models->segmentation = open(pack.files[0]);
        models->embedding = open(pack.files[1]);
        *cached = models;
        return models;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The speaker model could not be loaded: ") + e.what();
        return nullptr;
    }
}

Ort::MemoryInfo& cpu() {
    static Ort::MemoryInfo info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    return info;
}

// Per-frame 0/1 activity of the window's three local speakers.
using Labels = std::vector<std::array<int, kLocal>>;

Labels segmentWindow(Models& m, const float* samples) {
    std::vector<float> in(samples, samples + kWindow);
    const int64_t shape[3] = {1, 1, kWindow};
    Ort::Value x = Ort::Value::CreateTensor<float>(cpu(), in.data(), in.size(), shape, 3);
    const char* inName[] = {"x"};
    const char* outName[] = {"y"};
    auto y = m.segmentation->Run(Ort::RunOptions{nullptr}, inName, &x, 1, outName, 1);
    const auto shapeOut = y[0].GetTensorTypeAndShapeInfo().GetShape();
    const int frames = int(shapeOut[1]), classes = int(shapeOut[2]);
    const float* p = y[0].GetTensorData<float>();
    Labels labels(static_cast<size_t>(frames));
    for (int f = 0; f < frames; ++f) {
        const float* row = p + size_t(f) * size_t(classes);
        const int best = int(std::max_element(row, row + std::min(classes, 7)) - row);
        for (int s = 0; s < kLocal; ++s) labels[size_t(f)][size_t(s)] = kPowerset[best][s];
    }
    return labels;
}

std::vector<float> embed(Models& m, const std::vector<float>& audio) {
    std::vector<float> feats = speakerFeatures(audio.data(), audio.size());
    const int64_t frames = int64_t(feats.size() / kMelBins);
    if (frames <= 0) return {};
    // CAM++ expects features with their mean over the clip removed.
    for (int b = 0; b < kMelBins; ++b) {
        double mean = 0;
        for (int64_t f = 0; f < frames; ++f) mean += feats[size_t(f) * kMelBins + size_t(b)];
        mean /= double(frames);
        for (int64_t f = 0; f < frames; ++f) feats[size_t(f) * kMelBins + size_t(b)] -= float(mean);
    }
    const int64_t shape[3] = {1, frames, kMelBins};
    Ort::Value x = Ort::Value::CreateTensor<float>(cpu(), feats.data(), feats.size(), shape, 3);
    const char* inName[] = {"x"};
    const char* outName[] = {"embedding"};
    auto y = m.embedding->Run(Ort::RunOptions{nullptr}, inName, &x, 1, outName, 1);
    const size_t dim = y[0].GetTensorTypeAndShapeInfo().GetElementCount();
    const float* p = y[0].GetTensorData<float>();
    return std::vector<float>(p, p + dim);
}

}  // namespace

bool diarize(const std::vector<float>& audio, const DiarizeOptions& options, std::vector<SpeakerTurn>& turns,
             const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::string* error) {
    turns.clear();
    if (audio.empty()) return true;
    std::shared_ptr<Models> models = loadModels(error);
    if (!models) return false;
    const int64_t n = int64_t(audio.size());
    // Long recordings step further between windows (pyannote steps a tenth of a window).
    const int64_t shift = n > int64_t(kRate) * 1200 ? kWindow / 5 : kWindow / 10;
    try {
        // 1. Each window's local speakers.
        std::vector<int64_t> starts;
        if (n <= kWindow) starts.push_back(0);
        else {
            for (int64_t s = 0; s + kWindow <= n; s += shift) starts.push_back(s);
            if ((n - kWindow) % shift) starts.push_back(starts.back() + shift);
        }
        std::vector<Labels> windows;
        std::vector<float> buffer(kWindow);
        for (size_t c = 0; c < starts.size(); ++c) {
            if (cancel && cancel->load()) return false;
            std::fill(buffer.begin(), buffer.end(), 0.f);
            std::copy(audio.begin() + starts[c], audio.begin() + std::min(n, starts[c] + kWindow), buffer.begin());
            windows.push_back(segmentWindow(*models, buffer.data()));
            if (progress) progress(0.3 * double(c + 1) / double(starts.size()));
        }
        const int frames = int(windows[0].size());
        auto frameOf = [&](size_t c) { return int64_t(double(starts[c]) / kFieldShift + 0.5); };
        const int64_t total = frameOf(starts.size() - 1) + frames;
        // How many people speak in each frame (the average over the windows covering it).
        std::vector<double> sum(size_t(total), 0), cover(size_t(total), 0);
        for (size_t c = 0; c < windows.size(); ++c)
            for (int f = 0; f < frames; ++f) {
                const auto& l = windows[c][size_t(f)];
                sum[size_t(frameOf(c) + f)] += l[0] + l[1] + l[2];
                cover[size_t(frameOf(c) + f)] += 1;
            }
        std::vector<int> speaking(static_cast<size_t>(total));
        for (int64_t f = 0; f < total; ++f) speaking[size_t(f)] = int(sum[size_t(f)] / (cover[size_t(f)] + 1e-12) + 0.5);

        // 2. An embedding per (window, local speaker) with at least 10 frames
        // of speech on their own (frames with two speakers are left out).
        struct Voice {
            size_t window;
            int local;
            std::vector<float> embedding;
        };
        std::vector<Voice> voices;
        std::vector<std::pair<size_t, int>> wanted;
        for (size_t c = 0; c < windows.size(); ++c)
            for (int s = 0; s < kLocal; ++s) {
                int alone = 0;
                for (const auto& l : windows[c]) alone += l[size_t(s)] && l[0] + l[1] + l[2] < 2;
                if (alone >= 10) wanted.push_back({c, s});
            }
        for (size_t k = 0; k < wanted.size(); ++k) {
            if (cancel && cancel->load()) return false;
            const auto [c, s] = wanted[k];
            std::vector<float> speech;
            const auto& w = windows[c];
            for (int f = 0; f < frames;) {
                const auto on = [&](int g) { return w[size_t(g)][size_t(s)] && w[size_t(g)][0] + w[size_t(g)][1] + w[size_t(g)][2] < 2; };
                if (!on(f)) {
                    ++f;
                    continue;
                }
                int e = f;
                while (e < frames && on(e)) ++e;
                const int64_t a = starts[c] + int64_t(double(f) / frames * kWindow);
                const int64_t b = std::min(n, starts[c] + int64_t(double(std::min(e, frames - 1)) / frames * kWindow));
                if (b > a) speech.insert(speech.end(), audio.begin() + a, audio.begin() + b);
                f = e;
            }
            std::vector<float> v = embed(*models, speech);
            if (!v.empty() && std::none_of(v.begin(), v.end(), [](float x) { return std::isnan(x); }))
                voices.push_back({c, s, std::move(v)});
            if (progress) progress(0.3 + 0.65 * double(k + 1) / double(wanted.size()));
        }
        if (voices.empty()) return true;  // nobody speaks

        // 3. One label per voice across the recording.
        std::vector<std::vector<float>> embeddings;
        for (const Voice& v : voices) embeddings.push_back(v.embedding);
        const std::vector<int> cluster = windows.size() == 1 && options.speakers <= 0
                                             ? [&] {
                                                   // One window: its local speakers are the speakers.
                                                   std::vector<int> c;
                                                   for (const Voice& v : voices) c.push_back(v.local);
                                                   return c;
                                               }()
                                             : clusterSpeakers(embeddings, options.speakers, options.threshold);
        const int people = *std::max_element(cluster.begin(), cluster.end()) + 1;

        // 4. Back to frames: count each person's activity over the windows,
        // then keep the most active ones, as many as are speaking.
        std::vector<int> count(size_t(total) * size_t(people), 0);
        for (size_t k = 0; k < voices.size(); ++k) {
            const Voice& v = voices[k];
            for (int f = 0; f < frames; ++f)
                if (windows[v.window][size_t(f)][size_t(v.local)]) ++count[size_t(frameOf(v.window) + f) * size_t(people) + size_t(cluster[k])];
        }
        const int64_t last = std::min<int64_t>(total, n / kFieldShift + 1);
        std::vector<std::vector<bool>> on(size_t(people), std::vector<bool>(size_t(last), false));
        std::vector<int> order(static_cast<size_t>(people));
        for (int64_t f = 0; f < last; ++f) {
            const int k = std::min(speaking[size_t(f)], people);
            if (k <= 0) continue;
            std::iota(order.begin(), order.end(), 0);
            const int* row = &count[size_t(f) * size_t(people)];
            std::stable_sort(order.begin(), order.end(), [row](int a, int b) { return row[a] > row[b]; });
            for (int j = 0; j < k; ++j)
                if (row[order[size_t(j)]] > 0) on[size_t(order[size_t(j)])][size_t(f)] = true;
        }
        // 5. Turns: runs of frames, gaps under minPause joined, blips under minSpeech dropped.
        const double scale = double(kFieldShift) / kRate, offset = 0.5 * kFieldSize / kRate;
        for (int p = 0; p < people; ++p) {
            std::vector<SpeakerTurn> mine;
            for (int64_t f = 0; f < last;) {
                if (!on[size_t(p)][size_t(f)]) {
                    ++f;
                    continue;
                }
                int64_t e = f;
                while (e < last && on[size_t(p)][size_t(e)]) ++e;
                SpeakerTurn t{f * scale + offset, (e < last ? e : last - 1) * scale + offset, p};
                if (!mine.empty() && t.start - mine.back().end < options.minPause) mine.back().end = t.end;
                else mine.push_back(t);
                f = e;
            }
            for (const SpeakerTurn& t : mine)
                if (t.end - t.start > options.minSpeech) turns.push_back(t);
        }
        std::sort(turns.begin(), turns.end(), [](const SpeakerTurn& a, const SpeakerTurn& b) {
            return a.start < b.start || (a.start == b.start && a.speaker < b.speaker);
        });
        // Number people in the order they first speak.
        std::map<int, int> first;
        for (const SpeakerTurn& t : turns)
            if (!first.count(t.speaker)) first.emplace(t.speaker, int(first.size()));
        for (SpeakerTurn& t : turns) t.speaker = first[t.speaker];
        if (progress) progress(1.0);
        return true;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("Speaker detection failed: ") + e.what();
        return false;
    }
}

#endif

}  // namespace montage
