#include "ElasticAudio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <numeric>
#include <set>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "SpectralRepair.h"
#include "SpeechCleanup.h"
#include "TimeStretch.h"
#include "core/Effects.h"
#include "media/MediaPool.h"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/tx.h>
}

namespace montage {

namespace {

// av_tx needs SIMD-aligned buffers.
template <typename T>
struct AlignedBuf {
    T* p;
    explicit AlignedBuf(size_t n) : p(static_cast<T*>(av_calloc(n, sizeof(T)))) {}
    ~AlignedBuf() { av_free(p); }
    AlignedBuf(const AlignedBuf&) = delete;
    AlignedBuf& operator=(const AlignedBuf&) = delete;
    T& operator[](size_t i) { return p[i]; }
    T* data() { return p; }
};

struct Tx {
    AVTXContext* ctx = nullptr;
    av_tx_fn fn = nullptr;
    Tx(int n, bool inverse) {
        const float scale = inverse ? 1.0f / float(n) : 1.0f;
        av_tx_init(&ctx, &fn, AV_TX_FLOAT_RDFT, inverse ? 1 : 0, n, &scale, 0);
    }
    ~Tx() { av_tx_uninit(&ctx); }
    Tx(const Tx&) = delete;
    Tx& operator=(const Tx&) = delete;
};

// The sound as one channel: the mean of the channels, or an ambisonic field's W.
// Over [lo, hi) only (clipped to the sound), from index 0.
std::vector<float> monoOf(const AudioBuffer& in, int64_t lo, int64_t hi) {
    const int64_t n = in.frames();
    lo = std::clamp<int64_t>(lo, 0, n), hi = std::clamp<int64_t>(hi, lo, n);
    const size_t nch = size_t(std::max(1, in.channels));
    std::vector<float> mono(static_cast<size_t>(hi - lo));
    for (int64_t i = lo; i < hi; ++i) {
        if (nch >= 4) {
            mono[size_t(i - lo)] = in.samples[size_t(i) * nch];
            continue;
        }
        float sum = 0;
        for (size_t c = 0; c < nch; ++c) sum += in.samples[size_t(i) * nch + c];
        mono[size_t(i - lo)] = sum / float(nch);
    }
    return mono;
}
std::vector<float> monoOf(const AudioBuffer& in) { return monoOf(in, 0, in.frames()); }

bool isIdentity(const DialogueWarp& warp) {
    return std::all_of(warp.begin(), warp.end(), [](const AlignPoint& p) { return std::fabs(p.w - p.u) < 1e-9; });
}

// A windowed sinc (Blackman) read of channel `c` at fractional sample `at`, its band narrowed by `scale` (>= 1) when
// the sound is played faster than it was recorded, so nothing folds back.
double sincRead(const AudioBuffer& in, int c, double at, double scale) {
    const int64_t n = in.frames();
    const size_t nch = size_t(std::max(1, in.channels));
    const double half = 16 * scale;
    const int64_t lo = int64_t(std::ceil(at - half)), hi = int64_t(std::floor(at + half));
    double sum = 0;
    for (int64_t s = std::max<int64_t>(lo, 0); s <= std::min(hi, n - 1); ++s) {
        const double x = (double(s) - at) / scale;
        const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
        const double t = (x + 16) / 32;
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * t) + 0.08 * std::cos(4 * M_PI * t);
        sum += sinc * w * in.samples[size_t(s) * nch + size_t(c)];
    }
    return sum / scale;
}

double centsOf(double hz) { return 1200 * std::log2(hz / 440.0); }
double hzOf(double cents) { return 440.0 * std::pow(2.0, cents / 1200); }

double easeWeight(const PitchEdit& e, double t) {
    constexpr double ease = 0.02;
    if (t >= e.start && t < e.end) return 1;
    const double d = t < e.start ? e.start - t : t - e.end;
    if (d >= ease) return 0;
    return 0.5 + 0.5 * std::cos(M_PI * d / ease);
}

double formantAt(const std::vector<PitchEdit>& edits, double t) {
    double semis = 0;
    for (const PitchEdit& e : edits) semis += e.formant * easeWeight(e, t);
    return semis;
}

bool scaleHas(const std::string& scale, int key, int midi) {
    const int degree = ((midi - key) % 12 + 12) % 12;
    static const std::set<int> major{0, 2, 4, 5, 7, 9, 11}, minor{0, 2, 3, 5, 7, 8, 10};
    if (scale == "major") return major.count(degree) > 0;
    if (scale == "minor") return minor.count(degree) > 0;
    return true;
}

// The nearest note of the scale (cents from A 440).
double nearestNote(double cents, const std::string& scale, int key) {
    const double midi = 69 + cents / 100;
    double best = cents, bestD = 1e300;
    for (int k = int(std::floor(midi)) - 3; k <= int(std::ceil(midi)) + 3; ++k) {
        if (!scaleHas(scale, key, k)) continue;
        const double c = double(k - 69) * 100;
        if (std::fabs(c - cents) < bestD) bestD = std::fabs(c - cents), best = c;
    }
    return best;
}

}  // namespace

// ---- Names and strings ---------------------------------------------------------

const char* elasticModeName(ElasticMode m) {
    switch (m) {
    case ElasticMode::General: return "general";
    case ElasticMode::Varispeed: return "varispeed";
    default: return "speech";
    }
}

bool parseElasticMode(const std::string& name, ElasticMode& out) {
    if (name == "speech") out = ElasticMode::Speech;
    else if (name == "general") out = ElasticMode::General;
    else if (name == "varispeed") out = ElasticMode::Varispeed;
    else return false;
    return true;
}

ElasticMode elasticModeOf(const Effect& e) {
    const int m = int(std::lround(e.p("mode", 0)));
    return m == 1 ? ElasticMode::General : m == 2 ? ElasticMode::Varispeed : ElasticMode::Speech;
}

std::string elasticMembersToString(const std::vector<ElasticMember>& members) {
    std::string out;
    char buf[64];
    for (const ElasticMember& m : members) {
        if (!out.empty()) out += ';';
        out += std::to_string(m.media) + '|';
        for (size_t i = 0; i < m.channels.size(); ++i) out += (i ? "," : "") + std::to_string(m.channels[i]);
        std::snprintf(buf, sizeof buf, "|%.9f", m.offset);
        out += buf;
    }
    return out;
}

std::vector<ElasticMember> elasticMembersFromString(const std::string& text) {
    std::vector<ElasticMember> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(';', pos);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t a = item.find('|'), b = item.rfind('|');
        if (a == std::string::npos || b == a) continue;
        ElasticMember m;
        char* stop = nullptr;
        m.media = Id(std::strtoull(item.c_str(), &stop, 10));
        const std::string chans = item.substr(a + 1, b - a - 1);
        size_t cp = 0;
        bool bad = false;
        while (cp < chans.size()) {
            size_t ce = chans.find(',', cp);
            if (ce == std::string::npos) ce = chans.size();
            const int ch = std::atoi(chans.substr(cp, ce - cp).c_str());
            if (ch < 0 || ch > 1023) bad = true;
            m.channels.push_back(ch);
            cp = ce + 1;
        }
        const char* off = item.c_str() + b + 1;
        m.offset = std::strtod(off, &stop);
        if (bad || stop == off || !std::isfinite(m.offset) || std::fabs(m.offset) > 1e7 || !m.media) continue;
        out.push_back(m);
    }
    return out;
}

bool validPitchScale(const std::string& scale) { return scale == "chromatic" || scale == "major" || scale == "minor"; }

std::string pitchEditsToString(const std::vector<PitchEdit>& edits) {
    QJsonArray list;
    for (const PitchEdit& e : edits) {
        QJsonObject o{{"start", e.start}, {"end", e.end}};
        if (e.shift != 0) o["shift"] = e.shift;
        if (e.inflection != 1) o["inflection"] = e.inflection;
        if (e.correct != 0) {
            o["correct"] = e.correct;
            o["key"] = e.key;
            o["scale"] = QString::fromStdString(e.scale);
        }
        if (e.formant != 0) o["formant"] = e.formant;
        list.append(o);
    }
    return QJsonDocument(list).toJson(QJsonDocument::Compact).toStdString();
}

