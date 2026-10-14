#include "RoomResponse.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <unordered_map>

#include "SpeechCleanup.h"

namespace montage {

using cfloat = std::complex<float>;

const std::array<double, kRoomBands>& roomBandCentres() {
    static const std::array<double, kRoomBands> c{{125, 250, 500, 1000, 2000, 4000, 8000}};
    return c;
}

// ---- FFT ------------------------------------------------------------------------

namespace {

// e^(-2πik/N) for k < N/2, made once per size and thread.
const std::vector<cfloat>& twiddles(size_t n) {
    thread_local std::unordered_map<size_t, std::vector<cfloat>> tables;
    auto& t = tables[n];
    if (t.empty()) {
        t.resize(std::max<size_t>(1, n / 2));
        for (size_t k = 0; k < n / 2; ++k) t[k] = cfloat(std::polar(1.0, -2 * M_PI * double(k) / double(n)));
    }
    return t;
}

// An RBJ biquad (direct form I, double precision).
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    static Biquad make(int kind, double f, double q, double rate) {  // 0 band-pass, 1 low-pass, 2 high-pass
        Biquad b;
        f = std::clamp(f, 10.0, 0.45 * rate);
        const double w = 2 * M_PI * f / rate, cw = std::cos(w), alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
        if (kind == 0) b.b0 = alpha, b.b1 = 0, b.b2 = -alpha;
        else if (kind == 1) b.b0 = (1 - cw) / 2, b.b1 = 1 - cw, b.b2 = (1 - cw) / 2;
        else b.b0 = (1 + cw) / 2, b.b1 = -(1 + cw), b.b2 = (1 + cw) / 2;
        b.b0 /= a0, b.b1 /= a0, b.b2 /= a0;
        b.a1 = -2 * cw / a0, b.a2 = (1 - alpha) / a0;
        return b;
    }
    double operator()(double x) {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1, x1 = x, y2 = y1, y1 = y;
        return y;
    }
};

// The octave around `centre`, twice through a band-pass (Q √2 spans an octave); the lowest band is everything below
// its upper edge and the highest everything above its lower edge.
std::vector<float> octaveBand(const std::vector<float>& x, int rate, int band) {
    const double centre = roomBandCentres()[size_t(band)];
    const int kind = band == 0 ? 1 : band == kRoomBands - 1 ? 2 : 0;
    const double f = kind == 1 ? centre * M_SQRT2 : kind == 2 ? centre / M_SQRT2 : centre;
    const double q = kind == 0 ? M_SQRT2 : M_SQRT1_2;
    Biquad a = Biquad::make(kind, f, q, rate), b = a;
    std::vector<float> y(x.size());
    for (size_t i = 0; i < x.size(); ++i) y[i] = float(b(a(x[i])));
    return y;
}

}  // namespace

void fftInPlace(std::vector<cfloat>& a, bool inverse) {
    const size_t n = a.size();
    if (n < 2) return;
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    const auto& tw = twiddles(n);
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t half = len / 2, step = n / len;
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < half; ++k) {
                const cfloat w = inverse ? std::conj(tw[k * step]) : tw[k * step];
                const cfloat u = a[i + k], v = a[i + k + half] * w;
                a[i + k] = u + v;
                a[i + k + half] = u - v;
            }
    }
}

// ---- Rooms ----------------------------------------------------------------------

std::vector<float> roomImpulse(int sampleRate, const RoomShape& room, uint32_t seed) {
    const int sr = std::max(8000, sampleRate);
    double longest = 0.05;
    for (double t : room.rt60) longest = std::max(longest, t);
    const size_t pre = size_t(std::clamp(room.predelayMs, 0.0, 500.0) * 1e-3 * sr);
    const size_t len = size_t(std::min(10.0, 1.2 * longest) * sr);
    std::mt19937 rng(seed * 2654435761u + 12345u);
    std::normal_distribution<float> normal;
    std::vector<float> noise(len), out(pre + len, 0.0f);
    for (int b = 0; b < kRoomBands; ++b) {
        if (b == kRoomBands - 1 && roomBandCentres()[size_t(b)] / M_SQRT2 > 0.45 * sr) break;
        for (auto& v : noise) v = normal(rng);
        const std::vector<float> band = octaveBand(noise, sr, b);
        // The amplitude falls 60 dB over the band's RT60.
        const double rate = 6.907755 / std::max(0.05, room.rt60[size_t(b)]);
        for (size_t i = 0; i < len; ++i) out[pre + i] += band[i] * float(std::exp(-rate * double(i) / sr));
    }
    // A 2 ms fade in, so the reverberation does not start with a click.
    const size_t fade = std::min(len, size_t(0.002 * sr));
    for (size_t i = 0; i < fade; ++i) out[pre + i] *= float(0.5 - 0.5 * std::cos(M_PI * double(i + 1) / double(fade + 1)));
    double energy = 0;
    for (float v : out) energy += double(v) * v;
    if (energy > 0)
        for (auto& v : out) v = float(v / std::sqrt(energy));
    return out;
}

