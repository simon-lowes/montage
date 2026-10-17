#include "DialogueAlign.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numeric>

#include "TimeStretch.h"
#include "core/Effects.h"
#include "media/MediaPool.h"

namespace montage {

namespace {

constexpr int kBands = 40, kCeps = 6, kDim = 2 * kCeps + 2;
// How much the level's rise and fall counts against the spectrum's shape: it is the voice's rhythm, which a different
// voice saying the line keeps (tried on lines said by different voices of the speech model, at different speeds).
constexpr double kLevelWeight = 6;
constexpr double kAnalysisRate = 16000;  // speech features need no more
constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr double kMaxAlignSeconds = 120;  // the longest line aligned at once (the path's table grows with its length)
constexpr double kMaxOffset = 3600;       // seconds a warp may move a sound (a point beyond is not read)

std::vector<float> monoOf(const AudioBuffer& b) {
    const int64_t n = b.frames();
    const size_t nch = size_t(std::max(1, b.channels));
    std::vector<float> out(size_t(std::max<int64_t>(0, n)));
    for (int64_t i = 0; i < n; ++i) {
        double sum = 0;
        for (size_t c = 0; c < nch; ++c) sum += b.samples[size_t(i) * nch + c];
        out[size_t(i)] = float(sum / double(nch));
    }
    return out;
}

// In place, radix 2 (n a power of two); `inverse` unscaled.
void fft(std::vector<std::complex<double>>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const std::complex<double> step = std::polar(1.0, (inverse ? 2 : -2) * M_PI / double(len));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w = 1;
            for (size_t k = 0; k < len / 2; ++k, w *= step) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
        }
    }
}

size_t pow2AtLeast(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

double percentile(std::vector<float> v, double q) {
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(k), v.end());
    return v[k];
}

// A sound as frames `hop` seconds apart (each centred on its time): log-mel bands (dB) with each band's mean over
// the speech taken off, then their deltas; the level, where the speech is, and how sharply the bands rise (onsets).
struct Frames {
    int count = 0;
    std::vector<float> v;  // count x kDim
    std::vector<float> level;
    std::vector<uint8_t> speech;
    std::vector<float> flux;
    const float* at(int i) const { return v.data() + size_t(i) * kDim; }
};

