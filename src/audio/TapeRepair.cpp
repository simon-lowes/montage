#include "TapeRepair.h"

#include "media/Image.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <functional>
#include <memory>
#include <mutex>
#include <limits>
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

// An FFT as fft() makes it (to rounding: each twiddle read from `twiddle`, e^(-2 pi i k / L) for an FFT of L / 2 as
// realPower's, rather than fft()'s running products), quicker: the complex products written out (std::complex's
// multiply goes through a library call that checks for infinities).
void fftQuick(std::vector<cd>& a, const std::vector<cd>& twiddle, size_t L) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    double* d = reinterpret_cast<double*>(a.data());
    const double* t = reinterpret_cast<const double*>(twiddle.data());
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t stride = L / len;
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k) {
                double* u = d + 2 * (i + k);
                double* v = d + 2 * (i + k + len / 2);
                const double wr = t[2 * k * stride], wi = t[2 * k * stride + 1];
                const double vr = v[0] * wr - v[1] * wi, vi = v[0] * wi + v[1] * wr;
                v[0] = u[0] - vr, v[1] = u[1] - vi;
                u[0] += vr, u[1] += vi;
            }
    }
}

// |X[k]|^2, k = 0..L/2, of the DFT of L real samples `x` (L a power of two, 4 or more): one complex FFT of L/2 (the
// even samples as the real part, the odd as the imaginary), split into the two halves' spectra and joined. `z` and
// `twiddle` (e^(-2 pi i k / L), k < L/2) are the caller's, so a run of frames allocates nothing.
void realPower(const double* x, size_t L, const std::vector<cd>& twiddle, std::vector<cd>& z, double* power) {
    const size_t M = L / 2;
    z.resize(M);
    for (size_t n = 0; n < M; ++n) z[n] = {x[2 * n], x[2 * n + 1]};
    fftQuick(z, twiddle, L);
    for (size_t k = 0; k <= M; ++k) {
        const cd a = z[k % M], b = std::conj(z[(M - k) % M]);
        const cd even = 0.5 * (a + b), odd = cd(0, -0.5) * (a - b);
        power[k] = std::norm(even + (k < M ? twiddle[k] : cd(-1, 0)) * odd);
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
    // Every channel at `at` into `out` (nch floats), the taps worked out once: the same as read() for each, to rounding.
    void readAll(const float* x, size_t nch, int64_t n, double at, float* out) const {
        const int64_t base = int64_t(std::floor(at));
        const double frac = at - double(base);
        if (frac == 0) {
            for (size_t c = 0; c < nch; ++c) out[c] = float(base >= 0 && base < n ? double(x[size_t(base) * nch + c]) : 0.0);
            return;
        }
        const int64_t s0 = std::max<int64_t>(0, base - kZeros + 1), s1 = std::min<int64_t>(n - 1, base + kZeros);
        // (the taps a whole number of zero crossings apart share their place between table entries: at - s is
        // frac + (base - s) up to `base`, and (1 - frac) + (s - base - 1) after it, each under 16, so never past the table)
        double taps[2 * kZeros];
        const double left = frac * kRes, right = (1 - frac) * kRes;
        const size_t li = size_t(left), ri = size_t(right);
        const double lf = left - double(li), rf = right - double(ri);
        for (int64_t s = s0; s <= s1; ++s) {
            const size_t i = s <= base ? li + size_t(base - s) * kRes : ri + size_t(s - base - 1) * kRes;
            const double f = s <= base ? lf : rf;
            taps[s - s0] = k[i] + (k[i + 1] - k[i]) * f;
        }
        for (size_t c = 0; c < nch; ++c) {
            double sum = 0;
            for (int64_t s = s0; s <= s1; ++s) sum += taps[s - s0] * x[size_t(s) * nch + c];
            out[c] = float(sum);
        }
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

std::atomic<bool> gAsBefore{false};

// Progress over [from, to) of a caller's 0..1, never going back (bands finish out of order); safe from any thread.
struct Progress {
    const std::function<void(double)>* fn = nullptr;
    double from = 0, to = 1;
    std::shared_ptr<std::mutex> m = std::make_shared<std::mutex>();
    std::shared_ptr<double> last = std::make_shared<double>(-1);
    void operator()(double f) const {
        if (!fn || !*fn) return;
        const double v = from + (to - from) * std::clamp(f, 0.0, 1.0);
        std::lock_guard l(*m);
        if (v <= *last) return;
        *last = v;
        (*fn)(v);
    }
    Progress part(double a, double b) const {
        Progress p = *this;
        p.from = from + (to - from) * a, p.to = from + (to - from) * b;
        return p;
    }
};

// fn(i0, i1) over 0..count in runs across the cores, however few (parallelRows gives a core 16 rows or more: here 16
// rows an item).
void acrossCores(size_t count, const std::function<void(size_t, size_t)>& fn) {
    const int rows = int(std::min<size_t>(count, size_t(std::numeric_limits<int>::max() / 16)) * 16);
    parallelRows(rows, [&](int r0, int r1) {
        const size_t i0 = (size_t(r0) + 15) / 16, i1 = (size_t(r1) + 15) / 16;
        if (i0 < i1) fn(i0, i1);
    });
}

// The channels of interleaved `data` (C of them) mixed, sample i.
inline double mixed(const float* data, size_t C, size_t i) {
    if (C == 1) return double(data[i]);
    double sum = 0;
    for (size_t c = 0; c < C; ++c) sum += data[i * C + c];
    return sum / double(C);
}

// Each hop m's (0..hops) Hann-windowed DFT of N samples from m * H at each partial's frequency, into at[m * P + p]:
// the sum as written, each hop and partial on its own (how it was first made; kept to check the quicker one against).
bool directPartialDfts(const float* data, size_t C, size_t N, size_t H, size_t hops, const std::vector<double>& partials, double fs,
                       const std::atomic<bool>* cancel, const Progress& report, std::vector<cd>& at) {
    const size_t P = partials.size();
    at.assign((hops + 1) * P, 0.0);
    const std::vector<double> w = hann(N);
    for (size_t pi = 0; pi < P; ++pi) {
        if (cancelled(cancel)) return false;
        const double omega = 2 * M_PI * partials[pi] / fs;
        std::vector<cd> kernel(N);
        for (size_t n = 0; n < N; ++n) kernel[n] = w[n] * std::polar(1.0, -omega * double(n));
        std::atomic<bool> stop{false};
        const int bands = int(std::min<size_t>((hops + 1 + 1023) / 1024, size_t(std::numeric_limits<int>::max())));
        parallelRows(bands, [&](int b0, int b1) {
            for (size_t m = size_t(b0) * 1024; m < std::min(hops + 1, size_t(b1) * 1024); ++m) {
                if ((m & 1023) == 0 && (stop || cancelled(cancel))) {
                    stop = true;
                    return;
                }
                cd X = 0;
                if (C == 1) {
                    const float* p = data + m * H;
                    for (size_t n = 0; n < N; ++n) X += double(p[n]) * kernel[n];
                } else {
                    for (size_t n = 0; n < N; ++n) X += mixed(data, C, m * H + n) * kernel[n];
                }
                at[m * P + pi] = X;
            }
        });
        if (stop) return false;
        report(double(pi + 1) / double(P));
    }
    return true;
}

// The same sums, quicker and the same to rounding: the Hann window is three complex exponentials (0.5 at the partial's
// frequency, -0.25 e^(+-i pi / N) a bin either side), so each hop's windowed DFT is three plain DFTs, and a plain DFT of
// N samples from m * H is the sum, turned by each block's offset, of the DFTs of the H-sample blocks it covers (whole
// ones, then the first N mod H samples of the next): blocks every overlapping hop shares, worked out once for all the
// partials together. Hops in bands of 1024 across the cores, each band mixing its own samples.
bool partialDfts(const float* data, size_t C, size_t N, size_t H, size_t hops, const std::vector<double>& partials, double fs,
                 const std::atomic<bool>* cancel, const Progress& report, std::vector<cd>& at) {
    const size_t P = partials.size(), K = 3 * P;  // frequency f = 3 p + r: the partial's, a bin below (r 0) and above (r 2)
    const size_t Q = N / H, R = N - Q * H;
    at.assign((hops + 1) * P, 0.0);
    std::vector<double> omega(K);
    for (size_t p = 0; p < P; ++p)
        for (size_t r = 0; r < 3; ++r) omega[3 * p + r] = 2 * M_PI * partials[p] / fs + (double(r) - 1) * 2 * M_PI / double(N);
    const cd weight[3] = {-0.25 * std::polar(1.0, M_PI / double(N)), 0.5, -0.25 * std::polar(1.0, -M_PI / double(N))};
    // e^(-i w n) within a block (n < H), and e^(-i w q H) for its place in the window (q <= Q), frequency fastest.
    std::vector<double> er(H * K), ei(H * K);
    std::vector<cd> turn((Q + 1) * K);
    for (size_t n = 0; n < H; ++n)
        for (size_t f = 0; f < K; ++f) {
            const cd e = std::polar(1.0, -omega[f] * double(n));
            er[n * K + f] = e.real(), ei[n * K + f] = e.imag();
        }
    for (size_t q = 0; q <= Q; ++q)
        for (size_t f = 0; f < K; ++f) turn[q * K + f] = std::polar(1.0, -omega[f] * double(q * H));
    // (each core a run of hops, worked through 1024 at a time: the band's blocks, then its hops)
    constexpr size_t kBand = 1024;
    std::atomic<bool> stop{false};
    std::atomic<size_t> done{0};
    parallelRows(int(std::min<size_t>(hops + 1, size_t(std::numeric_limits<int>::max()))), [&](int h0, int h1) {
        std::vector<double> x;
        std::vector<cd> whole, part;  // each block's DFT (blocks of the band), and of its first R samples
        std::vector<double> sr(K), si(K), pr(K), pi_(K);
        for (size_t m0 = size_t(h0); m0 < size_t(h1); m0 += kBand) {
            if (stop || cancelled(cancel)) {
                stop = true;
                return;
            }
            const size_t m1 = std::min(size_t(h1), m0 + kBand);
            const size_t blocks = m1 - m0 + Q;  // blocks m0 .. m1 - 1 + Q (the last ones' R samples only, perhaps)
            const size_t first = m0 * H, samples = (m1 - 1) * H + N - first;  // (the last block's past R are not needed)
            x.resize(blocks * H);
            for (size_t i = 0; i < samples; ++i) x[i] = mixed(data, C, first + i);
            std::fill(x.begin() + std::ptrdiff_t(samples), x.end(), 0.0);
            whole.assign(blocks * K, 0.0);
            part.assign(blocks * K, 0.0);
            for (size_t j = 0; j < blocks; ++j) {
                std::fill(sr.begin(), sr.end(), 0.0);
                std::fill(si.begin(), si.end(), 0.0);
                const double* xs = x.data() + j * H;
                for (size_t n = 0; n < H; ++n) {
                    if (n == R) pr = sr, pi_ = si;
                    const double v = xs[n];
                    const double* a = er.data() + n * K;
                    const double* b = ei.data() + n * K;
                    for (size_t f = 0; f < K; ++f) sr[f] += v * a[f], si[f] += v * b[f];
                }
                for (size_t f = 0; f < K; ++f) whole[j * K + f] = {sr[f], si[f]}, part[j * K + f] = {pr[f], pi_[f]};
            }
            // (products written out: std::complex's goes through a library call that checks for infinities)
            const auto mul = [](const cd& a, const cd& b) { return cd(a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real()); };
            std::vector<cd> sum(K);
            for (size_t m = m0; m < m1; ++m) {
                const size_t j0 = m - m0;
                for (size_t f = 0; f < K; ++f) sum[f] = mul(turn[Q * K + f], part[(j0 + Q) * K + f]);
                for (size_t q = 0; q < Q; ++q) {
                    const cd* t = turn.data() + q * K;
                    const cd* b = whole.data() + (j0 + q) * K;
                    for (size_t f = 0; f < K; ++f) sum[f] += mul(t[f], b[f]);
                }
                for (size_t p = 0; p < P; ++p)
                    at[m * P + p] = weight[0] * sum[3 * p] + weight[1] * sum[3 * p + 1] + weight[2] * sum[3 * p + 2];
            }
            report(double(done.fetch_add(m1 - m0) + (m1 - m0)) / double(hops + 1));
        }
    });
    return !stop;
}

}  // namespace

void tapeRepairAsBefore(bool before) { gAsBefore = before; }

double TransferSpeed::at(double seconds) const {
    if (speed.empty() || hop <= 0) return 1;
    const double i = (seconds - start) / hop;
    if (i <= 0) return speed.front();
    if (i >= double(speed.size() - 1)) return speed.back();
    const size_t k = size_t(i);
    return speed[k] + (speed[k + 1] - speed[k]) * (i - double(k));
}

namespace {

TransferSpeed measureSpeed(const AudioBuffer& in, double pilotHz, const std::atomic<bool>* cancel, const Progress& report) {
    TransferSpeed out;
    const double fs = in.sampleRate;
    // The channels mixed as they are read (a mixed copy of hours of a sound is gigabytes).
    const size_t C = size_t(std::max(1, in.channels)), length = in.samples.size() / C;
    const float* data = in.samples.data();
    auto x = [data, C](size_t i) { return mixed(data, C, i); };
    const size_t N = powerOfTwoAtLeast(fs * 0.042);  // (2048 at 48 kHz: partials 50 Hz apart are told apart)
    const size_t H = size_t(std::max(1L, std::lround(fs * 0.004)));
    if (fs <= 0 || length < N * 4) return out;
    // The steady partials: narrow peaks standing well out of the long-term spectrum (four times the frame, so a peak
    // is a few bins wide), the strongest few, at least 60 Hz apart.
    const size_t L = N * 4;
    std::vector<double> power(L / 2 + 1, 0.0);
    {
        const std::vector<double> w = hann(L);
        const size_t frames = (length - L) / (L / 2) + 1, step = std::max<size_t>(1, frames / 2000);
        if (gAsBefore) {
            std::vector<cd> buf(L);
            for (size_t f = 0; f < frames; f += step) {
                if (cancelled(cancel)) return out;
                report(0.1 * double(f) / double(frames));
                for (size_t i = 0; i < L; ++i) buf[i] = x(f * (L / 2) + i) * w[i];
                fft(buf, false);
                for (size_t k = 0; k <= L / 2; ++k) power[k] += std::norm(buf[k]);
            }
        }
        // (64 frames at a time across the cores, each through a real FFT of half its length, then added up in their
        // order)
        constexpr size_t kChunk = 64;
        std::vector<cd> twiddle(L / 2);
        for (size_t k = 0; k < L / 2; ++k) twiddle[k] = std::polar(1.0, -2 * M_PI * double(k) / double(L));
        std::vector<double> each(kChunk * (L / 2 + 1));
        std::vector<size_t> picked;
        if (!gAsBefore)
            for (size_t f = 0; f < frames; f += step) picked.push_back(f);
        for (size_t c0 = 0; c0 < picked.size(); c0 += kChunk) {
            if (cancelled(cancel)) return out;
            report(0.1 * double(c0) / double(picked.size()));
            const size_t count = std::min(kChunk, picked.size() - c0);
            std::atomic<bool> stop{false};
            acrossCores(count, [&](size_t i0, size_t i1) {
                std::vector<double> buf(L);
                std::vector<cd> z;
                for (size_t i = i0; i < i1; ++i) {
                    if (stop || cancelled(cancel)) {
                        stop = true;
                        return;
                    }
                    const size_t f = picked[c0 + i];
                    for (size_t j = 0; j < L; ++j) buf[j] = x(f * (L / 2) + j) * w[j];
                    realPower(buf.data(), L, twiddle, z, each.data() + i * (L / 2 + 1));
                }
            });
            if (stop) return out;
            for (size_t i = 0; i < count; ++i) {
                const double* p = each.data() + i * (L / 2 + 1);
                for (size_t k = 0; k <= L / 2; ++k) power[k] += p[k];
            }
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
    const size_t hops = (length - N) / H;
    if (hops < 2) return out;
    const size_t P = partials.size();
    report(0.1);
    std::vector<cd> at;  // (hops + 1) x P: each hop's DFT at each partial's frequency
    const Progress dfts = report.part(0.1, 0.9);
    if (!(gAsBefore ? directPartialDfts(data, C, N, H, hops, partials, fs, cancel, dfts, at)
                     : partialDfts(data, C, N, H, hops, partials, fs, cancel, dfts, at)))
        return {};
    report(0.9);
    // (ratio, weight) for each hop, from each partial in turn; a weight of 0 where it did not vote
    std::vector<std::pair<float, float>> votes(hops * P, {1.0f, 0.0f});
    for (size_t pi = 0; pi < P; ++pi) {
        const double f0 = partials[pi];
        const double omega = 2 * M_PI * f0 / fs;
        std::vector<double> freq(hops), mag(hops);
        for (size_t m = 1; m <= hops; ++m) {
            const cd a = at[(m - 1) * P + pi], b = at[m * P + pi];
            double d = std::arg(b) - std::arg(a) - omega * double(H);
            d -= 2 * M_PI * std::round(d / (2 * M_PI));
            freq[m - 1] = f0 + d * fs / (2 * M_PI * double(H));
            mag[m - 1] = std::sqrt(std::abs(b) * std::abs(a));
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

}  // namespace

TransferSpeed measureTransferSpeed(const AudioBuffer& in, double pilotHz, const std::atomic<bool>* cancel,
                                   const std::function<void(double)>& progress) {
    Progress report;
    report.fn = &progress;
    TransferSpeed sp = measureSpeed(in, pilotHz, cancel, report);
    if (!sp.speed.empty()) report(1);
    return sp;
}

bool removeWowFlutter(const AudioBuffer& in, AudioBuffer& out, double wow, double flutter, double pilotHz, const std::atomic<bool>* cancel,
                      double* deviation, bool* corrected, const std::function<void(double)>& progress) {
    Progress report;
    report.fn = &progress;
    if (deviation) *deviation = 0;
    if (corrected) *corrected = false;
    // (`in` as it is, unless the caller only wants to hear of a correction: then nothing is copied)
    auto unchanged = [&] {
        if (!corrected) out = in;
        return true;
    };
    // (the speed read in the first half of the time, the sound resampled in the second)
    const TransferSpeed sp = measureSpeed(in, pilotHz, cancel, report.part(0, 0.5));
    if (cancelled(cancel)) return false;
    auto finish = [&](bool r) {
        if (r) report(1);
        return r;
    };
    if (sp.speed.empty()) return finish(unchanged());
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
    if (size[size.size() * 9 / 10] < 3e-5) return finish(unchanged());
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
    if (gAsBefore) {
        size_t j = 0;
        for (int64_t o = 0; o < n; ++o) {
            if ((o & 0xffff) == 0 && cancelled(cancel)) return false;
            const double want = double(o);
            while (j + 2 < points && tau[j + 1] < want) ++j;
            const double span = tau[j + 1] - tau[j];
            const double at = (double(j) + (span > 0 ? (want - tau[j]) / span : 0)) * step;
            for (size_t c = 0; c < nch; ++c) out.samples[size_t(o) * nch + c] = float(sinc.read(in.samples.data(), nch, c, n, at));
        }
        return finish(true);
    }
    // (the output in bands across the cores, each starting where the content it reads is found: tau only rises)
    const Progress resampled = report.part(0.5, 1);
    constexpr int64_t kBand = 1 << 12;  // (parallelRows gives each core 16 bands or more)
    const int64_t bandCount = (n + kBand - 1) / kBand;
    std::atomic<bool> stop{false};
    std::atomic<int64_t> done{0};
    parallelRows(int(std::min<int64_t>(bandCount, std::numeric_limits<int>::max())), [&](int b0, int b1) {
        for (int64_t band = b0; band < b1; ++band) {
            if (stop || cancelled(cancel)) {
                stop = true;
                return;
            }
            const int64_t o0 = band * kBand, o1 = std::min(n, o0 + kBand);
            // (the first j with tau[j + 1] >= o0, as a walk from the start finds it, at most points - 2)
            size_t j = size_t(std::lower_bound(tau.begin() + 1, tau.end(), double(o0)) - tau.begin()) - 1;
            j = std::min(j, points - 2);
            for (int64_t o = o0; o < o1; ++o) {
                const double want = double(o);
                while (j + 2 < points && tau[j + 1] < want) ++j;
                const double span = tau[j + 1] - tau[j];
                const double at = (double(j) + (span > 0 ? (want - tau[j]) / span : 0)) * step;
                sinc.readAll(in.samples.data(), nch, n, at, out.samples.data() + size_t(o) * nch);
            }
            resampled(double(done.fetch_add(1) + 1) / double(bandCount));
        }
    });
    if (stop) return false;
    return finish(true);
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
