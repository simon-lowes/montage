#include "SpeechCleanup.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/tx.h>
}

#ifdef MONTAGE_WITH_RNNOISE
#include <rnnoise.h>
#endif

#include "AudioRepair.h"
#include "media/SpeechEnhance.h"

namespace montage {

bool isSourceAudioEffect(const std::string& type) {
    return type == "denoise" || type == "voice_isolate" || type == "enhance_speech" || type == "declick" || type == "pitch_shift" ||
           type == "dereverb";
}

bool hasVoiceIsolation() {
#ifdef MONTAGE_WITH_RNNOISE
    return true;
#else
    return false;
#endif
}

// ---- Spectral noise reduction -------------------------------------------------

namespace {

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

void reduceNoise(const AudioBuffer& in, AudioBuffer& out, double reductionDb, double sensitivity,
                 const std::atomic<bool>* cancel) {
    out.sampleRate = in.sampleRate;
    out.samples.assign(in.samples.size(), 0.0f);
    const int64_t frames = in.frames();
    // About 43 ms windows, 75 % overlap.
    int N = 2048;
    while (N > 256 && double(N) / std::max(8000, in.sampleRate) > 0.06) N /= 2;
    while (double(N) / std::max(8000, in.sampleRate) < 0.03) N *= 2;
    const int hop = N / 4, bins = N / 2 + 1;
    if (frames < N) {
        out.samples = in.samples;
        return;
    }
    Tx fwd(N, false), inv(N, true);
    if (!fwd.ctx || !inv.ctx) {
        out.samples = in.samples;
        return;
    }
    std::vector<float> window(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) window[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / N));
    const int64_t nFrames = (frames + N) / hop + 1;
    const float floorGain = float(std::pow(10.0, -std::clamp(reductionDb, 0.0, 80.0) / 20.0));
    const double beta = 1.0 + 3.0 * std::clamp(sensitivity, 0.0, 100.0) / 100.0;  // over-subtraction
    AlignedBuf<float> frame(static_cast<size_t>(N)), timeOut(static_cast<size_t>(N));
    AlignedBuf<AVComplexFloat> spec(static_cast<size_t>(bins) + 1);
    std::vector<float> norm(size_t(frames) + size_t(N), 0.0f);
    for (int64_t f = 0; f < nFrames; ++f)
        for (int i = 0; i < N; ++i) {
            const int64_t s = f * hop - N + i;  // frames start before 0 so the edges are covered
            if (s >= 0 && s < frames) norm[size_t(s)] += window[size_t(i)] * window[size_t(i)];
        }

