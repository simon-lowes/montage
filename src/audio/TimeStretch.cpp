#include "TimeStretch.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

namespace montage {

namespace {

// Mono, and the same averaged over four samples, for finding where grains line up.
struct Guide {
    std::vector<float> mono, coarse;
    explicit Guide(const AudioBuffer& in) {
        const int64_t n = in.frames();
        mono.resize(size_t(n));
        for (int64_t i = 0; i < n; ++i) mono[size_t(i)] = 0.5f * (in.samples[size_t(i) * 2] + in.samples[size_t(i) * 2 + 1]);
        coarse.resize(size_t(n / 4));
        for (size_t i = 0; i < coarse.size(); ++i) coarse[i] = 0.25f * (mono[i * 4] + mono[i * 4 + 1] + mono[i * 4 + 2] + mono[i * 4 + 3]);
    }
};

// Sum of a[i] * b[i] for i in [0, len) by `stride`, both read as zero outside their signal.
double dot(const std::vector<float>& s, int64_t a, int64_t b, int64_t len, int64_t stride) {
    const int64_t n = int64_t(s.size());
    double sum = 0;
    for (int64_t i = 0; i < len; i += stride) {
        const int64_t x = a + i, y = b + i;
        if (x < 0 || y < 0 || x >= n || y >= n) continue;
        sum += double(s[size_t(x)]) * double(s[size_t(y)]);
    }
    return sum;
}

struct Job {
    bool done = false;
    AudioBufferPtr result;
};
std::mutex gMutex;
std::condition_variable gDone;
std::list<std::pair<std::string, std::shared_ptr<Job>>> gCache;  // most recent first
constexpr size_t kMaxCached = 6;
std::atomic<int> gRunning{0};

}  // namespace

int stretchHop(int sampleRate) { return std::max(64, int(std::lround(sampleRate * 0.012))); }

void wsolaStretch(const AudioBuffer& in, const std::vector<double>& positions, int hop, int64_t outFrames, AudioBuffer& out) {
    out.sampleRate = in.sampleRate;
    out.samples.assign(size_t(std::max<int64_t>(0, outFrames)) * 2, 0.0f);
    if (outFrames <= 0 || positions.empty() || in.frames() == 0) return;
    const int64_t N = int64_t(hop) * 2, half = hop;  // grains twice the hop: Hann windows that add up to one
    const int64_t tolerance = hop / 2;               // how far a grain may move to line up
    const int64_t n = in.frames();
    std::vector<float> window(static_cast<size_t>(N));
    for (int64_t i = 0; i < N; ++i) window[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * double(i) / double(N)));
    std::vector<float> weight(size_t(outFrames), 0.0f);
    const Guide guide(in);
    const float* src = in.samples.data();
    int64_t prev = std::numeric_limits<int64_t>::min();  // where the last grain started in the source
    const int64_t grains = outFrames / hop + 2;
    for (int64_t k = 0; k < grains; ++k) {
        const double at = positions[size_t(std::min<int64_t>(k, int64_t(positions.size()) - 1))];
        const int64_t nominal = int64_t(std::llround(at)) - half;  // the grain centred on its source position
        int64_t start = nominal;
        if (prev != std::numeric_limits<int64_t>::min()) {
            // Where the last grain would have carried on: the grain here that matches it best.
            const int64_t natural = prev + hop;
            if (std::llabs(natural - nominal) <= tolerance) {
                start = natural;  // it already continues there (speed 1, or close)
            } else {
                double best = -1e300;
                for (int64_t d = -tolerance; d <= tolerance; d += 4) {
                    const double c = dot(guide.coarse, (natural) / 4, (nominal + d) / 4, N / 4, 1);
                    if (c > best) best = c, start = nominal + d;
                }
                const int64_t coarse = start;
                best = -1e300;
                for (int64_t d = -3; d <= 3; ++d) {
                    const double c = dot(guide.mono, natural, coarse + d, N, 2);
                    if (c > best) best = c, start = coarse + d;
                }
            }
        }
        prev = start;
        const int64_t o0 = k * hop - half;
        for (int64_t i = 0; i < N; ++i) {
            const int64_t o = o0 + i, s = start + i;
            if (o < 0 || o >= outFrames) continue;
            const float w = window[size_t(i)];
            weight[size_t(o)] += w;
            if (s < 0 || s >= n) continue;
            out.samples[size_t(o) * 2] += w * src[size_t(s) * 2];
            out.samples[size_t(o) * 2 + 1] += w * src[size_t(s) * 2 + 1];
        }
    }
    for (int64_t o = 0; o < outFrames; ++o)
        if (weight[size_t(o)] > 1e-6f) {
            out.samples[size_t(o) * 2] /= weight[size_t(o)];
            out.samples[size_t(o) * 2 + 1] /= weight[size_t(o)];
        }
}

int stretchesRunning() { return gRunning.load(); }

AudioBufferPtr stretchedAudio(const std::string& keyIn, const AudioBufferPtr& source, const std::function<std::vector<double>()>& makePositions,
                              int hop, int64_t outFrames, bool blocking) {
    if (!source) return nullptr;
    const std::string key = keyIn + "|" + std::to_string(hop) + "|" + std::to_string(outFrames);
    std::shared_ptr<Job> job;
    {
        std::unique_lock lock(gMutex);
        auto it = std::find_if(gCache.begin(), gCache.end(), [&](const auto& kv) { return kv.first == key; });
        if (it != gCache.end()) {
            job = it->second;
            gCache.splice(gCache.begin(), gCache, it);
            if (job->done) return job->result;
            if (!blocking) return nullptr;
            gDone.wait(lock, [&] { return job->done; });
            return job->result;
        }
        job = std::make_shared<Job>();
        gCache.emplace_front(key, job);
        while (gCache.size() > kMaxCached) gCache.pop_back();
    }
    auto run = [job, source, positions = makePositions(), hop, outFrames] {
        auto out = std::make_shared<AudioBuffer>();
        wsolaStretch(*source, positions, hop, outFrames, *out);
        {
            std::lock_guard lock(gMutex);
            job->result = out;
            job->done = true;
        }
        gDone.notify_all();
    };
    if (blocking) {
        run();
        return job->result;
    }
    ++gRunning;
    std::thread([run] {
        run();
        --gRunning;
    }).detach();
    return nullptr;
}

}  // namespace montage
