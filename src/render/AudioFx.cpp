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
                        double releaseMs, const float* key) {
    const double det = std::exp(-1.0 / (0.002 * sr));  // the level detector's own smoothing
    const double att = std::exp(-1.0 / (std::max(0.05, attackMs) / 1000 * sr));
    const double rel = std::exp(-1.0 / (std::max(1.0, releaseMs) / 1000 * sr));
    const double floor = std::pow(10.0, std::min(0.0, rangeDb) / 20);
    const double thr = std::pow(10.0, thresholdDb / 20);
    const int hold = int(std::max(0.0, holdMs) / 1000 * sr);
    for (int i = 0; i < frames; ++i) {
        float* d = buf + i * 2;
        const float* k = key ? key + i * 2 : d;
        const double lvl = std::max(std::fabs(k[0]), std::fabs(k[1]));
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

// ---- Multiband compressor -----------------------------------------------------

void MultibandCompressor::process(float* buf, int frames, double sr, double lowHz, double highHz, const BandSettings bands[3],
                                  double attackMs, double releaseMs, double outputDb) {
    highHz = std::max(highHz, lowHz * 1.5);
    if (sr != sr_ || lowHz != lowHz_ || highHz != highHz_) {
        for (int k = 0; k < 2; ++k) {
            lo1_[k].lowPass(sr, lowHz), hi1_[k].highPass(sr, lowHz);
            lo2_[k].lowPass(sr, highHz), hi2_[k].highPass(sr, highHz);
            apLo_[k].lowPass(sr, highHz), apHi_[k].highPass(sr, highHz);
        }
        sr_ = sr, lowHz_ = lowHz, highHz_ = highHz;
    }
    const double att = std::exp(-1.0 / (std::max(0.05, attackMs) / 1000 * sr));
    const double rel = std::exp(-1.0 / (std::max(1.0, releaseMs) / 1000 * sr));
    const float out = dbToLin(outputDb);
    constexpr double knee = 6;
    for (int i = 0; i < frames; ++i) {
        float* d = buf + i * 2;
        float band[3][2];
        for (int ch = 0; ch < 2; ++ch) {
            const float x = d[ch];
            const float low = lo1_[1].process(ch, lo1_[0].process(ch, x));
            const float rest = hi1_[1].process(ch, hi1_[0].process(ch, x));
            // The low band through the upper crossover's all-pass (its low half plus its high half).
            band[0][ch] = apLo_[1].process(ch, apLo_[0].process(ch, low)) + apHi_[1].process(ch, apHi_[0].process(ch, low));
            band[1][ch] = lo2_[1].process(ch, lo2_[0].process(ch, rest));
            band[2][ch] = hi2_[1].process(ch, hi2_[0].process(ch, rest));
        }
        float sum[2] = {0, 0};
        for (int b = 0; b < 3; ++b) {
            const double level = std::max(std::fabs(band[b][0]), std::fabs(band[b][1]));
            env_[b] = level > env_[b] ? att * env_[b] + (1 - att) * level : rel * env_[b] + (1 - rel) * level;
            const double over = 20 * std::log10(env_[b] + 1e-9) - bands[b].thresholdDb;
            const double slope = 1 - 1 / std::max(1.0, bands[b].ratio);
            // Soft knee: the reduction grows smoothly over the 6 dB round the threshold.
            const double reduce = over <= -knee / 2 ? 0 : over >= knee / 2 ? over * slope : slope * (over + knee / 2) * (over + knee / 2) / (2 * knee);
            const float g = dbToLin(bands[b].gainDb - reduce);
            sum[0] += band[b][0] * g, sum[1] += band[b][1] * g;
        }
        d[0] = sum[0] * out;
        d[1] = sum[1] * out;
    }
}

// ---- De-hum ------------------------------------------------------------------

void DeHum::process(float* buf, int frames, double sr, double mainsHz, int harmonics, double reductionDb, double widthHz) {
    harmonics = std::clamp(harmonics, 1, int(notches_.size()));
    if (sr != sr_ || mainsHz != hz_ || harmonics != count_ || reductionDb != db_ || widthHz != width_) {
        count_ = 0;
        for (int k = 1; k <= harmonics && k * mainsHz < sr * 0.45; ++k) {
            // The same width in hertz at every harmonic: the notches stay tight up the series.
            const double f = k * mainsHz;
            notches_[size_t(count_++)].peaking(sr, f, f / std::max(0.1, widthHz), -std::max(0.0, reductionDb));
        }
        sr_ = sr, hz_ = mainsHz, db_ = reductionDb, width_ = widthHz;
    }
    for (int i = 0; i < frames; ++i)
        for (int ch = 0; ch < 2; ++ch) {
            float v = buf[i * 2 + ch];
            for (int k = 0; k < count_; ++k) v = notches_[size_t(k)].process(ch, v);
            buf[i * 2 + ch] = v;
        }
}

// ---- Chorus and flanger -------------------------------------------------------

void ModDelay::process(float* buf, int frames, double sr, double delayMs, double depthMs, double rateHz, double feedback,
                       double spread, double mix) {
    const size_t n = size_t(sr * 0.1) + 8;  // up to 100 ms
    if (sr != sr_ || line_.size() != n * 2) {
        line_.assign(n * 2, 0.0f);
        pos_ = 0;
        sr_ = sr;
    }
    const double base = std::max(0.0, delayMs) / 1000 * sr, depth = std::max(0.0, depthMs) / 1000 * sr;
    const double step = 2 * M_PI * std::max(0.0, rateHz) / sr, offset = std::clamp(spread, 0.0, 1.0) * M_PI / 2;
    const float fb = float(std::clamp(feedback, -0.95, 0.95)), wet = float(std::clamp(mix, 0.0, 1.0)), dry = 1 - wet;
    for (int i = 0; i < frames; ++i) {
        float* d = buf + i * 2;
        for (int ch = 0; ch < 2; ++ch) {
            const double delay = std::clamp(base + depth * std::sin(phase_ + (ch ? offset : 0)), 3.0, double(n - 4));
            // Cubic (Hermite) interpolation between the samples either side of the tap.
            const double at = double(pos_) + double(n) - delay;
            const size_t i1 = size_t(at) % n;
            const float t = float(at - std::floor(at));
            auto tap = [&](size_t k) { return line_[((i1 + n + k - 1) % n) * 2 + size_t(ch)]; };
            const float y0 = tap(0), y1 = tap(1), y2 = tap(2), y3 = tap(3);
            const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            const float delayed = ((c3 * t + c2) * t + c1) * t + y1;
            line_[pos_ * 2 + size_t(ch)] = d[ch] + delayed * fb;
            d[ch] = d[ch] * dry + delayed * wet;
        }
        pos_ = (pos_ + 1) % n;
        phase_ = std::fmod(phase_ + step, 2 * M_PI);
    }
}

// ---- Phaser -------------------------------------------------------------------

void Phaser::process(float* buf, int frames, double sr, int stages, double lowHz, double highHz, double rateHz, double feedback,
                     double spread, double mix) {
    stages = std::clamp(stages, 2, 12);
    lowHz = std::clamp(lowHz, 10.0, sr * 0.45), highHz = std::clamp(highHz, lowHz, sr * 0.45);
    const double step = 2 * M_PI * std::max(0.0, rateHz) / sr, offset = std::clamp(spread, 0.0, 1.0) * M_PI / 2;
    const float fb = float(std::clamp(feedback, -0.95, 0.95)), wet = float(std::clamp(mix, 0.0, 1.0)), dry = 1 - wet;
    const double ratio = highHz / lowHz;
    for (int i = 0; i < frames; ++i) {
        float* d = buf + i * 2;
        for (int ch = 0; ch < 2; ++ch) {
            // The corner moves evenly in pitch: low at phase 0, high half a turn on.
            const double sweep = 0.5 - 0.5 * std::cos(phase_ + (ch ? offset : 0));
            const double t = std::tan(M_PI * lowHz * std::pow(ratio, sweep) / sr);
            const float a = float((t - 1) / (t + 1));
            float v = d[ch] + last_[ch] * fb;
            for (int k = 0; k < stages; ++k) {
                const float y = a * v + state_[ch][k];
                state_[ch][k] = v - a * y;
                v = y;
            }
            last_[ch] = v;
            d[ch] = d[ch] * dry + v * wet;
        }
        phase_ = std::fmod(phase_ + step, 2 * M_PI);
    }
}

// ---- Tremolo and auto-pan -----------------------------------------------------

void Tremolo::process(float* buf, int frames, double sr, double rateHz, double depth, int shape, bool pan) {
    const double step = std::max(0.0, rateHz) / sr;
    depth = std::clamp(depth, 0.0, 1.0);
    for (int i = 0; i < frames; ++i) {
        // -1..1 at this point of the cycle.
        double lfo;
        switch (shape) {
            case 1: lfo = 1 - 4 * std::fabs(phase_ - 0.5); break;
            case 2: lfo = std::tanh(8 * std::sin(2 * M_PI * phase_)) / std::tanh(8.0); break;
            default: lfo = std::sin(2 * M_PI * phase_); break;
        }
        float* d = buf + i * 2;
        if (pan) {
            // Constant power, swinging round the centre.
            const double a = (1 + lfo * depth) * M_PI / 4;
            d[0] = float(d[0] * std::cos(a) * M_SQRT2);
            d[1] = float(d[1] * std::sin(a) * M_SQRT2);
        } else {
            const float g = float(1 - depth * (0.5 - 0.5 * lfo));
            d[0] *= g;
            d[1] *= g;
        }
        phase_ += step;
        phase_ -= std::floor(phase_);
    }
}

// ---- Saturation ---------------------------------------------------------------

namespace {

constexpr double kTubeBias = 0.35;

double logCosh(double x) {
    const double a = std::fabs(x);
    return a + std::log1p(std::exp(-2 * a)) - M_LN2;
}

double shape(int type, double x) {
    switch (type) {
        case 1: return std::tanh(x + kTubeBias) - std::tanh(kTubeBias);
        case 2: return std::clamp(x, -1.0, 1.0);
        default: return std::tanh(x);
    }
}

// The shaper's antiderivative.
double shapeIntegral(int type, double x) {
    switch (type) {
        case 1: return logCosh(x + kTubeBias) - x * std::tanh(kTubeBias);
        case 2: return std::fabs(x) <= 1 ? x * x / 2 : std::fabs(x) - 0.5;
        default: return logCosh(x);
    }
}

}  // namespace

void Saturator::process(float* buf, int frames, double sr, int type, double driveDb, double tone, double mix, double outputDb) {
    type = std::clamp(type, 0, 2);
    if (sr != sr_) {
        dc_.highPass(sr, 8);
        sr_ = sr;
        toneSet_ = -9;
    }
    tone = std::clamp(tone, -1.0, 1.0);
    if (tone != toneSet_) {
        tone_.highShelf(sr, 2500, tone * 9);
        toneSet_ = tone;
    }
    const double g = std::pow(10.0, std::max(0.0, driveDb) / 20);
    // A -12 dBFS sound comes out about as loud as it went in, whatever the drive.
    const double comp = 0.25 / std::max(1e-6, std::fabs(shape(type == 1 ? 0 : type, g * 0.25)));
    const float out = float(std::pow(10.0, outputDb / 20)), wet = float(std::clamp(mix, 0.0, 1.0)), dry = 1 - wet;
    for (int i = 0; i < frames; ++i) {
        float* d = buf + i * 2;
        for (int ch = 0; ch < 2; ++ch) {
            const double x = d[ch] * g, xp = prev_[ch];
            // First-order antiderivative anti-aliasing: the shaper averaged over the step from the last sample.
            const double dx = x - xp;
            double y = std::fabs(dx) > 1e-5 ? (shapeIntegral(type, x) - shapeIntegral(type, xp)) / dx : shape(type, (x + xp) / 2);
            prev_[ch] = x;
            y *= comp;
            if (type == 1) y = dc_.process(ch, float(y));
            if (tone != 0) y = tone_.process(ch, float(y));
            // That average lags half a sample: the dry sound is lagged to match.
            const double drySample = (d[ch] + prevDry_[ch]) / 2;
            prevDry_[ch] = d[ch];
            d[ch] = float(drySample * dry + y * wet) * out;
        }
    }
}

// ---- Stereo width -------------------------------------------------------------

void StereoWidth::process(float* buf, int frames, double sr, double width, double bassMonoHz) {
    const bool split = bassMonoHz > 0;
    if (split && (bassMonoHz != hz_ || sr != sr_)) {
        for (int k = 0; k < 2; ++k) {
            low_[k].lowPass(sr, bassMonoHz);
            high_[k].highPass(sr, bassMonoHz);
        }
        hz_ = bassMonoHz;
        sr_ = sr;
    }
    const float w = float(std::clamp(width, 0.0, 2.0));
    for (int i = 0; i < frames; ++i) {
        float* d = buf + i * 2;
        float mid = (d[0] + d[1]) * 0.5f, side = (d[0] - d[1]) * 0.5f;
        if (split) {
            // Mid through the whole crossover (low + high: an all-pass), side through its high half
            // only, so the two stay in phase above the split.
            mid = low_[1].process(0, low_[0].process(0, mid)) + high_[1].process(0, high_[0].process(0, mid));
            side = high_[1].process(1, high_[0].process(1, side));
        }
        side *= w;
        d[0] = mid + side;
        d[1] = mid - side;
    }
}

}  // namespace montage::fx
