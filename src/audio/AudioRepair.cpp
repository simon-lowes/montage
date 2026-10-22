#include "AudioRepair.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <complex>
#include <cstdint>
#include <vector>

namespace montage {

namespace {

bool cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(); }

std::vector<float> channel(const AudioBuffer& b, int c) {
    std::vector<float> x(static_cast<size_t>(b.frames()));
    for (size_t i = 0; i < x.size(); ++i) x[i] = b.samples[i * 2 + size_t(c)];
    return x;
}

void setChannel(AudioBuffer& b, int c, const std::vector<float>& x) {
    for (size_t i = 0; i < x.size(); ++i) b.samples[i * 2 + size_t(c)] = x[i];
}

// ---- Linear prediction ---------------------------------------------------------

// Autocorrelation r[0..p] of x[from, to), Hann-windowed, added to `r`.
void addAutocorrelation(const std::vector<float>& x, int64_t from, int64_t to, int p, std::vector<double>& r) {
    from = std::max<int64_t>(0, from), to = std::min<int64_t>(int64_t(x.size()), to);
    const int64_t n = to - from;
    if (n <= p) return;
    std::vector<double> w(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) w[size_t(i)] = x[size_t(from + i)] * (0.5 - 0.5 * std::cos(2 * M_PI * (double(i) + 0.5) / double(n)));
    for (int k = 0; k <= p; ++k) {
        double s = 0;
        for (int64_t i = k; i < n; ++i) s += w[size_t(i)] * w[size_t(i - k)];
        r[size_t(k)] += s;
    }
}

// Prediction-error filter a[0..p] (a[0] = 1) for autocorrelation r, by Levinson-Durbin.
std::vector<double> levinson(std::vector<double> r, int p) {
    std::vector<double> a(size_t(p) + 1, 0.0), prev;
    a[0] = 1;
    // A touch of white noise keeps pure tones well conditioned.
    r[0] = r[0] * (1 + 1e-6) + 1e-12;
    double err = r[0];
    for (int i = 1; i <= p; ++i) {
        double acc = r[size_t(i)];
        for (int j = 1; j < i; ++j) acc += a[size_t(j)] * r[size_t(i - j)];
        const double k = -acc / err;
        prev = a;
        for (int j = 1; j < i; ++j) a[size_t(j)] = prev[size_t(j)] + k * prev[size_t(i - j)];
        a[size_t(i)] = k;
        err *= 1 - k * k;
        if (err <= 1e-15 * r[0]) break;
    }
    return a;
}

constexpr int kOrder = 32;
constexpr int kBlock = 2048;

// Least-squares AR interpolation of x[s, e) from the sound around it: of the samples `unknown` (positions in x, in
// order, inside the span), or of the whole span. With `a` the predictor to use, else one from the sound either side.
void rebuild(std::vector<float>& x, int64_t s, int64_t e, const std::vector<int64_t>* unknown = nullptr,
             const std::vector<double>* predictor = nullptr) {
    const int p = kOrder;
    const int64_t n = int64_t(x.size());
    std::vector<int64_t> all;
    if (!unknown) {
        for (int64_t i = s; i < e; ++i) all.push_back(i);
        unknown = &all;
    }
    const std::vector<int64_t>& u = *unknown;
    const int64_t L = int64_t(u.size());
    std::vector<double> a;
    if (predictor) {
        a = *predictor;
    } else {
        std::vector<double> r(size_t(p) + 1, 0.0);
        addAutocorrelation(x, s - 1024, s, p, r);
        addAutocorrelation(x, e, e + 1024, p, r);
        if (r[0] <= 0) {
            for (int64_t i : u) x[size_t(i)] = 0;
            return;
        }
        a = levinson(r, p);
    }
    // Normal equations M u = rhs: M is Toeplitz in the filter's own autocorrelation.
    std::vector<double> ra(size_t(p) + 1, 0.0);
    for (int d = 0; d <= p; ++d)
        for (int k = 0; k + d <= p; ++k) ra[size_t(d)] += a[size_t(k)] * a[size_t(k + d)];
    // The prediction errors with the unknowns at zero.
    for (int64_t i : u) x[size_t(i)] = 0;
    const int64_t span = e - s;
    std::vector<double> b(static_cast<size_t>(span + p), 0.0);
    for (int64_t m = 0; m < span + p; ++m) {
        const int64_t t = s + m;
        if (t >= n) break;
        double v = 0;
        for (int k = 0; k <= p && t - k >= 0; ++k) v += a[size_t(k)] * x[size_t(t - k)];
        b[size_t(m)] = v;
    }
    // M is banded: the unknowns are in order, so two more than p apart in the list are more than p apart in time.
    // Stored by rows, each row's band to its left (M(i, j) for i - p <= j <= i), and solved by banded Cholesky.
    const int64_t w = p;
    std::vector<double> M(static_cast<size_t>(L * (w + 1)), 0.0), rhs(static_cast<size_t>(L), 0.0);
    auto at = [&](int64_t i, int64_t j) -> double& { return M[size_t(i * (w + 1) + (i - j))]; };
    for (int64_t i = 0; i < L; ++i) {
        for (int64_t j = std::max<int64_t>(0, i - w); j <= i; ++j) {
            const int64_t d = u[size_t(i)] - u[size_t(j)];
            at(i, j) = d <= p ? ra[size_t(d)] : 0.0;
        }
        at(i, i) += 1e-9 * ra[0];
        const int64_t off = u[size_t(i)] - s;
        for (int k = 0; k <= p && off + k < span + p; ++k) rhs[size_t(i)] -= a[size_t(k)] * b[size_t(off + k)];
    }
    for (int64_t j = 0; j < L; ++j) {
        double d = at(j, j);
        for (int64_t k = std::max<int64_t>(0, j - w); k < j; ++k) d -= at(j, k) * at(j, k);
        d = std::sqrt(std::max(d, 1e-30));
        at(j, j) = d;
        for (int64_t i = j + 1; i <= std::min(L - 1, j + w); ++i) {
            double v = at(i, j);
            for (int64_t k = std::max<int64_t>(0, i - w); k < j; ++k) v -= at(i, k) * at(j, k);
            at(i, j) = v / d;
        }
    }
    for (int64_t i = 0; i < L; ++i) {
        double v = rhs[size_t(i)];
        for (int64_t k = std::max<int64_t>(0, i - w); k < i; ++k) v -= at(i, k) * rhs[size_t(k)];
        rhs[size_t(i)] = v / at(i, i);
    }
    for (int64_t i = L - 1; i >= 0; --i) {
        double v = rhs[size_t(i)];
        for (int64_t k = i + 1; k <= std::min(L - 1, i + w); ++k) v -= at(k, i) * rhs[size_t(k)];
        rhs[size_t(i)] = v / at(i, i);
    }
    for (int64_t i = 0; i < L; ++i) x[size_t(u[size_t(i)])] = float(std::clamp(rhs[size_t(i)], -4.0, 4.0));
}

int declickChannel(std::vector<float>& x, double threshold, int64_t maxLen, const std::atomic<bool>* cancel) {
    const int p = kOrder;
    const int64_t n = int64_t(x.size());
    if (n < 4 * p) return 0;
    // How far each sample departs from what the samples before it (fwd) and after it (bwd)
    // predict, in typical errors (capped, to keep this to a byte a sample). A click shows in
    // fwd from its first sample on and in bwd up to its last, so a sample hit by one is high
    // in both; a burst of crackle can predict itself inside, but not its own start and end.
    std::vector<uint8_t> fwd(static_cast<size_t>(n), 0), bwd(static_cast<size_t>(n), 0);
    for (int64_t b0 = 0; b0 < n; b0 += kBlock) {
        if (cancelled(cancel)) return 0;
        const int64_t b1 = std::min(n, b0 + kBlock);
        std::vector<double> r(size_t(p) + 1, 0.0);
        addAutocorrelation(x, b0 - kBlock / 4, b1 + kBlock / 4, p, r);
        if (r[0] <= 0) continue;
        const std::vector<double> a = levinson(r, p);
        std::vector<double> f(static_cast<size_t>(b1 - b0)), g(static_cast<size_t>(b1 - b0));
        for (int64_t t = b0; t < b1; ++t) {
            double ff = 0, gg = 0;
            for (int k = 0; k <= p; ++k) {
                if (t - k >= 0) ff += a[size_t(k)] * x[size_t(t - k)];
                if (t + k < n) gg += a[size_t(k)] * x[size_t(t + k)];
            }
            f[size_t(t - b0)] = std::fabs(ff), g[size_t(t - b0)] = std::fabs(gg);
        }
        // The typical error: the median, which the clicks themselves hardly move.
        std::vector<double> mag = f;
        std::nth_element(mag.begin(), mag.begin() + long(mag.size() / 2), mag.end());
        const double sigma = std::max(1.4826 * mag[mag.size() / 2], 1e-6);
        for (int64_t t = b0; t < b1; ++t) {
            fwd[size_t(t)] = uint8_t(std::min(255.0, std::round(f[size_t(t - b0)] / sigma)));
            bwd[size_t(t)] = uint8_t(std::min(255.0, std::round(g[size_t(t - b0)] / sigma)));
        }
    }
    auto high = [&](const std::vector<uint8_t>& v, int64_t t) { return v[size_t(t)] > threshold; };
    // Cores: samples high both ways, a few apart at most; cores closer than the longest click are one click.
    std::vector<std::pair<int64_t, int64_t>> cores;
    for (int64_t t = p; t < n - p; ++t) {
        if (!high(fwd, t) || !high(bwd, t)) continue;
        if (!cores.empty() && t - cores.back().first <= maxLen) cores.back().second = t;
        else cores.push_back({t, t});
    }
    int found = 0;
    for (size_t i = 0; i < cores.size(); ++i) {
        if (cancelled(cancel)) return found;
        auto [cs, ce] = cores[i];
        // The click runs from the first sample before the core that the past cannot predict to
        // the last after it that the future cannot, not reaching into the next or last click's
        // own reach.
        const int64_t lo = std::max<int64_t>({p, cs - maxLen, i ? cores[i - 1].second + p + 1 : 0});
        const int64_t hi = std::min<int64_t>({n - p - 1, ce + maxLen, i + 1 < cores.size() ? cores[i + 1].first - p - 1 : n});
        int64_t first = cs, last = ce;
        for (int64_t t = lo; t < cs; ++t)
            if (high(fwd, t)) {
                first = t;
                break;
            }
        for (int64_t t = hi; t > ce; --t)
            if (high(bwd, t)) {
                last = t;
                break;
            }
        // Then the edges grow while the error stays over a lower level: crackle dies away rather than stops.
        const double low = std::max(3.0, threshold / 3);
        for (int64_t t = last + 1, gap = 0; t <= hi && gap <= 2; ++t) {
            if (bwd[size_t(t)] > low) last = t, gap = 0;
            else ++gap;
        }
        for (int64_t t = first - 1, gap = 0; t >= lo && gap <= 2; --t) {
            if (fwd[size_t(t)] > low) first = t, gap = 0;
            else ++gap;
        }
        const int64_t s = std::max<int64_t>(p, first - 2), e = std::min<int64_t>(n - p, last + 3);
        if (e - s > maxLen) continue;  // too long for a click: part of the sound
        rebuild(x, s, e);
        ++found;
    }
    return found;
}

// ---- Phase vocoder ---------------------------------------------------------------

constexpr size_t kFrameSize = 2048;

// In place, for the frame size; `inverse` unscaled.
void fft(std::vector<std::complex<double>>& a, bool inverse) {
    static const std::vector<std::complex<double>> twiddles = [] {
        std::vector<std::complex<double>> w(kFrameSize / 2);
        for (size_t k = 0; k < w.size(); ++k) w[k] = std::polar(1.0, -2 * M_PI * double(k) / kFrameSize);
        return w;
    }();
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t stride = kFrameSize / len;
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> w = inverse ? std::conj(twiddles[k * stride]) : twiddles[k * stride];
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
    }
}