bool loadImpulseResponse(const std::string& path, int sampleRate, std::vector<float>& left, std::vector<float>& right,
                         std::string* error) {
    std::string err;
    AudioBufferPtr a = decodeAudio(path, sampleRate, &err);
    if (!a || a->frames() == 0) {
        if (error) *error = err.empty() ? "cannot read the impulse response" : err;
        return false;
    }
    const int64_t frames = a->frames();
    const int ch = std::max(1, a->channels);
    left.resize(size_t(frames)), right.resize(size_t(frames));
    double energy = 0, peak = 0;
    for (int64_t i = 0; i < frames; ++i) {
        left[size_t(i)] = a->samples[size_t(i) * size_t(ch)];
        right[size_t(i)] = a->samples[size_t(i) * size_t(ch) + (ch > 1 ? 1 : 0)];
        energy += double(left[size_t(i)]) * left[size_t(i)] + double(right[size_t(i)]) * right[size_t(i)];
        peak = std::max({peak, double(std::abs(left[size_t(i)])), double(std::abs(right[size_t(i)]))});
    }
    if (energy <= 1e-12) {
        if (error) *error = "the impulse response is silent";
        return false;
    }
    // The silence after the response ends (90 dB below its peak) costs time and adds nothing.
    size_t end = std::min(size_t(frames), size_t(10) * size_t(std::max(1, sampleRate)));
    while (end > 1 && std::abs(left[end - 1]) < peak * 3e-5 && std::abs(right[end - 1]) < peak * 3e-5) --end;
    left.resize(end), right.resize(end);
    energy = 0;
    for (size_t i = 0; i < end; ++i) energy += double(left[i]) * left[i] + double(right[i]) * right[i];
    const float g = float(1 / std::sqrt(energy / 2));
    for (auto& v : left) v *= g;
    for (auto& v : right) v *= g;
    return true;
}

// ---- Convolution ----------------------------------------------------------------

std::shared_ptr<const ConvolutionKernel> makeConvolutionKernel(const std::vector<float>& ir, int block) {
    auto k = std::make_shared<ConvolutionKernel>();
    k->block = 1;
    while (k->block < std::max(16, block)) k->block <<= 1;
    if (ir.empty()) return k;
    const size_t B = size_t(k->block), bins = B + 1;
    k->head.assign(B, 0.0f);
    // Stored back to front, so the direct taps run forward through the history.
    for (size_t i = 0; i < std::min(B, ir.size()); ++i) k->head[B - 1 - i] = ir[i];
    std::vector<cfloat> frame(2 * B);
    for (size_t start = B; start < ir.size(); start += B) {
        std::fill(frame.begin(), frame.end(), cfloat());
        for (size_t i = 0; i < B && start + i < ir.size(); ++i) frame[i] = ir[start + i];
        fftInPlace(frame, false);
        k->parts.emplace_back(frame.begin(), frame.begin() + std::ptrdiff_t(bins));
    }
    return k;
}

void Convolver::setKernel(std::shared_ptr<const ConvolutionKernel> kernel) {
    kernel_ = std::move(kernel);
    const size_t B = kernel_ ? size_t(kernel_->block) : 0, bins = B + 1;
    inputs_.assign(kernel_ ? kernel_->parts.size() : 0, std::vector<cfloat>(bins));
    work_.assign(2 * B, cfloat());
    history_.assign(2 * B, 0.0f);
    previous_.assign(B, 0.0f), current_.assign(B, 0.0f), tail_.assign(B, 0.0f);
    newest_ = 0, at_ = 0, pos_ = 0;
}

