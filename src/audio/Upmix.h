// Montage — upmixing stereo to surround (5.1, 7.1 and the layouts with overhead speakers) or to an ambisonic field,
// so that the layout's stereo fold-down (core/Surround.h downmixToStereo) gives back the stereo it came from.
//
// Each bin of a short-time Fourier transform (2048-point square-root Hann windows, 75 % overlap) is split into a
// direct part, alike in both channels, and an ambient part, unrelated between them: from the channels' powers and
// cross-power, averaged over about 150 ms and the neighbouring bins, the ambient power is the smaller eigenvalue of
// their 2×2 covariance (principal components), and each channel's direct part its Wiener share of the bin. The direct
// part keeps the place the stereo gave it: what both sides have in common, in phase, goes to the centre speaker and
// is taken from each side as much as the centre folds back into it, so the fold-down's left and right are exactly the
// direct sound whatever its level, phase or time between the sides (Centre Width keeps more of it at the sides; in an
// ambisonic field it is encoded across the front by its level between the sides). The ambient part stays partly in
// front and goes partly (Surround Depth) to the surround and overhead speakers (Height), each through all-pass filters
// of its own and a short delay so the speakers do not sound alike, at a level that keeps the fold-down's loudness.
// Dialogue, given apart, goes to the centre alone, as mono. The LFE can be given a copy of the sum under 120 Hz (off
// unless asked; the mains keep it).
#pragma once

#include <complex>
#include <string>
#include <vector>

namespace montage {

struct UpmixSettings {
    double centreWidth = 0;      // 0: the direct sound across left, centre and right; 1: left and right only
    double surroundDepth = 0.5;  // the share of the ambient sound's power sent behind (and above)
    double height = 0.3;         // the share of that taken to the overhead speakers, where there are any
    double delayMs = 10;         // the surround and overhead speakers' delay
    bool lfe = false;            // the sum under 120 Hz to the LFE
};

class Upmixer {
public:
    static constexpr int kFft = 2048, kHop = 512;
    static constexpr int kLatency = kFft;  // samples
    Upmixer(std::string layout, int rate);
    const std::string& layout() const { return layout_; }
    int rate() const { return rate_; }
    int channels() const { return nch_; }
    // `stereo` (frames × 2, interleaved) upmixed and `dialogue` (frames × 2, or null) put in the centre, into `out`
    // (frames × the layout's channels, overwritten), kLatency samples late. `delayedStereo` (frames × 2, or null; it
    // may be `stereo` itself) gets the stereo and dialogue added together, as late.
    void process(const float* stereo, const float* dialogue, int frames, const UpmixSettings& s, float* out,
                 float* delayedStereo = nullptr);
    void reset();

private:
    struct AllPass {
        std::vector<float> line;
        size_t at = 0;
        float run(float x);
    };
    struct Destination {  // where one side's ambient sound goes behind or above
        int side = 0;     // 0 left, 1 right
        std::vector<float> gains;  // into each output channel
        float share = 0;  // of the ambient power sent behind and above (the fold-down's level folded in)
        bool above = false;
        AllPass stages[3];
    };
    void frame(const UpmixSettings& s);

    std::string layout_;
    int rate_, nch_;
    int lfeCh_ = -1, centreCh_ = -1, leftCh_ = -1, rightCh_ = -1;
    bool ambisonic_ = false;
    std::vector<float> window_;
    std::vector<int> front_;  // the output channels the direct sound can reach (the inverse transforms made)
    std::vector<float> dialogueGains_, frontLeft_, frontRight_;  // gain vectors (ambisonic: encoded directions)
    std::vector<Destination> dest_;
    // The direct sound's gains into the front channels, by its place between left (0) and right (kPlaces - 1), for no
    // centre width and for full width.
    static constexpr int kPlaces = 129;
    std::vector<std::vector<float>> narrow_, wide_;
    // Streaming state.
    std::vector<float> inL_, inR_, dlgL_, dlgR_;  // the latest kFft input samples
    int sinceFrame_ = 0;
    std::vector<double> pLL_, pRR_;
    std::vector<std::complex<double>> pLR_;
    bool primed_ = false;
    std::vector<std::vector<float>> acc_;  // overlap-add per front channel, then ambient left and right, then the LFE
    std::vector<float> ready_;             // finished output frames (channels + 2 for the delayed stereo), in order
    size_t readAt_ = 0;
    std::vector<float> delayLine_[2];  // the ambient sound behind, delayed
    size_t delayAt_ = 0;
};

}  // namespace montage
