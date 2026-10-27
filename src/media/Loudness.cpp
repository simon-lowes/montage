#include "Loudness.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/tx.h>
}

namespace montage {

namespace {

// The most channels measured (a third-order ambisonic field, a 22.2 bed).
constexpr int kMeterChannels = 24;

// K-weighting: a high shelf (head effects) followed by a high-pass (RLB),
// with coefficients derived for any sample rate (as in libebur128).
struct KFilter {
    double b[2][3], a[2][3];
    double z[2][kMeterChannels][2] = {};  // [stage][channel][state]
    explicit KFilter(double fs) {
        double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        double K = std::tan(M_PI * f0 / fs), Vh = std::pow(10.0, G / 20.0), Vb = std::pow(Vh, 0.4996667741545416);
        double a0 = 1.0 + K / Q + K * K;
        b[0][0] = (Vh + Vb * K / Q + K * K) / a0;
        b[0][1] = 2.0 * (K * K - Vh) / a0;
        b[0][2] = (Vh - Vb * K / Q + K * K) / a0;
        a[0][1] = 2.0 * (K * K - 1.0) / a0;
        a[0][2] = (1.0 - K / Q + K * K) / a0;
        f0 = 38.13547087602444;
        Q = 0.5003270373238773;
        K = std::tan(M_PI * f0 / fs);
        a0 = 1.0 + K / Q + K * K;
        b[1][0] = 1.0;
        b[1][1] = -2.0;
        b[1][2] = 1.0;
        a[1][1] = 2.0 * (K * K - 1.0) / a0;
        a[1][2] = (1.0 - K / Q + K * K) / a0;
    }
    double process(int ch, double x) {
        for (int s = 0; s < 2; ++s) {
            double y = b[s][0] * x + z[s][ch][0];
            z[s][ch][0] = b[s][1] * x - a[s][1] * y + z[s][ch][1];
            z[s][ch][1] = b[s][2] * x - a[s][2] * y;
            x = y;
        }
        return x;
    }
};

// 4× oversampling for true peak (BS.1770-4 Annex 2): a 48-tap windowed-sinc
// interpolator, 12 taps per phase.
constexpr int kPhases = 4, kTaps = 12;
const std::array<std::array<double, kTaps>, kPhases>& truePeakFilter() {
    static const auto f = [] {
        std::array<std::array<double, kTaps>, kPhases> h{};
        for (int ph = 0; ph < kPhases; ++ph)
            for (int k = 0; k < kTaps; ++k) {
                // Tap k of phase ph sits at time (k - kTaps/2 + 1) - ph/4 samples from the output point.
                const double t = double(k - kTaps / 2 + 1) - double(ph) / kPhases;
                const double sinc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(M_PI * t) / (M_PI * t);
                const double w = 0.5 + 0.5 * std::cos(M_PI * t / (kTaps / 2.0));  // Hann over the span
                h[size_t(ph)][size_t(k)] = sinc * std::max(0.0, w);
            }
        return h;
    }();
    return f;
}

}  // namespace

struct LoudnessMeter::Impl {
    explicit Impl(int rate) : fs(std::max(1, rate)), k(fs), step(std::max<int64_t>(1, int64_t(fs * 0.1))) {}
    double fs;
    KFilter k;
    int64_t step;
    std::vector<double> steps;  // mean square per 100 ms
    std::vector<double> stepPeaks;  // true peak (linear) per 100 ms
    double stepPeak = 0;
    std::vector<char> speech;   // per step: marked as speech
    bool speechNow = false;
    double acc = 0;
    int64_t n = 0;
    double peak = 0;
    std::array<std::array<double, kTaps>, kMeterChannels> history{};  // recent samples per channel, for oversampling
    size_t hpos = 0;
    double maxM = -200, maxS = -200;
    // The mean square of the last `count` steps (fewer at the start).
    double window(size_t count) const {
        if (steps.empty()) return 0;
        const size_t from = steps.size() > count ? steps.size() - count : 0;
        double sum = 0;
        for (size_t i = from; i < steps.size(); ++i) sum += steps[i];
        return sum / double(steps.size() - from);
    }
};

namespace {
double toLufs(double ms) { return ms > 0 ? -0.691 + 10.0 * std::log10(ms) : -200.0; }
}  // namespace

LoudnessMeter::LoudnessMeter(int sampleRate) : d_(std::make_unique<Impl>(sampleRate)) {}
LoudnessMeter::~LoudnessMeter() = default;

void LoudnessMeter::add(const float* stereo, int64_t frames) {
    static const double weights[2] = {1.0, 1.0};
    addChannels(stereo, frames, 2, weights);
}

void LoudnessMeter::addChannels(const float* data, int64_t frames, int channels, const double* weights) {
    Impl& d = *d_;
    // (Interleaved `channels` wide; the first kMeterChannels measured.)
    const int stride = std::max(1, channels);
    channels = std::clamp(channels, 1, kMeterChannels);
    const auto& h = truePeakFilter();
    for (int64_t i = 0; i < frames; ++i) {
        double sum = 0;
        for (int ch = 0; ch < channels; ++ch) {
            const double x = data[i * stride + ch];
            d.history[size_t(ch)][d.hpos] = x;
            d.stepPeak = std::max(d.stepPeak, std::fabs(x));
            const double k = d.k.process(ch, x);
            sum += weights[ch] * k * k;
        }
        d.hpos = (d.hpos + 1) % kTaps;
        // True peak: the samples between this one and the last, interpolated.
        for (int ch = 0; ch < channels; ++ch)
            for (int ph = 0; ph < kPhases; ++ph) {
                double v = 0;
                for (int k = 0; k < kTaps; ++k) v += h[size_t(ph)][size_t(k)] * d.history[size_t(ch)][(d.hpos + size_t(k)) % kTaps];
                d.stepPeak = std::max(d.stepPeak, std::fabs(v));
            }
        d.peak = std::max(d.peak, d.stepPeak);
        d.acc += sum;
        if (++d.n == d.step) {
            d.steps.push_back(d.acc / double(d.step));
            d.speech.push_back(d.speechNow);
            d.stepPeaks.push_back(d.stepPeak);
            d.stepPeak = 0;
            d.acc = 0;
            d.n = 0;
            if (d.steps.size() >= 4) d.maxM = std::max(d.maxM, toLufs(d.window(4)));
            if (d.steps.size() >= 30) d.maxS = std::max(d.maxS, toLufs(d.window(30)));
        }
    }
}

namespace {
// BS.1770's two gates over 400 ms block mean squares: absolute at -70 LUFS, then relative 10 LU below the gated mean.
LoudnessResult gated(const std::vector<double>& blocks, double peak) {
    LoudnessResult r;
    r.truePeakDb = peak > 0 ? 20.0 * std::log10(peak) : -96.0;
    double sum = 0;
    int cnt = 0;
    for (double b : blocks)
        if (toLufs(b) > -70.0) {
            sum += b;
            ++cnt;
        }
    if (cnt == 0) return r;
    const double relGate = toLufs(sum / cnt) - 10.0;
    double sum2 = 0;
    int cnt2 = 0;
    for (double b : blocks)
        if (toLufs(b) > -70.0 && toLufs(b) > relGate) {
            sum2 += b;
            ++cnt2;
        }
    if (cnt2 == 0) return r;
    r.integrated = toLufs(sum2 / cnt2);
    r.valid = true;
    return r;
}
}  // namespace

LoudnessResult LoudnessMeter::result() const {
    const Impl& d = *d_;
    // Blocks are 400 ms with 75 % overlap.
    const std::vector<double>& steps = d.steps;
    std::vector<double> blocks;
    for (size_t i = 3; i < steps.size(); ++i) blocks.push_back((steps[i] + steps[i - 1] + steps[i - 2] + steps[i - 3]) / 4.0);
    if (blocks.empty() && !steps.empty()) {  // shorter than 400 ms: use what we have
        double s = 0;
        for (double v : steps) s += v;
        blocks.push_back(s / double(steps.size()));
    }
    return gated(blocks, d.peak);
}

void LoudnessMeter::setSpeech(bool on) { d_->speechNow = on; }

void LoudnessMeter::markSpeech(size_t step, bool on) {
    if (step < d_->speech.size()) d_->speech[step] = on;
}

size_t LoudnessMeter::steps() const { return d_->steps.size(); }

double LoudnessMeter::momentaryAt(size_t i) const {
    const auto& st = d_->steps;
    if (i >= st.size()) return -200.0;
    const size_t from = i >= 3 ? i - 3 : 0;
    double sum = 0;
    for (size_t j = from; j <= i; ++j) sum += st[j];
    return std::max(-200.0, toLufs(sum / double(i - from + 1)));
}

LoudnessResult LoudnessMeter::speechResult() const {
    const Impl& d = *d_;
    std::vector<double> blocks;
    for (size_t i = 3; i < d.steps.size(); ++i)
        if (d.speech[i] && d.speech[i - 1] && d.speech[i - 2] && d.speech[i - 3])
            blocks.push_back((d.steps[i] + d.steps[i - 1] + d.steps[i - 2] + d.steps[i - 3]) / 4.0);
    return gated(blocks, d.peak);
}

double LoudnessMeter::shortTermAt(size_t i) const {
    const auto& st = d_->steps;
    if (i >= st.size()) return -200.0;
    const size_t from = i >= 29 ? i - 29 : 0;
    double sum = 0;
    for (size_t j = from; j <= i; ++j) sum += st[j];
    return std::max(-200.0, toLufs(sum / double(i - from + 1)));
}

double LoudnessMeter::truePeakAt(size_t i) const {
    const auto& p = d_->stepPeaks;
    return i < p.size() && p[i] > 0 ? std::max(-96.0, 20.0 * std::log10(p[i])) : -96.0;
}

double LoudnessMeter::speechShare() const {
    const Impl& d = *d_;
    size_t sound = 0, speech = 0;
    for (size_t i = 0; i < d.steps.size(); ++i)
        if (toLufs(d.steps[i]) > -70.0) {
            ++sound;
            speech += d.speech[i] ? 1 : 0;
        }
    return sound ? double(speech) / double(sound) : 0.0;
}

double LoudnessMeter::momentary() const { return std::max(-200.0, toLufs(d_->window(4))); }
double LoudnessMeter::shortTerm() const { return std::max(-200.0, toLufs(d_->window(30))); }
double LoudnessMeter::maxMomentary() const { return d_->maxM; }
double LoudnessMeter::maxShortTerm() const { return d_->maxS; }
double LoudnessMeter::seconds() const { return double(d_->steps.size()) * 0.1 + double(d_->n) / d_->fs; }

namespace {
// Tech 3342's range of short-term (3 s) blocks at every 100 ms step (it asks for at least ten a second), those for
// which `take(i)` holds (the block ending with step i).
template <class Take>
double rangeOf(const std::vector<double>& st, Take take) {
    if (st.size() < 30) return 0;
    std::vector<double> blocks;
    for (size_t i = 29; i < st.size(); ++i) {
        if (!take(i)) continue;
        double sum = 0;
        for (size_t j = i - 29; j <= i; ++j) sum += st[j];
        const double l = toLufs(sum / 30);
        if (l > -70) blocks.push_back(l);
    }
    if (blocks.size() < 2) return 0;
    // Relative gate 20 LU below the (energy) mean of what passed the absolute gate.
    double e = 0;
    for (double l : blocks) e += std::pow(10.0, (l + 0.691) / 10);
    const double gate = toLufs(e / double(blocks.size())) - 20;
    std::vector<double> kept;
    for (double l : blocks)
        if (l > gate) kept.push_back(l);
    if (kept.size() < 2) return 0;
    std::sort(kept.begin(), kept.end());
    auto pct = [&](double q) { return kept[size_t(std::lround(q * double(kept.size() - 1)))]; };
    return pct(0.95) - pct(0.10);
}
}  // namespace

double LoudnessMeter::loudnessRange() const {
    return rangeOf(d_->steps, [](size_t) { return true; });
}

double LoudnessMeter::speechLoudnessRange() const {
    const std::vector<char>& speech = d_->speech;
    return rangeOf(d_->steps, [&](size_t i) {
        int n = 0;
        for (size_t j = i - 29; j <= i; ++j) n += speech[j] ? 1 : 0;
        return n >= 20;
    });
}

void LoudnessMeter::reset() { d_ = std::make_unique<Impl>(int(d_->fs)); }

LoudnessResult measureLoudness(const AudioBuffer& buf, int64_t first, int64_t count) {
    const int64_t total = buf.frames();
    first = std::clamp<int64_t>(first, 0, total);
    const int64_t last = count < 0 ? total : std::min(total, first + count);
    if (last <= first || buf.sampleRate <= 0) return {};
    LoudnessMeter m(buf.sampleRate);
    m.add(buf.samples.data() + first * 2, last - first);
    return m.result();
}

LoudnessResult measureLoudness(const float* data, int64_t frames, int channels, const double* weights, int sampleRate) {
    if (frames <= 0 || sampleRate <= 0 || channels <= 0) return {};
    LoudnessMeter m(sampleRate);
    m.addChannels(data, frames, channels, weights);
    return m.result();
}

// ---------------------------------------------------------------------------
// LeqMMeter

double LeqMMeter::weightingDb(double f) {
    if (f <= 0) return -200;
    // ITU-R 468's network response (its formula in closed form), +18.2 dB at the top of the curve's reference, then
    // ISO 21727's 5.6 dB lower.
    const double h1 = -4.737338981378384e-24 * std::pow(f, 6) + 2.043828333606125e-15 * std::pow(f, 4) - 1.363894795463638e-7 * f * f + 1;
    const double h2 = 1.306612257412824e-19 * std::pow(f, 5) - 2.118150887518656e-11 * std::pow(f, 3) + 5.559488023498642e-4 * f;
    const double r = 1.246332637532143e-4 * f / std::sqrt(h1 * h1 + h2 * h2);
    return 18.2 + 20 * std::log10(r) - 5.6;
}

struct LeqMMeter::Impl {
    int fs = 48000, channels = 1;
    std::vector<double> gains;
    int n = 0, taps = 0, block = 0;  // FFT size, FIR length, input frames per block
    AVTXContext* fwd = nullptr;
    AVTXContext* inv = nullptr;
    av_tx_fn fwdFn = nullptr, invFn = nullptr;
    std::vector<AVComplexDouble> response;  // the FIR's spectrum, n/2 + 1
    std::vector<std::vector<double>> pending, tail;  // per channel: this block's input; the last block's overflow
    size_t filled = 0;
    double energy = 0;   // of the weighted channels, summed
    int64_t frames = 0;