    for (int ch = 0; ch < 2; ++ch) {
        auto sampleAt = [&](int64_t s) { return s >= 0 && s < frames ? in.samples[size_t(s) * 2 + size_t(ch)] : 0.0f; };
        // Pass 1: the power spectrum of every frame.
        std::vector<float> power(size_t(nFrames) * size_t(bins));
        std::vector<double> energy(static_cast<size_t>(nFrames));
        for (int64_t f = 0; f < nFrames; ++f) {
            if (cancel && cancel->load()) return;
            for (int i = 0; i < N; ++i) frame[size_t(i)] = sampleAt(f * hop - N + i) * window[size_t(i)];
            fwd.fn(fwd.ctx, spec.data(), frame.data(), sizeof(float));
            double e = 0;
            for (int k = 0; k < bins; ++k) {
                const float p = spec[size_t(k)].re * spec[size_t(k)].re + spec[size_t(k)].im * spec[size_t(k)].im;
                power[size_t(f) * size_t(bins) + size_t(k)] = p;
                e += p;
            }
            energy[size_t(f)] = e;
        }
        // The noise print: the mean spectrum of the quietest 10 % of frames
        // that are not digital silence.
        std::vector<int64_t> order;
        for (int64_t f = 0; f < nFrames; ++f)
            if (energy[size_t(f)] > 1e-12) order.push_back(f);
        if (order.empty()) {
            for (int64_t s = 0; s < frames; ++s) out.samples[size_t(s) * 2 + size_t(ch)] = in.samples[size_t(s) * 2 + size_t(ch)];
            continue;
        }
        const size_t quiet = std::max<size_t>(std::min<size_t>(5, order.size()), order.size() / 10);
        std::nth_element(order.begin(), order.begin() + long(quiet) - 1, order.end(),
                         [&](int64_t a, int64_t b) { return energy[size_t(a)] < energy[size_t(b)]; });
        std::vector<double> noise(static_cast<size_t>(bins), 0.0);
        for (size_t q = 0; q < quiet; ++q)
            for (int k = 0; k < bins; ++k) noise[size_t(k)] += power[size_t(order[q]) * size_t(bins) + size_t(k)];
        for (auto& v : noise) v /= double(quiet);

        // Pass 2: gains (smoothed across frequency and, falling, across time), resynthesis.
        std::vector<float> gain(static_cast<size_t>(bins), 1.0f), prev(static_cast<size_t>(bins), 1.0f), smooth(static_cast<size_t>(bins));
        std::vector<float> acc(size_t(frames) + size_t(N), 0.0f);
        for (int64_t f = 0; f < nFrames; ++f) {
            if (cancel && cancel->load()) return;
            const float* p = &power[size_t(f) * size_t(bins)];
            for (int k = 0; k < bins; ++k) {
                const double sub = 1.0 - beta * noise[size_t(k)] / std::max(1e-20, double(p[k]));
                gain[size_t(k)] = std::max(floorGain, float(std::sqrt(std::max(0.0, sub))));
            }
            for (int k = 0; k < bins; ++k) {
                float sum = 0;
                int n = 0;
                for (int d = -2; d <= 2; ++d)
                    if (k + d >= 0 && k + d < bins) sum += gain[size_t(k + d)], ++n;
                // Gains fall gradually (no "musical noise" from bins flickering on and off).
                smooth[size_t(k)] = std::max(sum / float(n), prev[size_t(k)] * 0.6f);
                smooth[size_t(k)] = std::max(smooth[size_t(k)], floorGain);
            }
            prev = smooth;
            for (int i = 0; i < N; ++i) frame[size_t(i)] = sampleAt(f * hop - N + i) * window[size_t(i)];
            fwd.fn(fwd.ctx, spec.data(), frame.data(), sizeof(float));
            for (int k = 0; k < bins; ++k) {
                spec[size_t(k)].re *= smooth[size_t(k)];
                spec[size_t(k)].im *= smooth[size_t(k)];
            }
            inv.fn(inv.ctx, timeOut.data(), spec.data(), sizeof(AVComplexFloat));
            for (int i = 0; i < N; ++i) {
                const int64_t s = f * hop - N + i;
                if (s >= 0 && s < frames) acc[size_t(s)] += timeOut[size_t(i)] * window[size_t(i)];
            }
        }
        for (int64_t s = 0; s < frames; ++s)
            out.samples[size_t(s) * 2 + size_t(ch)] = norm[size_t(s)] > 1e-6f ? acc[size_t(s)] / norm[size_t(s)] : 0.0f;
    }
}

// ---- De-reverb ------------------------------------------------------------------

double estimateReverbTime(const AudioBuffer& in) {
    // The level in 10 ms steps (500 Hz to 4 kHz, smoothed over 50 ms). After each loud moment that stops, the
    // level falls at the room's rate once the direct sound has gone; the time to fall from 5 to 25 dB below the
    // peak, times three, is that fall's RT60 (as T20 is measured on an impulse response). Sound that fades by itself
    // falls more slowly than the room allows, so the faster falls measure the room.
    const int sr = std::max(8000, in.sampleRate);
    const int64_t frames = in.frames();
    const int step = sr / 100;
    if (frames < step * 100) return 0;
    const double ah = std::exp(-2 * M_PI * 500.0 / sr), al = std::exp(-2 * M_PI * std::min(4000.0, 0.45 * sr) / sr);
    double hpPrevIn = 0, hp = 0, lp = 0, acc = 0;
    std::vector<double> level;
    for (int64_t s = 0; s < frames; ++s) {
        const double x = 0.5 * (double(in.samples[size_t(s) * 2]) + in.samples[size_t(s) * 2 + 1]);
        hp = ah * (hp + x - hpPrevIn);
        hpPrevIn = x;
        lp = (1 - al) * hp + al * lp;
        acc += lp * lp;
        if ((s + 1) % step == 0) {
            level.push_back(10 * std::log10(acc / step + 1e-20));
            acc = 0;
        }
    }
    const size_t n = level.size();
    std::vector<double> sm(n);
    for (size_t i = 0; i < n; ++i) {
        double sum = 0;
        int c = 0;
        for (int d = -2; d <= 2; ++d)
            if (i + size_t(d) < n) sum += level[i + size_t(d)], ++c;
        sm[i] = sum / c;
    }
    const double loudest = *std::max_element(sm.begin(), sm.end());
    std::vector<double> times;
    for (size_t p = 5; p + 5 < n; ++p) {
        if (sm[p] < loudest - 30) continue;
        bool peak = true;
        for (size_t k = 1; k <= 5 && peak; ++k) peak = sm[p] >= sm[p - k] && sm[p] >= sm[p + k];
        if (!peak) continue;
        // Follow the fall until the sound starts again (3 dB above the lowest so far) or two seconds pass.
        double lowest = sm[p];
        size_t t5 = 0, t25 = 0;
        for (size_t j = p + 1; j < n && j < p + 200; ++j) {
            if (sm[j] > lowest + 3) break;
            lowest = std::min(lowest, sm[j]);
            if (!t5 && sm[j] <= sm[p] - 5) t5 = j;
            if (!t25 && sm[j] <= sm[p] - 25) {
                t25 = j;
                break;
            }
        }
        if (t5 && t25 && t25 > t5 + 2) times.push_back(3.0 * double(t25 - t5) * 0.01);
    }
    if (times.size() < 3) return 0;
    std::sort(times.begin(), times.end());
    return std::clamp(times[size_t(double(times.size() - 1) * 0.25)], 0.1, 5.0);
}

void dereverb(const AudioBuffer& in, AudioBuffer& out, double amount, double reverbTime, double maxReductionDb,
              const std::atomic<bool>* cancel) {
    out.sampleRate = in.sampleRate;
    out.samples.assign(in.samples.size(), 0.0f);
    const int64_t frames = in.frames();
    const int sr = std::max(8000, in.sampleRate);
    int N = 2048;
    while (N > 256 && double(N) / sr > 0.04) N /= 2;
    while (double(N) / sr < 0.02) N *= 2;
    const int hop = N / 4, bins = N / 2 + 1;
    amount = std::clamp(amount, 0.0, 100.0) / 100.0;
    if (reverbTime <= 0) reverbTime = estimateReverbTime(in);
    if (reverbTime <= 0) reverbTime = 0.5;
    Tx fwd(N, false), inv(N, true);
    if (frames < N || amount <= 0 || !fwd.ctx || !inv.ctx) {
        out.samples = in.samples;
        return;
    }
    std::vector<float> window(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) window[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / N));
    const int64_t nFrames = (frames + N) / hop + 1;
    // Late reverberation starts about 50 ms after the direct sound; by then the room has decayed by this much.
    const double frameSec = double(hop) / sr;
    const int delayFrames = std::max(1, int(std::lround(0.05 / frameSec)));
    const double decay = std::exp(-2 * 3 * std::log(10.0) / reverbTime * delayFrames * frameSec);
    const double minGain = std::pow(10.0, -std::clamp(maxReductionDb, 0.0, 60.0) / 20.0);
    AlignedBuf<float> frame(static_cast<size_t>(N)), timeOut(static_cast<size_t>(N));
    AlignedBuf<AVComplexFloat> spec(static_cast<size_t>(bins) + 1);
    std::vector<float> norm(size_t(frames) + size_t(N), 0.0f);
    for (int64_t f = 0; f < nFrames; ++f)
        for (int i = 0; i < N; ++i) {
            const int64_t s = f * hop - N + i;
            if (s >= 0 && s < frames) norm[size_t(s)] += window[size_t(i)] * window[size_t(i)];
        }
    for (int ch = 0; ch < 2; ++ch) {
        auto sampleAt = [&](int64_t s) { return s >= 0 && s < frames ? in.samples[size_t(s) * 2 + size_t(ch)] : 0.0f; };
        // The power spectrum of every frame, smoothed over about 50 ms, kept for the delay.
        std::vector<float> smooth(size_t(nFrames) * size_t(bins), 0.0f);
        std::vector<float> acc(size_t(frames) + size_t(N), 0.0f), prevGain(size_t(bins), 1.0f);
        for (int64_t f = 0; f < nFrames; ++f) {
            if (cancel && cancel->load()) return;
            for (int i = 0; i < N; ++i) frame[size_t(i)] = sampleAt(f * hop - N + i) * window[size_t(i)];
            fwd.fn(fwd.ctx, spec.data(), frame.data(), sizeof(float));
            float* sp = &smooth[size_t(f) * size_t(bins)];
            const float* before = f > 0 ? &smooth[size_t(f - 1) * size_t(bins)] : nullptr;
            const float* late = f >= delayFrames ? &smooth[size_t(f - delayFrames) * size_t(bins)] : nullptr;
            for (int k = 0; k < bins; ++k) {
                const double p = double(spec[size_t(k)].re) * spec[size_t(k)].re + double(spec[size_t(k)].im) * spec[size_t(k)].im;
                sp[k] = float(before ? 0.85 * before[k] + 0.15 * p : p);
                // Over-subtracting a little (1.3) takes more of the tail for little cost to the voice.
                double g = 1;
                if (late) g = std::sqrt(std::max(minGain * minGain, 1.0 - 1.3 * decay * late[k] / std::max(1e-20, p)));
                // Gains recover gradually, so the tail does not flicker ("musical noise").
                g = std::max(g, double(prevGain[size_t(k)]) * 0.5);
                prevGain[size_t(k)] = float(g);
                const float gg = float(1.0 - amount * (1.0 - g));
                spec[size_t(k)].re *= gg;
                spec[size_t(k)].im *= gg;
            }
            inv.fn(inv.ctx, timeOut.data(), spec.data(), sizeof(AVComplexFloat));
            for (int i = 0; i < N; ++i) {
                const int64_t s = f * hop - N + i;
                if (s >= 0 && s < frames) acc[size_t(s)] += timeOut[size_t(i)] * window[size_t(i)];
            }
        }
        for (int64_t s = 0; s < frames; ++s)
            out.samples[size_t(s) * 2 + size_t(ch)] = norm[size_t(s)] > 1e-6f ? acc[size_t(s)] / norm[size_t(s)] : 0.0f;
    }
}

