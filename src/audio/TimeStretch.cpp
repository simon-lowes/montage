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

// Mono (an ambisonic field's W, its omnidirectional part), and the same averaged over four samples, for finding where
// grains line up.
struct Guide {
    std::vector<float> mono, coarse;
    explicit Guide(const AudioBuffer& in) {
        const int64_t n = in.frames();
        const size_t nch = size_t(std::max(1, in.channels));
        mono.resize(size_t(n));
        for (int64_t i = 0; i < n; ++i)
            mono[size_t(i)] = nch == 2 ? 0.5f * (in.samples[size_t(i) * 2] + in.samples[size_t(i) * 2 + 1]) : in.samples[size_t(i) * nch];
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

void wsolaStretch(const AudioBuffer& in, const std::vector<double>& positions, int hop, int64_t outFrames, AudioBuffer& out,
                  const std::atomic<bool>* cancel) {
    out.sampleRate = in.sampleRate;
    out.channels = std::max(1, in.channels);
    const size_t nch = size_t(out.channels);  // every channel cut at the same places, so a field keeps its directions
    out.samples.assign(size_t(std::max<int64_t>(0, outFrames)) * nch, 0.0f);
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
        if (cancel && (k & 255) == 0 && cancel->load()) break;
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
            for (size_t c = 0; c < nch; ++c) out.samples[size_t(o) * nch + c] += w * src[size_t(s) * nch + c];
        }
    }
    for (int64_t o = 0; o < outFrames; ++o)
        if (weight[size_t(o)] > 1e-6f)
            for (size_t c = 0; c < nch; ++c) out.samples[size_t(o) * nch + c] /= weight[size_t(o)];
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

SpeedStream::SpeedStream(int channels, int sampleRate, double speed, bool keepPitch, Source source)
    : nch_(std::max(1, channels)), speed_(std::clamp(speed, 0.25, 4.0)), keepPitch_(keepPitch), source_(std::move(source)) {
    if (keepPitch_) {
        hop_ = stretchHop(sampleRate);
        const int n = hop_ * 2;
        window_.resize(size_t(n));
        for (int i = 0; i < n; ++i) window_[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * double(i) / double(n)));
        accBase_ = -hop_;  // (the first grain is centred on sample 0)
    } else {
        // A windowed sinc (Blackman), its cut-off under the new Nyquist frequency when sped up, at kPhases + 1 fractions
        // of a sample, each normalised: tap t of phase p weighs the sample (p / kPhases) + t - taps before x.
        taps_ = 16;
        cutoff_ = std::min(1.0, 1.0 / speed_) * 0.97;
        const int span = taps_ * 2;
        kernel_.resize(size_t(kPhases + 1) * size_t(span));
        for (int p = 0; p <= kPhases; ++p) {
            double norm = 0;
            float* row = &kernel_[size_t(p) * size_t(span)];
            for (int t = 0; t < span; ++t) {
                const double d = double(p) / kPhases + t - taps_;
                const double arg = M_PI * d * cutoff_;
                const double sinc = std::fabs(d) < 1e-9 ? 1.0 : std::sin(arg) / arg;
                const double win = std::fabs(d) >= taps_ ? 0.0 : 0.42 + 0.5 * std::cos(M_PI * d / taps_) + 0.08 * std::cos(2 * M_PI * d / taps_);
                row[t] = float(sinc * win);
                norm += row[t];
            }
            if (norm > 1e-9)
                for (int t = 0; t < span; ++t) row[t] = float(row[t] / norm);
        }
    }
}

void SpeedStream::need(int64_t until) {
    const int64_t have = srcBase_ + int64_t(mono_.size());
    if (until <= have) return;
    const int frames = int(std::max<int64_t>(until - have, 4096));
    const size_t old = src_.size();
    src_.resize(old + size_t(frames) * size_t(nch_));
    source_(frames, src_.data() + old);
    mono_.reserve(mono_.size() + size_t(frames));
    for (int i = 0; i < frames; ++i) {
        const float* p = &src_[old + size_t(i) * size_t(nch_)];
        mono_.push_back(nch_ == 2 ? 0.5f * (p[0] + p[1]) : p[0]);
    }
}

float SpeedStream::sample(int64_t at, int c) const {
    const int64_t i = at - srcBase_;
    if (at < 0 || i < 0 || i >= int64_t(mono_.size())) return 0.0f;
    return src_[size_t(i) * size_t(nch_) + size_t(c)];
}