    ~Impl() {
        av_tx_uninit(&fwd);
        av_tx_uninit(&inv);
    }
    // Weights `count` pending frames of each channel and adds what comes out (and, at the end, the filter's tail).
    void flush(size_t count, bool last, double& total, std::vector<std::vector<double>>& tails) const {
        double* buf = static_cast<double*>(av_malloc(sizeof(double) * size_t(n + 2)));
        auto* spec = static_cast<AVComplexDouble*>(av_malloc(sizeof(AVComplexDouble) * size_t(n / 2 + 1)));
        for (int ch = 0; ch < channels; ++ch) {
            std::fill(buf, buf + n + 2, 0.0);
            std::copy(pending[size_t(ch)].begin(), pending[size_t(ch)].begin() + std::ptrdiff_t(count), buf);
            fwdFn(fwd, spec, buf, sizeof(double));
            for (int k = 0; k <= n / 2; ++k) {
                const AVComplexDouble a = spec[k], b = response[size_t(k)];
                spec[k] = {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re};
            }
            invFn(inv, buf, spec, sizeof(AVComplexDouble));
            std::vector<double>& t = tails[size_t(ch)];
            const size_t out = count + size_t(taps) - 1;
            for (size_t i = 0; i < out; ++i) {
                const double y = buf[i] + (i < t.size() ? t[i] : 0.0);
                if (i < count || last) total += y * y;
                else buf[i] = y;
            }
            // (what runs past this block waits for the next; a block shorter than the tail keeps the rest of it too)
            std::vector<double> next(size_t(taps) - 1, 0.0);
            if (!last) {
                for (size_t i = count; i < out; ++i) next[i - count] = buf[i];
                for (size_t i = out; i < t.size(); ++i) next[i - count] += t[i];
            }
            t = std::move(next);
        }
        av_free(buf);
        av_free(spec);
    }
};

LeqMMeter::LeqMMeter(int sampleRate, int channels, const double* gains) : d_(std::make_unique<Impl>()) {
    Impl& d = *d_;
    d.fs = std::max(1, sampleRate);
    d.channels = std::clamp(channels, 1, kMeterChannels);
    d.gains.assign(size_t(d.channels), 1.0);
    if (gains)
        for (int ch = 0; ch < d.channels; ++ch) d.gains[size_t(ch)] = gains[ch];
    // Fine enough in frequency to follow the curve's low end (a third of the rate: 16384 at 48 kHz).
    d.n = 1024;
    while (d.n < d.fs / 3) d.n *= 2;
    d.taps = d.n / 2 - 1;
    d.block = d.n - d.taps + 1;
    double scale = 1.0, invScale = 1.0 / d.n;
    av_tx_init(&d.fwd, &d.fwdFn, AV_TX_DOUBLE_RDFT, 0, d.n, &scale, 0);
    av_tx_init(&d.inv, &d.invFn, AV_TX_DOUBLE_RDFT, 1, d.n, &invScale, 0);
    if (!d.fwd || !d.inv) return;  // (measures nothing)
    // The FIR: the curve sampled at the FFT's bins as a zero-phase response, its impulse centred and windowed
    // (Blackman), then its own spectrum.
    const int bins = d.n / 2 + 1;
    double* h = static_cast<double*>(av_malloc(sizeof(double) * size_t(d.n + 2)));
    auto* spec = static_cast<AVComplexDouble*>(av_malloc(sizeof(AVComplexDouble) * size_t(bins)));
    for (int k = 0; k < bins; ++k) spec[k] = {k == 0 ? 0.0 : std::pow(10.0, weightingDb(double(k) * d.fs / d.n) / 20), 0.0};
    d.invFn(d.inv, h, spec, sizeof(AVComplexDouble));
    double* fir = static_cast<double*>(av_mallocz(sizeof(double) * size_t(d.n + 2)));
    const int half = (d.taps - 1) / 2;
    for (int i = 0; i < d.taps; ++i) {
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (d.taps - 1)) + 0.08 * std::cos(4 * M_PI * i / (d.taps - 1));
        fir[i] = h[(i - half + d.n) % d.n] * w;
    }
    d.fwdFn(d.fwd, spec, fir, sizeof(double));
    d.response.assign(spec, spec + bins);
    av_free(fir);
    av_free(h);
    av_free(spec);
    d.pending.assign(size_t(d.channels), std::vector<double>(size_t(d.block), 0.0));
    d.tail.assign(size_t(d.channels), std::vector<double>(size_t(d.taps) - 1, 0.0));
}