Frames analyse(const std::vector<float>& x, double rate, double hop) {
    Frames f;
    const int64_t n = int64_t(x.size());
    const int step = std::max(1, int(std::lround(hop * rate)));
    const int win = std::max(16, int(std::lround(0.025 * rate)));
    const size_t nfft = pow2AtLeast(size_t(win));
    const size_t bins = nfft / 2 + 1;
    f.count = int((n + step - 1) / step);
    if (f.count <= 0) return f;
    std::vector<double> window(static_cast<size_t>(win));
    for (int i = 0; i < win; ++i) window[size_t(i)] = 0.5 - 0.5 * std::cos(2 * M_PI * (i + 0.5) / win);
    // Triangular mel filters between 80 Hz and 7.6 kHz (or under the Nyquist frequency).
    auto mel = [](double hz) { return 2595 * std::log10(1 + hz / 700); };
    auto hzOf = [](double m) { return 700 * (std::pow(10, m / 2595) - 1); };
    const double lo = mel(80), hi = mel(std::min(7600.0, 0.475 * rate));
    std::vector<double> edge(kBands + 2);
    for (int b = 0; b < kBands + 2; ++b) edge[size_t(b)] = hzOf(lo + (hi - lo) * b / (kBands + 1)) * double(nfft) / rate;
    std::vector<float> logMel(size_t(f.count) * kBands);
    f.level.resize(size_t(f.count));
    std::vector<std::complex<double>> buf(nfft);
    std::vector<double> power(bins);
    for (int i = 0; i < f.count; ++i) {
        const int64_t start = int64_t(i) * step - win / 2;
        double energy = 0;
        for (size_t k = 0; k < nfft; ++k) {
            const int64_t s = start + int64_t(k);
            const double v = k < size_t(win) && s >= 0 && s < n ? x[size_t(s)] * window[k] : 0.0;
            buf[k] = v;
            energy += v * v;
        }
        f.level[size_t(i)] = float(10 * std::log10(energy / win + 1e-12));
        fft(buf, false);
        for (size_t k = 0; k < bins; ++k) power[k] = std::norm(buf[k]);
        for (int b = 0; b < kBands; ++b) {
            const double l = edge[size_t(b)], c = edge[size_t(b) + 1], r = edge[size_t(b) + 2];
            double sum = 0;
            for (size_t k = size_t(std::max(0.0, std::ceil(l))); k < bins && double(k) < r; ++k) {
                const double w = double(k) < c ? (double(k) - l) / std::max(1e-9, c - l) : (r - double(k)) / std::max(1e-9, r - c);
                if (w > 0) sum += w * power[k];
            }
            logMel[size_t(i) * kBands + size_t(b)] = float(10 * std::log10(sum + 1e-10));
        }
    }
    // Speech: more than 12 dB over the quiet (the tenth percentile, but never more than 60 dB under the loudest) and
    // no more than 45 dB under the loudest; filled across gaps under 30 ms and widened 10 ms either side.
    const double peak = percentile(f.level, 0.99);
    const double floor = std::max(percentile(f.level, 0.10), peak - 60);
    const double gate = std::max(floor + 12, peak - 45);
    f.speech.assign(size_t(f.count), 0);
    for (int i = 0; i < f.count; ++i) f.speech[size_t(i)] = f.level[size_t(i)] > gate;
    const int gap = int(std::lround(0.03 / hop)), widen = int(std::lround(0.01 / hop));
    for (int i = 0, last = -1; i < f.count; ++i)
        if (f.speech[size_t(i)]) {
            if (last >= 0 && i - last <= gap)
                for (int k = last + 1; k < i; ++k) f.speech[size_t(k)] = 1;
            last = i;
        }
    std::vector<uint8_t> wide = f.speech;
    for (int i = 0; i < f.count; ++i)
        if (f.speech[size_t(i)])
            for (int k = std::max(0, i - widen); k <= std::min(f.count - 1, i + widen); ++k) wide[size_t(k)] = 1;
    f.speech = std::move(wide);
    // Onset strength: how much the bands rose over 10 ms, in speech.
    f.flux.assign(size_t(f.count), 0.0f);
    const int back = std::max(1, int(std::lround(0.01 / hop)));
    for (int i = back; i < f.count; ++i) {
        if (!f.speech[size_t(i)]) continue;
        double sum = 0;
        for (int b = 0; b < kBands; ++b)
            sum += std::max(0.0f, logMel[size_t(i) * kBands + size_t(b)] - logMel[size_t(i - back) * kBands + size_t(b)]);
        f.flux[size_t(i)] = float(sum / kBands);
    }
    // Cepstra (the DCT of the bands, the first left out: the level) describe the shape of the spectrum, the first
    // few its broad shape (what is said) rather than the voice's fine detail; each less its mean and over its spread
    // across the speech (the microphone, the room and the voice), with their deltas over +-20 ms.
    const int C = kCeps;
    std::vector<double> ceps(size_t(f.count) * size_t(C));
    for (int i = 0; i < f.count; ++i)
        for (int k = 0; k < C; ++k) {
            double v = 0;
            for (int b = 0; b < kBands; ++b) v += logMel[size_t(i) * kBands + size_t(b)] * std::cos(M_PI * (k + 1) * (b + 0.5) / kBands);
            ceps[size_t(i) * size_t(C) + size_t(k)] = v * std::sqrt(2.0 / kBands);
        }
    std::vector<double> mean(size_t(C), 0.0), spread(size_t(C), 0.0);
    int counted = 0;
    for (int i = 0; i < f.count; ++i)
        if (f.speech[size_t(i)]) {
            for (int k = 0; k < C; ++k) mean[size_t(k)] += ceps[size_t(i) * size_t(C) + size_t(k)];
            ++counted;
        }
    for (int k = 0; k < C; ++k) mean[size_t(k)] = counted ? mean[size_t(k)] / counted : 0.0;
    for (int i = 0; i < f.count; ++i)
        if (f.speech[size_t(i)])
            for (int k = 0; k < C; ++k) spread[size_t(k)] += std::pow(ceps[size_t(i) * size_t(C) + size_t(k)] - mean[size_t(k)], 2);
    for (int k = 0; k < C; ++k) spread[size_t(k)] = counted > 1 ? std::sqrt(spread[size_t(k)] / counted) + 1e-3 : 1.0;
    for (int i = 0; i < f.count; ++i)
        for (int k = 0; k < C; ++k) {
            double& v = ceps[size_t(i) * size_t(C) + size_t(k)];
            v = (v - mean[size_t(k)]) / spread[size_t(k)];
        }
    const int K = std::max(1, int(std::lround(0.02 / hop)));
    double norm = 0;
    for (int k = 1; k <= K; ++k) norm += 2.0 * k * k;
    f.v.assign(size_t(f.count) * kDim, 0.0f);
    for (int i = 0; i < f.count; ++i) {
        float* row = f.v.data() + size_t(i) * kDim;
        for (int k = 0; k < C; ++k) {
            row[k] = float(ceps[size_t(i) * size_t(C) + size_t(k)]);
            double d = 0;
            for (int q = 1; q <= K; ++q) {
                const int a = std::min(f.count - 1, i + q), c = std::max(0, i - q);
                d += q * (ceps[size_t(a) * size_t(C) + size_t(k)] - ceps[size_t(c) * size_t(C) + size_t(k)]);
            }
            row[kCeps + k] = float(d / norm * K);  // (scaled to the change over the whole span, like the cepstra)
        }
    }
    // The level against the speech's own (how loud each moment is for this voice), and how fast it changes.
    {
        double m = 0, q = 0;
        int c = 0;
        for (int i = 0; i < f.count; ++i)
            if (f.speech[size_t(i)]) m += f.level[size_t(i)], q += double(f.level[size_t(i)]) * f.level[size_t(i)], ++c;
        m = c ? m / c : 0;
        const double sd = c > 1 ? std::sqrt(std::max(1.0, q / c - m * m)) : 10;
        std::vector<double> z(size_t(f.count));
        for (int i = 0; i < f.count; ++i) z[size_t(i)] = std::clamp((f.level[size_t(i)] - m) / sd, -4.0, 3.0);
        for (int i = 0; i < f.count; ++i) {
            float* row = f.v.data() + size_t(i) * kDim;
            double d = 0;
            for (int q2 = 1; q2 <= K; ++q2) d += q2 * (z[size_t(std::min(f.count - 1, i + q2))] - z[size_t(std::max(0, i - q2))]);
            row[2 * kCeps] = float(kLevelWeight * z[size_t(i)]);
            row[2 * kCeps + 1] = float(kLevelWeight * d / norm * K);
        }
    }
    return f;
}

AudioBuffer slice(const AudioBuffer& b, double from, double to) {
    AudioBuffer out;
    out.sampleRate = b.sampleRate;
    out.channels = b.channels;
    const size_t nch = size_t(std::max(1, b.channels));
    const int64_t a = int64_t(std::llround(from * b.sampleRate)), e = int64_t(std::llround(to * b.sampleRate));
    out.samples.assign(size_t(std::max<int64_t>(0, e - a)) * nch, 0.0f);
    for (int64_t i = std::max<int64_t>(a, 0); i < std::min(e, b.frames()); ++i)
        for (size_t c = 0; c < nch; ++c) out.samples[size_t(i - a) * nch + c] = b.samples[size_t(i) * nch + c];
    return out;
}

