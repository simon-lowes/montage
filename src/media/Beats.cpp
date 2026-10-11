#include "Beats.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <map>
#include <numeric>

#include "Decoder.h"

namespace montage {

namespace {

constexpr int kRate = 22050;
constexpr int kFft = 2048;  // harmony and timbre: long windows
constexpr int kHop = 256;   // 11.6 ms
constexpr int kOnsetFft = 1024;  // onsets: short windows, fine steps
constexpr int kOnsetHop = 128;   // 5.8 ms
constexpr int kMel = 64;
constexpr int kTimbre = 20;  // coarse mel bands for timbre

void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = float(-2 * M_PI / double(len));
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

double hzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
double melToHz(double m) { return 700.0 * (std::pow(10.0, m / 2595.0) - 1.0); }

// Triangular mel filters over the FFT bins: (bin, weight) per band.
std::vector<std::vector<std::pair<int, float>>> melBank(int bands, double lo, double hi, int fftSize) {
    std::vector<std::vector<std::pair<int, float>>> bank(static_cast<size_t>(bands));
    const double mlo = hzToMel(lo), mhi = hzToMel(hi);
    for (int b = 0; b < bands; ++b) {
        const double l = melToHz(mlo + (mhi - mlo) * b / (bands + 1));
        const double c = melToHz(mlo + (mhi - mlo) * (b + 1) / (bands + 1));
        const double r = melToHz(mlo + (mhi - mlo) * (b + 2) / (bands + 1));
        for (int i = 1; i <= fftSize / 2; ++i) {
            const double f = double(i) * kRate / fftSize;
            if (f <= l || f >= r) continue;
            const double w = f <= c ? (f - l) / (c - l) : (r - f) / (r - c);
            bank[size_t(b)].push_back({i, float(w)});
        }
        // Narrow low bands may fall between bins: take the nearest one.
        if (bank[size_t(b)].empty()) bank[size_t(b)].push_back({std::clamp(int(std::lround(c * fftSize / kRate)), 1, fftSize / 2), 1.0f});
    }
    return bank;
}

std::vector<float> resampleTo(const std::vector<float>& in, int from, int to) {
    if (from == to || in.empty()) return in;
    // Box-filter then linear interpolation: enough for onsets and chroma.
    std::vector<float> src = in;
    if (from > to) {
        const int w = std::max(1, int(std::lround(double(from) / to)));
        if (w > 1) {
            std::vector<float> f(src.size());
            double acc = 0;
            for (size_t i = 0; i < src.size(); ++i) {
                acc += src[i];
                if (i >= size_t(w)) acc -= src[i - size_t(w)];
                f[i] = float(acc / std::min<size_t>(i + 1, size_t(w)));
            }
            src.swap(f);
        }
    }
    const size_t n = size_t(double(in.size()) * to / from);
    std::vector<float> out(n);
    const double step = double(from) / to;
    for (size_t i = 0; i < n; ++i) {
        const double pos = double(i) * step;
        const size_t j = std::min(size_t(pos), src.size() - 1), j1 = std::min(j + 1, src.size() - 1);
        out[i] = src[j] + (src[j1] - src[j]) * float(pos - double(j));
    }
    return out;
}

std::vector<float> hann(int n) {
    std::vector<float> w(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) w[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * i / n));
    return w;
}

// Power spectrum of the frame of `size` samples centred on sample `centre`.
void spectrum(const std::vector<float>& x, int64_t centre, const std::vector<float>& window, std::vector<std::complex<float>>& buf,
              std::vector<float>& power) {
    const int size = int(window.size());
    const int64_t start = centre - size / 2, n = int64_t(x.size());
    for (int i = 0; i < size; ++i) {
        const int64_t s = start + i;
        buf[size_t(i)] = s >= 0 && s < n ? x[size_t(s)] * window[size_t(i)] : 0.0f;
    }
    fft(buf);
    for (int i = 0; i <= size / 2; ++i) power[size_t(i)] = std::norm(buf[size_t(i)]);
}

// Per-frame features of the whole piece.
struct Frames {
    int onsetCount = 0;                       // onset frames, kOnsetHop apart
    std::vector<float> onset;                 // spectral flux of the log-mel spectrogram
    std::vector<float> bass;                  // flux of the bass band (< 150 Hz)
    int count = 0;                            // harmony frames, kHop apart
    std::vector<std::array<float, 12>> chroma;
    std::vector<std::array<float, kTimbre>> timbre;  // dB
};

Frames analyse(const std::vector<float>& x, const std::atomic<bool>* cancel, bool onsets = true) {
    Frames f;
    if (x.size() < size_t(kFft)) return f;
    // Onsets.
    if (onsets) {
        static const auto mel = melBank(kMel, 30, 8000, kOnsetFft);
        static const std::vector<float> window = hann(kOnsetFft);
        const int bassBins = std::max(1, int(150.0 * kOnsetFft / kRate));
        f.onsetCount = int(x.size() / kOnsetHop) + 1;
        f.onset.assign(size_t(f.onsetCount), 0.0f);
        f.bass.assign(size_t(f.onsetCount), 0.0f);
        std::vector<std::complex<float>> buf(kOnsetFft);
        std::vector<float> power(kOnsetFft / 2 + 1), melDb(kMel), prevMel(kMel, -100.0f);
        float prevBass = -100;
        for (int t = 0; t < f.onsetCount; ++t) {
            if (cancel && cancel->load()) return {};
            spectrum(x, int64_t(t) * kOnsetHop, window, buf, power);
            float flux = 0;
            for (int b = 0; b < kMel; ++b) {
                float e = 0;
                for (auto [bin, w] : mel[size_t(b)]) e += power[size_t(bin)] * w;
                melDb[size_t(b)] = 10 * std::log10(std::max(e, 1e-10f));
                flux += std::max(0.0f, melDb[size_t(b)] - prevMel[size_t(b)]);
            }
            f.onset[size_t(t)] = t > 0 ? flux / kMel : 0;
            prevMel = melDb;
            float bass = 0;
            for (int i = 1; i <= bassBins; ++i) bass += power[size_t(i)];
            const float bassDb = 10 * std::log10(std::max(bass, 1e-10f));
            f.bass[size_t(t)] = t > 0 ? std::max(0.0f, bassDb - prevBass) : 0;
            prevBass = bassDb;
        }
    }
    // Harmony (chroma from 100 Hz to 4 kHz) and timbre.
    static const auto coarse = melBank(kTimbre, 30, 10000, kFft);
    static const std::vector<float> window = hann(kFft);
    static const std::vector<int> pitchClass = [] {
        std::vector<int> pc(kFft / 2 + 1, -1);
        for (int i = 1; i <= kFft / 2; ++i) {
            const double hz = double(i) * kRate / kFft;
            if (hz < 100 || hz > 4000) continue;
            const int midi = int(std::lround(69 + 12 * std::log2(hz / 440.0)));
            pc[size_t(i)] = ((midi % 12) + 12) % 12;
        }
        return pc;
    }();
    f.count = int(x.size() / kHop) + 1;
    f.chroma.assign(size_t(f.count), {});
    f.timbre.assign(size_t(f.count), {});
    std::vector<std::complex<float>> buf(kFft);
    std::vector<float> power(kFft / 2 + 1);
    for (int t = 0; t < f.count; ++t) {
        if (cancel && cancel->load()) return {};
        spectrum(x, int64_t(t) * kHop, window, buf, power);
        for (int i = 1; i <= kFft / 2; ++i)
            if (pitchClass[size_t(i)] >= 0) f.chroma[size_t(t)][size_t(pitchClass[size_t(i)])] += std::sqrt(power[size_t(i)]);
        for (int b = 0; b < kTimbre; ++b) {
            float e = 0;
            for (auto [bin, w] : coarse[size_t(b)]) e += power[size_t(bin)] * w;
            f.timbre[size_t(t)][size_t(b)] = 10 * std::log10(std::max(e, 1e-10f));
        }
    }
    return f;
}

// The time of an onset frame. Flux peaks while a hit is still entering the
// window, so it reads a little early: kOnsetLead puts it back on the hit.
constexpr double kOnsetLead = 0.017;  // measured on synthetic hits (about half the window's rise)
double frameTime(double frame) { return frame * kOnsetHop / kRate + kOnsetLead; }
int harmonyFrame(double seconds, int count) { return std::clamp(int(std::lround(seconds * kRate / kHop)), 0, count - 1); }

double estimatePeriod(const std::vector<float>& onset) {
    const double fps = double(kRate) / kOnsetHop;
    const int lo = int(fps * 60 / 240), hi = std::min(int(onset.size()) / 2, int(fps * 60 / 40));
    if (hi <= lo + 2) return 0;
    const double mean = std::accumulate(onset.begin(), onset.end(), 0.0) / double(onset.size());
    std::vector<double> o(onset.size());
    for (size_t i = 0; i < o.size(); ++i) o[i] = onset[i] - mean;
    std::vector<double> r(size_t(hi) + 2, 0.0);
    for (int lag = lo - 1; lag <= hi + 1; ++lag) {
        double acc = 0;
        for (size_t i = size_t(lag); i < o.size(); ++i) acc += o[i] * o[i - size_t(lag)];
        r[size_t(lag)] = acc / double(o.size() - size_t(lag));
    }
    double best = -1e30;
    int bestLag = 0;
    for (int lag = lo; lag <= hi; ++lag) {
        const double bpm = 60 * fps / lag;
        const double prior = std::exp(-0.5 * std::pow(std::log2(bpm / 120.0), 2));
        // A beat's multiples reinforce it: count the double period too.
        const double v = (r[size_t(lag)] + (2 * lag <= hi ? 0.5 * r[size_t(2 * lag)] : 0.0)) * prior;
        if (v > best) best = v, bestLag = lag;
    }
    if (bestLag <= 0) return 0;
    // Parabolic refinement.
    const double a = r[size_t(bestLag - 1)], b = r[size_t(bestLag)], c = r[size_t(bestLag + 1)];
    const double den = a - 2 * b + c;
    const double shift = std::fabs(den) > 1e-12 ? std::clamp(0.5 * (a - c) / den, -0.5, 0.5) : 0.0;
    return bestLag + shift;
}

// Ellis' dynamic-programming beat tracker.
std::vector<int> trackBeats(const std::vector<float>& onset, double period) {
    const int n = int(onset.size());
    std::vector<int> beats;
    if (n == 0 || period < 2) return beats;
    // Normalise, and smooth with a Gaussian a few frames wide.
    double sd = 0, mean = std::accumulate(onset.begin(), onset.end(), 0.0) / n;
    for (float v : onset) sd += (v - mean) * (v - mean);
    sd = std::sqrt(sd / n) + 1e-9;
    const int half = int(std::lround(period));
    std::vector<double> kernel(size_t(2 * half + 1));
    for (int i = -half; i <= half; ++i) kernel[size_t(i + half)] = std::exp(-0.5 * std::pow(i * 32.0 / period, 2));
    std::vector<double> local(size_t(n), 0.0);
    for (int t = 0; t < n; ++t) {
        double acc = 0;
        for (int i = -half; i <= half; ++i)
            if (t + i >= 0 && t + i < n) acc += kernel[size_t(i + half)] * onset[size_t(t + i)] / sd;
        local[size_t(t)] = acc;
    }
    const double tightness = 100;
    std::vector<double> cum(static_cast<size_t>(n));
    std::vector<int> back(size_t(n), -1);
    const int wlo = int(std::lround(period / 2)), whi = int(std::lround(2 * period));
    for (int t = 0; t < n; ++t) {
        double best = -std::numeric_limits<double>::infinity();
        int arg = -1;
        for (int tau = t - whi; tau <= t - wlo; ++tau) {
            if (tau < 0) continue;
            const double v = cum[size_t(tau)] - tightness * std::pow(std::log(double(t - tau) / period), 2);
            if (v > best) best = v, arg = tau;
        }
        cum[size_t(t)] = local[size_t(t)] + (arg >= 0 ? best : 0.0);
        back[size_t(t)] = arg;
    }
    // The last beat: the last local peak of the cumulative score that is strong enough.
    std::vector<double> peaks;
    for (int t = 1; t + 1 < n; ++t)
        if (cum[size_t(t)] > cum[size_t(t - 1)] && cum[size_t(t)] >= cum[size_t(t + 1)]) peaks.push_back(cum[size_t(t)]);
    if (peaks.empty()) return beats;
    std::vector<double> sorted = peaks;
    std::nth_element(sorted.begin(), sorted.begin() + long(sorted.size() / 2), sorted.end());
    const double median = sorted[sorted.size() / 2];
    int last = -1;
    for (int t = n - 2; t >= 1; --t)
        if (cum[size_t(t)] > cum[size_t(t - 1)] && cum[size_t(t)] >= cum[size_t(t + 1)] && cum[size_t(t)] > 0.5 * median) {
            last = t;
            break;
        }
    for (int t = last; t >= 0; t = back[size_t(t)]) beats.push_back(t);
    std::reverse(beats.begin(), beats.end());
    // Drop weak beats at either end (silence before and after the music).
    double rms = 0;
    for (int b : beats) rms += local[size_t(b)] * local[size_t(b)];
    rms = std::sqrt(rms / std::max<size_t>(1, beats.size()));
    while (!beats.empty() && local[size_t(beats.front())] < 0.5 * rms) beats.erase(beats.begin());
    while (!beats.empty() && local[size_t(beats.back())] < 0.5 * rms) beats.pop_back();
    return beats;
}

// Beat-synchronous mean of a per-frame vector feature over [beat k, beat k+1).
template <size_t N>
std::vector<std::array<float, N>> perBeat(const std::vector<std::array<float, N>>& v, const std::vector<int>& beats, int frames) {
    std::vector<std::array<float, N>> out(beats.size());
    for (size_t k = 0; k < beats.size(); ++k) {
        const int a = beats[k], b = k + 1 < beats.size() ? beats[k + 1] : std::min(frames, a + (k > 0 ? a - beats[k - 1] : 20));
        std::array<float, N> acc{};
        int cnt = 0;
        for (int t = a; t < std::max(a + 1, b) && t < int(v.size()); ++t, ++cnt)
            for (size_t i = 0; i < N; ++i) acc[i] += v[size_t(t)][i];
        for (float& x : acc) x /= float(std::max(1, cnt));
        out[k] = acc;
    }
    return out;
}

template <size_t N>
float cosine(const std::array<float, N>& a, const std::array<float, N>& b) {
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < N; ++i) ab += a[i] * b[i], aa += a[i] * a[i], bb += b[i] * b[i];
    return aa > 0 && bb > 0 ? float(ab / std::sqrt(aa * bb)) : 0.0f;
}