LeqMMeter::~LeqMMeter() = default;

void LeqMMeter::add(const float* data, int64_t frames) {
    Impl& d = *d_;
    if (!d.fwd || !d.inv) return;
    for (int64_t i = 0; i < frames; ++i) {
        for (int ch = 0; ch < d.channels; ++ch) d.pending[size_t(ch)][d.filled] = double(data[i * d.channels + ch]) * d.gains[size_t(ch)];
        if (++d.filled == size_t(d.block)) {
            d.flush(d.filled, false, d.energy, d.tail);
            d.filled = 0;
        }
    }
    d.frames += frames;
}

double LeqMMeter::leqm() const {
    const Impl& d = *d_;
    if (d.frames <= 0 || !d.fwd || !d.inv) return 0;
    // (the frames still pending, and the weighting's ringing after the last of them)
    double total = d.energy;
    std::vector<std::vector<double>> tails = d.tail;
    d.flush(d.filled, true, total, tails);
    const double mean = total / double(d.frames);
    return mean > 0 ? std::max(0.0, 10 * std::log10(mean) + 108.010299957) : 0.0;
}

double LeqMMeter::seconds() const { return double(d_->frames) / d_->fs; }

// ---------------------------------------------------------------------------
// PeakLimiter

PeakLimiter::PeakLimiter(int sampleRate, double ceilingDb, double lookaheadMs, double releaseMs, int channels)
    : ceiling_(float(std::pow(10.0, ceilingDb / 20.0))),
      channels_(std::max(1, channels)),
      lookahead_(std::max(1, int(std::lround(sampleRate * lookaheadMs / 1000.0)))),
      release_(std::exp(-1.0 / std::max(1.0, sampleRate * releaseMs / 1000.0))),
      delay_(size_t(lookahead_) * size_t(channels_), 0.f),
      box_(size_t(lookahead_), 1.f),
      boxSum_(double(lookahead_)) {}

