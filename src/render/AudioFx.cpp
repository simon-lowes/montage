#include "AudioFx.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace montage::fx {

namespace {
float dbToLin(double db) { return float(std::pow(10.0, db / 20.0)); }
double clampHz(double f, double fs) { return std::clamp(f, 10.0, fs * 0.49); }
}  // namespace

// RBJ cookbook biquads.
void Biquad::set(double nb0, double nb1, double nb2, double a0, double na1, double na2) {
    b0 = nb0 / a0;
    b1 = nb1 / a0;
    b2 = nb2 / a0;
    a1 = na1 / a0;
    a2 = na2 / a0;
}

void Biquad::peaking(double fs, double f0, double q, double db) {
    const double A = std::pow(10.0, db / 40), w0 = 2 * M_PI * clampHz(f0, fs) / fs, al = std::sin(w0) / (2 * std::max(0.05, q));
    set(1 + al * A, -2 * std::cos(w0), 1 - al * A, 1 + al / A, -2 * std::cos(w0), 1 - al / A);
}

void Biquad::lowShelf(double fs, double f0, double db) {
    const double A = std::pow(10.0, db / 40), w0 = 2 * M_PI * clampHz(f0, fs) / fs, cs = std::cos(w0);
    const double al = std::sin(w0) / 2 * std::sqrt(2.0), sa = 2 * std::sqrt(A) * al;
    set(A * ((A + 1) - (A - 1) * cs + sa), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - sa),
        (A + 1) + (A - 1) * cs + sa, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - sa);
}

void Biquad::highShelf(double fs, double f0, double db) {
    const double A = std::pow(10.0, db / 40), w0 = 2 * M_PI * clampHz(f0, fs) / fs, cs = std::cos(w0);
    const double al = std::sin(w0) / 2 * std::sqrt(2.0), sa = 2 * std::sqrt(A) * al;
    set(A * ((A + 1) + (A - 1) * cs + sa), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - sa),
        (A + 1) - (A - 1) * cs + sa, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - sa);
}