// ---- Voice isolation (RNNoise) -------------------------------------------------

namespace {
std::vector<float> resample(const std::vector<float>& in, double from, double to) {
    if (from == to || in.empty()) return in;
    const size_t n = size_t(std::llround(double(in.size()) * to / from));
    std::vector<float> out(n);
    const double step = from / to;
    for (size_t i = 0; i < n; ++i) {
        const double pos = double(i) * step;
        const size_t j = std::min(size_t(pos), in.size() - 1);
        const size_t j1 = std::min(j + 1, in.size() - 1);
        const float f = float(pos - double(j));
        out[i] = in[j] + (in[j1] - in[j]) * f;
    }
    return out;
}
}  // namespace

#ifdef MONTAGE_WITH_RNNOISE
namespace {
// RNNoise's output lags its input by this many samples at 48 kHz:
constexpr int kRnnoiseDelay = 960;  // two 10 ms frames (measured by the voice isolation test)
}  // namespace
#endif

bool isolateVoice(const AudioBuffer& in, AudioBuffer& out, double amount, const std::atomic<bool>* cancel) {
#ifdef MONTAGE_WITH_RNNOISE
    out.sampleRate = in.sampleRate;
    out.samples.assign(in.samples.size(), 0.0f);
    const int64_t frames = in.frames();
    const float mix = float(std::clamp(amount, 0.0, 100.0) / 100.0);
    const int fs = rnnoise_get_frame_size();
    for (int ch = 0; ch < 2; ++ch) {
        std::vector<float> mono(static_cast<size_t>(frames));
        for (int64_t s = 0; s < frames; ++s) mono[size_t(s)] = in.samples[size_t(s) * 2 + size_t(ch)] * 32768.0f;
        std::vector<float> x = resample(mono, in.sampleRate, 48000);
        const size_t n = x.size();
        x.resize(n + size_t(kRnnoiseDelay) + size_t(fs), 0.0f);  // flush the delay
        std::vector<float> y(x.size(), 0.0f);
        DenoiseState* st = rnnoise_create(nullptr);
        for (size_t i = 0; i + size_t(fs) <= x.size(); i += size_t(fs)) {
            if (cancel && cancel->load()) {
                rnnoise_destroy(st);
                return false;
            }
            rnnoise_process_frame(st, &y[i], &x[i]);
        }
        rnnoise_destroy(st);
        y.erase(y.begin(), y.begin() + std::min<size_t>(kRnnoiseDelay, y.size()));
        y.resize(n);
        std::vector<float> back = resample(y, 48000, in.sampleRate);
        back.resize(size_t(frames), 0.0f);
        for (int64_t s = 0; s < frames; ++s) {
            const float dry = in.samples[size_t(s) * 2 + size_t(ch)];
            out.samples[size_t(s) * 2 + size_t(ch)] = dry + (back[size_t(s)] / 32768.0f - dry) * mix;
        }
    }
    return true;
#else
    (void)in;
    (void)out;
    (void)amount;
    (void)cancel;
    return false;
#endif
}