double wrapPhase(double p) { return p - 2 * M_PI * std::round(p / (2 * M_PI)); }

constexpr int kFrame = int(kFrameSize);
constexpr int kSynthHop = kFrame / 4;

// x stretched to `ratio` times its length; sample t of the result (from `pad` on) is x at t / ratio.
std::vector<double> stretch(const std::vector<float>& x, double ratio, int64_t pad, const std::atomic<bool>* cancel) {
    const int N = kFrame, bins = N / 2 + 1;
    const int64_t len = int64_t(x.size());
    const double ha = kSynthHop / ratio;
    std::vector<double> y(size_t(std::ceil(double(len) * ratio)) + size_t(N) + size_t(2 * pad), 0.0);
    std::vector<double> win(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) win[size_t(i)] = 0.5 - 0.5 * std::cos(2 * M_PI * i / N);
    const size_t nb = static_cast<size_t>(bins);
    std::vector<double> prevPhase(nb), synth(nb), mag(nb), phase(nb), inst(nb);
    std::vector<std::complex<double>> buf(static_cast<size_t>(N));
    std::vector<int> peaks;
    int64_t prevCentre = 0;
    for (int64_t k = 0;; ++k) {
        const int64_t ca = std::llround(double(k) * ha), cs = k * kSynthHop;
        if (ca > len + N / 2) break;
        if ((k & 63) == 0 && cancelled(cancel)) break;
        for (int i = 0; i < N; ++i) {
            const int64_t t = ca - N / 2 + i;
            buf[size_t(i)] = t >= 0 && t < len ? x[size_t(t)] * win[size_t(i)] : 0.0;
        }
        fft(buf, false);
        for (int b = 0; b < bins; ++b) mag[size_t(b)] = std::abs(buf[size_t(b)]), phase[size_t(b)] = std::arg(buf[size_t(b)]);
        if (k == 0) {
            synth = phase;
        } else {
            const double dt = double(ca - prevCentre);
            for (int b = 0; b < bins; ++b) {
                const double omega = 2 * M_PI * b / N;
                inst[size_t(b)] = dt > 0 ? omega + wrapPhase(phase[size_t(b)] - prevPhase[size_t(b)] - omega * dt) / dt : omega;
            }
            // Identity phase locking: peaks advance at their own frequency; the bins round a
            // peak keep their phase relative to it, so the partial stays one coherent thing.
            peaks.clear();
            for (int b = 1; b < bins - 1; ++b)
                if (mag[size_t(b)] > mag[size_t(b - 1)] && mag[size_t(b)] >= mag[size_t(b + 1)]) peaks.push_back(b);
            std::vector<double> next(nb);
            if (peaks.empty()) {
                for (int b = 0; b < bins; ++b) next[size_t(b)] = synth[size_t(b)] + inst[size_t(b)] * kSynthHop;
            } else {
                for (int pk : peaks) next[size_t(pk)] = synth[size_t(pk)] + inst[size_t(pk)] * kSynthHop;
                size_t at = 0;
                for (int b = 0; b < bins; ++b) {
                    while (at + 1 < peaks.size() && std::abs(peaks[at + 1] - b) < std::abs(peaks[at] - b)) ++at;
                    const int pk = peaks[at];
                    if (b != pk) next[size_t(b)] = next[size_t(pk)] + phase[size_t(b)] - phase[size_t(pk)];
                }
            }
            for (int b = 0; b < bins; ++b) synth[size_t(b)] = wrapPhase(next[size_t(b)]);
        }
        prevPhase = phase;
        prevCentre = ca;
        for (int b = 0; b < bins; ++b) buf[size_t(b)] = std::polar(mag[size_t(b)], synth[size_t(b)]);
        for (int b = 1; b < N / 2; ++b) buf[size_t(N - b)] = std::conj(buf[size_t(b)]);
        fft(buf, true);
        // Hann in and out at a quarter-frame hop sums to 1.5; the inverse FFT is unscaled.
        const double scale = 1.0 / (1.5 * N);
        for (int i = 0; i < N; ++i) {
            const int64_t t = cs - N / 2 + i + pad;
            if (t >= 0 && t < int64_t(y.size())) y[size_t(t)] += buf[size_t(i)].real() * win[size_t(i)] * scale;
        }
    }
    return y;
}