void PeakLimiter::process(const float* in, float* out, int frames) {
    const size_t nc = size_t(channels_);
    for (int i = 0; i < frames; ++i, ++n_) {
        const float* x = in + size_t(i) * nc;
        // The gain this sample needs, and the smallest over the look-ahead window.
        float peak = 0;
        for (size_t c = 0; c < nc; ++c) peak = std::max(peak, std::fabs(x[c]));
        const float need = peak > ceiling_ ? ceiling_ / peak : 1.f;
        while (!minQueue_.empty() && minQueue_.back().second >= need) minQueue_.pop_back();
        minQueue_.emplace_back(n_, need);
        while (minQueue_.front().first < n_ - lookahead_) minQueue_.pop_front();
        const float lowest = minQueue_.front().second;
        // Averaged over the look-ahead, the gain ramps down before a peak and never exceeds what it needs.
        boxSum_ += double(lowest) - double(box_[boxPos_]);
        box_[boxPos_] = lowest;
        boxPos_ = (boxPos_ + 1) % box_.size();
        const double ramp = boxSum_ / double(lookahead_);
        gain_ = std::min(ramp, 1.0 - (1.0 - gain_) * release_);
        // Out goes the sample from `lookahead_` frames ago (read before `out` may overwrite `in`).
        float* slot = delay_.data() + delayPos_ * nc;
        for (size_t c = 0; c < nc; ++c) {
            const float v = x[c];
            const float d = slot[c];
            slot[c] = v;
            out[size_t(i) * nc + c] = float(d * gain_);
        }
        delayPos_ = (delayPos_ + 1) % size_t(lookahead_);
    }
}

}  // namespace montage