std::vector<PitchEdit> pitchEditsFromString(const std::string& text) {
    std::vector<PitchEdit> out;
    const QJsonArray list = QJsonDocument::fromJson(QByteArray::fromStdString(text)).array();
    for (const QJsonValue& v : list) {
        const QJsonObject o = v.toObject();
        PitchEdit e;
        e.start = o.value("start").toDouble(), e.end = o.value("end").toDouble();
        e.shift = std::clamp(o.value("shift").toDouble(0), -24.0, 24.0);
        e.inflection = std::clamp(o.value("inflection").toDouble(1), 0.0, 4.0);
        e.correct = std::clamp(o.value("correct").toDouble(0), 0.0, 1.0);
        e.key = std::clamp(o.value("key").toInt(0), 0, 11);
        e.scale = o.value("scale").toString("chromatic").toStdString();
        if (!validPitchScale(e.scale)) e.scale = "chromatic";
        e.formant = std::clamp(o.value("formant").toDouble(0), -12.0, 12.0);
        if (!std::isfinite(e.start) || !std::isfinite(e.end) || e.end <= e.start || std::fabs(e.start) > 1e7) continue;
        out.push_back(e);
    }
    return out;
}

// ---- Warping -------------------------------------------------------------------

void applyElasticWarp(const AudioBuffer& in, const DialogueWarp& warp, ElasticMode mode, double offset,
                      const std::vector<std::pair<const AudioBuffer*, double>>& groupIn, AudioBuffer& out,
                      const std::atomic<bool>* cancel) {
    out = in;
    const int rate = std::max(1, in.sampleRate);
    const int64_t n = in.frames();
    const size_t nch = size_t(std::max(1, in.channels));
    if (warp.size() < 1 || isIdentity(warp) || n == 0) return;
    if (!std::isfinite(offset) || std::fabs(offset) > 1e7) return;
    const int64_t off = std::llround(offset * rate);
    // The group's members (every member's own sound, so members rendered apart choose the same grains), or this sound.
    std::vector<std::pair<const AudioBuffer*, int64_t>> members;
    for (const auto& [b, o] : groupIn)
        if (b && b->frames() > 0 && std::isfinite(o) && std::fabs(o) <= 1e7) members.emplace_back(b, std::llround(o * rate));
    if (members.empty()) members.emplace_back(&in, off);
    // Only the stretch the markers reach is remade: past the first and last, where the sound plays as it was, it is
    // copied (a little either side made too, so grains there are whole). Group samples, the same for every member,
    // so their grains fall on the same grid.
    const bool anchored = std::fabs(warp.front().w - warp.front().u) < 1e-9 && std::fabs(warp.back().w - warp.back().u) < 1e-9;
    constexpr double margin = 0.15;
    int64_t r0, r1;
    if (anchored) {
        r0 = int64_t(std::floor((warp.front().u - margin) * rate));
        r1 = int64_t(std::ceil((warp.back().u + margin) * rate));
    } else {
        r0 = std::numeric_limits<int64_t>::max(), r1 = std::numeric_limits<int64_t>::min();
        for (const auto& [b, o] : members) r0 = std::min(r0, -o), r1 = std::max(r1, b->frames() - o);
    }
    // Nothing of this sound in it: as it was.
    if (std::min(r1, n - off) <= std::max(r0, -off)) return;

    if (mode == ElasticMode::Varispeed) {
        for (int64_t g = std::max(r0, -off); g < std::min(r1, n - off); ++g) {
            if (cancel && ((g - r0) & 0xffff) == 0 && cancel->load()) return;
            const double u = double(g) / rate;
            const double w = dialogueWarpAt(warp, u);
            const double slope = (dialogueWarpAt(warp, u + 0.5 / rate) - dialogueWarpAt(warp, u - 0.5 / rate)) * rate;
            const double at = w * rate + double(off);
            const double narrow = std::min(std::max(1.0, slope), 64.0);  // (taps bounded however steep a marker makes it)
            for (size_t c = 0; c < nch; ++c) out.samples[size_t(g + off) * nch + c] = float(sincRead(in, int(c), at, narrow));
        }
        return;
    }

    // The group's sound over the stretch, in group samples from r0.
    std::vector<float> guide(size_t(r1 - r0), 0.0f);
    for (const auto& [b, o] : members) {
        const size_t bch = size_t(std::max(1, b->channels));
        const int64_t bn = b->frames();
        for (int64_t g = std::max(r0, -o); g < std::min(r1, bn - o); ++g) {
            const size_t i = size_t(g + o);
            double sum = 0;
            for (size_t c = 0; c < bch; ++c) sum += b->samples[i * bch + c];
            guide[size_t(g - r0)] += float(sum / double(bch));
        }
    }
    const int hop = stretchHop(rate) * (mode == ElasticMode::General ? 2 : 1);
    std::vector<double> positions;
    positions.reserve(size_t((r1 - r0) / hop + 3));
    for (int64_t k = 0; k <= (r1 - r0) / hop + 2; ++k)
        positions.push_back(dialogueWarpAt(warp, double(r0 + k * hop) / rate) * rate - double(r0));
    // The grains that reach each marker taken from just where it says, so what it marks lands on time.
    std::vector<int64_t> locked;
    for (size_t i = 0; i < warp.size(); ++i) {
        if ((i == 0 || i + 1 == warp.size()) && anchored) continue;
        const double k = (warp[i].u * rate - double(r0)) / hop;
        for (int64_t j = int64_t(std::floor(k)) - 1; j <= int64_t(std::floor(k)) + 2; ++j) locked.push_back(j);
    }
    std::sort(locked.begin(), locked.end());
    const std::vector<int64_t> starts = wsolaGrainStarts(guide, positions, hop, r1 - r0, cancel, locked);
    if (cancel && cancel->load()) return;
    AudioBuffer made;
    wsolaOverlapAdd(in, starts, hop, r0 + off, n, made);
    // Grains are whole a couple of hops in from the stretch's ends.
    const int64_t from = anchored ? r0 + off + 2 * hop : 0, to = anchored ? r1 + off - 2 * hop : n;
    for (int64_t o = std::max<int64_t>(from, 0); o < std::min(to, n); ++o)
        for (size_t c = 0; c < nch; ++c) out.samples[size_t(o) * nch + c] = made.samples[size_t(o) * nch + c];
}

// ---- Onsets --------------------------------------------------------------------