// y read at `step` per output sample (from `pad`), low-passed first when that skips samples.
std::vector<float> resample(const std::vector<double>& y, double step, int64_t pad, int64_t len) {
    constexpr int kZeros = 16, kRes = 256;
    static const std::vector<double> kernel = [] {
        std::vector<double> k(size_t(kZeros * kRes) + 2);
        for (size_t i = 0; i < k.size(); ++i) {
            const double u = double(i) / kRes;
            const double sinc = u == 0 ? 1 : std::sin(M_PI * u) / (M_PI * u);
            const double w = u >= kZeros ? 0 : 0.42 + 0.5 * std::cos(M_PI * u / kZeros) + 0.08 * std::cos(2 * M_PI * u / kZeros);
            k[i] = sinc * w;
        }
        return k;
    }();
    const double fc = std::min(1.0, 1.0 / step) * 0.97;
    const double reach = kZeros / fc;
    std::vector<float> out(static_cast<size_t>(len));
    for (int64_t i = 0; i < len; ++i) {
        const double t = double(i) * step + double(pad);
        const int64_t j0 = int64_t(std::floor(t - reach)) + 1, j1 = int64_t(std::floor(t + reach));
        double s = 0;
        for (int64_t j = std::max<int64_t>(0, j0); j <= j1 && j < int64_t(y.size()); ++j) {
            const double u = std::fabs(t - double(j)) * fc * kRes;
            const size_t ui = size_t(u);
            if (ui + 1 >= kernel.size()) continue;
            const double f = u - double(ui);
            s += y[size_t(j)] * (kernel[ui] + (kernel[ui + 1] - kernel[ui]) * f);
        }
        out[size_t(i)] = float(s * fc);
    }
    return out;
}

}  // namespace

