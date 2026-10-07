#include "Loudness.h"

#include <algorithm>
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

}  // namespace

LoudnessResult measureLoudness(const AudioBuffer& buf, int64_t first, int64_t count) {
    LoudnessResult r;
    const int64_t total = buf.frames();
    first = std::clamp<int64_t>(first, 0, total);
    int64_t last = count < 0 ? total : std::min(total, first + count);
    if (last <= first || buf.sampleRate <= 0) return r;
    const double fs = buf.sampleRate;
    KFilter k(fs);
    // Mean square per 100 ms step; blocks are 400 ms with 75 % overlap.
    const int64_t step = std::max<int64_t>(1, int64_t(fs * 0.1));
    std::vector<double> steps;
    double acc = 0, peak = 0;
    int64_t n = 0;
    for (int64_t i = first; i < last; ++i) {
        double l = buf.samples[size_t(i) * 2], rr = buf.samples[size_t(i) * 2 + 1];
        peak = std::max({peak, std::fabs(l), std::fabs(rr)});
        double kl = k.process(0, l), kr = k.process(1, rr);
        acc += kl * kl + kr * kr;  // channel weights are 1.0 for L/R
        if (++n == step) {
            steps.push_back(acc / double(step));
            acc = 0;
            n = 0;
        }
    }
    r.truePeakDb = peak > 0 ? 20.0 * std::log10(peak) : -96.0;
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
    double relGate = lufs(sum / cnt) - 10.0;
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

}  // namespace montage
