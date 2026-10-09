#include "SpectralRepair.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "core/Effects.h"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/tx.h>
}

namespace montage {

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

// About 43 ms at 48 kHz: short enough to hold a click to its moment, long enough to separate a tone from speech.
int fftSizeFor(int rate) {
    int n = 256;
    while (n * 2 <= 0.05 * rate) n *= 2;
    return n;
}

// 1 inside [lo, hi], falling to 0 over `feather` outside (raised cosine).
double feathered(double x, double lo, double hi, double feather) {
    if (x >= lo && x <= hi) return 1;
    const double d = x < lo ? lo - x : x - hi;
    if (feather <= 0 || d >= feather) return 0;
    return 0.5 + 0.5 * std::cos(M_PI * d / feather);
}

}  // namespace

bool validSpectralMode(const std::string& mode) { return mode == "heal" || mode == "attenuate"; }

std::string spectralRegionsToString(const std::vector<SpectralRegion>& regions) {
    std::string out;
    char buf[160];
    for (const SpectralRegion& r : regions) {
        std::snprintf(buf, sizeof buf, "%.6g,%.6g,%.6g,%.6g,%s,%.6g,%d", r.start, r.end, r.low, r.high, r.mode.c_str(), r.gainDb, r.channel);
        if (!out.empty()) out += ';';
        out += buf;
    }
    return out;
}

std::vector<SpectralRegion> spectralRegionsFromString(const std::string& text) {
    std::vector<SpectralRegion> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t end = std::min(text.find(';', pos), text.size());
        const std::string item = text.substr(pos, end - pos);
        pos = end + 1;
        std::vector<std::string> f;
        size_t p = 0;
        while (p <= item.size()) {
            const size_t q = std::min(item.find(',', p), item.size());
            f.push_back(item.substr(p, q - p));
            p = q + 1;
        }
        if (f.size() < 7) continue;
        SpectralRegion r;
        r.start = std::atof(f[0].c_str());
        r.end = std::atof(f[1].c_str());
        r.low = std::atof(f[2].c_str());
        r.high = std::atof(f[3].c_str());
        r.mode = f[4];
        r.gainDb = std::atof(f[5].c_str());
        r.channel = std::atoi(f[6].c_str());
        if (r.end > r.start && validSpectralMode(r.mode) && r.channel >= -1 && r.channel <= 1) out.push_back(r);
    }
    return out;
}