bool enhanceSpeech(const AudioBuffer& in, AudioBuffer& out, double amount, double maxReductionDb, bool background,
                   const std::atomic<bool>* cancel, std::string* error) {
    if (!speechEnhancerAvailable()) {
        if (error) *error = "This build of Montage cannot enhance speech (it was built without ONNX Runtime)";
        return false;
    }
    const int64_t frames = in.frames();
    const float mix = float(std::clamp(amount, 0.0, 100.0) / 100.0);
    // Never further down than the limit: the original, that much quieter, stays under the result.
    const float floor = maxReductionDb >= 100 ? 0.0f : float(std::pow(10.0, -std::max(0.0, maxReductionDb) / 20.0));
    std::vector<float> ch[2];
    for (int c = 0; c < 2; ++c) {
        ch[c].resize(size_t(frames));
        for (int64_t s = 0; s < frames; ++s) ch[c][size_t(s)] = in.samples[size_t(s) * 2 + size_t(c)];
    }
    // A mono recording (both sides the same) is enhanced once.
    const bool mono = ch[0] == ch[1];
    std::vector<float> wet[2];
    bool ok[2] = {true, true};
    std::string err[2];
    auto run = [&](int c) {
        std::vector<float> y;
        ok[c] = enhanceSpeech48k(resample(ch[c], in.sampleRate, 48000), y, cancel, &err[c]);
        if (!ok[c]) return;
        wet[c] = resample(y, 48000, in.sampleRate);
        wet[c].resize(size_t(frames), 0.0f);
    };
    if (mono) {
        run(0);
    } else {
        std::thread other(run, 1);
        run(0);
        other.join();
    }
    if (!ok[0] || !ok[1]) {
        if (error) *error = !ok[0] ? err[0] : err[1];
        return false;
    }
    out.sampleRate = in.sampleRate;
    out.samples.assign(in.samples.size(), 0.0f);
    for (int c = 0; c < 2; ++c) {
        const std::vector<float>& w = wet[mono ? 0 : c];
        for (int64_t s = 0; s < frames; ++s) {
            const float dry = ch[c][size_t(s)], speech = w[size_t(s)];
            // What is kept, with what is taken out let through at the floor.
            const float kept = background ? dry - speech * (1 - floor) : speech * (1 - floor) + dry * floor;
            out.samples[size_t(s) * 2 + size_t(c)] = dry + (kept - dry) * mix;
        }
    }
    return true;
}