void declick(const AudioBuffer& in, AudioBuffer& out, double sensitivity, double maxClickMs, int* found,
             const std::atomic<bool>* cancel) {
    out = in;
    // 16 typical errors at 0, 8 at 50, 4 at 100.
    const double threshold = 16 * std::pow(0.25, std::clamp(sensitivity, 0.0, 100.0) / 100);
    const int64_t maxLen = std::max<int64_t>(2, int64_t(std::clamp(maxClickMs, 0.05, 20.0) / 1000 * in.sampleRate) + 5);
    int total = 0;
    for (int c = 0; c < 2; ++c) {
        std::vector<float> x = channel(in, c);
        total += declickChannel(x, threshold, maxLen, cancel);
        setChannel(out, c, x);
    }
    if (found) *found = total;
}

void pitchShift(const AudioBuffer& in, AudioBuffer& out, double semitones, const std::atomic<bool>* cancel) {
    out = in;
    semitones = std::clamp(semitones, -24.0, 24.0);
    if (std::fabs(semitones) < 1e-4 || in.frames() == 0) return;
    const double ratio = std::pow(2.0, semitones / 12);
    const int64_t pad = kFrame;
    for (int c = 0; c < 2; ++c) {
        const std::vector<double> y = stretch(channel(in, c), ratio, pad, cancel);
        if (cancelled(cancel)) return;
        setChannel(out, c, resample(y, ratio, pad, in.frames()));
    }
}

// ---- De-clip ---------------------------------------------------------------------