void spectralRepair(const AudioBuffer& in, AudioBuffer& out, const std::vector<SpectralRegion>& regions, const std::atomic<bool>* cancel) {
    out.sampleRate = in.sampleRate;
    out.samples = in.samples;
    const int64_t frames = in.frames();
    const int rate = in.sampleRate;
    if (frames == 0 || regions.empty() || rate <= 0) return;
    const int N = fftSizeFor(rate), hop = N / 4, bins = N / 2 + 1;
    const int64_t half = N / 2, feather = 2 * hop, context = int64_t(0.25 * rate);
    const double binHz = double(rate) / N;
    Tx fwd(N, false), inv(N, true);
    if (!fwd.ctx || !inv.ctx) return;
    // Square-root Hann windows for analysis and synthesis: their product, a Hann window, sums to 2 at 75 % overlap.
    std::vector<float> window(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) window[size_t(i)] = float(std::sqrt(0.5 - 0.5 * std::cos(2 * M_PI * i / N)));
    AlignedBuf<float> frame(static_cast<size_t>(N)), timeOut(static_cast<size_t>(N));
    AlignedBuf<AVComplexFloat> spec(size_t(bins) + 1);

    for (const SpectralRegion& r : regions) {
        if (!validSpectralMode(r.mode) || r.end <= r.start) continue;
        const int64_t s0 = std::clamp<int64_t>(int64_t(std::floor(r.start * rate)), 0, frames);
        const int64_t s1 = std::clamp<int64_t>(int64_t(std::ceil(r.end * rate)), 0, frames);
        if (s1 <= s0) continue;
        const double lo = std::max(0.0, r.low), hi = r.high > 0 ? r.high : rate / 2.0;
        if (hi <= lo) continue;
        // Frames whose window touches the region are treated in full; the treatment fades out over two hops beyond.
        const int64_t core0 = s0 - half, core1 = s1 + half;
        const float gain = float(std::pow(10.0, std::min(0.0, r.gainDb) / 20.0));
        for (int ch = 0; ch < 2; ++ch) {
            if (r.channel >= 0 && r.channel != ch) continue;
            if (cancel && cancel->load()) return;
            auto sampleAt = [&](int64_t s) { return s >= 0 && s < frames ? out.samples[size_t(s) * 2 + size_t(ch)] : 0.0f; };
            auto analyse = [&](int64_t centre) {
                for (int i = 0; i < N; ++i) frame[size_t(i)] = sampleAt(centre - half + i) * window[size_t(i)];
                fwd.fn(fwd.ctx, spec.data(), frame.data(), sizeof(float));
            };
            // Heal: the level of each frequency just before and just after (frames wholly inside the file).
            std::vector<double> before(size_t(bins), 0.0), after(size_t(bins), 0.0);
            int nBefore = 0, nAfter = 0;
            if (r.mode == "heal") {
                for (int64_t c = core0 - feather - hop; c >= core0 - feather - context && c - half >= 0; c -= hop) {
                    analyse(c);
                    for (int k = 0; k < bins; ++k) before[size_t(k)] += std::hypot(spec[size_t(k)].re, spec[size_t(k)].im);
                    ++nBefore;
                }
                for (int64_t c = core1 + feather + hop; c <= core1 + feather + context && c + half <= frames; c += hop) {
                    analyse(c);
                    for (int k = 0; k < bins; ++k) after[size_t(k)] += std::hypot(spec[size_t(k)].re, spec[size_t(k)].im);
                    ++nAfter;
                }
                for (int k = 0; k < bins; ++k) {
                    if (nBefore) before[size_t(k)] /= nBefore;
                    if (nAfter) after[size_t(k)] /= nAfter;
                }
            }
            const bool heal = r.mode == "heal" && (nBefore || nAfter);
            const float fallback = r.mode == "heal" ? float(std::pow(10.0, -30.0 / 20.0)) : gain;  // nothing around: down 30 dB
            // Overlap-add over the region and a window's length either side, so the stretch written back is rebuilt
            // exactly wherever nothing was changed.
            const int64_t first = core0 - feather - N;
            const int64_t count = (core1 + feather + N - first) / hop + 1;
            const int64_t last = first + (count - 1) * hop;
            const int64_t origin = first - half;
            std::vector<float> acc(size_t(last + half - origin), 0.0f);
            for (int64_t f = 0; f < count; ++f) {
                const int64_t c = first + f * hop;
                const double wt = feathered(double(c), double(core0), double(core1), double(feather));
                float* dst = acc.data() + (c - half - origin);
                if (wt <= 0) {
                    for (int i = 0; i < N; ++i) dst[i] += sampleAt(c - half + i) * window[size_t(i)] * window[size_t(i)] * 0.5f;
                    continue;
                }
                analyse(c);
                const double along = core1 > core0 ? std::clamp(double(c - core0) / double(core1 - core0), 0.0, 1.0) : 0.5;
                for (int k = 0; k < bins; ++k) {
                    const double w = wt * feathered(k * binHz, lo, hi, 2 * binHz);
                    if (w <= 0) continue;
                    AVComplexFloat& x = spec[size_t(k)];
                    const double mag = std::hypot(x.re, x.im);
                    if (mag <= 0) continue;
                    double factor = fallback;
                    if (heal) {
                        const double b = nBefore ? before[size_t(k)] : after[size_t(k)], a = nAfter ? after[size_t(k)] : before[size_t(k)];
                        const double target = std::exp((1 - along) * std::log(b + 1e-12) + along * std::log(a + 1e-12));
                        factor = std::min(1.0, target / mag);  // never louder than it was
                    }
                    const double g = factor > 0 ? std::pow(factor, w) : 1 - w;
                    x.re = float(x.re * g);
                    x.im = float(x.im * g);
                }
                inv.fn(inv.ctx, timeOut.data(), spec.data(), sizeof(AVComplexFloat));
                for (int i = 0; i < N; ++i) dst[i] += timeOut[size_t(i)] * window[size_t(i)] * 0.5f;
            }
            const int64_t from = std::max<int64_t>(0, first + half), to = std::min<int64_t>(frames, last - half + 1);
            for (int64_t s = from; s < to; ++s) out.samples[size_t(s) * 2 + size_t(ch)] = acc[size_t(s - origin)];
        }
    }
}

Spectrogram computeSpectrogram(const AudioBuffer& in, double start, double end, int columns, int channel) {
    Spectrogram g;
    const int rate = in.sampleRate;
    if (rate <= 0 || columns <= 0 || end <= start) return g;
    const int N = fftSizeFor(rate), bins = N / 2 + 1;
    Tx fwd(N, false);
    if (!fwd.ctx) return g;
    g.columns = columns;
    g.bins = bins;
    g.start = start;
    g.end = end;
    g.nyquist = rate / 2.0;
    g.db.assign(size_t(columns) * size_t(bins), -120.0f);
    std::vector<float> window(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) window[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * i / N));
    AlignedBuf<float> frame(static_cast<size_t>(N));
    AlignedBuf<AVComplexFloat> spec(size_t(bins) + 1);
    const int64_t frames = in.frames();
    auto sampleAt = [&](int64_t s) -> float {
        if (s < 0 || s >= frames) return 0.0f;
        if (channel == 0 || channel == 1) return in.samples[size_t(s) * 2 + size_t(channel)];
        return 0.5f * (in.samples[size_t(s) * 2] + in.samples[size_t(s) * 2 + 1]);
    };
    const double fullScale = N / 4.0;  // a full-scale sine's peak through a Hann window
    for (int j = 0; j < columns; ++j) {
        const int64_t centre = int64_t(std::llround((start + (j + 0.5) * (end - start) / columns) * rate));
        for (int i = 0; i < N; ++i) frame[size_t(i)] = sampleAt(centre - N / 2 + i) * window[size_t(i)];
        fwd.fn(fwd.ctx, spec.data(), frame.data(), sizeof(float));
        for (int k = 0; k < bins; ++k) {
            const double mag = std::hypot(spec[size_t(k)].re, spec[size_t(k)].im) / fullScale;
            g.db[size_t(j) * size_t(bins) + size_t(k)] = float(std::max(-120.0, 20 * std::log10(mag + 1e-9)));
        }
    }
    return g;
}