// ---- Cache --------------------------------------------------------------------

namespace {

struct Job {
    AudioBufferPtr result;
    bool done = false;
};

std::mutex gCacheMutex;
std::condition_variable gCacheDone;
std::list<std::pair<std::string, std::shared_ptr<Job>>> gCache;  // most recently used first
std::atomic<int> gRunning{0};
constexpr size_t kMaxCached = 12;

std::string keyFor(const std::string& path, int rate, const std::vector<const Effect*>& effects) {
    std::string key = path + "|" + std::to_string(rate);
    char buf[64];
    for (const Effect* e : effects) {
        key += "|" + e->type;
        for (const auto& [name, param] : e->params) {
            std::snprintf(buf, sizeof buf, ":%s=%.4g", name.c_str(), param.at(0));
            key += buf;
        }
    }
    return key;
}

AudioBufferPtr process(const AudioBufferPtr& source, const std::vector<Effect>& effects) {
    AudioBufferPtr cur = source;
    for (const Effect& e : effects) {
        auto out = std::make_shared<AudioBuffer>();
        bool ok = false;
        if (e.type == "denoise") {
            reduceNoise(*cur, *out, e.p("reduction_db", 0, 15), e.p("sensitivity", 0, 50));
            ok = true;
        } else if (e.type == "voice_isolate") {
            ok = isolateVoice(*cur, *out, e.p("amount", 0, 100));
        } else if (e.type == "enhance_speech") {
            ok = enhanceSpeech(*cur, *out, e.p("amount", 0, 100), e.p("max_reduction_db", 0, 100), e.p("keep", 0) > 0.5);
        } else if (e.type == "declick") {
            declick(*cur, *out, e.p("sensitivity", 0, 50), e.p("max_ms", 0, 2));
            ok = true;
        } else if (e.type == "dereverb") {
            dereverb(*cur, *out, e.p("amount", 0, 80), e.p("reverb_time", 0, 0), e.p("max_reduction_db", 0, 18));
            ok = true;
        } else if (e.type == "pitch_shift") {
            pitchShift(*cur, *out, e.p("semitones", 0, 0) + e.p("cents", 0, 0) / 100);
            ok = true;
        }
        if (ok) cur = out;
    }
    return cur;
}

}  // namespace

