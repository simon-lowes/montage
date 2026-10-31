#include "TapeRepair.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numeric>

namespace montage {

namespace {

bool cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(); }

using cd = std::complex<double>;

// In place, radix 2 (n a power of two); `inverse` unscaled.
void fft(std::vector<cd>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const cd step = std::polar(1.0, (inverse ? 2 : -2) * M_PI / double(len));
        for (size_t i = 0; i < n; i += len) {
            cd w = 1;
            for (size_t k = 0; k < len / 2; ++k, w *= step) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

std::vector<double> hann(size_t n) {
    std::vector<double> w(n);
    for (size_t i = 0; i < n; ++i) w[i] = 0.5 - 0.5 * std::cos(2 * M_PI * (double(i) + 0.5) / double(n));
    return w;
}

size_t powerOfTwoAtLeast(double v) {
    size_t n = 1;
    while (double(n) < v) n <<= 1;
    return n;
}

std::vector<float> mono(const AudioBuffer& b) {
    const size_t nch = size_t(std::max(1, b.channels));
    std::vector<float> x(static_cast<size_t>(b.frames()));
    for (size_t i = 0; i < x.size(); ++i) {
        double s = 0;
        for (size_t c = 0; c < nch; ++c) s += b.samples[i * nch + c];
        x[i] = float(s / double(nch));
    }
    return x;
}

// A windowed sinc (Blackman, 16 zero crossings each side) tabulated, read at fractional sample positions.
struct SincTable {
    static constexpr int kZeros = 16, kRes = 512;
    std::vector<float> k;
    SincTable() : k(size_t(kZeros * kRes) + 2) {
        for (size_t i = 0; i < k.size(); ++i) {
            const double u = double(i) / kRes;
            const double sinc = u == 0 ? 1 : std::sin(M_PI * u) / (M_PI * u);
            const double w = u >= kZeros ? 0 : 0.42 + 0.5 * std::cos(M_PI * u / kZeros) + 0.08 * std::cos(2 * M_PI * u / kZeros);
            k[i] = float(sinc * w);
        }
    }
    double tap(double u) const {
        u = std::fabs(u) * kRes;
        const size_t i = size_t(u);
        if (i + 1 >= k.size()) return 0;
        const double f = u - double(i);
        return k[i] + (k[i + 1] - k[i]) * f;
    }
    // Channel c of interleaved `x` (nch channels, n frames) at position `at`.
    double read(const float* x, size_t nch, size_t c, int64_t n, double at) const {
        const int64_t base = int64_t(std::floor(at));
        const double frac = at - double(base);
        if (frac == 0) return base >= 0 && base < n ? x[size_t(base) * nch + c] : 0.0;
        double sum = 0;
        for (int64_t s = std::max<int64_t>(0, base - kZeros + 1); s <= std::min<int64_t>(n - 1, base + kZeros); ++s)
            sum += tap(double(s) - at) * x[size_t(s) * nch + c];
        return sum;
    }
};

const SincTable& sincTable() {
    static const SincTable t;
    return t;
}

// Zero-phase second-order low-pass (forwards and backwards) at `fc` of rate `fs`.
std::vector<double> lowPass(const std::vector<double>& x, double fc, double fs) {
    const double w = 2 * M_PI * fc / fs, cw = std::cos(w), alpha = std::sin(w) / (2 * std::sqrt(0.5));
    const double a0 = 1 + alpha, b0 = (1 - cw) / 2 / a0, b1 = (1 - cw) / a0, b2 = b0, a1 = -2 * cw / a0, a2 = (1 - alpha) / a0;
    auto pass = [&](std::vector<double>& y) {
        double x1 = y.empty() ? 0 : y.front(), x2 = x1, y1 = x1, y2 = x1;  // (started settled at the first value)
        for (double& v : y) {
            const double out = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1, x1 = v, y2 = y1, y1 = out;
            v = out;
        }
    };
    std::vector<double> y = x;
    pass(y);
    std::reverse(y.begin(), y.end());
    pass(y);
    std::reverse(y.begin(), y.end());
    return y;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
    return v[v.size() / 2];
}

}  // namespace

double TransferSpeed::at(double seconds) const {
    if (speed.empty() || hop <= 0) return 1;
    const double i = (seconds - start) / hop;
    if (i <= 0) return speed.front();
    if (i >= double(speed.size() - 1)) return speed.back();
    const size_t k = size_t(i);
    return speed[k] + (speed[k + 1] - speed[k]) * (i - double(k));
}

TransferSpeed measureTransferSpeed(const AudioBuffer& in, double pilotHz, const std::atomic<bool>* cancel) {
    TransferSpeed out;
    const double fs = in.sampleRate;
    // (a mono sound read as it is: a copy of hours of it is gigabytes)
    std::vector<float> mixed;
    if (in.channels > 1) mixed = mono(in);
    const std::vector<float>& x = in.channels > 1 ? mixed : in.samples;
    const size_t N = powerOfTwoAtLeast(fs * 0.042);  // (2048 at 48 kHz: partials 50 Hz apart are told apart)
    const size_t H = size_t(std::max(1L, std::lround(fs * 0.004)));
    if (fs <= 0 || x.size() < N * 4) return out;
    // The steady partials: narrow peaks standing well out of the long-term spectrum (four times the frame, so a peak
    // is a few bins wide), the strongest few, at least 60 Hz apart.
    const size_t L = N * 4;
    std::vector<double> power(L / 2 + 1, 0.0);
    {
        const std::vector<double> w = hann(L);
        const size_t frames = (x.size() - L) / (L / 2) + 1, step = std::max<size_t>(1, frames / 2000);
        std::vector<cd> buf(L);
        for (size_t f = 0; f < frames; f += step) {
            if (cancelled(cancel)) return out;
            for (size_t i = 0; i < L; ++i) buf[i] = x[f * (L / 2) + i] * w[i];
            fft(buf, false);
            for (size_t k = 0; k <= L / 2; ++k) power[k] += std::norm(buf[k]);
        }
    }
    const double df = fs / double(L);
    std::vector<std::pair<double, size_t>> peaks;  // power, bin
    // (between 40 Hz and 8 kHz, or about a named pilot wherever it is)
    const size_t lo = size_t(std::ceil((pilotHz > 0 ? pilotHz * 0.97 : 40) / df)),
                 hi = std::min(L / 2 - 4, size_t((pilotHz > 0 ? pilotHz * 1.03 : 8000) / df) + 1);
    for (size_t k = std::max<size_t>(lo, 4); k < hi; ++k) {
        bool top = true;
        for (size_t j = k - 3; j <= k + 3 && top; ++j) top = j == k || power[j] < power[k];
        if (!top) continue;
        std::vector<double> around;
        for (size_t j = k > 64 ? k - 64 : 0; j <= std::min(L / 2, k + 64); ++j) around.push_back(power[j]);
        if (power[k] < 30 * median(around)) continue;
        peaks.emplace_back(power[k], k);
    }
    std::sort(peaks.begin(), peaks.end(), std::greater<>());
    std::vector<double> partials;
    for (const auto& [pw, k] : peaks) {
        // (the peak's frequency between bins, from the parabola through it and its neighbours, on a log scale)
        const double a = std::log(power[k - 1] + 1e-300), b = std::log(power[k] + 1e-300), c = std::log(power[k + 1] + 1e-300);
        const double off = std::clamp(0.5 * (a - c) / (a - 2 * b + c), -0.5, 0.5);
        const double f = (double(k) + off) * df;
        if (pilotHz > 0 && std::fabs(f / pilotHz - 1) > 0.03) continue;
        if (std::any_of(partials.begin(), partials.end(), [&](double g) { return std::fabs(g - f) < 60; })) continue;
        partials.push_back(f);
        if (partials.size() >= (pilotHz > 0 ? 1u : 8u)) break;
    }
    if (partials.empty()) return out;
    // Each partial's frequency hop by hop, from the phase its own DFT advances by.
    const std::vector<double> w = hann(N);
    const size_t hops = (x.size() - N) / H;
    if (hops < 2) return out;
    // (ratio, weight) for each hop, from each partial in turn; a weight of 0 where it did not vote
    const size_t P = partials.size();
    std::vector<std::pair<float, float>> votes(hops * P, {1.0f, 0.0f});
    for (size_t pi = 0; pi < P; ++pi) {
        const double f0 = partials[pi];
        if (cancelled(cancel)) return {};
        const double omega = 2 * M_PI * f0 / fs;
        std::vector<cd> kernel(N);
        for (size_t n = 0; n < N; ++n) kernel[n] = w[n] * std::polar(1.0, -omega * double(n));
        std::vector<double> freq(hops), mag(hops);
        cd prev = 0;
        for (size_t m = 0; m <= hops; ++m) {
            cd X = 0;
            if ((m & 1023) == 0 && cancelled(cancel)) return {};
            const float* p = x.data() + m * H;
            for (size_t n = 0; n < N; ++n) X += double(p[n]) * kernel[n];
            if (m > 0) {
                double d = std::arg(X) - std::arg(prev) - omega * double(H);
                d -= 2 * M_PI * std::round(d / (2 * M_PI));
                freq[m - 1] = f0 + d * fs / (2 * M_PI * double(H));
                mag[m - 1] = std::sqrt(std::abs(X) * std::abs(prev));
            }
            prev = X;
        }
        // Counted where it is within 20 dB of its loudest (it may come and go) and within 1 % of its usual frequency
        // (not a neighbour leaking into it while it is away), against that frequency.
        const double loud = *std::max_element(mag.begin(), mag.end());
        std::vector<double> present;
        for (size_t m = 0; m < hops; ++m)
            if (mag[m] >= 0.1 * loud) present.push_back(freq[m]);
        if (present.size() < hops / 5 || loud <= 0) continue;
        const double usual = median(present);
        for (size_t m = 0; m < hops; ++m)
            if (mag[m] >= 0.1 * loud && std::fabs(freq[m] / usual - 1) < 0.01) votes[m * P + pi] = {float(freq[m] / usual), float(mag[m] * mag[m])};
    }
    // Hops no partial reached take the speed either side of them.
    // Each hop's speed: the weighted median of its partials' (so one that strays is outvoted).
    std::vector<double> speed(hops, std::nan(""));
    std::vector<std::pair<double, double>> v;
    for (size_t m = 0; m < hops; ++m) {
        v.clear();
        for (size_t pi = 0; pi < P; ++pi)
            if (const auto& [r, w] = votes[m * P + pi]; w > 0) v.emplace_back(r, w);
        if (v.empty()) continue;
        std::sort(v.begin(), v.end());
        double total = 0;
        for (const auto& [r, w] : v) total += w;
        double run = 0;
        for (const auto& [r, w] : v)
            if ((run += w) >= total / 2) {
                speed[m] = r;
                break;
            }
    }
    size_t first = 0;
    while (first < hops && std::isnan(speed[first])) ++first;
    if (first == hops) return out;
    for (size_t m = 0; m < first; ++m) speed[m] = speed[first];
    for (size_t m = first + 1; m < hops; ++m) {
        if (!std::isnan(speed[m])) continue;
        size_t next = m;
        while (next < hops && std::isnan(speed[next])) ++next;
        const double a = speed[m - 1], b = next < hops ? speed[next] : a;
        for (size_t k = m; k < next; ++k) speed[k] = a + (b - a) * double(k - m + 1) / double(next - m + 1);
        m = next;
    }
    // (a median of five hops, 20 ms: a transient's single wild reading goes, flutter stays)
    {
        std::vector<double> steady(hops);
        for (size_t m = 0; m < hops; ++m) {
            std::vector<double> five;
            for (size_t k = m >= 2 ? m - 2 : 0; k <= std::min(hops - 1, m + 2); ++k) five.push_back(speed[k]);
            steady[m] = median(five);
        }
        speed = std::move(steady);
    }
    const double mean = std::accumulate(speed.begin(), speed.end(), 0.0) / double(hops);
    for (double& v : speed) v /= mean;
    out.hop = double(H) / fs;
    out.start = (double(N) / 2 + double(H) / 2) / fs;  // (between the two frames each value is from)
    out.speed = std::move(speed);
    return out;
}

bool removeWowFlutter(const AudioBuffer& in, AudioBuffer& out, double wow, double flutter, double pilotHz, const std::atomic<bool>* cancel,
                      double* deviation, bool* corrected) {
    if (deviation) *deviation = 0;
    if (corrected) *corrected = false;
    // (`in` as it is, unless the caller only wants to hear of a correction: then nothing is copied)
    auto unchanged = [&] {
        if (!corrected) out = in;
        return true;
    };
    const TransferSpeed sp = measureTransferSpeed(in, pilotHz, cancel);
    if (cancelled(cancel)) return false;
    if (sp.speed.empty()) return unchanged();
    // The deviation split at 4 Hz: wow below, flutter above, each by its own amount.
    std::vector<double> d(sp.speed.size());
    for (size_t i = 0; i < d.size(); ++i) d[i] = sp.speed[i] - 1;
    const std::vector<double> slow = lowPass(d, 4, 1 / sp.hop);
    double most = 0;
    std::vector<double> s(d.size()), size(d.size());
    for (size_t i = 0; i < d.size(); ++i) {
        const double v = std::clamp(wow, 0.0, 1.0) * slow[i] + std::clamp(flutter, 0.0, 1.0) * (d[i] - slow[i]);
        s[i] = 1 + v;
        most = std::max(most, std::fabs(d[i]));
        size[i] = std::fabs(v);
    }
    if (deviation) *deviation = most;
    // Nothing to put right when nine readings in ten are within 0.003 % (the odd one off is a note's onset, not the tape).
    std::nth_element(size.begin(), size.begin() + std::ptrdiff_t(size.size() * 9 / 10), size.end());
    if (size[size.size() * 9 / 10] < 3e-5) return unchanged();
    // Content time against tape time (the speed integrated), then each output sample read where its content was.
    const double fs = in.sampleRate;
    const int64_t n = in.frames();
    const size_t nch = size_t(std::max(1, in.channels));
    const double step = sp.hop * fs;  // samples per speed value
    TransferSpeed applyAt = sp;
    applyAt.speed = s;
    // tau at tape sample j * step, j = 0.. (trapezoids)
    const size_t points = size_t(std::ceil(double(n) / step)) + 2;
    std::vector<double> tau(points, 0.0);
    for (size_t j = 1; j < points; ++j) {
        const double t0 = double(j - 1) * step / fs, t1 = double(j) * step / fs;
        tau[j] = tau[j - 1] + 0.5 * (applyAt.at(t0) + applyAt.at(t1)) * step;
    }
    const SincTable& sinc = sincTable();
    out.sampleRate = in.sampleRate;
    out.channels = in.channels;
    out.samples.assign(in.samples.size(), 0.0f);
    if (corrected) *corrected = true;
    size_t j = 0;
    for (int64_t o = 0; o < n; ++o) {
        if ((o & 0xffff) == 0 && cancelled(cancel)) return false;
        const double want = double(o);
        while (j + 2 < points && tau[j + 1] < want) ++j;
        const double span = tau[j + 1] - tau[j];
        const double at = (double(j) + (span > 0 ? (want - tau[j]) / span : 0)) * step;
        for (size_t c = 0; c < nch; ++c) out.samples[size_t(o) * nch + c] = float(sinc.read(in.samples.data(), nch, c, n, at));
    }
    return true;
}

Azimuth measureAzimuth(const AudioBuffer& in, int64_t from, int64_t to) {
    Azimuth a;
    const size_t nch = size_t(std::max(1, in.channels));
    const int64_t n = in.frames();
    if (nch < 2) return a;
    from = std::clamp<int64_t>(from, 0, n), to = to < 0 ? n : std::clamp<int64_t>(to, from, n);
    constexpr size_t N = 4096;
    if (to - from < int64_t(N)) return a;
    const std::vector<double> w = hann(N);
    std::vector<cd> slr(N, 0.0), bl(N), br(N);
    std::vector<double> sll(N, 0.0), srr(N, 0.0);
    const size_t frames = size_t((to - from - int64_t(N)) / int64_t(N / 2)) + 1, every = std::max<size_t>(1, frames / 1500);
    for (size_t f = 0; f < frames; f += every) {
        const size_t at = size_t(from) + f * (N / 2);
        for (size_t i = 0; i < N; ++i) {
            bl[i] = in.samples[(at + i) * nch] * w[i];
            br[i] = in.samples[(at + i) * nch + 1] * w[i];
        }
        fft(bl, false);
        fft(br, false);
        for (size_t k = 0; k < N; ++k) {
            slr[k] += bl[k] * std::conj(br[k]);
            sll[k] += std::norm(bl[k]);
            srr[k] += std::norm(br[k]);
        }
    }
    // Whole samples: the generalised cross-correlation's peak (phase transform), at -D for a right channel D behind.
    std::vector<cd> g(N);
    double coh = 0, cohWeight = 0;
    for (size_t k = 0; k < N; ++k) {
        const double m = std::abs(slr[k]);
        g[k] = m > 0 ? slr[k] / m : 0.0;
        if (k > 0 && k < N / 2) {
            coh += m;
            cohWeight += std::sqrt(sll[k] * srr[k]);
        }
    }
    a.coherence = cohWeight > 0 ? coh / cohWeight : 0;
    fft(g, true);
    // (a head out of square is a millisecond off at most: the peak is looked for within 64 samples)
    size_t best = 0;
    for (size_t i = 0; i < N; ++i)
        if ((i <= 64 || i >= N - 64) && std::abs(g[i].real()) > std::abs(g[best].real())) best = i;
    a.inverted = g[best].real() < 0;
    double D = -(best < N / 2 ? double(best) : double(best) - double(N));
    // The fraction: the slope of the cross-spectrum's phase (D behind: phase 2 pi k D / N), weighted by how much of
    // each frequency the channels share; twice, the second about the first.
    for (int pass = 0; pass < 2; ++pass) {
        double num = 0, den = 0;
        for (size_t k = 1; k < N / 2 - N / 16; ++k) {
            const double e = sll[k] * srr[k];
            if (!(e > 0)) continue;
            const double gamma2 = std::norm(slr[k]) / e;
            if (gamma2 < 0.5) continue;
            const double omega = 2 * M_PI * double(k) / double(N);
            const cd turned = (a.inverted ? -1.0 : 1.0) * slr[k] * std::polar(1.0, -omega * D);
            const double wt = gamma2 * std::abs(slr[k]);
            num += wt * omega * std::arg(turned);
            den += wt * omega * omega;
        }
        if (den > 0) D += num / den;
    }
    a.delay = D;
    return a;
}

bool correctAzimuth(const AudioBuffer& in, AudioBuffer& out, bool adaptive, bool polarity, const std::atomic<bool>* cancel, double* delay) {
    out = in;
    const size_t nch = size_t(std::max(1, in.channels));
    const int64_t n = in.frames();
    if (delay) *delay = 0;
    if (nch != 2 || n < 4096) return true;  // (two channels: an ambisonic field's are not a tape's two tracks)
    const Azimuth whole = measureAzimuth(in);
    if (cancelled(cancel)) return false;
    // (two channels with little in common, a spaced pair or two microphones, are not one track out of line)
    if (whole.coherence < 0.3) return true;
    if (delay) *delay = whole.delay;
    // Adaptive: a delay every half second from the two seconds around it (where the channels share enough), steadied
    // by a median of five, and between them a straight line.
    const int64_t hop = std::max<int64_t>(1, in.sampleRate / 2), win = int64_t(in.sampleRate) * 2;
    std::vector<double> at, d;
    if (adaptive) {
        for (int64_t c = 0; c < n; c += hop) {
            if (cancelled(cancel)) return false;
            const Azimuth here = measureAzimuth(in, c - win / 2, c + win / 2);
            at.push_back(double(c));
            d.push_back(here.coherence > 0.3 && std::fabs(here.delay - whole.delay) < 8 ? here.delay : whole.delay);
        }
        std::vector<double> steady(d.size());
        for (size_t i = 0; i < d.size(); ++i) {
            std::vector<double> five;
            for (size_t k = i >= 2 ? i - 2 : 0; k <= std::min(d.size() - 1, i + 2); ++k) five.push_back(d[k]);
            steady[i] = median(five);
        }
        d = std::move(steady);
    }
    auto delayAt = [&](int64_t o) {
        if (!adaptive || d.empty()) return whole.delay;
        const double i = double(o) / double(hop);
        if (i >= double(d.size() - 1)) return d.back();
        const size_t k = size_t(i);
        return d[k] + (d[k + 1] - d[k]) * (i - double(k));
    };
    const double sign = polarity && whole.inverted ? -1.0 : 1.0;
    if (!adaptive && std::fabs(whole.delay) < 1e-4 && sign > 0) return true;
    const SincTable& sinc = sincTable();
    for (int64_t o = 0; o < n; ++o) {
        if ((o & 0xffff) == 0 && cancelled(cancel)) return false;
        out.samples[size_t(o) * nch + 1] = float(sign * sinc.read(in.samples.data(), nch, 1, n, double(o) + delayAt(o)));
    }
    return true;
}

std::vector<std::pair<int64_t, int64_t>> soundRegions(const AudioBuffer& in, double thresholdDb, double minSilence, double padBefore,
                                                      double padAfter) {
    std::vector<std::pair<int64_t, int64_t>> out;
    const size_t nch = size_t(std::max(1, in.channels));
    const int64_t n = in.frames();
    const double fs = in.sampleRate;
    const float thr = float(std::pow(10.0, thresholdDb / 20));
    const int64_t gap = std::max<int64_t>(1, std::llround(std::max(0.0, minSilence) * fs));
    int64_t start = -1, last = -1;
    for (int64_t i = 0; i < n; ++i) {
        bool loud = false;
        for (size_t c = 0; c < nch && !loud; ++c) loud = std::fabs(in.samples[size_t(i) * nch + c]) > thr;
        if (!loud) continue;
        if (start >= 0 && i - last - 1 >= gap) {
            out.emplace_back(start, last + 1);
            start = -1;
        }
        if (start < 0) start = i;
        last = i;
    }
    if (start >= 0) out.emplace_back(start, last + 1);
    // (a quiet start or end shorter than the shortest silence stays)
    if (!out.empty() && out.front().first < gap) out.front().first = 0;
    if (!out.empty() && n - out.back().second < gap) out.back().second = n;
    const int64_t before = std::llround(std::max(0.0, padBefore) * fs), after = std::llround(std::max(0.0, padAfter) * fs);
    std::vector<std::pair<int64_t, int64_t>> padded;
    for (auto [a, b] : out) {
        a = std::max<int64_t>(0, a - before), b = std::min(n, b + after);
        if (!padded.empty() && a <= padded.back().second) padded.back().second = std::max(padded.back().second, b);
        else padded.emplace_back(a, b);
    }
    return padded;
}

std::vector<std::pair<FrameTime, FrameTime>> clipSoundFrames(const Sequence& s, const Clip& c, const std::vector<std::pair<int64_t, int64_t>>& regions,
                                                             int rate) {
    std::vector<std::pair<FrameTime, FrameTime>> out;
    if (c.speed != 1 || c.reverse || c.ramped() || rate <= 0) return out;
    const double fps = s.fpsValue();
    for (const auto& [a, b] : regions) {
        const double fa = double(c.start) + double(a) / rate * fps - c.sourceIn, fb = double(c.start) + double(b) / rate * fps - c.sourceIn;
        const FrameTime x = std::max(c.start, FrameTime(std::floor(fa + 1e-9))), y = std::min(c.end(), FrameTime(std::ceil(fb - 1e-9)));
        if (y <= x) continue;
        if (!out.empty() && x <= out.back().second) out.back().second = std::max(out.back().second, y);
        else out.emplace_back(x, y);
    }
    return out;
}

}  // namespace montage
