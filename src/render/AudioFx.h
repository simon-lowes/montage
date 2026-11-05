// Montage — built-in audio effects past EQ3, compressor, limiter and delay:
// a parametric EQ of up to sixteen bands, a de-esser, a noise gate, a reverb (Freeverb's
// design), channel tools (fill left or right, mono, swap, polarity), a
// de-hummer, chorus and flanger, a phaser, tremolo and auto-pan, saturation
// and stereo width. All work in place on interleaved stereo and keep their
// state between blocks.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace montage {
struct Effect;
}

namespace montage::fx {

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] = {0, 0}, z2[2] = {0, 0};
    void peaking(double fs, double f0, double q, double db);
    // Shelves: Q 0.707 rises without overshoot (the cookbook's slope S = 1); more makes a bump either side.
    void lowShelf(double fs, double f0, double db, double q = 0.70710678118654752);
    void highShelf(double fs, double f0, double db, double q = 0.70710678118654752);
    void highPass(double fs, double f0, double q = 0.70710678);
    void lowPass(double fs, double f0, double q = 0.70710678);
    void notch(double fs, double f0, double q);
    void bandPass(double fs, double f0, double q);  // 0 dB at f0
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

// A parametric EQ: up to sixteen bands in turn, then an output gain. Each band is one of the RBJ Audio EQ Cookbook's
// biquads (bell, shelves, notch, band-pass), a Butterworth high- or low-pass of 12 to 48 dB an octave (a cut), or a
// tilt: a low shelf and a high shelf about the same frequency, cutting one side by half the gain and lifting the other
// by half, so it is flat at the frequency. Frequency, gain and Q are independent: Q never follows the gain.
enum class EqShape { Bell, LowShelf, HighShelf, LowCut, HighCut, Notch, BandPass, Tilt };  // as core/Effects.h EqBandType
struct EqBand {
    double hz = 1000, db = 0, q = 1;
    EqShape shape = EqShape::Bell;
    int slope = 24;  // a cut's, dB an octave: 12, 24, 36 or 48
    bool on = true;
};
class ParametricEq {
public:
    static constexpr int kMaxBands = 16;
    // The five bands an EQ had before bands could be added: a low shelf, three bells and a high shelf (the shelves'
    // Q is the cookbook's 0.707 whatever they are given).
    void set(double sr, const EqBand& low, const EqBand& b1, const EqBand& b2, const EqBand& b3, const EqBand& high, double outputDb);
    // Up to kMaxBands bands (more are left out). `solo`, a band's index: only the part of the sound that band works on
    // is heard (soloFilter), the other bands and the output gain left out; -1 none. The filters keep their state, so
    // moving a band as it plays does not click.
    void set(double sr, const std::vector<EqBand>& bands, double outputDb, int solo = -1);
    void process(float* buf, int frames);
    // The whole EQ's gain at `hz`, in dB, as it is heard (output gain included; the solo filter while soloing).
    double responseDb(double hz) const;
    // One band's own gain at `hz`, in dB (on or off).
    double bandResponseDb(int band, double hz) const;
    int bandCount() const { return int(stages_.size()); }

private:
    struct Stage {
        std::array<Biquad, 4> sections;
        int count = 0;
        bool on = true;
    };
    static void design(Stage& st, double sr, const EqBand& b);
    // The part of the sound a band works on: a band-pass at a bell's, notch's or band-pass's frequency and Q; below a
    // low shelf's frequency or a high cut's (low-pass), above a high shelf's or a low cut's (high-pass); a tilt as it is.
    static void soloFilter(Stage& st, double sr, const EqBand& b);
    static double stageDb(const Stage& st, double sr, double hz);
    std::vector<Stage> stages_;
    Stage solo_;
    bool soloing_ = false;
    double sr_ = 48000;
    float output_ = 1;
};

// The Parametric EQ effect's bands at frame `t` (core/Effects.h eqBands), and which is soloed: its "audition" string
// names the band only on the copy of the project playback hears while Solo is held (-1 when none).
std::vector<EqBand> eqBandsOf(const Effect& e, int64_t t, int* solo = nullptr);

// Turns down the sibilant band (above `hz`) when it is louder than the
// threshold. The band is split off with a Linkwitz-Riley crossover, whose two
// halves add back up flat, so nothing changes until it acts.
class DeEsser {
public:
    // `gains` (a sample each): where to note the band's gain, or with `replay` the gains to use instead of listening.
    void process(float* buf, int frames, double sr, double hz, double thresholdDb, double maxReductionDb, float* gains = nullptr,
                 bool replay = false);

private:
    Biquad low_[2], high_[2];  // LR4: two Butterworth sections each
    double hz_ = -1, sr_ = 0;
    double env_ = 0;
};