// `seconds` of a sound from `u` as a warp plays it (read between samples linearly: for measuring, not for listening).
AudioBuffer warpedSlice(const AudioBuffer& b, const DialogueWarp& warp, double u, double seconds) {
    if (warp.empty()) return slice(b, u, u + seconds);
    AudioBuffer out;
    out.sampleRate = b.sampleRate;
    out.channels = b.channels;
    const size_t nch = size_t(std::max(1, b.channels));
    const int64_t n = int64_t(std::llround(seconds * b.sampleRate));
    out.samples.assign(size_t(std::max<int64_t>(0, n)) * nch, 0.0f);
    for (int64_t i = 0; i < n; ++i) {
        const double at = dialogueWarpAt(warp, u + double(i) / b.sampleRate) * b.sampleRate;
        const int64_t k = int64_t(std::floor(at));
        const float f = float(at - double(k));
        if (k < 0 || k + 1 >= b.frames()) continue;
        for (size_t c = 0; c < nch; ++c)
            out.samples[size_t(i) * nch + c] = b.samples[size_t(k) * nch + c] * (1 - f) + b.samples[size_t(k + 1) * nch + c] * f;
    }
    return out;
}

// A windowed sinc (Blackman, 16 taps either side) for reading between samples.
double sincTap(double x) {
    constexpr double half = 16;
    if (std::fabs(x) >= half) return 0;
    const double s = std::fabs(x) < 1e-12 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
    const double t = (x + half) / (2 * half);
    return s * (0.42 - 0.5 * std::cos(2 * M_PI * t) + 0.08 * std::cos(4 * M_PI * t));
}

}  // namespace

std::string dialogueWarpToString(const DialogueWarp& warp) {
    std::string out;
    char buf[64];
    for (const AlignPoint& p : warp) {
        std::snprintf(buf, sizeof buf, "%s%.6f:%.6f", out.empty() ? "" : ",", p.u, p.w);
        out += buf;
    }
    return out;
}

DialogueWarp dialogueWarpFromString(const std::string& text) {
    DialogueWarp out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t comma = text.find(',', pos);
        if (comma == std::string::npos) comma = text.size();
        const std::string item = text.substr(pos, comma - pos);
        pos = comma + 1;
        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        char* end = nullptr;
        const double u = std::strtod(item.c_str(), &end);
        if (end != item.c_str() + colon) continue;
        const char* wText = item.c_str() + colon + 1;
        const double w = std::strtod(wText, &end);
        if (end == wText || !std::isfinite(u) || !std::isfinite(w)) continue;
        if (std::fabs(u) > 1e7 || std::fabs(w - u) > kMaxOffset) continue;
        if (!out.empty() && (u <= out.back().u || w < out.back().w)) continue;
        out.push_back({u, w});
    }
    return out;
}

double dialogueWarpAt(const DialogueWarp& warp, double u) {
    if (warp.empty()) return u;
    if (u <= warp.front().u) return u + warp.front().w - warp.front().u;
    if (u >= warp.back().u) return u + warp.back().w - warp.back().u;
    const auto it = std::upper_bound(warp.begin(), warp.end(), u, [](double v, const AlignPoint& p) { return v < p.u; });
    const AlignPoint& b = *it;
    const AlignPoint& a = *(it - 1);
    return a.w + (b.w - a.w) * (u - a.u) / (b.u - a.u);
}

double dialogueWarpInverse(const DialogueWarp& warp, double w) {
    if (warp.empty()) return w;
    if (w <= warp.front().w) return w - (warp.front().w - warp.front().u);
    if (w >= warp.back().w) return w - (warp.back().w - warp.back().u);
    const auto it = std::upper_bound(warp.begin(), warp.end(), w, [](double v, const AlignPoint& p) { return v < p.w; });
    const AlignPoint& b = *it;
    const AlignPoint& a = *(it - 1);
    return b.w > a.w ? a.u + (b.u - a.u) * (w - a.w) / (b.w - a.w) : a.u;
}

void applyDialogueWarp(const AudioBuffer& in, const DialogueWarp& warp, bool invert, AudioBuffer& out,
                       const std::atomic<bool>* cancel) {
    const double rate = std::max(1, in.sampleRate);
    const size_t nch = size_t(std::max(1, in.channels));
    const int64_t n = in.frames();
    const double lastOffset = warp.empty() ? 0 : warp.back().w - warp.back().u;
    // At most a minute of silence past the file's own length (an offset the string guard let through).
    const int64_t nOut = std::clamp<int64_t>(n - int64_t(std::llround(lastOffset * rate)), 0, n + int64_t(60 * rate));
    const bool fixed = std::all_of(warp.begin(), warp.end(), [&](const AlignPoint& p) { return std::fabs(p.w - p.u - lastOffset) < 1e-9; });
    if (fixed) {
        // A delay: each sample read `lastOffset` later, between samples through the sinc.
        out.sampleRate = in.sampleRate;
        out.channels = int(nch);
        out.samples.assign(size_t(nOut) * nch, 0.0f);
        const double shift = lastOffset * rate;
        const int64_t whole = int64_t(std::floor(shift));
        const double frac = shift - double(whole);
        std::vector<double> taps;
        if (frac > 1e-6 && frac < 1 - 1e-6)
            for (int k = -15; k <= 16; ++k) taps.push_back(sincTap(double(k) - frac));
        const float sign = invert ? -1.0f : 1.0f;
        for (int64_t i = 0; i < nOut; ++i) {
            if (cancel && (i & 0xffff) == 0 && cancel->load()) return;
            for (size_t c = 0; c < nch; ++c) {
                double v = 0;
                if (taps.empty()) {
                    const int64_t s = i + whole + (frac >= 0.5 ? 1 : 0);
                    v = s >= 0 && s < n ? in.samples[size_t(s) * nch + c] : 0.0;
                } else {
                    for (int k = -15; k <= 16; ++k) {
                        const int64_t s = i + whole + k;
                        if (s >= 0 && s < n) v += taps[size_t(k + 15)] * in.samples[size_t(s) * nch + c];
                    }
                }
                out.samples[size_t(i) * nch + c] = sign * float(v);
            }
        }
        return;
    }
    const int hop = stretchHop(in.sampleRate);
    std::vector<double> positions;
    positions.reserve(size_t(nOut / hop + 3));
    for (int64_t k = 0; k <= nOut / hop + 2; ++k) positions.push_back(dialogueWarpAt(warp, double(k * hop) / rate) * rate);
    wsolaStretch(in, positions, hop, nOut, out, cancel);
    if (invert)
        for (float& v : out.samples) v = -v;
}

