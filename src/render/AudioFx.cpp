#include "AudioFx.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "core/Effects.h"

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#include <xmmintrin.h>
#define MONTAGE_DENORMALS_MXCSR 1
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define MONTAGE_DENORMALS_FPCR 1
#elif defined(__arm__) && defined(__ARM_FP) && (defined(__GNUC__) || defined(__clang__))
#define MONTAGE_DENORMALS_FPSCR 1
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM))
#include <float.h>
#define MONTAGE_DENORMALS_CONTROLFP 1
#endif

namespace montage::fx {

namespace {
float dbToLin(double db) { return float(std::pow(10.0, db / 20.0)); }
double clampHz(double f, double fs) { return std::clamp(f, 10.0, fs * 0.49); }
}  // namespace

DenormalsOff::DenormalsOff() {
#if defined(MONTAGE_DENORMALS_MXCSR)
    was_ = _mm_getcsr();
    _mm_setcsr(unsigned(was_) | 0x8040u);  // flush to zero (bit 15), denormals are zero (bit 6)
#elif defined(MONTAGE_DENORMALS_FPCR)
    uint64_t fpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    was_ = fpcr;
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (uint64_t(1) << 24)));  // FZ: flush to zero, in and out
#elif defined(MONTAGE_DENORMALS_FPSCR)
    uint32_t fpscr = 0;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(fpscr));
    was_ = fpscr;
    __asm__ __volatile__("vmsr fpscr, %0" : : "r"(fpscr | (uint32_t(1) << 24)));  // FZ, as on 64-bit ARM
#elif defined(MONTAGE_DENORMALS_CONTROLFP)
    unsigned int cw = 0;
    _controlfp_s(&cw, 0, 0);
    was_ = cw;
    _controlfp_s(&cw, _DN_FLUSH, _MCW_DN);  // (the FPCR's or FPSCR's FZ bit, there)
#endif
}

DenormalsOff::~DenormalsOff() {
#if defined(MONTAGE_DENORMALS_MXCSR)
    _mm_setcsr(unsigned(was_));
#elif defined(MONTAGE_DENORMALS_FPCR)
    __asm__ __volatile__("msr fpcr, %0" : : "r"(was_));
#elif defined(MONTAGE_DENORMALS_FPSCR)
    __asm__ __volatile__("vmsr fpscr, %0" : : "r"(uint32_t(was_)));
#elif defined(MONTAGE_DENORMALS_CONTROLFP)
    unsigned int cw = 0;
    _controlfp_s(&cw, unsigned(was_) & _MCW_DN, _MCW_DN);
#endif
}

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

void Biquad::lowShelf(double fs, double f0, double db, double q) {
    const double A = std::pow(10.0, db / 40), w0 = 2 * M_PI * clampHz(f0, fs) / fs, cs = std::cos(w0);
    const double al = std::sin(w0) / (2 * std::max(0.05, q)), sa = 2 * std::sqrt(A) * al;
    set(A * ((A + 1) - (A - 1) * cs + sa), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - sa),
        (A + 1) + (A - 1) * cs + sa, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - sa);
}

