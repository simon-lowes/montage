#include "AudioRepair.h"

#include <algorithm>
#include <cmath>
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

// Least-squares AR interpolation of x[s, e) from the sound around it.
void rebuild(std::vector<float>& x, int64_t s, int64_t e) {
    const int p = kOrder;
    const int64_t L = e - s, n = int64_t(x.size());
    std::vector<double> r(size_t(p) + 1, 0.0);
    addAutocorrelation(x, s - 1024, s, p, r);
    addAutocorrelation(x, e, e + 1024, p, r);
    if (r[0] <= 0) {
        for (int64_t i = s; i < e; ++i) x[size_t(i)] = 0;
        return;
    }
    const std::vector<double> a = levinson(r, p);
    // Normal equations M u = rhs: M is Toeplitz in the filter's own autocorrelation.
    std::vector<double> ra(size_t(p) + 1, 0.0);
    for (int d = 0; d <= p; ++d)
        for (int k = 0; k + d <= p; ++k) ra[size_t(d)] += a[size_t(k)] * a[size_t(k + d)];
    // The prediction errors with the unknowns at zero.
    for (int64_t i = s; i < e; ++i) x[size_t(i)] = 0;
    std::vector<double> b(static_cast<size_t>(L + p), 0.0);
    for (int64_t m = 0; m < L + p; ++m) {
        const int64_t t = s + m;
        if (t >= n) break;
        double v = 0;
        for (int k = 0; k <= p && t - k >= 0; ++k) v += a[size_t(k)] * x[size_t(t - k)];
        b[size_t(m)] = v;
    }
    std::vector<double> M(static_cast<size_t>(L * L)), rhs(static_cast<size_t>(L), 0.0);
    for (int64_t i = 0; i < L; ++i) {
        for (int64_t j = 0; j < L; ++j) {
            const int64_t d = std::abs(i - j);
            M[size_t(i * L + j)] = d <= p ? ra[size_t(d)] : 0.0;
        }
        M[size_t(i * L + i)] += 1e-9 * ra[0];
        for (int k = 0; k <= p && i + k < L + p; ++k) rhs[size_t(i)] -= a[size_t(k)] * b[size_t(i + k)];
    }
    // Cholesky (M is symmetric positive definite).
    for (int64_t j = 0; j < L; ++j) {
        double d = M[size_t(j * L + j)];
        for (int64_t k = 0; k < j; ++k) d -= M[size_t(j * L + k)] * M[size_t(j * L + k)];
        d = std::sqrt(std::max(d, 1e-30));
        M[size_t(j * L + j)] = d;
        for (int64_t i = j + 1; i < L; ++i) {
            double v = M[size_t(i * L + j)];
            for (int64_t k = 0; k < j; ++k) v -= M[size_t(i * L + k)] * M[size_t(j * L + k)];
            M[size_t(i * L + j)] = v / d;
        }
    }
    for (int64_t i = 0; i < L; ++i) {
        double v = rhs[size_t(i)];
        for (int64_t k = 0; k < i; ++k) v -= M[size_t(i * L + k)] * rhs[size_t(k)];
        rhs[size_t(i)] = v / M[size_t(i * L + i)];
    }
    for (int64_t i = L - 1; i >= 0; --i) {
        double v = rhs[size_t(i)];
        for (int64_t k = i + 1; k < L; ++k) v -= M[size_t(k * L + i)] * rhs[size_t(k)];
        rhs[size_t(i)] = v / M[size_t(i * L + i)];
    }
    for (int64_t i = 0; i < L; ++i) x[size_t(s + i)] = float(std::clamp(rhs[size_t(i)], -4.0, 4.0));
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

}  // namespace montage