std::vector<double> detectOnsets(const AudioBuffer& in, double sensitivity, double minGap, const std::atomic<bool>* cancel) {
    std::vector<double> onsets;
    const int rate = in.sampleRate;
    if (rate <= 0 || in.frames() < 64) return onsets;
    const std::vector<float> x = monoOf(in);
    const int64_t n = int64_t(x.size());
    int N = 256;
    while (N * 2 <= 0.025 * rate) N *= 2;
    const int hop = N / 4, bins = N / 2 + 1;
    Tx fwd(N, false);
    if (!fwd.ctx) return onsets;
    std::vector<float> window(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) window[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * i / N));
    AlignedBuf<float> frame(static_cast<size_t>(N));
    AlignedBuf<AVComplexFloat> spec(size_t(bins) + 1);
    const int64_t frames = n / hop + 1;
    std::vector<double> flux(size_t(frames), 0.0), level(size_t(frames), 0.0);
    std::vector<float> prev(size_t(bins), 0.0f), cur(static_cast<size_t>(bins));
    for (int64_t t = 0; t < frames; ++t) {
        if (cancel && (t & 1023) == 0 && cancel->load()) return {};
        const int64_t c = t * hop - N / 2;
        double energy = 0;
        for (int i = 0; i < N; ++i) {
            const int64_t s = c + i;
            const float v = s >= 0 && s < n ? x[size_t(s)] : 0.0f;
            frame[size_t(i)] = v * window[size_t(i)];
            energy += double(frame[size_t(i)]) * frame[size_t(i)];
        }
        level[size_t(t)] = 10 * std::log10(energy / N + 1e-20);
        fwd.fn(fwd.ctx, spec.data(), frame.data(), sizeof(float));
        double f = 0;
        for (int b = 0; b < bins; ++b) {
            const float mag = std::hypot(spec[size_t(b)].re, spec[size_t(b)].im) * 2.0f / float(N);
            cur[size_t(b)] = std::log1p(1000.0f * mag);
            if (t > 0) f += std::max(0.0f, cur[size_t(b)] - prev[size_t(b)]);
        }
        flux[size_t(t)] = f / bins;
        std::swap(prev, cur);
    }
    double mean = 0;
    for (double f : flux) mean += f;
    mean /= double(std::max<int64_t>(1, frames));
    const double lambda = 0.1 + 2.0 * (1 - std::clamp(sensitivity, 0.0, 100.0) / 100);
    const int span = std::max(4, int(0.08 * rate / hop));
    const int L = std::max(8, int(rate * 0.0005));
    std::vector<double> prefix(size_t(n) + 1, 0.0);
    for (int64_t i = 0; i < n; ++i) prefix[size_t(i) + 1] = prefix[size_t(i)] + double(x[size_t(i)]) * x[size_t(i)];
    auto energy = [&](int64_t a, int64_t b) { return prefix[size_t(std::clamp<int64_t>(b, 0, n))] - prefix[size_t(std::clamp<int64_t>(a, 0, n))]; };
    std::vector<std::pair<double, double>> found;  // seconds, strength
    for (int64_t t = 1; t < frames; ++t) {
        const double f = flux[size_t(t)];
        if (f <= 0 || level[size_t(t)] < -70) continue;
        bool peak = true;
        for (int64_t d = -3; d <= 3 && peak; ++d)
            if (d && t + d >= 0 && t + d < frames && flux[size_t(t + d)] > f) peak = false;
        if (!peak) continue;
        std::vector<double> around;
        for (int64_t d = -span; d <= span; ++d)
            if (t + d >= 0 && t + d < frames) around.push_back(flux[size_t(t + d)]);
        std::nth_element(around.begin(), around.begin() + long(around.size() / 2), around.end());
        if (f < around[around.size() / 2] + lambda * mean) continue;
        // A sound stopping changes the spectrum too (its edge splashes across it): only a rise in level counts.
        if (level[size_t(std::min<int64_t>(t + 1, frames - 1))] <= level[size_t(t - 1)] + 0.5) continue;
        // To the sample: where the level rises most sharply, around the windows that heard it.
        const int64_t a = std::max<int64_t>(4 * L, t * hop - N / 2 - hop), b = std::min<int64_t>(n - L, t * hop + N / 2 + 2 * hop);
        double peakEnergy = 0;
        for (int64_t i = a; i < b; ++i) peakEnergy = std::max(peakEnergy, energy(i, i + L));
        const double floor = 1e-12 + 1e-6 * peakEnergy;
        int64_t best = t * hop;
        double bestRatio = -1;
        for (int64_t i = a; i < b; ++i) {
            const double ratio = (energy(i, i + L) / L) / (energy(i - 4 * L, i) / (4 * L) + floor / L);
            if (ratio >= bestRatio) bestRatio = ratio, best = i;
        }
        found.emplace_back(double(best) / rate, f);
    }
    // At least minGap apart: the stronger kept.
    for (const auto& [at, strength] : found) {
        if (!onsets.empty() && at - onsets.back() < minGap) {
            const auto prevStrength = std::find_if(found.begin(), found.end(), [&](const auto& p) { return p.first == onsets.back(); })->second;
            if (strength > prevStrength) onsets.back() = at;
            continue;
        }
        onsets.push_back(at);
    }
    return onsets;
}

// ---- Pitch tracking (pYIN) -----------------------------------------------------

double PitchTrack::at(double t) const {
    if (f0.empty() || hop <= 0) return 0;
    const double i = (t - start) / hop;
    if (i <= 0) return f0.front();
    if (i >= double(f0.size() - 1)) return f0.back();
    const size_t a = size_t(std::floor(i));
    const double fa = f0[a], fb = f0[a + 1];
    if (fa <= 0 || fb <= 0) return (i - double(a)) < 0.5 ? fa : fb;
    return fa + (fb - fa) * (i - double(a));
}