struct Analysis {
    Frames frames;
    std::vector<int> beats;  // frames
    BeatGrid grid;
};

Analysis analyseMusic(const std::vector<float>& mono, int rate, const std::atomic<bool>* cancel) {
    Analysis a;
    const std::vector<float> x = resampleTo(mono, rate, kRate);
    a.grid.duration = double(mono.size()) / std::max(1, rate);
    a.frames = analyse(x, cancel);
    if (a.frames.count == 0) return a;
    const double period = estimatePeriod(a.frames.onset);
    if (period <= 0) return a;
    a.beats = trackBeats(a.frames.onset, period);
    if (a.beats.size() < 4) {
        a.beats.clear();
        return a;
    }
    // The tempo from the beats found: a line through their times (frames are 11.6 ms apart,
    // so single gaps would round it).
    {
        const double n = double(a.beats.size());
        double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t k = 0; k < a.beats.size(); ++k) {
            const double t = frameTime(a.beats[k]);
            sx += double(k), sy += t, sxx += double(k) * double(k), sxy += double(k) * t;
        }
        const double slope = (n * sxy - sx * sy) / std::max(1e-12, n * sxx - sx * sx);
        a.grid.tempo = slope > 0 ? 60.0 / slope : 0;
    }
    for (int b : a.beats) a.grid.beats.push_back(frameTime(b));

    // Bars: the phase where bass onsets and chord changes fall.
    std::vector<int> harmonyBeats;
    for (double t : a.grid.beats) harmonyBeats.push_back(harmonyFrame(t, a.frames.count));
    const auto chroma = perBeat(a.frames.chroma, harmonyBeats, a.frames.count);
    std::vector<double> bassAt(a.beats.size()), change(a.beats.size(), 0.0);
    for (size_t k = 0; k < a.beats.size(); ++k) {
        float m = 0;
        for (int d = -2; d <= 2; ++d) {
            const int t = a.beats[k] + d;
            if (t >= 0 && t < a.frames.onsetCount) m = std::max(m, a.frames.bass[size_t(t)]);
        }
        bassAt[k] = m;
        if (k > 0) change[k] = 1.0 - cosine(chroma[k], chroma[k - 1]);
    }
    auto z = [](std::vector<double>& v) {
        const double mean = std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
        double sd = 0;
        for (double x : v) sd += (x - mean) * (x - mean);
        sd = std::sqrt(sd / double(v.size())) + 1e-9;
        for (double& x : v) x = (x - mean) / sd;
    };
    z(bassAt);
    z(change);
    const int bpb = a.grid.beatsPerBar;
    int phase = 0;
    double best = -1e30;
    for (int p = 0; p < bpb; ++p) {
        double s = 0;
        int cnt = 0;
        for (size_t k = size_t(p); k < a.beats.size(); k += size_t(bpb)) s += bassAt[k] + change[k], ++cnt;
        if (cnt && s / cnt > best) best = s / cnt, phase = p;
    }
    for (size_t k = size_t(phase); k < a.beats.size(); k += size_t(bpb)) a.grid.downbeats.push_back(a.grid.beats[k]);
    return a;
}

}  // namespace