std::vector<SpectralBand> prominentBands(const AudioBuffer& in, double start, double end, int maxBands, int channel) {
    std::vector<SpectralBand> out;
    const double length = in.sampleRate > 0 ? double(in.frames()) / in.sampleRate : 0;
    start = std::max(0.0, start), end = std::min(length, end);
    if (end - start < 0.01) return out;
    auto meanDb = [&](double a, double b) {
        std::vector<double> mean;
        if (b - a < 0.01) return mean;
        const Spectrogram g = computeSpectrogram(in, a, b, std::max(4, int((b - a) * 60)), channel);
        mean.assign(size_t(g.bins), 0.0);
        for (int k = 0; k < g.bins; ++k) {
            double power = 0;
            for (int j = 0; j < g.columns; ++j) power += std::pow(10.0, g.at(j, k) / 10);
            mean[size_t(k)] = 10 * std::log10(power / g.columns + 1e-15);
        }
        return mean;
    };
    const std::vector<double> inside = meanDb(start, end);
    const std::vector<double> before = meanDb(std::max(0.0, start - 1.0), start), after = meanDb(end, std::min(length, end + 1.0));
    if (inside.empty()) return out;
    const size_t bins = inside.size();
    std::vector<double> base(bins);
    if (!before.empty() || !after.empty()) {
        for (size_t k = 0; k < bins; ++k)
            base[k] = before.empty() ? after[k] : after.empty() ? before[k] : 10 * std::log10((std::pow(10.0, before[k] / 10) + std::pow(10.0, after[k] / 10)) / 2);
    } else {
        std::vector<double> sorted = inside;
        std::nth_element(sorted.begin(), sorted.begin() + long(bins / 2), sorted.end());
        base.assign(bins, sorted[bins / 2]);
    }
    const double nyquist = in.sampleRate / 2.0, binHz = nyquist / double(bins - 1);
    // From the bin that stands out most, outwards while its neighbours stand out too (at least 6 dB above their
    // surroundings, a gap of one bin bridged) and are within 20 dB of its level; then the next, until enough are found.
    std::vector<double> excess(bins);
    for (size_t k = 0; k < bins; ++k) excess[k] = inside[k] - base[k];
    std::vector<bool> taken(bins, false);
    taken[0] = true;  // DC
    while (int(out.size()) < maxBands) {
        size_t peak = 0;
        for (size_t k = 1; k < bins; ++k)
            if (!taken[k] && excess[k] >= 6 && (peak == 0 || excess[k] > excess[peak])) peak = k;
        if (peak == 0) break;
        auto belongs = [&](size_t j) { return !taken[j] && excess[j] >= 6 && inside[j] >= inside[peak] - 20; };
        size_t lo = peak, hi = peak;
        while (lo > 1 && (belongs(lo - 1) || (lo > 2 && belongs(lo - 2)))) lo -= belongs(lo - 1) ? 1 : 2;
        while (hi + 1 < bins && (belongs(hi + 1) || (hi + 2 < bins && belongs(hi + 2)))) hi += belongs(hi + 1) ? 1 : 2;
        for (size_t j = lo > 2 ? lo - 2 : 1; j <= std::min(bins - 1, hi + 2); ++j) taken[j] = true;
        SpectralBand b;
        b.low = std::max(0.0, (double(lo) - 1) * binHz);
        b.high = std::min(nyquist, (double(hi) + 1) * binHz);
        b.excessDb = excess[peak];
        out.push_back(b);
    }
    return out;
}

Effect* spectralRepairEffect(Project& p, Clip& c, bool create) {
    for (Effect& e : c.effects)
        if (e.type == "spectral_repair") return &e;
    if (!create) return nullptr;
    c.effects.insert(c.effects.begin(), makeEffect(p, "spectral_repair"));
    return &c.effects.front();
}

std::vector<SpectralRegion> spectralRegionsOf(const Clip& c) {
    for (const Effect& e : c.effects)
        if (e.type == "spectral_repair") return spectralRegionsFromString(e.s("regions"));
    return {};
}

double clipSourceSeconds(const Sequence& s, const Clip& c, double t) {
    const double fps = s.fpsValue();
    const double f = t * fps, base = std::floor(f);
    const double a = c.sourceFrameAt(FrameTime(base)), b = c.sourceFrameAt(FrameTime(base) + 1);
    return (a + (b - a) * (f - base)) / fps;
}

}  // namespace montage
