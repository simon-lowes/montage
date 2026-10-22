#include "StereoAnalysis.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>

#include "audio/RoomResponse.h"

namespace montage {

namespace {

constexpr double kFloorDb = -120;
constexpr double kPeakFall = 20;   // dB a second
constexpr int kRecent = 2048;      // goniometer samples kept

double toDb(double power) { return power > 1e-12 ? std::max(kFloorDb, 10 * std::log10(power)) : kFloorDb; }

}  // namespace

double stereoCorrelation(double ll, double rr, double lr) {
    if (ll <= 1e-9 || rr <= 1e-9) return 0;
    return std::clamp(lr / std::sqrt(ll * rr), -1.0, 1.0);
}

double foldDownLossDb(double ll, double rr, double lr) {
    const double stereo = (ll + rr) / 2;
    if (stereo <= 1e-9) return 0;
    const double mono = std::max(0.0, (ll + rr + 2 * lr) / 4);
    if (mono <= stereo * 1e-6) return 60;
    return std::clamp(10 * std::log10(stereo / mono), 0.0, 60.0);
}

StereoAnalyser::StereoAnalyser(int rate, double window) : rate_(std::max(1, rate)), window_(std::max(1, int(window * rate_))) {}

void StereoAnalyser::add(const float* stereo, int frames) {
    if (frames <= 0) return;
    std::array<double, 3> s{};
    for (int i = 0; i < frames; ++i) {
        const double l = stereo[2 * i], r = stereo[2 * i + 1];
        s[0] += l * l;
        s[1] += r * r;
        s[2] += l * r;
    }
    blocks_.push_back(s);
    counts_.push_back(frames);
    summed_ += frames;
    while (blocks_.size() > 1 && summed_ - counts_.front() >= window_) {
        summed_ -= counts_.front();
        blocks_.pop_front();
        counts_.pop_front();
    }
    recent_.insert(recent_.end(), stereo, stereo + 2 * size_t(frames));
    if (recent_.size() > 2 * size_t(kRecent)) recent_.erase(recent_.begin(), recent_.end() - 2 * kRecent);
}

void StereoAnalyser::reset() {
    blocks_.clear();
    counts_.clear();
    summed_ = 0;
    recent_.clear();
}

double StereoAnalyser::correlation() const {
    // Summed afresh each time (a few dozen blocks), so nothing drifts from adding and taking away.
    std::array<double, 3> s{};
    for (const auto& b : blocks_)
        for (int k = 0; k < 3; ++k) s[size_t(k)] += b[size_t(k)];
    return stereoCorrelation(s[0], s[1], s[2]);
}

double StereoAnalyser::foldDownLossDb() const {
    std::array<double, 3> s{};
    for (const auto& b : blocks_)
        for (int k = 0; k < 3; ++k) s[size_t(k)] += b[size_t(k)];
    return montage::foldDownLossDb(s[0], s[1], s[2]);
}

std::vector<std::pair<float, float>> StereoAnalyser::points(int maxPoints) const {
    std::vector<std::pair<float, float>> out;
    const int n = int(recent_.size() / 2);
    if (n == 0 || maxPoints <= 0) return out;
    const int count = std::min(n, maxPoints);
    out.reserve(size_t(count));
    for (int i = 0; i < count; ++i) {
        const int at = int(int64_t(i) * n / count);
        const float l = recent_[2 * size_t(at)], r = recent_[2 * size_t(at) + 1];
        out.emplace_back((r - l) / 2, (l + r) / 2);  // left alone leans up and to the left
    }
    return out;
}

const std::vector<double>& SpectrumAnalyser::bandCentres() {
    static const std::vector<double> c = {20,   25,   31.5, 40,   50,   63,   80,    100,   125,   160,   200,
                                          250,  315,  400,  500,  630,  800,  1000,  1250,  1600,  2000,  2500,
                                          3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000};
    return c;
}

SpectrumAnalyser::SpectrumAnalyser(int rate) : rate_(std::max(1, rate)), hop_(std::max(1, rate_ / 20)), window_(kFft) {
    double sw = 0, s2 = 0;
    for (int i = 0; i < kFft; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / kFft);
        window_[size_t(i)] = float(w);
        sw += w;
        s2 += w * w;
    }
    // Bands: mean square, doubled so a full-scale sine (mean square 0.5) reads 0 dB. Bins: amplitude, a full-scale sine
    // on a bin reading 0 dB.
    bandScale_ = 2.0 / (double(kFft) * s2) * 2;
    binScale_ = 4.0 / (sw * sw);
    const double df = double(rate_) / kFft;
    for (double c : bandCentres()) {
        const double lo = c * std::pow(2.0, -1.0 / 6), hi = c * std::pow(2.0, 1.0 / 6);
        std::vector<std::pair<int, double>> bins;
        for (int k = std::max(1, int(lo / df) - 1); k <= std::min(kFft / 2 - 1, int(hi / df) + 1); ++k) {
            const double share = (std::min(hi, (k + 0.5) * df) - std::max(lo, (k - 0.5) * df)) / df;
            if (share > 0) bins.emplace_back(k, std::min(1.0, share));
        }
        bandBins_.push_back(std::move(bins));
    }
    for (int k = 0; k < kFft / 2; ++k) kGain_.push_back(std::pow(10.0, kWeightDb(k * df) / 10));
    reset();
}