PitchTrack trackPitch(const AudioBuffer& in, double from, double to, double fmin, double fmax, const std::atomic<bool>* cancel) {
    PitchTrack track;
    const int rate = in.sampleRate;
    if (rate <= 0 || in.frames() == 0) return track;
    fmin = std::clamp(fmin, 20.0, rate / 8.0), fmax = std::clamp(fmax, fmin * 1.5, rate / 4.0);
    const int64_t n = in.frames();
    const int hopS = std::max(1, int(std::lround(0.005 * rate)));
    track.hop = double(hopS) / rate;
    from = std::max(0.0, from);
    to = std::min(to, double(n) / rate);
    if (to <= from) return track;
    track.start = from;
    const int64_t first = std::llround(from * rate), frames = (std::llround(to * rate) - first) / hopS + 1;
    const int tauMin = std::max(2, int(std::floor(rate / fmax))), tauMax = int(std::ceil(rate / fmin)) + 1;
    const int W = tauMax;
    int N = 64;
    while (N < W + tauMax + 1) N *= 2;
    Tx fwd(N, false), inv(N, true);
    if (!fwd.ctx || !inv.ctx) return track;
    AlignedBuf<float> a(static_cast<size_t>(N)), b(static_cast<size_t>(N)), r(static_cast<size_t>(N));
    AlignedBuf<AVComplexFloat> A(size_t(N / 2 + 1) + 1), B(size_t(N / 2 + 1) + 1);
    // Only the sound the frames reach, so a short stretch of a long file costs what the stretch does.
    const int64_t reach = 2 * (int64_t(W) + tauMax) + 2;
    const int64_t lo = std::max<int64_t>(0, first - reach), hi = std::min<int64_t>(n, first + frames * hopS + reach);
    const std::vector<float> x = monoOf(in, lo, hi);
    std::vector<double> prefix(x.size() + 1, 0.0);
    for (size_t i = 0; i < x.size(); ++i) prefix[i + 1] = prefix[i] + double(x[i]) * x[i];
    auto sample = [&](int64_t i) -> double { return i >= lo && i < hi ? x[size_t(i - lo)] : 0.0; };
    auto energy = [&](int64_t s, int64_t len) {
        return prefix[size_t(std::clamp<int64_t>(s + len, lo, hi) - lo)] - prefix[size_t(std::clamp<int64_t>(s, lo, hi) - lo)];
    };
    // The difference function at one lag, in double, over the window from `base` (for the final period).
    auto diffAt = [&](int tau, int64_t base) {
        double d = 0;
        for (int j = 0; j < W; ++j) {
            const double v = sample(base + j) - sample(base + j + tau);
            d += v * v;
        }
        return d;
    };
    // Thresholds 0.01..1 weighed by a beta distribution (2, 18), as pYIN.
    constexpr int kThresholds = 100;
    std::vector<double> prior(kThresholds);
    {
        double sum = 0;
        for (int i = 0; i < kThresholds; ++i) {
            const double s = (i + 1) / double(kThresholds);
            prior[size_t(i)] = s * std::pow(1 - s + 1e-9, 17.0);
            sum += prior[size_t(i)];
        }
        for (double& p : prior) p /= sum;
    }
    const double binCents = 20;
    const int nBins = int(std::ceil(1200 * std::log2(fmax / fmin) / binCents)) + 1;
    auto binOf = [&](double hz) { return 1200 * std::log2(hz / fmin) / binCents; };
    struct Candidate {
        int tau;
        double p;
    };
    std::vector<std::vector<Candidate>> candidates(static_cast<size_t>(frames));
    std::vector<double> dprime(size_t(tauMax) + 2), d(size_t(tauMax) + 2);
    const int tauMid = int(std::lround(rate / std::sqrt(fmin * fmax)));
    for (int64_t f = 0; f < frames; ++f) {
        if (cancel && (f & 255) == 0 && cancel->load()) return {};
        const int64_t centre = first + f * hopS;
        const int64_t s = centre - (W + tauMid) / 2;
        for (int i = 0; i < N; ++i) {
            a[size_t(i)] = i < W ? float(sample(s + i)) : 0.0f;
            b[size_t(i)] = i < W + tauMax ? float(sample(s + i)) : 0.0f;
        }
        fwd.fn(fwd.ctx, A.data(), a.data(), sizeof(float));
        fwd.fn(fwd.ctx, B.data(), b.data(), sizeof(float));
        for (int k = 0; k <= N / 2; ++k) {
            const AVComplexFloat p = A[size_t(k)], q = B[size_t(k)];
            A[size_t(k)] = {p.re * q.re + p.im * q.im, p.re * q.im - p.im * q.re};  // conj(a) b
        }
        inv.fn(inv.ctx, r.data(), A.data(), sizeof(AVComplexFloat));
        const double e0 = energy(s, W);
        double running = 0;
        dprime[0] = 1;
        for (int tau = 1; tau <= tauMax; ++tau) {
            d[size_t(tau)] = std::max(0.0, e0 + energy(s + tau, W) - 2.0 * r[size_t(tau)]);
            running += d[size_t(tau)];
            dprime[size_t(tau)] = running > 0 ? d[size_t(tau)] * tau / running : 1;
        }
        if (e0 < 1e-10 * W) continue;  // silence
        std::map<int, double> mass;
        for (int i = 0; i < kThresholds; ++i) {
            const double thr = (i + 1) / double(kThresholds);
            for (int tau = tauMin; tau < tauMax; ++tau) {
                if (dprime[size_t(tau)] >= thr) continue;
                while (tau + 1 < tauMax && dprime[size_t(tau + 1)] < dprime[size_t(tau)]) ++tau;
                mass[tau] += prior[size_t(i)];
                break;
            }
        }
        for (const auto& [tau, p] : mass) candidates[size_t(f)].push_back({tau, p});
    }
    // Viterbi over pitch bins, voiced and unvoiced (pYIN's model): the pitch moves at most 25 bins (5 semitones) a
    // frame, likelier the less it moves; voicing changes with probability 0.01.
    const int states = 2 * nBins, jump = 25;
    std::vector<double> trans(size_t(jump) + 1);
    {
        double sum = 0;
        for (int dj = -jump; dj <= jump; ++dj) sum += jump + 1 - std::abs(dj);
        for (int dj = 0; dj <= jump; ++dj) trans[size_t(dj)] = std::log((jump + 1 - dj) / sum);
    }
    const double stay = std::log(0.99), change = std::log(0.01);
    std::vector<double> score(size_t(states), -std::log(double(states))), next(static_cast<size_t>(states));
    std::vector<uint16_t> back(size_t(frames) * size_t(states));  // (states < 2^16: 20-cent bins over at most 10 octaves)
    std::vector<double> obs(static_cast<size_t>(states));
    std::vector<double> best(static_cast<size_t>(states));
    std::vector<int32_t> bestFrom(static_cast<size_t>(states));
    for (int64_t f = 0; f < frames; ++f) {
        if (cancel && (f & 255) == 0 && cancel->load()) return {};
        double voicedMass = 0;
        std::fill(obs.begin(), obs.end(), 0.0);
        for (const Candidate& c : candidates[size_t(f)]) {
            const double bin = binOf(double(rate) / c.tau);
            const int k = int(std::lround(bin));
            if (k < 0 || k >= nBins) continue;
            obs[size_t(k)] += c.p;
            voicedMass += c.p;
        }
        voicedMass = std::min(voicedMass, 1.0);
        for (int k = 0; k < nBins; ++k) {
            obs[size_t(k)] = std::log(obs[size_t(k)] + 1e-9);
            obs[size_t(nBins + k)] = std::log((1 - voicedMass) / nBins + 1e-9);
        }
        if (f == 0) {
            for (int st = 0; st < states; ++st) score[size_t(st)] += obs[size_t(st)];
            for (int st = 0; st < states; ++st) back[size_t(st)] = uint16_t(st);
            continue;
        }
        // Best predecessor within each half (a band of bins), then the switch between halves.
        for (int half = 0; half < 2; ++half) {
            for (int k = 0; k < nBins; ++k) {
                double bv = -1e300;
                int bf = half * nBins + k;
                for (int dj = -jump; dj <= jump; ++dj) {
                    const int j = k + dj;
                    if (j < 0 || j >= nBins) continue;
                    const double v = score[size_t(half * nBins + j)] + trans[size_t(std::abs(dj))];
                    if (v > bv) bv = v, bf = half * nBins + j;
                }
                best[size_t(half * nBins + k)] = bv + stay;
                bestFrom[size_t(half * nBins + k)] = bf;
            }
        }
        for (int st = 0; st < states; ++st) {
            const int other = st < nBins ? st + nBins : st - nBins;
            double v = best[size_t(st)];
            int32_t from = bestFrom[size_t(st)];
            if (score[size_t(other)] + change > v) v = score[size_t(other)] + change, from = other;
            next[size_t(st)] = v + obs[size_t(st)];
            back[size_t(f) * size_t(states) + size_t(st)] = uint16_t(from);
        }
        const double top = *std::max_element(next.begin(), next.end());
        for (int st = 0; st < states; ++st) score[size_t(st)] = next[size_t(st)] - top;
    }
    std::vector<int> path(static_cast<size_t>(frames));
    path.back() = int(std::max_element(score.begin(), score.end()) - score.begin());
    for (int64_t f = frames - 1; f > 0; --f) path[size_t(f - 1)] = back[size_t(f) * size_t(states) + size_t(path[size_t(f)])];
    track.f0.assign(size_t(frames), 0.0);
    track.voicing.assign(size_t(frames), 0.0f);
    for (int64_t f = 0; f < frames; ++f) {
        double mass = 0;
        for (const Candidate& c : candidates[size_t(f)]) mass += c.p;
        track.voicing[size_t(f)] = float(std::min(1.0, mass));
        const int st = path[size_t(f)];
        if (st >= nBins) continue;
        // The candidate nearest the path's bin, its period found between samples (a parabola through the
        // difference function, worked out in full precision over a window centred on the frame).
        const Candidate* pick = nullptr;
        double pickD = 1e300;
        for (const Candidate& c : candidates[size_t(f)]) {
            const double dist = std::fabs(binOf(double(rate) / c.tau) - st);
            if (dist < pickD) pickD = dist, pick = &c;
        }
        if (!pick || pickD > 2) {
            track.f0[size_t(f)] = fmin * std::pow(2.0, st * binCents / 1200);
            continue;
        }
        const int tau = pick->tau;
        const int64_t centre = first + f * hopS;
        const int64_t base = centre - (W + tau) / 2;
        const double y0 = diffAt(tau - 1, base), y1 = diffAt(tau, base), y2 = diffAt(tau + 1, base);
        const double den = y0 - 2 * y1 + y2;
        const double shift = den > 0 ? std::clamp(0.5 * (y0 - y2) / den, -1.0, 1.0) : 0.0;
        track.f0[size_t(f)] = double(rate) / (tau + shift);
    }
    return track;
}

PitchTrack trackPitch(const AudioBuffer& in, double fmin, double fmax, const std::atomic<bool>* cancel) {
    return trackPitch(in, 0, double(in.frames()) / std::max(1, in.sampleRate), fmin, fmax, cancel);
}

// ---- Pitch edits ---------------------------------------------------------------