double Biquad::responseDb(double fs, double hz) const {
    const std::complex<double> z1 = std::polar(1.0, -2 * M_PI * hz / fs), z2 = z1 * z1;
    const std::complex<double> h = (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
    return 20 * std::log10(std::max(1e-12, std::abs(h)));
}

void Biquad::lowPass(double fs, double f0, double q) {
    const double w0 = 2 * M_PI * clampHz(f0, fs) / fs, cs = std::cos(w0), al = std::sin(w0) / (2 * q);
    set((1 - cs) / 2, 1 - cs, (1 - cs) / 2, 1 + al, -2 * cs, 1 - al);
}

void Biquad::highPass(double fs, double f0, double q) {
    const double w0 = 2 * M_PI * clampHz(f0, fs) / fs, cs = std::cos(w0), al = std::sin(w0) / (2 * q);
    set((1 + cs) / 2, -(1 + cs), (1 + cs) / 2, 1 + al, -2 * cs, 1 - al);
}

// ---- Parametric EQ ----------------------------------------------------------

void ParametricEq::set(double sr, const EqBand& low, const EqBand& b1, const EqBand& b2, const EqBand& b3, const EqBand& high,
                       double outputDb) {
    bands_[0].lowShelf(sr, low.hz, low.db);
    bands_[1].peaking(sr, b1.hz, b1.q, b1.db);
    bands_[2].peaking(sr, b2.hz, b2.q, b2.db);
    bands_[3].peaking(sr, b3.hz, b3.q, b3.db);
    bands_[4].highShelf(sr, high.hz, high.db);
    output_ = dbToLin(outputDb);
    sr_ = sr;
}

double ParametricEq::responseDb(double hz) const {
    double db = 20 * std::log10(std::max(1e-12f, output_));
    for (const Biquad& b : bands_) db += b.responseDb(sr_, hz);
    return db;
}

void ParametricEq::process(float* buf, int frames) {
    for (int i = 0; i < frames; ++i)
        for (int ch = 0; ch < 2; ++ch) {
            float v = buf[i * 2 + ch];
            for (Biquad& b : bands_) v = b.process(ch, v);
            buf[i * 2 + ch] = v * output_;
        }
}

// ---- De-esser ----------------------------------------------------------------

void DeEsser::process(float* buf, int frames, double sr, double hz, double thresholdDb, double maxReductionDb) {
    if (hz != hz_ || sr != sr_) {
        for (int k = 0; k < 2; ++k) {
            low_[k].lowPass(sr, hz);
            high_[k].highPass(sr, hz);
        }
        hz_ = hz;
        sr_ = sr;
    }
    const double att = std::exp(-1.0 / (0.0005 * sr)), rel = std::exp(-1.0 / (0.05 * sr));
    for (int i = 0; i < frames; ++i) {
        float* d = buf + i * 2;
        float lo[2], hi[2];
        for (int ch = 0; ch < 2; ++ch) {
            lo[ch] = low_[1].process(ch, low_[0].process(ch, d[ch]));
            hi[ch] = high_[1].process(ch, high_[0].process(ch, d[ch]));
        }
        const double lvl = std::max(std::fabs(hi[0]), std::fabs(hi[1]));
        env_ = lvl > env_ ? att * env_ + (1 - att) * lvl : rel * env_ + (1 - rel) * lvl;
        // Over the threshold the band comes down 1:1, up to the most reduction allowed.
        const double over = 20 * std::log10(env_ + 1e-9) - thresholdDb;
        const float keep = dbToLin(-std::clamp(over, 0.0, maxReductionDb));
        d[0] = lo[0] + hi[0] * keep;
        d[1] = lo[1] + hi[1] * keep;
    }
}

// ---- Noise gate --------------------------------------------------------------

void NoiseGate::process(float* buf, int frames, double sr, double thresholdDb, double rangeDb, double attackMs, double holdMs,
                        double releaseMs) {
    const double det = std::exp(-1.0 / (0.002 * sr));  // the level detector's own smoothing
    const double att = std::exp(-1.0 / (std::max(0.05, attackMs) / 1000 * sr));
    const double rel = std::exp(-1.0 / (std::max(1.0, releaseMs) / 1000 * sr));
    const double floor = std::pow(10.0, std::min(0.0, rangeDb) / 20);
    const double thr = std::pow(10.0, thresholdDb / 20);
    const int hold = int(std::max(0.0, holdMs) / 1000 * sr);
    for (int i = 0; i < frames; ++i) {
        float* d = buf + i * 2;
        const double lvl = std::max(std::fabs(d[0]), std::fabs(d[1]));
        env_ = lvl > env_ ? lvl : det * env_ + (1 - det) * lvl;
        double target = floor;
        if (env_ >= thr) {
            target = 1;
            hold_ = hold;
        } else if (hold_ > 0) {
            --hold_;
            target = 1;
        }
        gain_ = target > gain_ ? att * gain_ + (1 - att) * target : rel * gain_ + (1 - rel) * target;
        d[0] = float(d[0] * gain_);
        d[1] = float(d[1] * gain_);
    }
}

// ---- Reverb (Freeverb) -------------------------------------------------------

void Reverb::prepare(double sr) {
    static const int combTuning[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int allpassTuning[4] = {556, 441, 341, 225};
    const double scale = sr / 44100.0;
    for (int ch = 0; ch < 2; ++ch) {
        const int spread = ch ? 23 : 0;  // the right channel's taps are a little longer, for width
        auto taps = [&](int n) { return std::vector<float>(size_t(std::max(1.0, (n + spread) * scale)), 0.f); };
        for (int k = 0; k < 8; ++k) combs_[size_t(ch)][size_t(k)] = {taps(combTuning[k]), 0, 0};
        for (int k = 0; k < 4; ++k) allpasses_[size_t(ch)][size_t(k)] = {taps(allpassTuning[k]), 0};
    }
    sr_ = sr;
}

void Reverb::process(float* buf, int frames, double sr, double size, double damping, double width, double mix) {
    if (sr != sr_) prepare(sr);
    const float feedback = float(std::clamp(size, 0.0, 1.0) * 0.28 + 0.7);
    const float damp = float(std::clamp(damping, 0.0, 1.0) * 0.4);
    const float wet = float(std::clamp(mix, 0.0, 1.0)), dry = 1 - wet;
    const float w = float(std::clamp(width, 0.0, 1.0)), wet1 = w / 2 + 0.5f, wet2 = (1 - w) / 2;
    for (int i = 0; i < frames; ++i) {
        float* d = buf + i * 2;
        const float input = (d[0] + d[1]) * 0.015f;
        float out[2] = {0, 0};
        for (int ch = 0; ch < 2; ++ch) {
            for (Comb& c : combs_[size_t(ch)]) {
                const float y = c.buf[c.pos];
                c.store = y * (1 - damp) + c.store * damp;
                c.buf[c.pos] = input + c.store * feedback;
                c.pos = (c.pos + 1) % c.buf.size();
                out[ch] += y;
            }
            for (AllPass& a : allpasses_[size_t(ch)]) {
                const float b = a.buf[a.pos];
                a.buf[a.pos] = out[ch] + b * 0.5f;
                a.pos = (a.pos + 1) % a.buf.size();
                out[ch] = b - out[ch];
            }
        }
        // The tail at Freeverb's level (its wet scale is 3).
        const float l = (out[0] * wet1 + out[1] * wet2) * 3, r = (out[1] * wet1 + out[0] * wet2) * 3;
        d[0] = d[0] * dry + l * wet;
        d[1] = d[1] * dry + r * wet;
    }
}

// ---- Channel tools -----------------------------------------------------------

void channelTools(float* buf, int frames, int mode, bool invertLeft, bool invertRight) {
    for (int i = 0; i < frames; ++i) {
        float& l = buf[i * 2];
        float& r = buf[i * 2 + 1];
        switch (mode) {
            case 1: l = r = (l + r) * 0.5f; break;
            case 2: r = l; break;
            case 3: l = r; break;
            case 4: std::swap(l, r); break;
            default: break;
        }
        if (invertLeft) l = -l;
        if (invertRight) r = -r;
    }
}

}  // namespace montage::fx