const char* alignPrecisionName(AlignPrecision p) {
    switch (p) {
    case AlignPrecision::Loose: return "loose";
    case AlignPrecision::Tight: return "tight";
    default: return "normal";
    }
}

bool parseAlignPrecision(const std::string& name, AlignPrecision& out) {
    for (AlignPrecision p : {AlignPrecision::Loose, AlignPrecision::Normal, AlignPrecision::Tight})
        if (name == alignPrecisionName(p)) {
            out = p;
            return true;
        }
    return false;
}

SpeechAlignment alignSpeech(const AudioBuffer& guideIn, const AudioBuffer& takeIn, double startSlack, double endSlack,
                            AlignPrecision precision, const std::atomic<bool>* cancel, std::string* error) {
    SpeechAlignment result;
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return SpeechAlignment{};
    };
    if (guideIn.sampleRate <= 0 || takeIn.sampleRate <= 0) return fail("No sound");
    const double gRate = guideIn.sampleRate, tRate = takeIn.sampleRate;
    const double guideSeconds = double(guideIn.frames()) / gRate;
    // Frames 5 ms apart, 10 ms for lines over 20 seconds (the path's table grows with the square of the length).
    const double hop = guideSeconds > 20 ? 0.01 : 0.005;
    const Frames g = analyse(monoOf(guideIn), gRate, hop), t = analyse(monoOf(takeIn), tRate, hop);
    const int N = g.count, M = t.count;
    if (N < 4 || M < 4) return fail("The sound is too short to align");
    if (std::none_of(g.speech.begin(), g.speech.end(), [](uint8_t v) { return v; })) return fail("The guide has no speech there");
    if (std::none_of(t.speech.begin(), t.speech.end(), [](uint8_t v) { return v; })) return fail("The take has no speech there");
    if (cancel && cancel->load()) return fail("Cancelled");

    // The band: either side of the line from where the take would start to where it would end without its handles,
    // a quarter of the line's length (at least half a second, at most three) plus the handles.
    const double s0 = std::clamp(startSlack / hop, 0.0, double(M - 1)), s1 = std::clamp(endSlack / hop, 0.0, double(M - 1));
    const double span = std::max(1.0, double(M - 1) - s0 - s1);
    const int R = int(std::clamp(0.25 * N, 0.5 / hop, 3.0 / hop)) + int(std::max(s0, s1) * 0.5);
    std::vector<int> lo(static_cast<size_t>(N)), hi(static_cast<size_t>(N));
    std::vector<size_t> row(size_t(N) + 1, 0);
    for (int i = 0; i < N; ++i) {
        const double c = s0 + double(i) * span / std::max(1, N - 1);
        lo[size_t(i)] = std::clamp(int(std::floor(c)) - R, 0, M - 1);
        hi[size_t(i)] = std::clamp(int(std::ceil(c)) + R, 0, M - 1);
        row[size_t(i) + 1] = row[size_t(i)] + size_t(hi[size_t(i)] - lo[size_t(i)] + 1);
    }
    const size_t cells = row.back();
    std::vector<float> D(cells, kInf);
    std::vector<uint8_t> from(cells, 0);    // 1 diagonal, 2 two take frames, 3 two guide frames
    std::vector<int32_t> first(cells, 0);   // the take frame the path started on
    auto idx = [&](int i, int j) -> int64_t {
        if (i < 0 || j < lo[size_t(i)] || j > hi[size_t(i)]) return -1;
        return int64_t(row[size_t(i)]) + (j - lo[size_t(i)]);
    };
    const int dim = kDim;
    auto dist = [&](int i, int j) {
        const float* a = g.at(i);
        const float* b = t.at(j);
        double sum = 0;
        for (int k = 0; k < dim; ++k) {
            const double d = double(a[k]) - double(b[k]);
            sum += d * d;
        }
        double d = std::sqrt(sum / dim);
        if (!g.speech[size_t(i)] && !t.speech[size_t(j)]) d *= 0.2;  // pauses take up the difference
        return float(d);
    };
    // The distances of the row before (for the 2:1 step) and this one.
    std::vector<float> dPrev, dCur;
    auto dAt = [&](const std::vector<float>& d, int i, int j) {
        return i >= 0 && j >= lo[size_t(i)] && j <= hi[size_t(i)] ? d[size_t(j - lo[size_t(i)])] : kInf;
    };
    for (int i = 0; i < N; ++i) {
        if (cancel && (i & 63) == 0 && cancel->load()) return fail("Cancelled");
        dCur.assign(size_t(hi[size_t(i)] - lo[size_t(i)] + 1), 0.0f);
        for (int j = lo[size_t(i)]; j <= hi[size_t(i)]; ++j) dCur[size_t(j - lo[size_t(i)])] = dist(i, j);
        for (int j = lo[size_t(i)]; j <= hi[size_t(i)]; ++j) {
            const float d = dCur[size_t(j - lo[size_t(i)])];
            const int64_t here = idx(i, j);
            if (i == 0) {
                // Any start in the take's handles (or later only by waiting through a pause, below).
                first[size_t(here)] = j;
                if (j <= int(std::ceil(s0))) {
                    D[size_t(here)] = 2 * d;
                } else if (const int64_t a = idx(0, j - 1); !t.speech[size_t(j)] && a >= 0 && D[size_t(a)] < kInf) {
                    D[size_t(here)] = D[size_t(a)] + d, from[size_t(here)] = 5, first[size_t(here)] = first[size_t(a)];
                }
                continue;
            }
            float best = kInf;
            uint8_t step = 0;
            int32_t start = 0;
            if (const int64_t a = idx(i - 1, j - 1); a >= 0 && D[size_t(a)] < kInf) {
                const float c = D[size_t(a)] + 2 * d;
                if (c < best) best = c, step = 1, start = first[size_t(a)];
            }
            if (const int64_t a = idx(i - 1, j - 2); a >= 0 && D[size_t(a)] < kInf) {
                const float mid = dAt(dCur, i, j - 1);
                const float c = D[size_t(a)] + 2 * mid + d;
                if (c < best) best = c, step = 2, start = first[size_t(a)];
            }
            if (const int64_t a = idx(i - 2, j - 1); a >= 0 && D[size_t(a)] < kInf) {
                const float mid = dAt(dPrev, i - 1, j);
                const float c = D[size_t(a)] + 2 * mid + d;
                if (c < best) best = c, step = 3, start = first[size_t(a)];
            }
            // In a pause either side may wait for the other, as long as it lasts: a pause of any length fits.
            if (!g.speech[size_t(i)])
                if (const int64_t a = idx(i - 1, j); a >= 0 && D[size_t(a)] < kInf) {
                    const float c = D[size_t(a)] + d;
                    if (c < best) best = c, step = 4, start = first[size_t(a)];
                }
            if (!t.speech[size_t(j)])
                if (const int64_t a = idx(i, j - 1); a >= 0 && D[size_t(a)] < kInf) {
                    const float c = D[size_t(a)] + d;
                    if (c < best) best = c, step = 5, start = first[size_t(a)];
                }
            D[size_t(here)] = best;
            from[size_t(here)] = step;
            first[size_t(here)] = start;
        }
        std::swap(dPrev, dCur);
    }
    // The end: any take frame within the last row's band, the cost per step of the path (its weight is the frames it
    // crossed on both sides).
    int endJ = -1;
    double bestEnd = std::numeric_limits<double>::infinity();
    for (int j = std::max(lo[size_t(N - 1)], int(std::floor(double(M - 1) - s1))); j <= hi[size_t(N - 1)]; ++j) {
        const int64_t e = idx(N - 1, j);
        if (D[size_t(e)] >= kInf) continue;
        const double c = D[size_t(e)] / double(N + j - first[size_t(e)] + 1);
        if (c < bestEnd) bestEnd = c, endJ = j;
    }
    if (endJ < 0) return fail("The take is too far from the guide to align (more than twice as fast or slow)");
    // Back along the path: the take frames each guide frame was matched with (in the guide's speech, those of the
    // take's speech when there are any: where a pause waited, the moment it ends).
    std::vector<double> sum(size_t(N), 0.0), count(size_t(N), 0.0), speechSum(size_t(N), 0.0), speechCount(size_t(N), 0.0);
    double distSum = 0;
    int distCount = 0;
    auto visit = [&](int i, int j) {
        sum[size_t(i)] += j, count[size_t(i)] += 1;
        if (t.speech[size_t(j)]) speechSum[size_t(i)] += j, speechCount[size_t(i)] += 1;
    };
    for (int i = N - 1, j = endJ; i >= 0 && j >= 0;) {
        visit(i, j);
        if (g.speech[size_t(i)]) distSum += dist(i, j), ++distCount;
        const uint8_t step = from[size_t(idx(i, j))];
        if (step == 0) break;
        if (step == 1) {
            --i, --j;
        } else if (step == 2) {
            visit(i, j - 1);
            --i, j -= 2;
        } else if (step == 3) {
            visit(i - 1, j);
            i -= 2, --j;
        } else if (step == 4) {
            --i;
        } else {
            --j;
        }
    }
    std::vector<double> tau(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        const bool speech = g.speech[size_t(i)] && speechCount[size_t(i)] > 0;
        tau[size_t(i)] = speech ? speechSum[size_t(i)] / speechCount[size_t(i)] : count[size_t(i)] > 0 ? sum[size_t(i)] / count[size_t(i)] : -1;
    }
    for (int i = 1; i < N; ++i)
        if (tau[size_t(i)] < 0) tau[size_t(i)] = tau[size_t(i) - 1];
    // Smoothed within each stretch of speech or pause (never across where one meets the other): the median of five,
    // then the mean of five (a 25 ms span at 5 ms).
    std::vector<int> runStart(static_cast<size_t>(N)), runEnd(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) runStart[size_t(i)] = i > 0 && g.speech[size_t(i)] == g.speech[size_t(i) - 1] ? runStart[size_t(i) - 1] : i;
    for (int i = N - 1; i >= 0; --i) runEnd[size_t(i)] = i + 1 < N && g.speech[size_t(i)] == g.speech[size_t(i) + 1] ? runEnd[size_t(i) + 1] : i;
    auto around = [&](const std::vector<double>& v, int i, int d) { return v[size_t(std::clamp(i + d, runStart[size_t(i)], runEnd[size_t(i)]))]; };
    std::vector<double> med(static_cast<size_t>(N)), smooth(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        double w[5];
        int k = 0;
        for (int d = -2; d <= 2; ++d) w[k++] = around(tau, i, d);
        std::sort(w, w + 5);
        med[size_t(i)] = w[2];
    }
    for (int i = 0; i < N; ++i) {
        double s = 0;
        for (int d = -2; d <= 2; ++d) s += around(med, i, d);
        smooth[size_t(i)] = s / 5;
    }
    smooth.front() = med.front(), smooth.back() = med.back();

    // Anchors: the ends, where speech starts and stops, the guide's onsets, and a grid in the speech as close as the
    // precision asks; none inside pauses, which the line between their ends stretches.
    const double spacing = precision == AlignPrecision::Loose ? 0.2 : precision == AlignPrecision::Tight ? 0.03 : 0.08;
    const int grid = std::max(1, int(std::lround(spacing / hop)));
    std::vector<int> strong = {0, N - 1};
    for (int i = 1; i < N; ++i)
        if (g.speech[size_t(i)] != g.speech[size_t(i) - 1]) strong.push_back(g.speech[size_t(i)] ? i : i - 1);
    {
        double fs = 0, fq = 0;
        int fn = 0;
        for (int i = 0; i < N; ++i)
            if (g.speech[size_t(i)]) fs += g.flux[size_t(i)], fq += double(g.flux[size_t(i)]) * g.flux[size_t(i)], ++fn;
        const double mean = fn ? fs / fn : 0, sd = fn ? std::sqrt(std::max(0.0, fq / fn - mean * mean)) : 0;
        const int around = std::max(1, int(std::lround(std::max(0.05, spacing * 0.5) / hop)));
        for (int i = 1; i + 1 < N; ++i) {
            const float v = g.flux[size_t(i)];
            if (!g.speech[size_t(i)] || v <= mean + sd) continue;
            bool peak = true;
            for (int k = std::max(0, i - around); k <= std::min(N - 1, i + around) && peak; ++k)
                if (g.flux[size_t(k)] > v || (g.flux[size_t(k)] == v && k < i)) peak = false;
            if (peak) strong.push_back(i);
        }
    }
    std::sort(strong.begin(), strong.end());
    strong.erase(std::unique(strong.begin(), strong.end()), strong.end());
    std::vector<int> anchors = strong;
    for (int i = 0; i < N; i += grid) {
        if (!g.speech[size_t(i)]) continue;
        const auto near = std::lower_bound(strong.begin(), strong.end(), i - grid / 2);
        if (near != strong.end() && std::abs(*near - i) < std::max(1, grid / 2)) continue;
        anchors.push_back(i);
    }
    std::sort(anchors.begin(), anchors.end());
    anchors.erase(std::unique(anchors.begin(), anchors.end()), anchors.end());
    // Monotone, with every stretch of speech between 1:2 and 2:1; a pause between two anchors may be stretched or
    // squeezed as far as it needs.
    std::vector<std::pair<double, double>> out;
    result.minRate = 2, result.maxRate = 0.5;
    for (int i : anchors) {
        const double gs = i * hop;
        double ts = smooth[size_t(i)] * hop;
        bool quiet = false;
        if (!out.empty()) {
            const int a = int(std::lround(out.back().first / hop));
            quiet = i - a > 1;
            for (int k = a + 1; k < i && quiet; ++k) quiet = !g.speech[size_t(k)];
            const double dg = gs - out.back().first;
            ts = quiet ? std::clamp(ts, out.back().second + 0.02 * dg, out.back().second + 20 * dg)
                       : std::clamp(ts, out.back().second + 0.5 * dg, out.back().second + 2 * dg);
            if (!quiet) {
                const double r = (ts - out.back().second) / dg;
                result.minRate = std::min(result.minRate, r), result.maxRate = std::max(result.maxRate, r);
            }
        }
        out.push_back({gs, ts});
    }
    if (result.minRate > result.maxRate) result.minRate = result.maxRate = 1;
    result.anchors = std::move(out);
    result.distance = distCount ? distSum / distCount : 0;
    return result;
}