std::vector<double> editedPitch(const PitchTrack& track, const std::vector<PitchEdit>& edits) {
    std::vector<double> out = track.f0;
    const size_t n = out.size();
    auto timeOf = [&](size_t i) { return track.start + double(i) * track.hop; };
    std::vector<double> cents(n, 0.0);
    for (const PitchEdit& e : edits) {
        for (size_t i = 0; i < n; ++i) cents[i] = out[i] > 0 ? centsOf(out[i]) : 0;
        // The stretch's median pitch.
        std::vector<double> inside;
        for (size_t i = 0; i < n; ++i)
            if (out[i] > 0 && timeOf(i) >= e.start && timeOf(i) < e.end) inside.push_back(cents[i]);
        if (inside.empty()) continue;
        std::nth_element(inside.begin(), inside.begin() + long(inside.size() / 2), inside.end());
        const double median = inside[inside.size() / 2];
        std::vector<double> c1(n, 0.0);
        for (size_t i = 0; i < n; ++i)
            if (out[i] > 0) c1[i] = median + (cents[i] - median) * e.inflection;
        const int smooth = std::max(1, int(std::lround(0.075 / track.hop)));
        for (size_t i = 0; i < n; ++i) {
            if (out[i] <= 0) continue;
            const double w = easeWeight(e, timeOf(i));
            if (w <= 0) continue;
            double c2 = c1[i];
            if (e.correct > 0) {
                double sum = 0;
                int count = 0;
                for (long j = long(i) - smooth; j <= long(i) + smooth; ++j)
                    if (j >= 0 && size_t(j) < n && out[size_t(j)] > 0) sum += c1[size_t(j)], ++count;
                const double held = sum / count;
                c2 += e.correct * (nearestNote(held, e.scale, e.key) - held);
            }
            const double target = c2 + 100 * e.shift;
            out[i] = hzOf(cents[i] + w * (target - cents[i]));
        }
    }
    return out;
}

namespace {

// Pitch marks: a glottal pulse each period where there is a pitch (the largest peak of the slightly smoothed sound,
// then one period on each time, moved to the peak there), every 5 ms where there is none.
struct Mark {
    int64_t at;
    bool voiced;
};

std::vector<Mark> pitchMarks(const AudioBuffer& in, const PitchTrack& track, int rate, int64_t a, int64_t b) {
    std::vector<Mark> marks;
    const int64_t n = in.frames();
    const int smoothLen = std::max(1, rate / 1000);
    // The sound smoothed over 1 ms (centred), over the stretch and a longest period past it.
    const int64_t ylo = std::max<int64_t>(0, a), yhi = std::min<int64_t>(n, b + rate / 20 + 2);
    const int64_t xlo = ylo - smoothLen, xhi = yhi + smoothLen;
    const std::vector<float> x = monoOf(in, xlo, xhi);
    const int64_t xbase = std::max<int64_t>(0, xlo);
    std::vector<double> ys(size_t(std::max<int64_t>(0, yhi - ylo)), 0.0);
    for (int64_t j = ylo; j < yhi; ++j) {
        const int64_t last = j + smoothLen / 2;
        if (last >= n) break;
        double run = 0;
        for (int64_t k = std::max<int64_t>(0, last - smoothLen + 1); k <= last; ++k) run += x[size_t(k - xbase)];
        ys[size_t(j - ylo)] = run / smoothLen;
    }
    struct {
        const std::vector<double>& v;
        int64_t lo;
        double operator[](size_t i) const {
            const int64_t k = int64_t(i) - lo;
            return k >= 0 && k < int64_t(v.size()) ? v[size_t(k)] : -1e300;
        }
    } y{ys, ylo};
    const int64_t unvoicedStep = std::max<int64_t>(16, std::llround(0.005 * rate));
    int64_t pos = std::max<int64_t>(0, a);
    bool lastVoiced = false;
    while (pos < b) {
        const double f = track.at(double(pos) / rate);
        if (f > 0) {
            const double T = rate / f;
            int64_t m;
            if (!lastVoiced) {
                m = pos;
                for (int64_t i = pos; i < std::min<int64_t>(n, pos + int64_t(T)); ++i)
                    if (y[size_t(i)] > y[size_t(m)]) m = i;
                if (!marks.empty() && m - marks.back().at < T / 2) m = marks.back().at + int64_t(T / 2);
            } else {
                const int64_t guess = marks.back().at + int64_t(std::lround(T));
                if (guess >= b) break;
                const int64_t reach = std::max<int64_t>(1, int64_t(T / 5));
                m = guess;
                for (int64_t i = std::max<int64_t>(0, guess - reach); i <= std::min<int64_t>(n - 1, guess + reach); ++i)
                    if (y[size_t(i)] > y[size_t(m)]) m = i;
                m = std::max<int64_t>(m, marks.back().at + int64_t(T / 2));
            }
            if (m >= b) break;
            marks.push_back({m, true});
            lastVoiced = true;
            pos = m + 1;
        } else {
            const int64_t m = marks.empty() ? pos : std::max(pos, marks.back().at + unvoicedStep);
            if (m >= b) break;
            marks.push_back({m, false});
            lastVoiced = false;
            pos = m + unvoicedStep;
        }
    }
    return marks;
}

}  // namespace

void applyPitchEdits(const AudioBuffer& in, const std::vector<PitchEdit>& edits, AudioBuffer& out, const std::atomic<bool>* cancel,
                     const PitchTrack* given) {
    out = in;
    const int rate = in.sampleRate;
    const int64_t n = in.frames();
    const size_t nch = size_t(std::max(1, in.channels));
    std::vector<PitchEdit> active;
    for (const PitchEdit& e : edits)
        if (e.shift != 0 || e.inflection != 1 || e.correct > 0 || e.formant != 0) active.push_back(e);
    if (active.empty() || rate <= 0 || n == 0) return;
    // The stretches remade: each edit and a little either side, carried on to where the voice next stops (where
    // the grains can fall back into step with the original unheard), merged where they meet.
    std::vector<std::pair<double, double>> regions;
    for (const PitchEdit& e : active) regions.emplace_back(e.start - 0.1, e.end + 0.1);
    std::sort(regions.begin(), regions.end());
    std::vector<std::pair<double, double>> merged;
    for (const auto& r : regions) {
        if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second);
        else merged.push_back(r);
    }
    // (each remade a minute or so at a time, split where the voice stops, so tracking and grains stay small
    // however long the edit: the pieces meet with 5 ms fades where nothing is changed)
    constexpr double kPiece = 60;
    // (`startTs`, when given, is where the grains go on from the piece before, so the two agree where they meet; the
    // grain at or after `carryFrom` is passed on in `carry`)
    auto remake = [&](const PitchTrack& track, int64_t a, int64_t b, double startTs, double carryFrom, double* carry) -> bool {
        const std::vector<double> target = editedPitch(track, active);
        auto targetAt = [&](double t) {
            const double i = (t - track.start) / track.hop;
            if (i <= 0) return target.front();
            if (i >= double(target.size() - 1)) return target.back();
            const size_t k = size_t(std::floor(i));
            return target[k] > 0 && target[k + 1] > 0 ? target[k] + (target[k + 1] - target[k]) * (i - double(k)) : target[k];
        };
        const std::vector<Mark> marks = pitchMarks(in, track, rate, a, b);
        if (marks.size() < 3) return true;
        // TD-PSOLA: synthesis marks at the new period, each taking the grain of the nearest analysis mark (its two
        // half-periods, half-Hann each side), resampled for a formant shift; with nothing changed the grains add up
        // to the original exactly.
        std::vector<double> acc(size_t(b - a) * nch, 0.0), wsum(size_t(b - a), 0.0);
        auto sampleAt = [&](double at, size_t c) {
            const int64_t k = int64_t(std::floor(at));
            const double fr = at - double(k);
            auto get = [&](int64_t i) { return i >= 0 && i < n ? double(in.samples[size_t(i) * nch + c]) : 0.0; };
            return get(k) * (1 - fr) + get(k + 1) * fr;
        };
        double ts = startTs > 0 ? startTs : double(marks.front().at);
        size_t j = 0;
        while (ts < double(b)) {
            if (cancel && cancel->load()) return false;
            if (carry && *carry < 0 && ts >= carryFrom) *carry = ts;
            while (j + 1 < marks.size() && std::fabs(double(marks[j + 1].at) - ts) <= std::fabs(double(marks[j].at) - ts)) ++j;
            const Mark& m = marks[j];
            const double L = double(j > 0 ? m.at - marks[j - 1].at : (j + 1 < marks.size() ? marks[j + 1].at - m.at : 0));
            const double R = double(j + 1 < marks.size() ? marks[j + 1].at - m.at : L);
            if (L <= 0 || R <= 0) break;
            const double t = ts / rate;
            const double phi = std::pow(2.0, formantAt(active, t) / 12);
            double step = R;
            double ratio = 1, target = 0;
            if (m.voiced) {
                const double f0 = track.at(double(m.at) / rate), f1 = targetAt(double(m.at) / rate);
                if (f0 > 0 && f1 > 0) ratio = f1 / f0, target = f1;
            }
            const bool unchanged = std::fabs(ratio - 1) < 1e-9 && std::fabs(phi - 1) < 1e-9;
            const double drift = ts - double(m.at);
            if (unchanged) {
                // Back in step with the original: at once where there is no pitch, gently where there is.
                step = m.voiced && std::fabs(drift) >= 0.5 ? R - std::clamp(drift, -0.1 * R, 0.1 * R) : R - drift;
            } else if (m.voiced && target > 0) {
                step = rate / target;  // the period asked for, whatever the marks' own spacing
            }
            const int64_t lo = int64_t(std::ceil(ts - L / phi)), hi = int64_t(std::floor(ts + R / phi));
            for (int64_t o = std::max(lo, a); o <= std::min(hi, b - 1); ++o) {
                const double rel = (double(o) - ts) * phi;  // where in the grain
                double w;
                if (rel < 0) w = 0.5 + 0.5 * std::cos(M_PI * rel / L);
                else w = 0.5 + 0.5 * std::cos(M_PI * rel / R);
                if (rel <= -L || rel >= R) continue;
                const double src = double(m.at) + rel;
                const size_t oi = size_t(o - a);
                wsum[oi] += w;
                for (size_t c = 0; c < nch; ++c) acc[oi * nch + c] += w * sampleAt(src, c);
            }
            ts += std::max(step, 8.0);
        }
        // Into the sound, faded in and out over 5 ms at the stretch's ends (where nothing is changed).
        const int64_t fade = std::max<int64_t>(1, std::llround(0.005 * rate));
        const int64_t from = a + std::llround(0.05 * rate), to = b - std::llround(0.02 * rate);
        for (int64_t o = std::max<int64_t>(from, a); o < to; ++o) {
            const size_t oi = size_t(o - a);
            const double norm = std::max(1.0, wsum[oi]);
            const double edge = std::min({1.0, double(o - from) / fade, double(to - o) / fade});
            for (size_t c = 0; c < nch; ++c) {
                float& dst = out.samples[size_t(o) * nch + c];
                dst = float(dst * (1 - edge) + acc[oi * nch + c] / norm * edge);
            }
        }
        return true;
    };
    for (auto [ra, rb] : merged) {
        ra = std::max(0.0, ra);
        double startTs = -1;
        while (ra < rb) {
            if (cancel && cancel->load()) return;
            const double pieceEnd = std::min(rb, ra + kPiece);
            const bool last = pieceEnd >= rb;
            const double reach = std::min(double(n) / rate, pieceEnd + 1.0);
            PitchTrack own;
            const PitchTrack& track = given ? *given : (own = trackPitch(in, ra - 0.05, reach + 0.05, 50, 800, cancel));
            if (track.f0.empty()) break;
            double end = pieceEnd;
            for (double t = pieceEnd; t < reach; t += track.hop)
                if (track.at(t) <= 0) {
                    end = t;
                    break;
                }
            // (a piece before another runs 30 ms on, written to 10 ms past `end`; the next starts 50 ms back, written from
            // `end`, its grains going on from the first's, and fades in over the first's output)
            const double bEnd = last ? end : end + 0.03;
            const int64_t a = int64_t(std::floor(ra * rate)), b = std::min<int64_t>(n, int64_t(std::ceil(bEnd * rate)));
            double carry = -1;
            if (b - a >= 64 && !remake(track, a, b, startTs, (end - 0.03) * rate, last ? nullptr : &carry)) return;
            if (last || end >= double(n) / rate) break;
            ra = end - 0.05;
            startTs = carry;
        }
    }
}

