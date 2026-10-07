#include "AudioSync.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace montage {

namespace {

// RMS envelope with `rate` values per second, mean-removed and normalised.
std::vector<float> envelope(const AudioBuffer& b, double rate) {
    std::vector<float> env;
    if (b.sampleRate <= 0) return env;
    const int64_t win = std::max<int64_t>(1, int64_t(b.sampleRate / rate));
    const int64_t frames = b.frames();
    env.reserve(size_t(frames / win + 1));
    for (int64_t s = 0; s + win <= frames; s += win) {
        double acc = 0;
        for (int64_t i = s; i < s + win; ++i) {
            double m = 0.5 * (b.samples[size_t(i) * 2] + b.samples[size_t(i) * 2 + 1]);
            acc += m * m;
        }
        env.push_back(float(std::sqrt(acc / double(win))));
    }
    // Onsets matter more than absolute level: use the positive derivative of
    // log energy, which is robust to different gains and microphones.
    std::vector<float> out(env.size(), 0.0f);
    for (size_t i = 1; i < env.size(); ++i) {
        float d = std::log(env[i] + 1e-4f) - std::log(env[i - 1] + 1e-4f);
        out[i] = std::max(0.0f, d);
    }
    double mean = 0;
    for (float v : out) mean += v;
    mean /= std::max<size_t>(1, out.size());
    double var = 0;
    for (float& v : out) {
        v = float(v - mean);
        var += double(v) * v;
    }
    float norm = float(std::sqrt(var) + 1e-9);
    for (float& v : out) v /= norm;
    return out;
}

// Normalised cross-correlation of a and b at lag L (b shifted right by L).
double corrAt(const std::vector<float>& a, const std::vector<float>& b, long lag) {
    double acc = 0;
    long n0 = std::max(0L, lag), n1 = std::min(long(a.size()), long(b.size()) + lag);
    for (long i = n0; i < n1; ++i) acc += double(a[size_t(i)]) * b[size_t(i - lag)];
    return acc;
}

}  // namespace

SyncResult findAudioOffset(const AudioBuffer& ref, const AudioBuffer& other, double maxLagSeconds) {
    SyncResult r;
    // Coarse search at 100 Hz over the whole lag range.
    const double coarseRate = 100.0;
    std::vector<float> a = envelope(ref, coarseRate), b = envelope(other, coarseRate);
    if (a.size() < 20 || b.size() < 20) return r;
    long maxLag = long(maxLagSeconds * coarseRate);
    long lo = std::max(-long(b.size()) + 10, -maxLag), hi = std::min(long(a.size()) - 10, maxLag);
    double best = -1e18, second = -1e18;
    long bestLag = 0;
    for (long lag = lo; lag <= hi; ++lag) {
        double c = corrAt(a, b, lag);
        if (c > best) {
            if (std::labs(lag - bestLag) > 20) second = best;
            best = c;
            bestLag = lag;
        } else if (c > second && std::labs(lag - bestLag) > 20) {
            second = c;
        }
    }
    if (best <= 0) return r;
    // Refine at 1 kHz around the coarse peak.
    const double fineRate = 1000.0;
    std::vector<float> fa = envelope(ref, fineRate), fb = envelope(other, fineRate);
    long center = long(std::lround(double(bestLag) * fineRate / coarseRate));
    long span = long(fineRate / coarseRate) * 2;
    double fineBest = -1e18;
    long fineLag = center;
    for (long lag = center - span; lag <= center + span; ++lag) {
        double c = corrAt(fa, fb, lag);
        if (c > fineBest) {
            fineBest = c;
            fineLag = lag;
        }
    }
    r.offset = double(fineLag) / fineRate;
    r.confidence = std::clamp(best, 0.0, 1.0);
    // Require a clear, unique peak.
    r.found = best > 0.15 && (second < 0 || best > second * 1.25);
    return r;
}

}  // namespace montage