int cleanupsRunning() { return gRunning.load(); }

AudioBufferPtr cleanedAudio(const std::string& path, const AudioBufferPtr& source, const std::vector<const Effect*>& effects,
                            bool blocking) {
    if (!source || effects.empty()) return source;
    const std::string key = keyFor(path, source->sampleRate, effects);
    std::shared_ptr<Job> job;
    bool start = false;
    {
        std::unique_lock lock(gCacheMutex);
        auto it = std::find_if(gCache.begin(), gCache.end(), [&](const auto& kv) { return kv.first == key; });
        if (it != gCache.end()) {
            job = it->second;
            gCache.splice(gCache.begin(), gCache, it);
        } else {
            job = std::make_shared<Job>();
            gCache.emplace_front(key, job);
            while (gCache.size() > kMaxCached) gCache.pop_back();
            start = true;
        }
        if (!start) {
            if (job->done) return job->result;
            if (!blocking) return nullptr;
            gCacheDone.wait(lock, [&] { return job->done; });
            return job->result;
        }
    }
    std::vector<Effect> copies;
    for (const Effect* e : effects) copies.push_back(*e);
    auto run = [job, source, copies] {
        AudioBufferPtr r = process(source, copies);
        {
            std::lock_guard lock(gCacheMutex);
            job->result = r;
            job->done = true;
        }
        gCacheDone.notify_all();
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