void Convolver::reset() {
    for (auto& v : inputs_) std::fill(v.begin(), v.end(), cfloat());
    std::fill(history_.begin(), history_.end(), 0.0f);
    std::fill(previous_.begin(), previous_.end(), 0.0f);
    std::fill(current_.begin(), current_.end(), 0.0f);
    std::fill(tail_.begin(), tail_.end(), 0.0f);
    newest_ = 0, at_ = 0, pos_ = 0;
}

void Convolver::process(const float* in, float* out, int n, int stride) {
    if (empty()) {
        for (int i = 0; i < n; ++i) out[size_t(i) * size_t(stride)] = 0;
        return;
    }
    const auto& head = kernel_->head;
    const auto& parts = kernel_->parts;
    const int block = kernel_->block;
    const size_t B = size_t(block), bins = B + 1, P = parts.size();
    for (int i = 0; i < n; ++i) {
        const float x = in[size_t(i) * size_t(stride)];
        // The history is kept twice over, so the last B inputs always lie in one run: history_[at_+1 .. at_+B].
        history_[at_] = history_[at_ + B] = x;
        const float* h = history_.data() + at_ + 1;
        float y = 0;
        for (size_t k = 0; k < B; ++k) y += h[k] * head[k];
        at_ = (at_ + 1) % B;
        out[size_t(i) * size_t(stride)] = y + tail_[size_t(pos_)];
        current_[size_t(pos_)] = x;
        if (++pos_ < block) continue;
        pos_ = 0;
        if (P) {
            // This frame's spectrum joins the ring; the later taps' output for the next block is every frame
            // still in reach, each times the part of the response that has reached it.
            for (size_t k = 0; k < B; ++k) work_[k] = previous_[k], work_[B + k] = current_[k];
            fftInPlace(work_, false);
            newest_ = (newest_ + 1) % P;
            std::copy(work_.begin(), work_.begin() + std::ptrdiff_t(bins), inputs_[newest_].begin());
            std::fill(work_.begin(), work_.end(), cfloat());
            for (size_t p = 0; p < P; ++p) {
                const auto& X = inputs_[(newest_ + P - p) % P];
                const auto& H = parts[p];
                for (size_t k = 0; k < bins; ++k) work_[k] += X[k] * H[k];
            }
            for (size_t k = 1; k < B; ++k) work_[2 * B - k] = std::conj(work_[k]);
            fftInPlace(work_, true);
            const float scale = 1.0f / float(2 * B);
            for (size_t k = 0; k < B; ++k) tail_[k] = work_[B + k].real() * scale;
        }
        std::swap(previous_, current_);
    }
}

std::vector<float> convolve(const std::vector<float>& signal, const std::vector<float>& ir) {
    std::vector<float> out(signal.size(), 0.0f);
    if (signal.empty() || ir.empty()) return out;
    // Overlap-add in FFT blocks at least twice the response's length.
    size_t N = 64;
    while (N < 2 * ir.size()) N <<= 1;
    const size_t L = N - ir.size() + 1;
    std::vector<cfloat> H(N), X(N);
    std::copy(ir.begin(), ir.end(), H.begin());
    fftInPlace(H, false);
    for (size_t start = 0; start < signal.size(); start += L) {
        std::fill(X.begin(), X.end(), cfloat());
        for (size_t i = 0; i < L && start + i < signal.size(); ++i) X[i] = signal[start + i];
        fftInPlace(X, false);
        for (size_t k = 0; k < N; ++k) X[k] *= H[k];
        fftInPlace(X, true);
        for (size_t i = 0; i < N && start + i < out.size(); ++i) out[start + i] += X[i].real() / float(N);
    }
    return out;
}

// ---- Measuring a room -----------------------------------------------------------

namespace {

std::vector<double> levels10ms(const std::vector<float>& x, int rate) {
    const size_t step = size_t(std::max(1, rate / 100));
    std::vector<double> level;
    level.reserve(x.size() / step);
    for (size_t s = 0; s + step <= x.size(); s += step) {
        double acc = 0;
        for (size_t i = 0; i < step; ++i) acc += double(x[s + i]) * x[s + i];
        level.push_back(10 * std::log10(acc / double(step) + 1e-20));
    }
    return level;
}

}  // namespace