void SpectrumAnalyser::reset() {
    left_.clear();
    right_.clear();
    sinceFrame_ = 0;
    bands_.assign(bandCentres().size(), kFloorDb);
    bandPeaks_ = bands_;
    bins_.assign(kFft / 2, kFloorDb);
    binPeaks_ = bins_;
}

void SpectrumAnalyser::add(const float* stereo, int frames) {
    for (int i = 0; i < frames; ++i) {
        left_.push_back(stereo[2 * i]);
        right_.push_back(stereo[2 * i + 1]);
        if (++sinceFrame_ >= hop_ && int(left_.size()) >= kFft) {
            sinceFrame_ = 0;
            analyse();
        }
    }
    if (left_.size() > 2 * size_t(kFft)) {
        left_.erase(left_.begin(), left_.end() - kFft);
        right_.erase(right_.begin(), right_.end() - kFft);
    }
}

double SpectrumAnalyser::kWeightDb(double hz) {
    // The two BS.1770 filters at 48 kHz: the head's high shelf, then the revised low-frequency B high-pass.
    const double w = 2 * M_PI * std::clamp(hz, 1.0, 23999.0) / 48000;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
    const auto h = [&](double b0, double b1, double b2, double a1, double a2) {
        return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
    };
    const std::complex<double> shelf = h(1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585);
    const std::complex<double> highPass = h(1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621);
    return 20 * std::log10(std::max(1e-12, std::abs(shelf * highPass)));
}

void SpectrumAnalyser::analyse() {
    const size_t start = left_.size() - kFft;
    std::vector<double> power(kFft / 2, 0.0);
    for (const std::vector<float>* ch : {&left_, &right_}) {
        std::vector<std::complex<float>> a(kFft);
        for (int i = 0; i < kFft; ++i) a[size_t(i)] = (*ch)[start + size_t(i)] * window_[size_t(i)];
        fftInPlace(a, false);
        for (int k = 0; k < kFft / 2; ++k) power[size_t(k)] += std::norm(a[size_t(k)]) / 2;  // both channels averaged
    }
    if (kWeighted_)
        for (int k = 0; k < kFft / 2; ++k) power[size_t(k)] *= kGain_[size_t(k)];
    const double fall = kPeakFall * hop_ / rate_;
    for (int k = 0; k < kFft / 2; ++k) {
        bins_[size_t(k)] = toDb(power[size_t(k)] * binScale_);
        binPeaks_[size_t(k)] = std::max(bins_[size_t(k)], binPeaks_[size_t(k)] - fall);
    }
    for (size_t b = 0; b < bandBins_.size(); ++b) {
        double s = 0;
        for (const auto& [k, share] : bandBins_[b]) s += power[size_t(k)] * share;
        bands_[b] = toDb(s * bandScale_);
        bandPeaks_[b] = std::max(bands_[b], bandPeaks_[b] - fall);
    }
}

AudioAnalysisPtr takeAudioAnalysis(const StereoAnalyser& stereo, const SpectrumAnalyser& spectrum, int maxPoints) {
    auto a = std::make_shared<AudioAnalysis>();
    a->correlation = stereo.correlation();
    a->foldDownLossDb = stereo.foldDownLossDb();
    a->points = stereo.points(maxPoints);
    a->bands = spectrum.bands();
    a->bandPeaks = spectrum.bandPeaks();
    a->bins = spectrum.bins();
    a->binPeaks = spectrum.binPeaks();
    a->rate = spectrum.rate();
    a->kWeighted = spectrum.kWeighting();
    return a;
}

}  // namespace montage
