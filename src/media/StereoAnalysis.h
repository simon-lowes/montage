// Montage — audio analysis beside the loudness meter: the phase correlation of a channel pair (how alike left and right
// are: +1 the same, 0 unrelated, -1 one the other inverted), the goniometer's mid-against-side picture, and a spectrum
// analyser in third-octave bands or FFT bins with peak hold and optional K-weighting (ITU-R BS.1770's curve). Quality
// Check uses the same sums for mono compatibility (what a stereo-to-mono fold-down loses), sustained negative
// correlation, a channel that is the other inverted, and DC offset.
#pragma once

#include <array>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace montage {

// From a stretch's sums of left², right² and left×right: the correlation (0 when either side is silent) and how many
// dB quieter the mono fold-down ((L+R)/2) is than the stereo ((L²+R²)/2); 0 for identical channels, 3 for unrelated
// ones, large for one the other inverted (capped at 60).
double stereoCorrelation(double ll, double rr, double lr);
double foldDownLossDb(double ll, double rr, double lr);

// The live correlation over the last `window` seconds and the latest samples as goniometer points (mid up, side
// across, each as a fraction of full scale).
class StereoAnalyser {
public:
    explicit StereoAnalyser(int rate, double window = 0.3);
    void add(const float* stereo, int frames);
    void reset();
    double correlation() const;
    double foldDownLossDb() const;
    // Up to `maxPoints` of the latest samples, evenly picked, as (side, mid).
    std::vector<std::pair<float, float>> points(int maxPoints) const;

private:
    int rate_;
    int window_;  // samples
    std::deque<std::array<double, 3>> blocks_;  // each added block's sums (ll, rr, lr) and their sample count below
    std::deque<int> counts_;
    int summed_ = 0;
    std::vector<float> recent_;  // the latest samples, interleaved
};

// Third-octave bands (ISO 266 centres, 20 Hz to 20 kHz) or FFT bins of what is added, with a full-scale sine reading
// 0 dB (the bands from about 63 Hz: below that a band is narrower than the FFT resolves at 48 kHz, and a tone reads up
// to 4 dB low, the rest of it in the bands beside); held peaks fall 20 dB a second.
class SpectrumAnalyser {
public:
    static constexpr int kFft = 8192;  // 4096 bins
    static const std::vector<double>& bandCentres();  // 31 bands
    explicit SpectrumAnalyser(int rate);
    void add(const float* stereo, int frames);  // both channels' power, averaged
    void reset();
    void setKWeighting(bool on) { kWeighted_ = on; }
    bool kWeighting() const { return kWeighted_; }
    // The latest frame's levels (dB) and the held peaks.
    const std::vector<double>& bands() const { return bands_; }
    const std::vector<double>& bandPeaks() const { return bandPeaks_; }
    const std::vector<double>& bins() const { return bins_; }  // kFft / 2 bins, 0 .. rate / 2
    const std::vector<double>& binPeaks() const { return binPeaks_; }
    int rate() const { return rate_; }
    // BS.1770's K-weighting at a frequency, in dB (about +0.7 at 1 kHz, +4 above 2 kHz, falling under 100 Hz).
    static double kWeightDb(double hz);

private:
    void analyse();
    int rate_;
    int hop_;  // samples between analyses (20 a second)
    bool kWeighted_ = false;
    std::vector<float> window_;
    std::vector<float> left_, right_;  // the latest samples, up to kFft (more until trimmed)
    std::vector<std::vector<std::pair<int, double>>> bandBins_;  // each band's bins and the share of each inside it
    std::vector<double> kGain_;  // each bin's K-weighting, as power
    std::vector<double> bands_, bandPeaks_, bins_, binPeaks_;
    int sinceFrame_ = 0;
    double bandScale_ = 1, binScale_ = 1;
};

// What the Meters dock shows (app/AudioAnalysisWidget.h), taken from both analysers.
struct AudioAnalysis {
    double correlation = 0, foldDownLossDb = 0;
    std::vector<std::pair<float, float>> points;  // goniometer (side, mid)
    std::vector<double> bands, bandPeaks, bins, binPeaks;
    int rate = 48000;
    bool kWeighted = false;
};
using AudioAnalysisPtr = std::shared_ptr<const AudioAnalysis>;
AudioAnalysisPtr takeAudioAnalysis(const StereoAnalyser& stereo, const SpectrumAnalyser& spectrum, int maxPoints = 512);

}  // namespace montage