// Opens above the threshold; closed, it turns the sound down by `rangeDb`.
class NoiseGate {
public:
    // `key` (interleaved stereo, `frames` long) opens and closes it in place of the signal itself (a sidechain).
    // `gains` (a sample each): where to note its gain, or with `replay` the gains to use instead of listening.
    void process(float* buf, int frames, double sr, double thresholdDb, double rangeDb, double attackMs, double holdMs,
                 double releaseMs, const float* key = nullptr, float* gains = nullptr, bool replay = false);

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

// Takes out mains hum: a narrow notch at the mains frequency and at each of
// its first `harmonics` multiples, `widthHz` wide, `reductionDb` deep.
class DeHum {
public:
    void process(float* buf, int frames, double sr, double mainsHz, int harmonics, double reductionDb, double widthHz);

private:
    std::array<Biquad, 16> notches_;
    int count_ = 0;
    double sr_ = 0, hz_ = 0, db_ = 0, width_ = 0;
};

// Chorus and flanger: the sound mixed with a copy delayed by `delayMs`, the
// delay swept `depthMs` either way by a sine at `rateHz`. `feedback` (-0.95..0.95)
// feeds the delayed copy back in (a flanger's resonance); `spread` (0..1) puts
// the right channel's sweep up to a quarter turn after the left's.
class ModDelay {
public:
    void process(float* buf, int frames, double sr, double delayMs, double depthMs, double rateHz, double feedback, double spread,
                 double mix);

private:
    std::vector<float> line_;  // interleaved stereo
    size_t pos_ = 0;
    double phase_ = 0, sr_ = 0;
};

// Phaser: `stages` first-order all-passes whose corner sweeps from `lowHz` to
// `highHz` and back at `rateHz`, mixed with the dry sound so the notches move.
class Phaser {
public:
    void process(float* buf, int frames, double sr, int stages, double lowHz, double highHz, double rateHz, double feedback,
                 double spread, double mix);

private:
    float state_[2][12] = {};
    float last_[2] = {0, 0};
    double phase_ = 0;
};

// Tremolo (level) or auto-pan (position) moved by an LFO: shape 0 sine,
// 1 triangle, 2 square (with soft edges, so it does not click).
class Tremolo {
public:
    void process(float* buf, int frames, double sr, double rateHz, double depth, int shape, bool pan);

private:
    double phase_ = 0;
};

// Saturation: type 0 tape (tanh), 1 tube (asymmetric, so even harmonics too),
// 2 hard clip. Antiderivative anti-aliasing keeps the harmonics a waveshaper
// makes above Nyquist from folding back down. The level of a -12 dBFS sound is
// kept as the drive goes up; `tone` (-1..1) darkens or brightens what comes out.
class Saturator {
public:
    void process(float* buf, int frames, double sr, int type, double driveDb, double tone, double mix, double outputDb);

private:
    double prev_[2] = {0, 0}, prevDry_[2] = {0, 0};
    Biquad dc_, tone_;
    double sr_ = 0, toneSet_ = -9;
};

// Stereo width (mid/side): 0 mono, 1 as recorded, 2 twice as wide. With
// `bassMonoHz` > 0 the sides below it are taken out, as for vinyl or clubs.
class StereoWidth {
public:
    void process(float* buf, int frames, double sr, double width, double bassMonoHz);

private:
    Biquad low_[2], high_[2];  // LR4 crossover: mid on channel 0, side on 1
    double hz_ = -1, sr_ = 0;
};

// Three-band compressor: split at `lowHz` and `highHz` by Linkwitz-Riley
// crossovers (the low band also goes through the upper crossover's all-pass,
// so the bands add back up flat), each band compressed on its own with a soft
// knee, then given its own gain.
struct BandSettings {
    double thresholdDb = 0, ratio = 1, gainDb = 0;
};
class MultibandCompressor {
public:
    // `gains` (three a sample, the bands'): where to note them, or with `replay` the gains to use instead of listening.
    void process(float* buf, int frames, double sr, double lowHz, double highHz, const BandSettings bands[3], double attackMs,
                 double releaseMs, double outputDb, float* gains = nullptr, bool replay = false);

private:
    Biquad lo1_[2], hi1_[2], lo2_[2], hi2_[2], apLo_[2], apHi_[2];
    double sr_ = 0, lowHz_ = -1, highHz_ = -1;
    double env_[3] = {0, 0, 0};
};

// mode: 0 stereo, 1 mono (the average), 2 left to both, 3 right to both, 4 swap.
void channelTools(float* buf, int frames, int mode, bool invertLeft, bool invertRight);

}  // namespace montage::fx