namespace {

// The level a channel was clipped at on one side (`sign` +1 or -1; a recording with an offset clips sooner on one),
// as a magnitude: where its loudest samples on that side pile up (hard clipping puts many at one level, soft clipping
// crowds them just under it), or 0 when they don't.
double clipLevel(const std::vector<float>& x, float sign) {
    constexpr int kBins = 200;
    double peak = 0;
    for (float v : x) peak = std::max(peak, double(v * sign));
    if (peak < 1e-4) return 0;
    std::vector<double> h(kBins, 0.0);
    for (float v : x)
        if (v * sign > 0) h[size_t(std::min<double>(kBins - 1, v * sign / peak * kBins))] += 1;
    // Below a pile-up, a recording's loudest samples thin out towards its peak; how many a bin holds there.
    auto typical = [&](int k) {
        double sum = 0;
        for (int j = std::max(0, k - 20); j < k - 4; ++j) sum += h[size_t(j)];
        return sum / std::max(1, (k - 4) - std::max(0, k - 20));
    };
    if (h[kBins - 1] < 8 || h[kBins - 1] < 4 * typical(kBins - 1)) return 0;
    int k = kBins - 1;
    while (k > kBins / 2 && h[size_t(k - 1)] > 2 * std::max(1.0, typical(k - 1))) --k;
    return double(k) / kBins * peak;
}

// Clipped samples (at or past `up` above zero, or `down` below it, in magnitude) rebuilt from the sound around them by AR interpolation, held to
// lie beyond the level on their side (a clipped peak was at least that loud). Runs closer than the predictor's reach
// are rebuilt together. Returns how many runs.
int declipChannel(std::vector<float>& x, double up, double down, const std::atomic<bool>* cancel) {
    auto level = [&](float v) { return v > 0 ? up : down; };  // (the level on a sample's side)
    auto clipped = [&](float v) { return std::fabs(v) >= level(v); };
    const int p = kOrder;
    const int64_t n = int64_t(x.size());
    std::vector<std::pair<int64_t, int64_t>> runs;
    for (int64_t t = 0; t < n; ++t) {
        if (!clipped(x[size_t(t)])) continue;
        const bool up = x[size_t(t)] > 0;
        if (!runs.empty() && runs.back().second == t && (x[size_t(t - 1)] > 0) == up) ++runs.back().second;
        else runs.push_back({t, t + 1});
    }
    const std::vector<float> original = x;
    int count = 0;
    // Twice over: the second time the predictors are fitted to the sound as the first rebuilt it, clipped stretches
    // and all, which a heavily clipped voice leaves too little of otherwise.
    for (int round = 0; round < 2; ++round) {
    count = 0;
    for (size_t i = 0; i < runs.size();) {
        if (cancelled(cancel)) return count;
        // A group: runs whose gaps are shorter than the predictor, so each one's neighbours are rebuilt too.
        size_t j = i + 1;
        while (j < runs.size() && runs[j].first - runs[j - 1].second <= p && runs[j].second - runs[i].first < 4096) ++j;
        const int64_t s = runs[i].first, e = runs[j - 1].second;
        if (s < p || e > n - p) {
            i = j;
            continue;  // too near the ends to predict from both sides
        }
        // The predictor from the sound around the group: its clipped samples left out of the estimate at first, then
        // as rebuilt.
        std::vector<double> r(size_t(p) + 1, 0.0);
        auto addClean = [&](int64_t from, int64_t to) {
            int64_t a = from;
            for (int64_t t = from; t <= to; ++t)
                if (t == to || clipped(original[size_t(std::clamp<int64_t>(t, 0, n - 1))])) {
                    if (t - a > 4 * p) addAutocorrelation(original, a, t, p, r);
                    a = t + 1;
                }
        };
        if (round == 0) {
            addClean(std::max<int64_t>(0, s - 2048), s);
            addClean(e, std::min<int64_t>(n, e + 2048));
        } else {
            addAutocorrelation(x, s - 1024, e + 1024, p, r);
        }
        if (r[0] <= 0) {
            i = j;
            continue;
        }
        const std::vector<double> a = levinson(r, p);
        std::vector<int64_t> unknown;
        for (size_t k = i; k < j; ++k)
            for (int64_t t = runs[k].first; t < runs[k].second; ++t) unknown.push_back(t);
        // Rebuilt, then any sample that came back inside the level is set at it (or more than 12 dB past it, where
        // nothing can be told any more, at that) and the rest rebuilt again.
        for (int64_t t : unknown) x[size_t(t)] = original[size_t(t)];
        for (int pass = 0; pass < 12 && !unknown.empty(); ++pass) {
            rebuild(x, s, e, &unknown, &a);
            std::vector<int64_t> keep;
            bool moved = false;
            for (int64_t t : unknown) {
                const float sign = original[size_t(t)] > 0 ? 1.0f : -1.0f;
                const float at = float(level(sign));
                if (x[size_t(t)] * sign < at || x[size_t(t)] * sign > 4 * at) {
                    x[size_t(t)] = sign * std::clamp(x[size_t(t)] * sign, at, 4 * at);
                    moved = true;
                } else {
                    keep.push_back(t);
                }
            }
            if (!moved) break;
            unknown.swap(keep);
        }
        count += int(j - i);
        i = j;
    }
    }
    return count;
}

}  // namespace

bool declip(const AudioBuffer& in, AudioBuffer& out, double clipLevelDb, int* found, const std::atomic<bool>* cancel) {
    out = in;
    if (found) *found = 0;
    const int nch = std::max(1, in.channels);
    const int64_t n = in.frames();
    int total = 0;
    bool any = false;
    for (int c = 0; c < nch; ++c) {
        std::vector<float> x(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) x[size_t(i)] = in.samples[size_t(i) * size_t(nch) + size_t(c)];
        // A level given (dBFS) is used on both sides; else the ones the samples show, a hair inside them
        // (a side that shows none is left).
        constexpr double kNone = std::numeric_limits<double>::infinity();
        double up = kNone, down = kNone;
        if (clipLevelDb < 0) {
            up = down = std::pow(10.0, (clipLevelDb - 0.1) / 20);  // (within the setting's 0.1 dB step)
        } else {
            if (const double l = clipLevel(x, 1.0f); l > 0) up = l * 0.999;
            if (const double l = clipLevel(x, -1.0f); l > 0) down = l * 0.999;
        }
        bool reached = false;
        for (float v : x) reached = reached || (v > 0 ? v >= up : -v >= down);
        if (!reached) continue;
        total += declipChannel(x, up, down, cancel);
        any = true;
        if (cancelled(cancel)) return false;
        for (int64_t i = 0; i < n; ++i) out.samples[size_t(i) * size_t(nch) + size_t(c)] = x[size_t(i)];
    }
    if (any) {
        // The rebuilt peaks can rise past full scale: then the whole recording is lowered (all channels alike) just
        // enough to bring them under it, so their shape and the sound's dynamics are kept.
        constexpr float kCeiling = 0.989f;  // -0.1 dBFS
        float top = 0;
        for (float v : out.samples) top = std::max(top, std::fabs(v));
        if (top > kCeiling)
            for (float& v : out.samples) v *= kCeiling / top;
    }
    if (found) *found = total;
    return true;
}