void Biquad::highShelf(double fs, double f0, double db, double q) {
    const double A = std::pow(10.0, db / 40), w0 = 2 * M_PI * clampHz(f0, fs) / fs, cs = std::cos(w0);
    const double al = std::sin(w0) / (2 * std::max(0.05, q)), sa = 2 * std::sqrt(A) * al;
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

void Biquad::notch(double fs, double f0, double q) {
    const double w0 = 2 * M_PI * clampHz(f0, fs) / fs, cs = std::cos(w0), al = std::sin(w0) / (2 * std::max(0.05, q));
    set(1, -2 * cs, 1, 1 + al, -2 * cs, 1 - al);
}

void Biquad::bandPass(double fs, double f0, double q) {
    const double w0 = 2 * M_PI * clampHz(f0, fs) / fs, cs = std::cos(w0), al = std::sin(w0) / (2 * std::max(0.05, q));
    set(al, 0, -al, 1 + al, -2 * cs, 1 - al);
}

// ---- Parametric EQ ----------------------------------------------------------

void ParametricEq::design(Stage& st, double sr, const EqBand& b) {
    const double q = std::max(0.05, b.q);
    st.on = b.on;
    switch (b.shape) {
        case EqShape::Bell: st.count = 1, st.sections[0].peaking(sr, b.hz, q, b.db); break;
        case EqShape::LowShelf: st.count = 1, st.sections[0].lowShelf(sr, b.hz, b.db, q); break;
        case EqShape::HighShelf: st.count = 1, st.sections[0].highShelf(sr, b.hz, b.db, q); break;
        case EqShape::Notch: st.count = 1, st.sections[0].notch(sr, b.hz, q); break;
        case EqShape::BandPass: st.count = 1, st.sections[0].bandPass(sr, b.hz, q); break;
        case EqShape::Tilt:
            st.count = 2;
            st.sections[0].lowShelf(sr, b.hz, -b.db / 2, q);
            st.sections[1].highShelf(sr, b.hz, b.db / 2, q);
            break;
        case EqShape::LowCut:
        case EqShape::HighCut: {
            // A Butterworth filter of order slope / 6, as biquads: section k's Q is 1 / (2 cos((2k - 1) pi / 2n)). The
            // band's Q scales the last (most resonant) one: at 0.707 the cut is maximally flat.
            const int n = std::clamp(b.slope / 6, 2, 8) & ~1;
            st.count = n / 2;
            for (int k = 1; k <= st.count; ++k) {
                double sq = 1 / (2 * std::cos((2 * k - 1) * M_PI / (2 * n)));
                if (k == st.count) sq *= q / M_SQRT1_2;
                if (b.shape == EqShape::LowCut) st.sections[size_t(k - 1)].highPass(sr, b.hz, sq);
                else st.sections[size_t(k - 1)].lowPass(sr, b.hz, sq);
            }
            break;
        }
    }
    st.clearSpare();
}

void ParametricEq::soloFilter(Stage& st, double sr, const EqBand& b) {
    st.on = true;
    switch (b.shape) {
        case EqShape::LowShelf:
        case EqShape::LowCut: st.count = 1, st.sections[0].lowPass(sr, b.hz); break;
        case EqShape::HighShelf:
        case EqShape::HighCut: st.count = 1, st.sections[0].highPass(sr, b.hz); break;
        case EqShape::Tilt: design(st, sr, b), st.on = true; break;
        default: st.count = 1, st.sections[0].bandPass(sr, b.hz, b.q); break;
    }
    st.clearSpare();
}

void ParametricEq::set(double sr, const EqBand& low, const EqBand& b1, const EqBand& b2, const EqBand& b3, const EqBand& high,
                       double outputDb) {
    EqBand l = low, h = high;
    l.shape = EqShape::LowShelf, h.shape = EqShape::HighShelf;
    l.q = h.q = M_SQRT1_2;
    l.on = h.on = true;
    l.slot = eqBandSlot("low"), h.slot = eqBandSlot("high");
    EqBand bells[3] = {b1, b2, b3};
    for (int k = 0; k < 3; ++k) bells[k].shape = EqShape::Bell, bells[k].on = true, bells[k].slot = eqBandSlot("b" + std::to_string(k + 1));
    set(sr, {l, bells[0], bells[1], bells[2], h}, outputDb);
}

void ParametricEq::set(double sr, const std::vector<EqBand>& bands, double outputDb, int solo) {
    const size_t n = std::min(bands.size(), size_t(kMaxBands));
    if (sr != sr_) {
        stages_.clear();
        solo_ = soloOut_ = Stage{};
        soloMix_ = soloOutMix_ = 0;
        primed_ = false;
    }
    // (room for every stage there can be, kept from then on: set is called every block while a setting is keyframed)
    if (stages_.capacity() < size_t(kMaxStages)) stages_.reserve(size_t(kMaxStages));
    if (scratch_.capacity() < size_t(kMaxStages)) scratch_.reserve(size_t(kMaxStages));
    auto idOf = [&](size_t i) { return bands[i].slot >= 0 ? bands[i].slot : int(i); };
    auto byId = [](std::vector<Stage>& in, int id) { return std::find_if(in.begin(), in.end(), [&](const Stage& x) { return x.id == id; }); };
    // Each band takes its own filter back (a band whose filter changes shape keeps its sections' state: it may click
    // once, as any switch does).
    std::vector<Stage>& next = scratch_;
    next.clear();
    for (size_t i = 0; i < n; ++i) {
        const int id = idOf(i);
        const auto was = byId(stages_, id);
        next.push_back(was != stages_.end() ? *was : Stage{});
        Stage& st = next.back();
        st.id = id, st.gone = false;
        design(st, sr, bands[i]);
        if (!primed_) st.mix = st.on ? 1 : 0;
        else if (st.on && st.mix <= 0) st.clear();  // (switched back on, or new: from rest, as it fades in)
    }
    // A band removed as it plays fades out where it was in the chain, as one switched off does, then goes.
    if (primed_) {
        size_t at = 0;  // (where the next removed one goes: after the last band before it that is kept)
        for (const Stage& old : stages_) {
            if (const auto kept = byId(next, old.id); kept != next.end()) {
                at = size_t(kept - next.begin()) + 1;
            } else if (old.mix > 0 && next.size() < size_t(kMaxStages)) {
                Stage& st = *next.insert(next.begin() + ptrdiff_t(at++), old);
                st.on = false, st.gone = true;
            }
        }
    }
    std::swap(stages_, scratch_);
    const bool soloNow = solo >= 0 && size_t(solo) < n;
    if (soloNow) {
        const int id = idOf(size_t(solo));
        if (soloMix_ <= 0) {
            solo_.clear();
        } else if (id != solo_.id) {
            // Moved to another band while held: the one it was on fades out as the new one fades in (back to the one
            // still fading out, the two change places; a third so soon, the fainter of the two let go).
            if (soloOutMix_ > 0 && id == soloOut_.id) {
                std::swap(solo_, soloOut_);
                std::swap(soloMix_, soloOutMix_);
            } else {
                if (soloMix_ >= soloOutMix_) soloOut_ = solo_, soloOutMix_ = soloMix_;
                solo_.clear();
                soloMix_ = 0;
            }
        }
        soloFilter(solo_, sr, bands[size_t(solo)]);
        solo_.id = id;
    }
    soloing_ = soloNow;
    if (!primed_) soloMix_ = soloing_ ? 1 : 0, soloOutMix_ = 0;
    output_ = dbToLin(outputDb);
    sr_ = sr;
}

double ParametricEq::stageDb(const Stage& st, double sr, double hz) {
    double db = 0;
    for (int k = 0; k < st.count; ++k) db += st.sections[size_t(k)].responseDb(sr, hz);
    return db;
}

double ParametricEq::responseDb(double hz) const {
    if (soloing_) return stageDb(solo_, sr_, hz);
    double db = 20 * std::log10(std::max(1e-12f, output_));
    for (const Stage& st : stages_)
        if (st.on) db += stageDb(st, sr_, hz);
    return db;
}

int ParametricEq::bandCount() const {
    return int(std::count_if(stages_.begin(), stages_.end(), [](const Stage& st) { return !st.gone; }));
}

double ParametricEq::bandResponseDb(int band, double hz) const {
    for (const Stage& st : stages_)
        if (!st.gone && band-- == 0) return stageDb(st, sr_, hz);
    return 0.0;
}

void ParametricEq::process(float* buf, int frames) {
    const float step = float(1 / (kFadeSeconds * sr_));
    for (int i = 0; i < frames; ++i) {
        float* f = buf + size_t(i) * 2;
        float v[2] = {f[0], f[1]};
        for (Stage& st : stages_) {
            if (!st.on && st.mix <= 0) continue;
            float w[2] = {v[0], v[1]};
            st.run(w);
            if (st.on && st.mix >= 1) {
                v[0] = w[0], v[1] = w[1];
                continue;
            }
            for (int ch = 0; ch < 2; ++ch) v[ch] += (w[ch] - v[ch]) * st.mix;
            st.mix = st.on ? std::min(1.0f, st.mix + step) : std::max(0.0f, st.mix - step);
        }
        v[0] *= output_, v[1] *= output_;
        if (soloing_ || soloMix_ > 0 || soloOutMix_ > 0) {
            float w[2] = {f[0], f[1]};
            solo_.run(w);
            if (soloing_ && soloMix_ >= 1 && soloOutMix_ <= 0) {
                v[0] = w[0], v[1] = w[1];
            } else {
                float u[2] = {f[0], f[1]};
                if (soloOutMix_ > 0) soloOut_.run(u);
                for (int ch = 0; ch < 2; ++ch) v[ch] += (w[ch] - v[ch]) * soloMix_ + (u[ch] - v[ch]) * soloOutMix_;
                soloMix_ = soloing_ ? std::min(1.0f, soloMix_ + step) : std::max(0.0f, soloMix_ - step);
                soloOutMix_ = std::max(0.0f, soloOutMix_ - step);
            }
        }
        f[0] = v[0], f[1] = v[1];
    }
    // (a filter no longer heard put at rest, as it would be made when next heard)
    if (!soloing_ && soloMix_ <= 0) solo_.clear();
    if (soloOutMix_ <= 0) soloOut_.clear();
    for (Stage& st : stages_)
        if (!st.on && st.mix <= 0) st.clear();
    for (Stage* st : {&solo_, &soloOut_})
        for (Biquad& b : st->sections) b.flushTiny();
    for (Stage& st : stages_)
        for (Biquad& b : st.sections) b.flushTiny();
    primed_ = primed_ || frames > 0;
}

bool ParametricEq::atRest() const {
    auto still = [](const Stage& st) {
        return std::all_of(st.sections.begin(), st.sections.end(), [](const Biquad& b) { return !b.z1[0] && !b.z1[1] && !b.z2[0] && !b.z2[1]; });
    };
    return still(solo_) && still(soloOut_) && std::all_of(stages_.begin(), stages_.end(), still);
}

std::vector<EqBand> eqBandsOf(const Effect& e, int64_t t, int* solo) {
    std::vector<EqBand> out;
    const std::string audition = e.s("audition");
    if (solo) *solo = -1;
    for (const std::string& band : eqBands(e)) {
        EqBand b;
        const int type = eqBandType(e, band);
        b.shape = EqShape(type);
        b.on = e.p(band + "_on", t, 1) > 0.5;
        b.hz = std::clamp(e.p(band + "_hz", t, eqDefaultHz(band)), 10.0, 24000.0);
        b.db = eqTypeHasGain(type) ? e.p(band + "_db", t, 0) : 0;
        b.q = std::clamp(e.p(band + "_q", t, eqDefaultQ(type)), 0.05, eqMaxQ(type));
        b.slope = 12 * (1 + std::clamp(int(std::lround(e.p(band + "_slope", t, 1))), 0, 3));
        b.slot = eqBandSlot(band);
        if (solo && band == audition) *solo = int(out.size());
        out.push_back(b);
        if (int(out.size()) == ParametricEq::kMaxBands) break;
    }
    return out;
}

// ---- De-esser ----------------------------------------------------------------

void DeEsser::process(float* buf, int frames, double sr, double hz, double thresholdDb, double maxReductionDb, float* gains,
                      bool replay) {
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
        float keep;
        if (gains && replay) {
            keep = gains[i];
        } else {
            const double lvl = std::max(std::fabs(hi[0]), std::fabs(hi[1]));
            env_ = lvl > env_ ? att * env_ + (1 - att) * lvl : rel * env_ + (1 - rel) * lvl;
            // Over the threshold the band comes down 1:1, up to the most reduction allowed.
            const double over = 20 * std::log10(env_ + 1e-9) - thresholdDb;
            keep = dbToLin(-std::clamp(over, 0.0, maxReductionDb));
            if (gains) gains[i] = keep;
        }
        d[0] = lo[0] + hi[0] * keep;
        d[1] = lo[1] + hi[1] * keep;
    }
}