// ---- Playing the effects -------------------------------------------------------

void resolveElasticMembers(const Project& p, std::vector<const Effect*>& effects, std::deque<Effect>& keep) {
    for (const Effect*& e : effects) {
        if (e->type != "elastic_audio" || e->s("members").empty()) continue;
        std::string keys;
        char buf[64];
        for (const ElasticMember& m : elasticMembersFromString(e->s("members"))) {
            const MediaItem* media = p.findMedia(m.media);
            if (!media || !media->hasAudio) continue;
            std::snprintf(buf, sizeof buf, "%.9f\t", m.offset);
            keys += buf + audioKey(media->path, m.channels) + "\n";
        }
        keep.push_back(*e);
        keep.back().strings["member_keys"] = keys;
        e = &keep.back();
    }
}

void playElasticEffect(const Effect& e, const AudioBuffer& in, AudioBuffer& out, const std::atomic<bool>* cancel) {
    if (e.type == "pitch_contour") {
        applyPitchEdits(in, pitchEditsFromString(e.s("edits")), out, cancel);
        return;
    }
    const DialogueWarp warp = dialogueWarpFromString(e.s("points"));
    const double offset = std::strtod(e.s("offset").c_str(), nullptr);
    std::vector<AudioBufferPtr> held;
    std::vector<std::pair<const AudioBuffer*, double>> group;
    const std::string keys = e.s("member_keys");
    size_t pos = 0;
    bool whole = !keys.empty();
    while (pos < keys.size()) {
        size_t end = keys.find('\n', pos);
        if (end == std::string::npos) end = keys.size();
        const std::string line = keys.substr(pos, end - pos);
        pos = end + 1;
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        AudioBufferPtr b = MediaPool::instance().audio(line.substr(tab + 1), in.sampleRate);
        if (!b || b->frames() == 0) {
            whole = false;  // a member that cannot be read: this sound cut on its own
            break;
        }
        held.push_back(b);
        group.emplace_back(b.get(), std::strtod(line.c_str(), nullptr));
    }
    if (!whole) group.clear();
    applyElasticWarp(in, warp, elasticModeOf(e), std::isfinite(offset) ? offset : 0, group, out, cancel);
}

AudioBufferPtr soundBeforePitchEdits(const Project& p, const Clip& c, int rate) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || !m->hasAudio || m->kind == MediaKind::Sequence) return nullptr;
    const std::string key = audioKey(m->path, c.channels);
    AudioBufferPtr sound = MediaPool::instance().audio(key, rate);
    if (!sound) return nullptr;
    std::vector<const Effect*> fx;
    for (const Effect& e : c.effects) {
        if (e.type == "pitch_contour") break;
        if (e.enabled && isSourceAudioEffect(e.type) && dialogueAlignmentApplies(e, c) && bandwidthExtensionApplies(e, c)) fx.push_back(&e);
    }
    std::deque<Effect> resolved;
    resolveElasticMembers(p, fx, resolved);
    if (!fx.empty())
        if (AudioBufferPtr played = cleanedAudio(key, sound, fx, true)) return played;
    return sound;
}

const Effect* elasticEffectOf(const Clip& c) {
    for (const Effect& e : c.effects)
        if (e.type == "elastic_audio") return &e;
    return nullptr;
}

const Effect* pitchContourOf(const Clip& c) {
    for (const Effect& e : c.effects)
        if (e.type == "pitch_contour") return &e;
    return nullptr;
}

double elasticOffsetOf(const Clip& c) {
    const Effect* e = elasticEffectOf(c);
    if (!e) return 0;
    const double v = std::strtod(e->s("offset").c_str(), nullptr);
    return std::isfinite(v) ? v : 0;
}

// ---- Edits ---------------------------------------------------------------------

namespace {

Effect* elasticOf(Clip& c) {
    for (Effect& e : c.effects)
        if (e.type == "elastic_audio") return &e;
    return nullptr;
}

std::string groupOf(const Clip& c) {
    const Effect* e = elasticEffectOf(c);
    return e ? e->s("group") : std::string();
}

std::vector<Clip*> groupClips(Sequence& s, const std::string& group) {
    std::vector<Clip*> out;
    if (group.empty()) return out;
    for (Track& t : s.audioTracks)
        for (Clip& c : t.clips)
            if (groupOf(c) == group) out.push_back(&c);
    return out;
}

// Source seconds less timeline seconds (a clip at its own speed).
double sourceLead(const Sequence& s, const Clip& c) { return (c.sourceIn - double(c.start)) / s.fpsValue(); }

std::string warpable(const Project& p, const Sequence& s, const Clip& c) {
    const auto where = edit::locate(s, c.id);
    if (!where || where->track.kind != TrackKind::Audio) return "Elastic audio is for audio clips";
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || !m->hasAudio || m->kind == MediaKind::Sequence) return "Elastic audio needs a clip of a sound file";
    if (std::fabs(c.speed - 1) > 1e-9 || c.reverse || c.ramped()) return "Elastic audio needs the clip at its own speed, forwards";
    return {};
}