// ---- De-plosive ------------------------------------------------------------------

namespace {

// A biquad (RBJ low-pass) run forwards then backwards, so it has no phase shift.
void lowpassZeroPhase(std::vector<double>& x, double fc, double q, int rate) {
    const double w = 2 * M_PI * fc / rate, alpha = std::sin(w) / (2 * q), cw = std::cos(w);
    const double a0 = 1 + alpha;
    const double b0 = (1 - cw) / 2 / a0, b1 = (1 - cw) / a0, b2 = b0, a1 = -2 * cw / a0, a2 = (1 - alpha) / a0;
    auto run = [&](auto begin, auto end) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto it = begin; it != end; ++it) {
            const double v = *it, y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1, x1 = v, y2 = y1, y1 = y;
            *it = y;
        }
    };
    run(x.begin(), x.end());
    run(x.rbegin(), x.rend());
}

// The low band of a signal: 4th-order Butterworth at `fc`, forwards and backwards.
std::vector<double> lowBand(const std::vector<double>& x, double fc, int rate) {
    std::vector<double> lo = x;
    lowpassZeroPhase(lo, fc, 0.5412, rate);
    lowpassZeroPhase(lo, fc, 1.3066, rate);
    return lo;
}

}  // namespace

void deplosive(const AudioBuffer& in, AudioBuffer& out, double sensitivity, double reductionDb, int* found,
               const std::atomic<bool>* cancel) {
    out = in;
    if (found) *found = 0;
    const int nch = std::max(1, in.channels);
    const int64_t n = in.frames();
    const int rate = in.sampleRate;
    const int64_t hop = std::max(1, rate / 200);  // 5 ms
    if (n < hop * 8) return;
    constexpr double kSplit = 150;
    // Levels every 5 ms (dB) of the channels' mix: its low band (the plosive's, split off without phase shift), the
    // rest, and the low band's usual level around (median over a second).
    const int64_t frames = n / hop;
    std::vector<double> eLo(static_cast<size_t>(frames)), eHi(static_cast<size_t>(frames));
    {
        std::vector<double> mono(static_cast<size_t>(n), 0.0);
        for (int64_t i = 0; i < n; ++i)
            for (int c = 0; c < nch; ++c) mono[size_t(i)] += in.samples[size_t(i) * size_t(nch) + size_t(c)] / double(nch);
        const std::vector<double> monoLo = lowBand(mono, kSplit, rate);
        for (int64_t f = 0; f < frames; ++f) {
            double a = 0, b = 0;
            for (int64_t i = f * hop; i < (f + 1) * hop; ++i) {
                const double l = monoLo[size_t(i)], h = mono[size_t(i)] - l;
                a += l * l, b += h * h;
            }
            eLo[size_t(f)] = 10 * std::log10(a / double(hop) + 1e-12);
            eHi[size_t(f)] = 10 * std::log10(b / double(hop) + 1e-12);
        }
    }
    if (cancelled(cancel)) return;
    std::vector<double> usual(static_cast<size_t>(frames));
    {
        const int64_t half = 100;  // ±0.5 s
        std::vector<double> win;
        for (int64_t f = 0; f < frames; ++f) {
            win.assign(eLo.begin() + std::max<int64_t>(0, f - half), eLo.begin() + std::min(frames, f + half + 1));
            std::nth_element(win.begin(), win.begin() + long(win.size() / 2), win.end());
            usual[size_t(f)] = win[win.size() / 2];
        }
    }
    // A plosive: the low band jumps well above its usual level within 10 ms, and outweighs the rest of the sound.
    const double rise = 20 - 14 * std::clamp(sensitivity, 0.0, 100.0) / 100;  // 20 dB at 0, 13 at 50, 6 at 100
    const double maxCut = std::clamp(reductionDb, 0.0, 60.0);
    std::vector<double> cutDb(static_cast<size_t>(frames), 0.0);
    const int64_t longest = 16;  // 80 ms
    int count = 0;
    for (int64_t f = 2; f < frames; ++f) {
        if (eLo[size_t(f)] - usual[size_t(f)] < rise || eLo[size_t(f)] - eLo[size_t(f - 2)] < rise / 2 || eLo[size_t(f)] < eHi[size_t(f)] + 3)
            continue;
        // From its start to where the low band settles back (80 ms at most).
        const int64_t first = f - 1;
        int64_t last = f;
        while (last + 1 < frames && last + 1 - first < longest && eLo[size_t(last + 1)] - usual[size_t(last + 1)] > rise / 2) ++last;
        for (int64_t g = first; g <= last; ++g)
            cutDb[size_t(g)] = std::max(cutDb[size_t(g)], std::min(maxCut, eLo[size_t(g)] - usual[size_t(g)] - 3));
        ++count;
        f = last + 1;
    }
    if (!count) return;
    // The cut, per sample, eased in and out over 5 ms, taken from each channel's own low band in turn (the rest of
    // the sound is left exactly).
    auto keep = [&](int64_t i) {
        const double fpos = (double(i) + 0.5) / double(hop) - 0.5;
        const int64_t f0 = std::clamp<int64_t>(int64_t(std::floor(fpos)), 0, frames - 1), f1 = std::min(frames - 1, f0 + 1);
        const double t = std::clamp(fpos - double(f0), 0.0, 1.0);
        const double db = cutDb[size_t(f0)] * (1 - t) + cutDb[size_t(f1)] * t;
        return db > 0 ? std::pow(10.0, -db / 20) : 1.0;
    };
    for (int c = 0; c < nch; ++c) {
        if (cancelled(cancel)) {
            out = in;
            return;
        }
        std::vector<double> x(static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) x[size_t(i)] = in.samples[size_t(i) * size_t(nch) + size_t(c)];
        const std::vector<double> lo = lowBand(x, kSplit, rate);
        for (int64_t i = 0; i < n; ++i)
            if (const double k = keep(i); k < 1) out.samples[size_t(i) * size_t(nch) + size_t(c)] -= float((1 - k) * lo[size_t(i)]);
    }
    if (found) *found = count;
}