// ---- Noise gate --------------------------------------------------------------

void NoiseGate::process(float* buf, int frames, double sr, double thresholdDb, double rangeDb, double attackMs, double holdMs,
                        double releaseMs, const float* key, float* gains, bool replay) {
    const double det = std::exp(-1.0 / (0.002 * sr));  // the level detector's own smoothing
    const double att = std::exp(-1.0 / (std::max(0.05, attackMs) / 1000 * sr));
    const double rel = std::exp(-1.0 / (std::max(1.0, releaseMs) / 1000 * sr));
    const double floor = std::pow(10.0, std::min(0.0, rangeDb) / 20);
    const double thr = std::pow(10.0, thresholdDb / 20);
    const int hold = int(std::max(0.0, holdMs) / 1000 * sr);
    for (int i = 0; i < frames; ++i) {
        float* d = buf + i * 2;
        if (gains && replay) {
            d[0] *= gains[i], d[1] *= gains[i];
            continue;
        }
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
        if (gains) gains[i] = float(gain_);
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
                                  double attackMs, double releaseMs, double outputDb, float* gains, bool replay) {
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
            if (gains && replay) {
                const float g = gains[i * 3 + b];
                sum[0] += band[b][0] * g, sum[1] += band[b][1] * g;
                continue;
            }
            const double level = std::max(std::fabs(band[b][0]), std::fabs(band[b][1]));
            env_[b] = level > env_[b] ? att * env_[b] + (1 - att) * level : rel * env_[b] + (1 - rel) * level;
            const double over = 20 * std::log10(env_[b] + 1e-9) - bands[b].thresholdDb;
            const double slope = 1 - 1 / std::max(1.0, bands[b].ratio);
            // Soft knee: the reduction grows smoothly over the 6 dB round the threshold.
            const double reduce = over <= -knee / 2 ? 0 : over >= knee / 2 ? over * slope : slope * (over + knee / 2) * (over + knee / 2) / (2 * knee);
            const float g = dbToLin(bands[b].gainDb - reduce);
            if (gains) gains[i * 3 + b] = g;
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
