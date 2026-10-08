#include "Loudness.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace montage {

namespace {

// K-weighting: a high shelf (head effects) followed by a high-pass (RLB),
// with coefficients derived for any sample rate (as in libebur128).
struct KFilter {
    double b[2][3], a[2][3];
    double z[2][2][2] = {};  // [stage][channel][state]
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
    double acc = 0;
    int64_t n = 0;
    double peak = 0;
    std::array<std::array<double, kTaps>, 2> history{};  // recent samples per channel, for oversampling
    size_t hpos = 0;
};

LoudnessMeter::LoudnessMeter(int sampleRate) : d_(std::make_unique<Impl>(sampleRate)) {}
LoudnessMeter::~LoudnessMeter() = default;

void LoudnessMeter::add(const float* stereo, int64_t frames) {
    Impl& d = *d_;
    const auto& h = truePeakFilter();
    for (int64_t i = 0; i < frames; ++i) {
        const double l = stereo[i * 2], r = stereo[i * 2 + 1];
        // True peak: the samples between this one and the last, interpolated.
        d.history[0][d.hpos] = l;
        d.history[1][d.hpos] = r;
        d.hpos = (d.hpos + 1) % kTaps;
        for (int ch = 0; ch < 2; ++ch)
            for (int ph = 0; ph < kPhases; ++ph) {
                double v = 0;
                for (int k = 0; k < kTaps; ++k) v += h[size_t(ph)][size_t(k)] * d.history[size_t(ch)][(d.hpos + size_t(k)) % kTaps];
                d.peak = std::max(d.peak, std::fabs(v));
            }
        d.peak = std::max({d.peak, std::fabs(l), std::fabs(r)});
        const double kl = d.k.process(0, l), kr = d.k.process(1, r);
        d.acc += kl * kl + kr * kr;  // channel weights are 1.0 for L/R
        if (++d.n == d.step) {
            d.steps.push_back(d.acc / double(d.step));
            d.acc = 0;
            d.n = 0;
        }
    }
}

LoudnessResult LoudnessMeter::result() const {
    const Impl& d = *d_;
    LoudnessResult r;
    r.truePeakDb = d.peak > 0 ? 20.0 * std::log10(d.peak) : -96.0;
    // Blocks are 400 ms with 75 % overlap.
    const std::vector<double>& steps = d.steps;
    std::vector<double> blocks;
    for (size_t i = 3; i < steps.size(); ++i) blocks.push_back((steps[i] + steps[i - 1] + steps[i - 2] + steps[i - 3]) / 4.0);
    if (blocks.empty() && !steps.empty()) {  // shorter than 400 ms: use what we have
        double s = 0;
        for (double v : steps) s += v;
        blocks.push_back(s / double(steps.size()));
    }
    auto lufs = [](double ms) { return ms > 0 ? -0.691 + 10.0 * std::log10(ms) : -200.0; };
    // Absolute gate at -70 LUFS, then relative gate 10 LU below the gated mean.
    double sum = 0;
    int cnt = 0;
    for (double b : blocks)
        if (lufs(b) > -70.0) {
            sum += b;
            ++cnt;
        }
    if (cnt == 0) return r;
    const double relGate = lufs(sum / cnt) - 10.0;
    double sum2 = 0;
    int cnt2 = 0;
    for (double b : blocks)
        if (lufs(b) > -70.0 && lufs(b) > relGate) {
            sum2 += b;
            ++cnt2;
        }
    if (cnt2 == 0) return r;
    r.integrated = lufs(sum2 / cnt2);
    r.valid = true;
    return r;
}

LoudnessResult measureLoudness(const AudioBuffer& buf, int64_t first, int64_t count) {
    const int64_t total = buf.frames();
    first = std::clamp<int64_t>(first, 0, total);
    const int64_t last = count < 0 ? total : std::min(total, first + count);
    if (last <= first || buf.sampleRate <= 0) return {};
    LoudnessMeter m(buf.sampleRate);
    m.add(buf.samples.data() + first * 2, last - first);
    return m.result();
}

// ---------------------------------------------------------------------------
// PeakLimiter

PeakLimiter::PeakLimiter(int sampleRate, double ceilingDb, double lookaheadMs, double releaseMs)
    : ceiling_(float(std::pow(10.0, ceilingDb / 20.0))),
      lookahead_(std::max(1, int(std::lround(sampleRate * lookaheadMs / 1000.0)))),
      release_(std::exp(-1.0 / std::max(1.0, sampleRate * releaseMs / 1000.0))),
      delay_(size_t(lookahead_) * 2, 0.f),
      box_(size_t(lookahead_), 1.f),
      boxSum_(double(lookahead_)) {}

void PeakLimiter::process(const float* in, float* out, int frames) {
    for (int i = 0; i < frames; ++i, ++n_) {
        const float l = in[i * 2], r = in[i * 2 + 1];
        // The gain this sample needs, and the smallest over the look-ahead window.
        const float peak = std::max(std::fabs(l), std::fabs(r));
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
        // Out goes the sample from `lookahead_` frames ago.
        const float dl = delay_[delayPos_ * 2], dr = delay_[delayPos_ * 2 + 1];
        delay_[delayPos_ * 2] = l;
        delay_[delayPos_ * 2 + 1] = r;
        delayPos_ = (delayPos_ + 1) % size_t(lookahead_);
        out[i * 2] = float(dl * gain_);
        out[i * 2 + 1] = float(dr * gain_);
    }
}

}  // namespace montage