// ---- De-wind ---------------------------------------------------------------------

void dewind(const AudioBuffer& in, AudioBuffer& out, double amount, double maxReductionDb, const std::atomic<bool>* cancel) {
    out = in;
    const int nch = std::max(1, in.channels);
    const int64_t n = in.frames();
    if (n == 0 || nch < 1) return;
    const int N = kFrame, hop = N / 4, bins = N / 2 + 1;
    const double binHz = double(in.sampleRate) / N;
    const double depth = std::clamp(amount, 0.0, 100.0) / 100;
    const double floorDb = -std::clamp(maxReductionDb, 0.0, 60.0);
    // Two microphones hear a voice alike but their wind apart: the second channel stands in for the first's "voice"
    // (the part they share) and the reverse. One channel (or two the same) can only be judged on its gusts: the
    // lowest bins pushed back to their recent level.
    // (A second channel that is silent, or the first, leaves one mic to judge too: the live one.)
    bool same = nch < 2;
    int src = 0;  // the channel judged, when one mic is
    std::vector<int> outs{0};
    if (!same) {
        same = true;
        double e0 = 0, e1 = 0;
        for (int64_t i = 0; i < n; ++i) {
            const float a = in.samples[size_t(i) * size_t(nch)], b = in.samples[size_t(i) * size_t(nch) + 1];
            same = same && a == b;
            e0 += double(a) * a, e1 += double(b) * b;
        }
        outs = {0, 1};
        if (!same && e1 < 1e-6 * e0) same = true, outs = {0};
        else if (!same && e0 < 1e-6 * e1) same = true, src = 1, outs = {1};
    }
    const int top = std::min(bins - 1, int(std::ceil((same ? 100.0 : 700.0) / binHz)));
    auto taper = [&](int b) {  // full below 500 Hz (two mics) or 80 Hz (one), none past the top
        const double f = b * binHz, full = same ? 80.0 : 500.0, end = same ? 100.0 : 700.0;
        return f <= full ? 1.0 : f >= end ? 0.0 : 0.5 + 0.5 * std::cos(M_PI * (f - full) / (end - full));
    };
    std::vector<double> win(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) win[size_t(i)] = 0.5 - 0.5 * std::cos(2 * M_PI * i / N);
    const int used = same ? 1 : 2;
    std::vector<std::vector<std::complex<double>>> spec(static_cast<size_t>(used), std::vector<std::complex<double>>(size_t(N)));
    std::vector<double> sLL(size_t(top) + 1, 0.0), sRR(size_t(top) + 1, 0.0), ref(size_t(top) + 1, 0.0);
    std::vector<std::complex<double>> sLR(size_t(top) + 1);
    // What is taken away from each channel, overlap-added: the rest of the sound stays exactly as it was.
    std::vector<std::vector<double>> removed(static_cast<size_t>(std::min(nch, 2)), std::vector<double>(static_cast<size_t>(n) + size_t(N), 0.0));
    const double smooth = 0.75;
    std::vector<std::complex<double>> buf(static_cast<size_t>(N));
    const int64_t frames = (n + N / 2) / hop + 1;
    auto analyse = [&](int64_t k) {  // frame k's spectra
        const int64_t centre = k * hop;
        for (int c = 0; c < used; ++c) {
            auto& X = spec[size_t(c)];
            for (int i = 0; i < N; ++i) {
                const int64_t t = centre - N / 2 + i;
                X[size_t(i)] = t >= 0 && t < n ? in.samples[size_t(t) * size_t(nch) + size_t(same ? src : c)] * win[size_t(i)] : 0.0;
            }
            fft(X, false);
        }
    };
    // A first look over the whole recording. One mic: each low bin's median level, under which the recent level is
    // never taken to fall (so a silent start, or a long quiet stretch, does not leave everything after it cut).
    // Two: how much louder the voice is on the first than the second, so a voice louder on one mic still counts as
    // shared. From 1 to 8 kHz, the cross level summed over the whole recording keeps what the mics share (the wind's
    // part cancels) and bounds it: over the second's own level it is exact when the second has no wind, and the first's
    // own level over it is exact when the first has none. The bound from the mic that has less wind (the less low
    // sound for its highs) is taken.
    std::vector<double> floorPw(size_t(top) + 1, 0.0);
    double g = 1;
    {
        std::vector<std::vector<double>> pw(size_t(top) + 1);
        const int from = int(1000 / binHz), to = std::min(bins - 1, int(8000 / binHz));
        std::vector<std::complex<double>> cross(size_t(std::max(0, to - from + 1)));
        double eL = 0, eR = 0, lowL = 0, lowR = 0;
        for (int64_t k = 0; k < frames; ++k) {
            if ((k & 63) == 0 && cancelled(cancel)) return;
            analyse(k);
            if (same) {
                for (int b = 0; b <= top; ++b) pw[size_t(b)].push_back(std::norm(spec[0][size_t(b)]));
                continue;
            }
            for (int b = from; b <= to; ++b) {
                const auto L = spec[0][size_t(b)], R = spec[1][size_t(b)];
                eL += std::norm(L), eR += std::norm(R), cross[size_t(b - from)] += L * std::conj(R);
            }
            for (int b = 1; b <= top; ++b) lowL += std::norm(spec[0][size_t(b)]), lowR += std::norm(spec[1][size_t(b)]);
        }
        for (int b = 0; b <= top && same; ++b) {
            auto& v = pw[size_t(b)];
            std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
            floorPw[size_t(b)] = v[v.size() / 2];
        }
        double shared = 0;
        for (const auto& x : cross) shared += std::abs(x);
        if (shared > 1e-20 && eL > 1e-20 && eR > 1e-20)
            g = std::clamp(lowL / eL > lowR / eR ? shared / eR : eL / shared, 0.25, 4.0);
    }
    for (int64_t k = 0; k < frames; ++k) {
        if ((k & 63) == 0 && cancelled(cancel)) {
            out = in;
            return;
        }
        analyse(k);
        const int64_t centre = k * hop;
        for (int c : outs) {  // (a third channel and on are left)
            const auto& X = spec[size_t(same ? 0 : c)];
            for (int b = 0; b <= top; ++b) {
                double gainDb = 0;
                if (same) {
                    const double pw = std::norm(X[size_t(b)]);
                    if (c == outs[0]) {
                        // The recent level falls with the sound at once and rises slowly (about 8 dB a second).
                        double& r = ref[size_t(b)];
                        r = std::max(floorPw[size_t(b)], k == 0 || pw < r ? (k == 0 ? pw : 0.7 * r + 0.3 * pw) : r * 1.02);
                    }
                    if (pw > 0) gainDb = std::min(0.0, 10 * std::log10((ref[size_t(b)] + 1e-20) / pw));
                } else {
                    if (c == 0) {
                        const auto L = spec[0][size_t(b)], R = spec[1][size_t(b)];
                        sLL[size_t(b)] = smooth * sLL[size_t(b)] + (1 - smooth) * std::norm(L);
                        sRR[size_t(b)] = smooth * sRR[size_t(b)] + (1 - smooth) * std::norm(R);
                        sLR[size_t(b)] = smooth * sLR[size_t(b)] + (1 - smooth) * L * std::conj(R);
                    }
                    // The share of this channel's level the other one shares: the cross level scaled to this channel's
                    // side (by how much louder it is) over its own. (Within 1.5 dB of agreeing counts as agreeing: the
                    // estimate wavers that much on clean sound.)
                    const double own = c == 0 ? sLL[size_t(b)] : sRR[size_t(b)];
                    const double shared = std::abs(sLR[size_t(b)]) * (c == 0 ? g : 1 / g);
                    if (own > 1e-20) gainDb = std::min(0.0, 20 * std::log10(shared / own / 0.85 + 1e-10));
                }
                gainDb = std::max(floorDb, gainDb * depth) * taper(b);
                const double take = 1 - std::pow(10.0, gainDb / 20);
                buf[size_t(b)] = X[size_t(b)] * take;
            }
            for (int b = top + 1; b < bins; ++b) buf[size_t(b)] = 0;
            for (int b = 1; b < N / 2; ++b) buf[size_t(N - b)] = std::conj(buf[size_t(b)]);
            fft(buf, true);
            const double scale = 1.0 / (1.5 * N);
            auto& rem = removed[size_t(c)];
            for (int i = 0; i < N; ++i) {
                const int64_t t = centre - N / 2 + i;
                if (t >= 0 && t < n) rem[size_t(t)] += buf[size_t(i)].real() * win[size_t(i)] * scale;
            }
        }
    }
    for (int c : outs)
        for (int64_t i = 0; i < n; ++i) out.samples[size_t(i) * size_t(nch) + size_t(c)] -= float(removed[size_t(c)][size_t(i)]);
}

}  // namespace montage