DialogueShift findDialogueShift(const AudioBuffer& ref, const AudioBuffer& other, double maxLag) {
    DialogueShift r;
    if (ref.sampleRate <= 0 || ref.sampleRate != other.sampleRate) return r;
    const double rate = ref.sampleRate;
    const std::vector<float> a = monoOf(ref), b = monoOf(other);
    const int64_t n = int64_t(a.size());
    if (n < int64_t(rate * 0.05) || b.empty()) return r;
    // Coarse, about 8 kHz (box-averaged), over up to 30 seconds by FFT: where |correlation| peaks.
    const int q = std::max(1, int(rate / 8000));
    auto decimate = [&](const std::vector<float>& x) {
        std::vector<double> out(x.size() / size_t(q));
        for (size_t i = 0; i < out.size(); ++i) {
            double s = 0;
            for (int k = 0; k < q; ++k) s += x[i * size_t(q) + size_t(k)];
            out[i] = s / q;
        }
        return out;
    };
    const std::vector<double> da = decimate(a), db = decimate(b);
    const int64_t L = std::min<int64_t>(int64_t(da.size()), int64_t(30 * rate / q));
    const int64_t lag = std::max<int64_t>(1, int64_t(std::ceil(maxLag * rate / q)));
    const size_t P = pow2AtLeast(size_t(L + L + 2 * lag + 1));
    std::vector<std::complex<double>> fa(P), fb(P);
    for (int64_t i = 0; i < L; ++i) fa[size_t(i)] = da[size_t(i)];
    for (int64_t i = 0; i < L + 2 * lag; ++i) {
        const int64_t s = i - lag;
        fb[size_t(i)] = s >= 0 && s < int64_t(db.size()) ? db[size_t(s)] : 0.0;
    }
    fft(fa, false);
    fft(fb, false);
    for (size_t k = 0; k < P; ++k) fa[k] = std::conj(fa[k]) * fb[k];
    fft(fa, true);
    int64_t coarse = 0;
    double best = -1;
    for (int64_t k = 0; k <= 2 * lag; ++k) {
        const double v = std::fabs(fa[size_t(k)].real());
        if (v > best) best = v, coarse = k - lag;
    }
    // Fine, at the full rate around that, over the loudest two seconds of the reference.
    const int64_t W = std::min<int64_t>(n, int64_t(2 * rate));
    int64_t w0 = 0;
    {
        const int64_t block = std::max<int64_t>(1, int64_t(rate * 0.25));
        double loud = -1;
        for (int64_t s = 0; s + W <= n; s += block) {
            double e = 0;
            for (int64_t i = s; i < s + W; i += 4) e += double(a[size_t(i)]) * a[size_t(i)];
            if (e > loud) loud = e, w0 = s;
        }
    }
    auto corr = [&](int64_t d) {
        double s = 0;
        for (int64_t i = w0; i < w0 + W; ++i) {
            const int64_t j = i + d;
            if (j >= 0 && j < int64_t(b.size())) s += double(a[size_t(i)]) * b[size_t(j)];
        }
        return s;
    };
    const int64_t centre = coarse * q, reach = 2 * q + 2;
    int64_t at = centre;
    double peak = 0;
    std::vector<double> c(size_t(2 * reach + 1));
    for (int64_t d = -reach; d <= reach; ++d) {
        c[size_t(d + reach)] = corr(centre + d);
        if (std::fabs(c[size_t(d + reach)]) > std::fabs(peak)) peak = c[size_t(d + reach)], at = centre + d;
    }
    const double sign = peak < 0 ? -1 : 1;
    const int64_t k = at - centre + reach;
    double frac = 0;
    if (k > 0 && k < int64_t(c.size()) - 1) {
        const double y0 = sign * c[size_t(k - 1)], y1 = sign * c[size_t(k)], y2 = sign * c[size_t(k + 1)];
        const double den = y0 - 2 * y1 + y2;
        if (den < 0) frac = std::clamp(0.5 * (y0 - y2) / den, -0.5, 0.5);
    }
    double ea = 0, eb = 0;
    for (int64_t i = w0; i < w0 + W; ++i) {
        ea += double(a[size_t(i)]) * a[size_t(i)];
        const int64_t j = i + at;
        if (j >= 0 && j < int64_t(b.size())) eb += double(b[size_t(j)]) * b[size_t(j)];
    }
    r.confidence = ea > 0 && eb > 0 ? std::fabs(peak) / std::sqrt(ea * eb) : 0;
    r.delay = (double(at) + frac) / rate;
    r.inverted = sign < 0;
    r.found = r.confidence > 0.2;
    return r;
}