double bandReverbTime(const std::vector<float>& mono, int sampleRate, double centreHz) {
    const auto& c = roomBandCentres();
    int band = 0;
    for (int b = 1; b < kRoomBands; ++b)
        if (std::abs(std::log(c[size_t(b)] / centreHz)) < std::abs(std::log(c[size_t(band)] / centreHz))) band = b;
    if (c[size_t(band)] > 0.4 * sampleRate) return 0;
    std::vector<float> x = mono;
    // A band-pass on the actual centre asked for (the octave table only picks the filter's shape).
    const int kind = band == 0 ? 1 : band == kRoomBands - 1 ? 2 : 0;
    const double f = kind == 1 ? centreHz * M_SQRT2 : kind == 2 ? centreHz / M_SQRT2 : centreHz;
    const double q = kind == 0 ? M_SQRT2 : M_SQRT1_2;
    Biquad a = Biquad::make(kind, f, q, sampleRate), b = a;
    for (auto& v : x) v = float(b(a(v)));
    return reverbTimeFromLevels(levels10ms(x, sampleRate));
}

RoomEstimate estimateRoom(const std::vector<float>& mono, int sampleRate) {
    RoomEstimate e;
    for (int b = 0; b < kRoomBands; ++b) {
        e.rt60[size_t(b)] = bandReverbTime(mono, sampleRate, roomBandCentres()[size_t(b)]);
        if (e.rt60[size_t(b)] > 0) ++e.bandsFound;
    }
    AudioBuffer buf;
    buf.sampleRate = sampleRate;
    buf.samples.resize(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); ++i) buf.samples[2 * i] = buf.samples[2 * i + 1] = mono[i];
    e.broadband = estimateReverbTime(buf);
    return e;
}

RoomShape RoomEstimate::shape() const {
    RoomShape s;
    const double fallback = broadband > 0 ? broadband : 0.5;
    for (int b = 0; b < kRoomBands; ++b) {
        if (rt60[size_t(b)] > 0) {
            s.rt60[size_t(b)] = rt60[size_t(b)];
            continue;
        }
        double best = 0;
        for (int d = 1; d < kRoomBands && best <= 0; ++d) {
            const double lo = b - d >= 0 ? rt60[size_t(b - d)] : 0, hi = b + d < kRoomBands ? rt60[size_t(b + d)] : 0;
            best = lo > 0 && hi > 0 ? (lo + hi) / 2 : std::max(lo, hi);
        }
        s.rt60[size_t(b)] = best > 0 ? best : fallback;
    }
    return s;
}

double lateReverbDb(const std::vector<float>& mono, int sampleRate, double rt60) {
    const int sr = std::max(8000, sampleRate);
    if (rt60 <= 0) return -60;
    std::vector<float> x = mono;
    Biquad h1 = Biquad::make(2, 300, M_SQRT1_2, sr), h2 = h1, l1 = Biquad::make(1, 4000, M_SQRT1_2, sr), l2 = l1;
    for (auto& v : x) v = float(l2(l1(h2(h1(v)))));
    const std::vector<double> db = levels10ms(x, sr);
    const size_t n = db.size();
    if (n < 100) return -60;
    std::vector<double> E(n);
    for (size_t i = 0; i < n; ++i) E[i] = std::pow(10.0, db[i] / 10);
    std::vector<double> sorted = E;
    std::sort(sorted.begin(), sorted.end());
    const double noise = sorted[n * 3 / 100];
    // What the room would hold at each moment if all the sound before it (more than 50 ms before) had gone into a
    // room of this decay time with as much reverberant energy as direct: the energy, smoothed by the decay.
    const double a = std::exp(-2 * 6.907755 / rt60 * 0.01);
    std::vector<double> late(n, 0.0);
    double acc = 0;
    for (size_t i = 0; i < n; ++i) {
        acc = a * acc + (1 - a) * E[i];
        if (i + 5 < n) late[i + 5] = acc * std::pow(a, 5);
    }
    // Where it would stand well clear of the noise, the quieter moments (pauses and the ends of words) show how much
    // of it is really there.
    std::vector<std::pair<double, double>> quiet;  // level, measured / predicted
    for (size_t i = 5; i < n; ++i)
        if (late[i] > 10 * noise) quiet.push_back({E[i], std::max(0.0, E[i] - noise) / late[i]});
    if (quiet.size() < 10) return -60;
    std::sort(quiet.begin(), quiet.end());
    quiet.resize(quiet.size() * 4 / 10);
    std::vector<double> ratio;
    for (const auto& q : quiet) ratio.push_back(q.second);
    std::sort(ratio.begin(), ratio.end());
    return 10 * std::log10(std::max(1e-6, ratio[ratio.size() / 2]));
}

}  // namespace montage