void SpeedStream::grain() {
    const int64_t N = int64_t(hop_) * 2, half = hop_, tolerance = hop_ / 2;
    const int64_t k = grains_++;
    const int64_t nominal = int64_t(std::llround(double(k) * double(hop_) * speed_)) - half;
    int64_t start = nominal;
    need(nominal + tolerance + N + 8);
    auto mono = [&](int64_t at) {
        const int64_t i = at - srcBase_;
        return at < 0 || i < 0 || i >= int64_t(mono_.size()) ? 0.0f : mono_[size_t(i)];
    };
    if (havePrev_) {
        const int64_t natural = prev_ + hop_;
        if (std::llabs(natural - nominal) <= tolerance) {
            start = natural;
        } else {
            // Where the last grain would carry on: the grain here that matches it best, coarsely then to the sample.
            auto coarse = [&](int64_t at) { return 0.25f * (mono(at) + mono(at + 1) + mono(at + 2) + mono(at + 3)); };
            double best = -1e300;
            for (int64_t d = -tolerance; d <= tolerance; d += 4) {
                double c = 0;
                for (int64_t i = 0; i < N; i += 4) c += double(coarse(natural + i)) * double(coarse(nominal + d + i));
                if (c > best) best = c, start = nominal + d;
            }
            const int64_t around = start;
            best = -1e300;
            for (int64_t d = -3; d <= 3; ++d) {
                double c = 0;
                for (int64_t i = 0; i < N; i += 2) c += double(mono(natural + i)) * double(mono(around + d + i));
                if (c > best) best = c, start = around + d;
            }
        }
    }
    prev_ = start;
    havePrev_ = true;
    // Overlapped in at k * hop - half.
    const int64_t o0 = k * hop_ - half;
    const int64_t need = o0 + N - accBase_;
    if (need > int64_t(weight_.size())) {
        acc_.resize(size_t(need) * size_t(nch_), 0.0f);
        weight_.resize(size_t(need), 0.0f);
    }
    for (int64_t i = 0; i < N; ++i) {
        const int64_t o = o0 + i - accBase_;
        if (o < 0) continue;
        const float w = window_[size_t(i)];
        weight_[size_t(o)] += w;
        for (int c = 0; c < nch_; ++c) acc_[size_t(o) * size_t(nch_) + size_t(c)] += w * sample(start + i, c);
    }
    // What no later grain reaches is done; the source before where the next grain could start is not needed again.
    const int64_t keepFrom = std::min(prev_, nominal) - tolerance - 16;
    if (keepFrom - srcBase_ > 65536) {
        const int64_t drop = keepFrom - srcBase_;
        src_.erase(src_.begin(), src_.begin() + long(drop * nch_));
        mono_.erase(mono_.begin(), mono_.begin() + long(drop));
        srcBase_ += drop;
    }
}

void SpeedStream::read(int frames, float* out) {
    if (frames <= 0) return;
    if (!keepPitch_) {
        // Varispeed: output sample j is the source at j * speed, through the filter (taken at the nearest of its phases).
        const int span = taps_ * 2;
        for (int j = 0; j < frames; ++j) {
            const double x = double(outDone_ + j) * speed_;
            const int64_t base = int64_t(std::floor(x));
            need(base + taps_ + 2);
            const int phase = std::clamp(int(std::lround((x - double(base)) * kPhases)), 0, kPhases);
            const float* w = &kernel_[size_t(phase) * size_t(span)];
            float* o = out + size_t(j) * size_t(nch_);
            for (int c = 0; c < nch_; ++c) o[c] = 0.0f;
            const int64_t first = base + taps_;  // tap t reads sample first - t
            if (first - (span - 1) >= srcBase_ && first - srcBase_ < int64_t(mono_.size())) {
                const float* q = &src_[size_t(first - srcBase_) * size_t(nch_)];
                for (int t = 0; t < span; ++t, q -= nch_)
                    for (int c = 0; c < nch_; ++c) o[c] += w[t] * q[c];
            } else {
                for (int t = 0; t < span; ++t)
                    for (int c = 0; c < nch_; ++c) o[c] += w[t] * sample(first - t, c);
            }
        }
        outDone_ += frames;
        const int64_t keepFrom = int64_t(std::floor(double(outDone_) * speed_)) - taps_ - 4;
        if (keepFrom - srcBase_ > 65536) {
            const int64_t drop = keepFrom - srcBase_;
            src_.erase(src_.begin(), src_.begin() + long(drop * nch_));
            mono_.erase(mono_.begin(), mono_.begin() + long(drop));
            srcBase_ += drop;
        }
        return;
    }
    // WSOLA: grains until the frames asked for are finished (no later grain reaches them).
    const int64_t until = outDone_ + frames;
    while (grains_ * hop_ - hop_ < until) grain();  // (grain k reaches from k * hop - hop)
    for (int j = 0; j < frames; ++j) {
        const int64_t o = outDone_ + j - accBase_;
        const float w = o >= 0 && o < int64_t(weight_.size()) ? weight_[size_t(o)] : 0.0f;
        for (int c = 0; c < nch_; ++c) {
            const float v = o >= 0 && o < int64_t(weight_.size()) ? acc_[size_t(o) * size_t(nch_) + size_t(c)] : 0.0f;
            out[size_t(j) * size_t(nch_) + size_t(c)] = w > 1e-6f ? v / w : 0.0f;
        }
    }
    outDone_ = until;
    const int64_t drop = std::min<int64_t>(outDone_ - accBase_, int64_t(weight_.size()));
    if (drop > 0) {
        acc_.erase(acc_.begin(), acc_.begin() + long(drop * nch_));
        weight_.erase(weight_.begin(), weight_.begin() + long(drop));
        accBase_ += drop;
    }
}

}  // namespace montage
