// Montage — built-in audio effects past EQ3, compressor, limiter and delay:
// a five-band parametric EQ, a de-esser, a noise gate, a reverb (Freeverb's
// design), and channel tools (fill left or right, mono, swap, polarity). All
// work in place on interleaved stereo and keep their state between blocks.
#pragma once

#include <array>
#include <vector>

namespace montage::fx {

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] = {0, 0}, z2[2] = {0, 0};
    void peaking(double fs, double f0, double q, double db);
    void lowShelf(double fs, double f0, double db);
    void highShelf(double fs, double f0, double db);
    void highPass(double fs, double f0, double q = 0.70710678);
    void lowPass(double fs, double f0, double q = 0.70710678);
    // The filter's gain at `hz`, in dB.
    double responseDb(double fs, double hz) const;
    float process(int ch, float x) {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return float(y);
    }

private:
    void set(double nb0, double nb1, double nb2, double a0, double na1, double na2);
};

// Low shelf, three bells and a high shelf, then an output gain.
struct EqBand {
    double hz = 1000, db = 0, q = 1;
};
class ParametricEq {
public:
    void set(double sr, const EqBand& low, const EqBand& b1, const EqBand& b2, const EqBand& b3, const EqBand& high, double outputDb);
    void process(float* buf, int frames);
    // The whole EQ's gain at `hz`, in dB (output gain included).
    double responseDb(double hz) const;

private:
    std::array<Biquad, 5> bands_;
    double sr_ = 48000;
    float output_ = 1;
};

// Turns down the sibilant band (above `hz`) when it is louder than the
// threshold. The band is split off with a Linkwitz-Riley crossover, whose two
// halves add back up flat, so nothing changes until it acts.
class DeEsser {
public:
    void process(float* buf, int frames, double sr, double hz, double thresholdDb, double maxReductionDb);

private:
    Biquad low_[2], high_[2];  // LR4: two Butterworth sections each
    double hz_ = -1, sr_ = 0;
    double env_ = 0;
};

// Opens above the threshold; closed, it turns the sound down by `rangeDb`.
class NoiseGate {
public:
    void process(float* buf, int frames, double sr, double thresholdDb, double rangeDb, double attackMs, double holdMs,
                 double releaseMs);

private:
    double env_ = 0, gain_ = 0;
    int hold_ = 0;
};

// Freeverb: eight damped combs and four all-passes per channel.
class Reverb {
public:
    void process(float* buf, int frames, double sr, double size, double damping, double width, double mix);

private:
    struct Comb {
        std::vector<float> buf;
        size_t pos = 0;
        float store = 0;
    };
    struct AllPass {
        std::vector<float> buf;
        size_t pos = 0;
    };
    void prepare(double sr);
    double sr_ = 0;
    std::array<std::array<Comb, 8>, 2> combs_;
    std::array<std::array<AllPass, 4>, 2> allpasses_;
};

// mode: 0 stereo, 1 mono (the average), 2 left to both, 3 right to both, 4 swap.
void channelTools(float* buf, int frames, int mode, bool invertLeft, bool invertRight);

}  // namespace montage::fx