bool decodeMono(const std::string& path, int rate, std::vector<float>& out, const std::atomic<bool>* cancel, std::string* error) {
    AudioBufferPtr buf = decodeAudio(path, rate, error, cancel);
    if (!buf) return false;
    out.resize(size_t(buf->frames()));
    for (int64_t i = 0; i < buf->frames(); ++i) out[size_t(i)] = 0.5f * (buf->samples[size_t(i) * 2] + buf->samples[size_t(i) * 2 + 1]);
    return true;
}

BeatGrid detectBeats(const std::vector<float>& mono, int rate, const std::atomic<bool>* cancel) {
    return analyseMusic(mono, rate, cancel).grid;
}

BeatGrid detectBeatsInFile(const std::string& path, const std::atomic<bool>* cancel, std::string* error) {
    std::vector<float> mono;
    if (!decodeMono(path, kRate, mono, cancel, error)) return {};
    BeatGrid g = detectBeats(mono, kRate, cancel);
    if (g.empty() && error && error->empty()) *error = "No beat was found";
    return g;
}

MusicFit fitMusic(const std::vector<float>& mono, int rate, const BeatGrid& grid, double target, const FitOptions& o,
                  const std::atomic<bool>* cancel) {
    MusicFit fit;
    const double total = double(mono.size()) / std::max(1, rate);
    if (grid.downbeats.size() < 4 || target <= 0 || total <= 0) return fit;
    const auto& bars = grid.downbeats;  // bar starts
    const int B = int(bars.size());
    const double barLen = (bars.back() - bars.front()) / std::max(1, B - 1);
    // Close enough already: the whole piece.
    if (std::fabs(total - target) <= barLen / 2) {
        fit.segments = {{0, total}};
        fit.duration = total;
        fit.similarity = 1;
        return fit;
    }

    // Beat-synchronous features: chroma (harmony) and timbre, each normalised.
    const std::vector<float> x = resampleTo(mono, rate, kRate);
    const Frames fr = analyse(x, cancel, false);
    if (fr.count == 0) return fit;
    std::vector<int> beatFrames;
    for (double b : grid.beats) beatFrames.push_back(harmonyFrame(b, fr.count));
    const auto chroma = perBeat(fr.chroma, beatFrames, fr.count);
    auto timbre = perBeat(fr.timbre, beatFrames, fr.count);
    for (size_t i = 0; i < kTimbre; ++i) {
        double mean = 0, sd = 0;
        for (const auto& t : timbre) mean += t[i];
        mean /= double(timbre.size());
        for (const auto& t : timbre) sd += (t[i] - mean) * (t[i] - mean);
        sd = std::sqrt(sd / double(timbre.size())) + 1e-6;
        for (auto& t : timbre) t[i] = float((t[i] - mean) / sd);
    }
    const int nb = int(grid.beats.size());
    auto similar = [&](int a, int b) {
        const double c = cosine(chroma[size_t(a)], chroma[size_t(b)]);
        const double t = cosine(timbre[size_t(a)], timbre[size_t(b)]);
        return 0.5 * (0.6 * c + 0.4 * t) + 0.5;  // 0..1
    };
    // The beat index of each bar start.
    std::vector<int> barBeat(static_cast<size_t>(B));
    for (int b = 0; b < B; ++b)
        barBeat[size_t(b)] = int(std::lower_bound(grid.beats.begin(), grid.beats.end(), bars[size_t(b)] - 1e-6) - grid.beats.begin());
    // How alike the music is around two bar starts: a bar before and a bar after.
    auto joinQuality = [&](int from, int to) {
        double s = 0;
        int cnt = 0;
        for (int k = -grid.beatsPerBar; k < grid.beatsPerBar; ++k) {
            const int a = barBeat[size_t(from)] + k, b = barBeat[size_t(to)] + k;
            if (a < 0 || b < 0 || a >= nb || b >= nb) continue;
            s += similar(a, b);
            ++cnt;
        }
        return cnt >= grid.beatsPerBar ? s / cnt : 0.0;
    };
    // Jumps are allowed between bar starts inside the middle of the piece, to the best few matches.
    const double lo = std::max(o.keepStart, bars.front()), hi = total - o.keepEnd;
    std::vector<std::vector<std::pair<int, double>>> jumps(static_cast<size_t>(B));
    for (int from = 1; from < B - 1; ++from) {
        if (bars[size_t(from)] < lo || bars[size_t(from)] > hi) continue;
        std::vector<std::pair<int, double>> cands;
        for (int to = 1; to < B - 1; ++to) {
            if (std::abs(to - from) < 1 || bars[size_t(to)] < lo || bars[size_t(to)] > hi) continue;
            cands.push_back({to, joinQuality(from, to)});
        }
        std::sort(cands.begin(), cands.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if (cands.size() > 10) cands.resize(10);
        jumps[size_t(from)] = std::move(cands);
    }
    if (cancel && cancel->load()) return fit;

    // Paths through the bars: from bar 0, playing bars in order or jumping, to the last bar (and the tail).
    // State: (bar start x, bars played n, jumps used k, just jumped j). Cost: unlikeness of each join.
    const double pickup = bars.front(), tail = total - bars.back();
    std::vector<double> barDur(static_cast<size_t>(B));
    for (int b = 0; b + 1 < B; ++b) barDur[size_t(b)] = bars[size_t(b) + 1] - bars[size_t(b)];
    barDur[size_t(B - 1)] = tail;
    const int maxBars = int(std::ceil((target - pickup - tail) / barLen)) + 3;
    if (maxBars < 1) return fit;
    const int K = std::max(0, o.maxJumps);
    const double inf = std::numeric_limits<double>::infinity();
    struct Cell {
        double cost = std::numeric_limits<double>::infinity();
        double secs = 0;
        int px = -1, pn = -1, pk = -1, pj = -1;
    };
    const auto idx = [&](int x, int n, int k, int j) { return ((size_t(x) * size_t(maxBars + 1) + size_t(n)) * size_t(K + 1) + size_t(k)) * 2 + size_t(j); };
    std::vector<Cell> dp(size_t(B) * size_t(maxBars + 1) * size_t(K + 1) * 2);
    dp[idx(0, 0, 0, 0)].cost = 0;
    dp[idx(0, 0, 0, 0)].secs = pickup;
    // Bars played only grow, so process by n; jumps keep n and are followed by a bar.
    for (int n = 0; n <= maxBars; ++n) {
        for (int k = 0; k <= K; ++k)
            for (int x = 0; x < B; ++x) {
                const Cell c = dp[idx(x, n, k, 0)];
                if (c.cost == inf || k == K) continue;
                for (auto [to, q] : jumps[size_t(x)]) {
                    Cell& d = dp[idx(to, n, k + 1, 1)];
                    const double cost = c.cost + (1 - q) + 0.02;
                    if (cost < d.cost) d = {cost, c.secs, x, n, k, 0};
                }
            }
        if (n == maxBars) break;
        for (int k = 0; k <= K; ++k)
            for (int x = 0; x + 1 < B; ++x)
                for (int j = 0; j < 2; ++j) {
                    const Cell c = dp[idx(x, n, k, j)];
                    if (c.cost == inf) continue;
                    Cell& d = dp[idx(x + 1, n + 1, k, 0)];
                    if (c.cost < d.cost) d = {c.cost, c.secs + barDur[size_t(x)], x, n, k, j};
                }
    }
    // The best ending: at the last bar start, then the tail; the error from the target weighs like a poor join.
    double bestScore = inf;
    int bn = -1, bk = -1, bj = -1;
    for (int n = 0; n <= maxBars; ++n)
        for (int k = 0; k <= K; ++k)
            for (int j = 0; j < 2; ++j) {
                const Cell& c = dp[idx(B - 1, n, k, j)];
                if (c.cost == inf) continue;
                const double err = std::fabs(c.secs + tail - target);
                const double score = c.cost + err / barLen;
                if (score < bestScore) bestScore = score, bn = n, bk = k, bj = j;
            }
    if (bn < 0) return fit;
    // Back through the path: the bars played, in order.
    std::vector<int> played;  // bar start indices visited, with -1 marking a jump
    for (int x = B - 1, n = bn, k = bk, j = bj; x >= 0;) {
        const Cell& c = dp[idx(x, n, k, j)];
        played.push_back(x);
        if (j == 1) played.push_back(-1);  // reached by a jump (reversed below)
        if (c.px < 0) break;
        const int px = c.px, pn = c.pn, pk = c.pk, pj = c.pj;
        x = px, n = pn, k = pk, j = pj;
    }
    std::reverse(played.begin(), played.end());
    // Segments: from the start to each jump, then on from where it lands, to the end.
    double segStart = 0, worst = 1;
    int prev = 0;
    for (size_t i = 0; i < played.size(); ++i) {
        if (played[i] != -1) {
            prev = played[i];
            continue;
        }
        const int from = prev, to = played[i + 1];
        fit.segments.push_back({segStart, bars[size_t(from)]});
        worst = std::min(worst, joinQuality(from, to));
        segStart = bars[size_t(to)];
    }
    fit.segments.push_back({segStart, total});
    for (const auto& s : fit.segments) fit.duration += s.out - s.in;
    fit.similarity = worst;
    return fit;
}

}  // namespace montage