bool trackLocked(const Sequence& s, Id clip) {
    const auto where = edit::locate(s, clip);
    if (!where) return true;
    const Track* t = where->track.kind == TrackKind::Audio ? &s.audioTracks[size_t(where->track.index)] : nullptr;
    return t && t->locked;
}

// The group's span (group seconds): every member clip's.
std::pair<double, double> groupSpan(const Sequence& s, const std::vector<Clip*>& clips) {
    double lo = 1e300, hi = -1e300;
    const double fps = s.fpsValue();
    for (const Clip* c : clips) {
        const double a = c->sourceIn / fps - elasticOffsetOf(*c);
        lo = std::min(lo, a), hi = std::max(hi, a + double(c->duration) / fps);
    }
    return {lo, hi};
}

DialogueWarp pointsOf(const Clip& c) {
    const Effect* e = elasticEffectOf(c);
    return e ? dialogueWarpFromString(e->s("points")) : DialogueWarp{};
}

// Anchors at the group's ends, so the sound outside the markers stays put.
void anchor(DialogueWarp& w, std::pair<double, double> span) {
    if (w.empty()) return;
    if (span.first < w.front().u - 1e-4 && span.first < w.front().w - 1e-4) w.insert(w.begin(), {span.first, span.first});
    if (span.second > w.back().u + 1e-4 && span.second > w.back().w + 1e-4) w.push_back({span.second, span.second});
}

void keepPitchAfter(Clip& c) {
    auto pitch = std::find_if(c.effects.begin(), c.effects.end(), [](const Effect& e) { return e.type == "pitch_contour"; });
    auto elastic = std::find_if(c.effects.begin(), c.effects.end(), [](const Effect& e) { return e.type == "elastic_audio"; });
    if (pitch == c.effects.end() || elastic == c.effects.end() || pitch > elastic) return;
    Effect moved = *pitch;
    c.effects.erase(pitch);
    elastic = std::find_if(c.effects.begin(), c.effects.end(), [](const Effect& e) { return e.type == "elastic_audio"; });
    c.effects.insert(elastic + 1, moved);
}

edit::Result writePoints(Sequence& s, Clip& c, const DialogueWarp& points) {
    std::vector<Clip*> clips = groupClips(s, groupOf(c));
    if (clips.empty()) return edit::Result::fail("Elastic audio is not on for that clip");
    for (Clip* m : clips)
        if (trackLocked(s, m->id)) return edit::Result::fail("A clip warped with it is on a locked track");
    const std::string text = isIdentity(points) && points.size() <= 2 ? std::string() : dialogueWarpToString(points);
    bool changed = false;
    for (Clip* m : clips) {
        Effect* e = elasticOf(*m);
        if (e->s("points") == text) continue;
        e->strings["points"] = text;
        changed = true;
    }
    return changed ? edit::Result{} : edit::Result::fail("Nothing changed");
}

}  // namespace