namespace {

std::string channelsText(const std::vector<int>& channels) {
    std::string out;
    for (int c : channels) out += (out.empty() ? "" : ",") + std::to_string(c);
    return out;
}

}  // namespace

bool dialogueAlignmentApplies(const Effect& e, const Clip& c) {
    if (e.type != "dialogue_align") return true;
    if (!e.s("media").empty() && e.s("media") != std::to_string(c.mediaId)) return false;
    if (e.strings.count("channels") && e.s("channels") != channelsText(c.channels)) return false;
    // The part of the file it was made for, in source frames: another take from the same file plays another part.
    if (const std::string span = e.s("span"); !span.empty()) {
        double a = 0, b = 0;
        if (std::sscanf(span.c_str(), "%lf:%lf", &a, &b) == 2) {
            const double c0 = c.sourceIn, c1 = c.sourceIn + std::max(1.0, c.sourceExtent());
            if (std::min(b, c1) <= std::max(a, c0)) return false;
        }
    }
    return true;
}

namespace edit {

Result alignDialogue(Project& p, Sequence& s, Id takeId, Id guideId, const DialogueAlignOptions& o, DialogueAlignReport* report,
                     const std::atomic<bool>* cancel) {
    const auto takeLoc = locate(s, takeId);
    const auto guideLoc = locate(s, guideId);
    if (!takeLoc || !guideLoc) return Result::fail("No such clip");
    if (takeId == guideId) return Result::fail("The take and the guide are the same clip");
    if (takeLoc->track.kind != TrackKind::Audio) return Result::fail("The take must be an audio clip");
    const Track* track = trackAt(s, takeLoc->track);
    if (track && track->locked) return Result::fail("The take's track is locked");
    Clip& take = *clipById(s, takeId);
    const Clip& guide = *clipById(s, guideId);
    const MediaItem* tm = p.findMedia(take.mediaId);
    const MediaItem* gm = p.findMedia(guide.mediaId);
    if (!tm || !tm->hasAudio || take.isGenerator()) return Result::fail("The take has no sound of its own");
    if (!gm || !gm->hasAudio || guide.isGenerator()) return Result::fail("The guide has no sound of its own");
    for (const Clip* c : {static_cast<const Clip*>(&take), &guide})
        if (c->reverse || c->ramped() || std::fabs(c->speed - 1) > 1e-9)
            return Result::fail("Both clips must play at their own speed, forwards");
    const double fps = s.fps.toDouble();
    if (fps <= 0) return Result::fail("The sequence has no frame rate");
    FrameTime from = o.from >= 0 ? o.from : std::max(take.start, guide.start);
    FrameTime to = o.to >= 0 ? o.to : std::min(take.end(), guide.end());
    from = std::max({from, take.start, guide.start});
    to = std::min(to, guide.end());
    if (to - from < 2) return Result::fail("The take and the guide do not overlap there");
    const double length = double(to - from) / fps;
    if (!o.shift && length > kMaxAlignSeconds)
        return Result::fail("A line can be aligned two minutes at a time: choose a range within it");

    // Times of each clip's own file: the guide's through any alignment of its own, the take's through the one being
    // replaced (so it is found where it was heard).
    DialogueWarp guideWarp, oldWarp;
    for (const Effect& e : guide.effects)
        if (e.enabled && e.type == "dialogue_align" && dialogueAlignmentApplies(e, guide)) guideWarp = dialogueWarpFromString(e.s("warp"));
    for (const Effect& e : take.effects)
        if (e.enabled && e.type == "dialogue_align" && dialogueAlignmentApplies(e, take)) oldWarp = dialogueWarpFromString(e.s("warp"));
    auto guideU = [&](FrameTime t) { return (guide.sourceIn + double(t - guide.start)) / fps; };
    auto takeU = [&](FrameTime t) { return (take.sourceIn + double(t - take.start)) / fps; };
    auto takeAt = [&](FrameTime t) { return dialogueWarpAt(oldWarp, takeU(t)); };
    const std::string guideKey = audioKey(gm->path, guide.channels), takeKey = audioKey(tm->path, take.channels);

    DialogueWarp warp;
    bool invert = false;
    DialogueAlignReport rep;
    rep.from = double(from) / fps, rep.to = double(to) / fps;
    try {
        if (o.shift) {
            const int rate = s.sampleRate > 0 ? s.sampleRate : 48000;
            const AudioBufferPtr gb = MediaPool::instance().audio(guideKey, rate);
            const AudioBufferPtr tb = MediaPool::instance().audio(takeKey, rate);
            if (!gb || !tb || gb->frames() == 0 || tb->frames() == 0) return Result::fail("Could not read the sound");
            const AudioBuffer ref = warpedSlice(*gb, guideWarp, guideU(from), length);
            const AudioBuffer other = slice(*tb, takeAt(from), takeAt(from) + length);
            const DialogueShift sh = findDialogueShift(ref, other, std::max(0.05, o.handles));
            if (!sh.found) return Result::fail("The take does not sound like the guide there (no common sound to line up)");
            warp = {{takeU(from), takeAt(from) + sh.delay}};
            invert = sh.inverted;
            rep.delay = sh.delay, rep.inverted = sh.inverted, rep.confidence = sh.confidence;
            rep.anchors = 1;
        } else {
            const int rate = int(kAnalysisRate);
            const AudioBufferPtr gb = MediaPool::instance().audio(guideKey, rate);
            const AudioBufferPtr tb = MediaPool::instance().audio(takeKey, rate);
            if (!gb || !tb || gb->frames() == 0 || tb->frames() == 0) return Result::fail("Could not read the sound");
            const AudioBuffer ref = warpedSlice(*gb, guideWarp, guideU(from), length);
            // The take with handles: what the band may reach (a quarter of the line, at most three seconds) beyond them.
            const double handles = std::max(0.0, o.handles) + std::min(3.0, 0.25 * length);
            const double fileEnd = double(tb->frames()) / rate;
            const double t0 = takeAt(from), t1 = takeAt(to);
            const double a = std::max(0.0, t0 - handles), b = std::min(fileEnd, t1 + handles);
            if (b - a < 0.1) return Result::fail("The take's file does not reach the guide's range");
            const AudioBuffer other = slice(*tb, a, b);
            std::string err;
            const SpeechAlignment al = alignSpeech(ref, other, std::max(0.0, t0 - a), std::max(0.0, b - t1), o.precision, cancel, &err);
            if (al.anchors.empty()) return Result::fail(err.empty() ? "Could not align the take" : err);
            const double u0 = takeU(from);
            for (const auto& [gs, ts] : al.anchors) {
                const AlignPoint pt{u0 + gs, a + ts};
                if (!warp.empty() && (pt.u <= warp.back().u || pt.w < warp.back().w)) continue;
                warp.push_back(pt);
            }
            rep.anchors = int(warp.size());
            rep.minRate = al.minRate, rep.maxRate = al.maxRate;
            rep.confidence = std::clamp(1 - al.distance / 3, 0.0, 1.0);
        }
    } catch (const std::bad_alloc&) {
        return Result::fail("Not enough memory to align a line this long");
    }
    if (cancel && cancel->load()) return Result::fail("Cancelled");

    std::erase_if(take.effects, [&](const Effect& e) { return e.type == "dialogue_align" && dialogueAlignmentApplies(e, take); });
    Effect e = makeEffect(p, "dialogue_align");
    e.strings["warp"] = dialogueWarpToString(warp);
    e.strings["media"] = std::to_string(take.mediaId);
    e.strings["channels"] = channelsText(take.channels);
    char span[64];
    std::snprintf(span, sizeof span, "%.3f:%.3f", take.sourceIn, take.sourceIn + double(take.duration));
    e.strings["span"] = span;
    e.params["invert"] = Param(invert ? 1.0 : 0.0);
    take.effects.push_back(std::move(e));
    // A take that stops short of the guide's range is lengthened to it, where its track has room and its file reaches.
    if (take.end() < to && take.linkGroup == 0) {
        FrameTime limit = std::min(to, take.start + FrameTime(std::floor(sourceLimit(p, s, take) - take.sourceIn)));
        if (track)
            for (const Clip& c : track->clips)
                if (c.id != take.id && c.start >= take.end()) limit = std::min(limit, c.start);
        if (limit > take.end()) take.duration = limit - take.start;
    }
    if (report) *report = rep;
    return {};
}

Result removeDialogueAlignment(Project&, Sequence& s, Id clip) {
    Clip* c = clipById(s, clip);
    const auto loc = locate(s, clip);
    if (!c || !loc) return Result::fail("No such clip");
    if (const Track* t = trackAt(s, loc->track); t && t->locked) return Result::fail("The clip's track is locked");
    const size_t before = c->effects.size();
    std::erase_if(c->effects, [&](const Effect& e) { return e.type == "dialogue_align" && dialogueAlignmentApplies(e, *c); });
    if (c->effects.size() == before) return Result::fail("The clip has no Dialogue Alignment");
    return {};
}

}  // namespace edit

}  // namespace montage