namespace edit {

std::vector<Id> elasticGroup(const Sequence& s, Id clip) {
    std::vector<Id> out;
    const Clip* c = clipById(s, clip);
    if (!c) return out;
    const std::string g = groupOf(*c);
    if (g.empty()) return {clip};
    for (const Track& t : s.audioTracks)
        for (const Clip& o : t.clips)
            if (groupOf(o) == g) out.push_back(o.id);
    return out;
}

Result enableElasticAudio(Project& p, Sequence& s, const std::vector<Id>& ids, ElasticMode mode) {
    // The clips, the audio clips linked to them, and whole groups any of them is in.
    std::vector<Id> all;
    auto addId = [&](Id id) {
        if (std::find(all.begin(), all.end(), id) == all.end()) all.push_back(id);
    };
    for (Id id : ids) {
        const Clip* c = clipById(s, id);
        if (!c) return Result::fail("No such clip");
        const auto where = locate(s, id);
        if (where && where->track.kind == TrackKind::Audio) addId(id);
        for (Id l : linkedClips(s, id))
            if (const auto lw = locate(s, l); lw && lw->track.kind == TrackKind::Audio) addId(l);
    }
    for (size_t i = 0; i < all.size(); ++i)
        for (Id g : elasticGroup(s, all[i])) addId(g);
    if (all.empty()) return Result::fail("Elastic audio is for audio clips");
    for (Id id : all) {
        const Clip* c = clipById(s, id);
        if (const std::string why = warpable(p, s, *c); !why.empty()) return Result::fail(why);
        if (trackLocked(s, id)) return Result::fail("The clip's track is locked");
    }
    // The group's time: an existing group's (the first member found with markers, else the first found), else the
    // first clip's own source time. Two groups each with markers of their own are not merged: one set would be lost.
    const Clip* leader = nullptr;
    for (Id id : all)
        if (const Clip* c = clipById(s, id); elasticEffectOf(*c) && (!leader || (pointsOf(*leader).empty() && !pointsOf(*c).empty()))) leader = c;
    if (leader)
        for (Id id : all)
            if (const Clip* c = clipById(s, id); elasticEffectOf(*c) && groupOf(*c) != groupOf(*leader) && !pointsOf(*c).empty() &&
                                                 pointsOf(*c) != pointsOf(*leader))
                return Result::fail("The clips are warped in two groups, each with its own markers: take elastic audio off one first");
    const bool fresh = !leader;
    if (!leader) leader = clipById(s, all.front());
    const double leaderOffset = elasticOffsetOf(*leader);
    const double leaderLead = sourceLead(s, *leader);
    const DialogueWarp points = pointsOf(*leader);
    const std::string group = fresh ? std::to_string(p.newId()) : groupOf(*leader);
    std::vector<ElasticMember> members;
    std::map<Id, double> offsets;
    for (Id id : all) {
        const Clip* c = clipById(s, id);
        // Clips of a group made elsewhere keep their own place in it.
        const double off = !fresh && groupOf(*c) == group ? elasticOffsetOf(*c) : leaderOffset + sourceLead(s, *c) - leaderLead;
        offsets[id] = off;
        ElasticMember m{c->mediaId, c->channels, std::round(off * 1e9) / 1e9};
        if (std::find(members.begin(), members.end(), m) == members.end()) members.push_back(m);
    }
    const std::string memberText = members.size() > 1 ? elasticMembersToString(members) : std::string();
    char buf[64];
    for (Id id : all) {
        Clip* c = clipById(s, id);
        Effect* e = elasticOf(*c);
        if (!e) {
            c->effects.push_back(makeEffect(p, "elastic_audio"));
            e = &c->effects.back();
        }
        e->params["mode"] = Param(double(int(mode)));
        e->strings["group"] = group;
        std::snprintf(buf, sizeof buf, "%.9f", offsets[id]);
        e->strings["offset"] = buf;
        e->strings["points"] = points.empty() ? std::string() : dialogueWarpToString(points);
        e->strings["members"] = memberText;
        keepPitchAfter(*c);
    }
    Result r;
    r.created = all;
    return r;
}

Result removeElasticAudio(Project&, Sequence& s, Id clip) {
    const Clip* c = clipById(s, clip);
    if (!c) return Result::fail("No such clip");
    std::vector<Id> ids = elasticGroup(s, clip);
    bool changed = false;
    for (Id id : ids) {
        if (trackLocked(s, id)) return Result::fail("The clip's track is locked");
    }
    for (Id id : ids) {
        Clip* m = clipById(s, id);
        const size_t before = m->effects.size();
        std::erase_if(m->effects, [](const Effect& e) { return e.type == "elastic_audio" || e.type == "pitch_contour"; });
        changed |= m->effects.size() != before;
    }
    return changed ? Result{} : Result::fail("The clip has no elastic audio");
}

Result setElasticMode(Project&, Sequence& s, Id clip, ElasticMode mode) {
    Clip* c = clipById(s, clip);
    if (!c || !elasticEffectOf(*c)) return Result::fail("Elastic audio is not on for that clip");
    bool changed = false;
    for (Clip* m : groupClips(s, groupOf(*c))) {
        if (trackLocked(s, m->id)) return Result::fail("The clip's track is locked");
        Effect* e = elasticOf(*m);
        if (elasticModeOf(*e) == mode) continue;
        e->params["mode"] = Param(double(int(mode)));
        changed = true;
    }
    return changed ? Result{} : Result::fail("Nothing changed");
}

Result setWarpPoints(Project&, Sequence& s, Id clip, DialogueWarp points) {
    Clip* c = clipById(s, clip);
    if (!c || !elasticEffectOf(*c)) return Result::fail("Elastic audio is not on for that clip");
    for (size_t i = 0; i < points.size(); ++i) {
        const AlignPoint& pt = points[i];
        if (!std::isfinite(pt.u) || !std::isfinite(pt.w) || std::fabs(pt.u) > 1e7 || std::fabs(pt.w - pt.u) > 3600)
            return Result::fail("A marker is out of range");
        if (i > 0 && (pt.u <= points[i - 1].u + 1e-4 || pt.w <= points[i - 1].w + 1e-4))
            return Result::fail("Markers must be in order, both where they play and what they play");
    }
    anchor(points, groupSpan(s, groupClips(s, groupOf(*c))));
    return writePoints(s, *c, points);
}

Result moveWarpMarker(Project& p, Sequence& s, Id clip, double from, double to, double grab) {
    Clip* c = clipById(s, clip);
    if (!c || !elasticEffectOf(*c)) return Result::fail("Elastic audio is not on for that clip");
    DialogueWarp w = pointsOf(*c);
    const auto span = groupSpan(s, groupClips(s, groupOf(*c)));
    size_t k = w.size();
    double nearest = grab;
    for (size_t i = 0; i < w.size(); ++i)
        if (std::fabs(w[i].u - from) <= nearest) nearest = std::fabs(w[i].u - from), k = i;
    if (k == w.size()) {
        if (from <= span.first || from >= span.second) return Result::fail("A marker must be inside the clip");
        const AlignPoint made{from, dialogueWarpAt(w, from)};
        w.insert(std::upper_bound(w.begin(), w.end(), made, [](const AlignPoint& a, const AlignPoint& b) { return a.u < b.u; }), made);
        anchor(w, span);
        for (k = 0; k < w.size() && w[k].u != from; ++k) {}
    }
    constexpr double gap = 0.005;
    const double lo = k > 0 ? w[k - 1].u + gap : -1e300, hi = k + 1 < w.size() ? w[k + 1].u - gap : 1e300;
    if (lo > hi) return Result::fail("No room to move that marker");
    w[k].u = std::clamp(to, lo, hi);
    return setWarpPoints(p, s, clip, w);
}

Result removeWarpMarker(Project&, Sequence& s, Id clip, double at, double grab) {
    Clip* c = clipById(s, clip);
    if (!c || !elasticEffectOf(*c)) return Result::fail("Elastic audio is not on for that clip");
    DialogueWarp w = pointsOf(*c);
    size_t k = w.size();
    double nearest = grab;
    for (size_t i = 0; i < w.size(); ++i)
        if (std::fabs(w[i].u - at) <= nearest) nearest = std::fabs(w[i].u - at), k = i;
    if (k == w.size()) return Result::fail("No marker there");
    w.erase(w.begin() + long(k));
    if (isIdentity(w)) w.clear();
    return writePoints(s, *c, w);
}

Result addWarpMarkersAtOnsets(Project& p, Sequence& s, Id clip, double sensitivity, int* added) {
    if (added) *added = 0;
    Clip* c = clipById(s, clip);
    if (!c) return Result::fail("No such clip");
    if (!elasticEffectOf(*c))
        if (Result r = enableElasticAudio(p, s, {clip}); !r.ok) return r;
    c = clipById(s, clip);
    std::vector<Clip*> clips = groupClips(s, groupOf(*c));
    const auto span = groupSpan(s, clips);
    // The group's sound over its span (each member's own source, at its offset), as one.
    constexpr int rate = 48000;
    AudioBuffer mix;
    mix.sampleRate = rate;
    mix.channels = 1;
    const int64_t len = int64_t(std::ceil((span.second - span.first) * rate));
    if (len <= 0) return Result::fail("The clip is too short");
    mix.samples.assign(size_t(len), 0.0f);
    std::set<std::pair<Id, std::vector<int>>> seen;
    for (const Clip* m : clips) {
        if (!seen.insert({m->mediaId, m->channels}).second) continue;
        const MediaItem* media = p.findMedia(m->mediaId);
        if (!media) continue;
        AudioBufferPtr b = MediaPool::instance().audio(audioKey(media->path, m->channels), rate);
        if (!b) continue;
        const std::vector<float> mono = monoOf(*b);
        const int64_t base = std::llround((span.first + elasticOffsetOf(*m)) * rate);
        for (int64_t i = 0; i < len; ++i) {
            const int64_t at = base + i;
            if (at >= 0 && at < int64_t(mono.size())) mix.samples[size_t(i)] += mono[size_t(at)];
        }
    }
    DialogueWarp w = pointsOf(*c);
    anchor(w, span);
    int count = 0;
    for (double t : detectOnsets(mix, sensitivity)) {
        const double fileTime = span.first + t;  // what is heard there in the file (group seconds)
        const double u = dialogueWarpInverse(w, fileTime);
        if (u <= span.first + 0.01 || u >= span.second - 0.01) continue;
        if (std::any_of(w.begin(), w.end(), [&](const AlignPoint& pt) { return std::fabs(pt.u - u) < 0.02 || std::fabs(pt.w - fileTime) < 0.02; }))
            continue;
        const AlignPoint made{u, fileTime};
        const auto at = std::upper_bound(w.begin(), w.end(), made, [](const AlignPoint& a, const AlignPoint& b) { return a.u < b.u; });
        // Kept in order both ways (an onset between two moved markers can break that): such ones are left out.
        if ((at != w.begin() && std::prev(at)->w >= fileTime - 1e-4) || (at != w.end() && at->w <= fileTime + 1e-4)) continue;
        w.insert(at, made);
        ++count;
    }
    if (added) *added = count;
    if (!count) return Result::fail("No onsets found");
    anchor(w, span);
    return writePoints(s, *c, w);
}

Result setPitchEdits(Project& p, Sequence& s, Id clip, const std::vector<PitchEdit>& edits) {
    Clip* c = clipById(s, clip);
    if (!c) return Result::fail("No such clip");
    if (const std::string why = warpable(p, s, *c); !why.empty()) return Result::fail(why);
    if (trackLocked(s, clip)) return Result::fail("The clip's track is locked");
    for (const PitchEdit& e : edits) {
        if (!std::isfinite(e.start) || !std::isfinite(e.end) || e.end <= e.start) return Result::fail("Each stretch must end after it starts");
        if (!(std::fabs(e.shift) <= 24) || !(e.inflection >= 0 && e.inflection <= 4) || !(e.correct >= 0 && e.correct <= 1) ||
            e.key < 0 || e.key > 11 || !validPitchScale(e.scale) || !(std::fabs(e.formant) <= 12))
            return Result::fail("Shift is -24..24 semitones, inflection 0..4, correct 0..1, key 0..11, formant -12..12");
    }
    const std::string text = edits.empty() ? std::string() : pitchEditsToString(edits);
    auto it = std::find_if(c->effects.begin(), c->effects.end(), [](const Effect& e) { return e.type == "pitch_contour"; });
    if (text.empty()) {
        if (it == c->effects.end()) return Result::fail("The clip has no pitch edits");
        c->effects.erase(it);
        return {};
    }
    if (it == c->effects.end()) {
        c->effects.push_back(makeEffect(p, "pitch_contour"));
        it = c->effects.end() - 1;
    } else if (it->s("edits") == text) {
        return Result::fail("Nothing changed");
    }
    it->strings["edits"] = text;
    keepPitchAfter(*c);
    return {};
}

}  // namespace edit

}  // namespace montage
