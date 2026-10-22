#include "Intelligibility.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <deque>

#include "audio/RoomResponse.h"
#include "core/EditOps.h"
#include "media/StemSeparation.h"
#include "render/Compositor.h"

namespace montage {

namespace {

// ANSI S3.5-1997 Table 3 (one-third-octave bands): centre frequencies, band importance (average speech), the reference
// internal noise spectrum level and the standard speech spectrum level at normal vocal effort.
constexpr std::array<double, kSiiBands> kCentres = {160, 200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000};
constexpr std::array<double, kSiiBands> kImportance = {0.0083, 0.0095, 0.0150, 0.0289, 0.0440, 0.0578, 0.0653, 0.0711, 0.0818,
                                                       0.0844, 0.0882, 0.0898, 0.0868, 0.0844, 0.0771, 0.0527, 0.0364, 0.0185};
constexpr std::array<double, kSiiBands> kInternalNoise = {0.6,   -1.7,  -3.9,  -6.1,  -8.2,  -9.7,  -10.8, -11.9, -12.5,
                                                          -13.5, -15.4, -17.7, -21.2, -24.2, -25.9, -23.6, -15.8, -7.1};
constexpr std::array<double, kSiiBands> kStandardSpeech = {32.41, 34.48, 34.75, 33.98, 34.59, 34.27, 32.06, 28.30, 25.01,
                                                           23.00, 20.15, 17.32, 13.18, 11.55, 9.33,  5.31,  2.59,  1.13};
// A one-third-octave band is this much of its centre frequency wide (2^(1/6) - 2^(-1/6)).
constexpr double kBandWidth = 0.2316;
constexpr int kFft = 8192;
constexpr int kHop = 4096;
constexpr double kPause = 1.0;  // seconds without speech that do not end a hard stretch

// Each band's power in a stretch of interleaved stereo sound, both channels averaged, Hann-windowed.
class BandAnalyser {
public:
    explicit BandAnalyser(int rate) : rate_(rate), window_(kFft) {
        for (int i = 0; i < kFft; ++i) window_[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * i / kFft));
        double s2 = 0;
        for (float w : window_) s2 += double(w) * w;
        scale_ = 2.0 / (double(kFft) * s2);
        for (int b = 0; b < kSiiBands; ++b) {
            const double lo = kCentres[size_t(b)] * std::pow(2.0, -1.0 / 6), hi = kCentres[size_t(b)] * std::pow(2.0, 1.0 / 6);
            first_[size_t(b)] = std::max(1, int(std::ceil(lo * kFft / rate)));
            last_[size_t(b)] = std::min(kFft / 2, int(std::floor(hi * kFft / rate)));
        }
    }
    // The band powers (mean square, full scale 1) of the frame starting at `at` in `l`/`r` (mono: r null).
    std::array<double, kSiiBands> bands(const float* l, const float* r) {
        std::array<double, kSiiBands> out{};
        for (const float* ch : {l, r}) {
            if (!ch) continue;
            std::vector<std::complex<float>> a(kFft);
            for (int i = 0; i < kFft; ++i) a[size_t(i)] = ch[i] * window_[size_t(i)];
            fftInPlace(a, false);
            for (int b = 0; b < kSiiBands; ++b) {
                double s = 0;
                for (int k = first_[size_t(b)]; k <= last_[size_t(b)]; ++k) s += std::norm(a[size_t(k)]);
                out[size_t(b)] += s * scale_ / (r ? 2.0 : 1.0);
            }
        }
        return out;
    }
    int rate() const { return rate_; }

private:
    int rate_;
    std::vector<float> window_;
    double scale_ = 1;
    std::array<int, kSiiBands> first_{}, last_{};
};

// Dialogue and masker pushed in step; each 400 ms block's band powers (summed over the frames centred in it) kept.
struct Collector {
    BandAnalyser analyser;
    int64_t blockLen;
    std::vector<float> dl, dr, ml, mr;  // what has not yet been framed, from `offset` on
    size_t offset = 0;
    int64_t framed = 0;                 // samples (from the range's start) `offset` is at
    struct Block {
        std::array<double, kSiiBands> speech{}, masker{};
        int frames = 0;
    };
    std::vector<Block> blocks;

    Collector(int rate, double block) : analyser(rate), blockLen(std::max<int64_t>(1, int64_t(std::llround(block * rate)))) {}

    void push(const float* dialogue, const float* masker, int n) {
        for (auto* v : {&dl, &dr, &ml, &mr}) v->erase(v->begin(), v->begin() + long(offset));  // (once a push)
        offset = 0;
        for (int i = 0; i < n; ++i) {
            dl.push_back(dialogue[2 * i]), dr.push_back(dialogue[2 * i + 1]);
            ml.push_back(masker[2 * i]), mr.push_back(masker[2 * i + 1]);
        }
        while (dl.size() - offset >= size_t(kFft)) {
            const auto sp = analyser.bands(dl.data() + offset, dr.data() + offset), ms = analyser.bands(ml.data() + offset, mr.data() + offset);
            const size_t b = size_t((framed + kFft / 2) / blockLen);
            if (blocks.size() <= b) blocks.resize(b + 1);
            for (int k = 0; k < kSiiBands; ++k) blocks[b].speech[size_t(k)] += sp[size_t(k)], blocks[b].masker[size_t(k)] += ms[size_t(k)];
            ++blocks[b].frames;
            offset += kHop;
            framed += kHop;
        }
    }
};

}  // namespace

const std::array<double, kSiiBands>& siiBandCentres() { return kCentres; }
const std::array<double, kSiiBands>& siiBandImportance() { return kImportance; }

double speechIntelligibilityIndex(const std::array<double, kSiiBands>& speechDb, const std::array<double, kSiiBands>& maskerDb) {
    // Spectrum levels (per hertz), as the standard works in.
    std::array<double, kSiiBands> E{}, N{}, B{}, C{};
    for (int i = 0; i < kSiiBands; ++i) {
        const double perHz = 10 * std::log10(kBandWidth * kCentres[size_t(i)]);
        E[size_t(i)] = std::max(-100.0, speechDb[size_t(i)]) - perHz;
        N[size_t(i)] = std::max(-100.0, maskerDb[size_t(i)]) - perHz;
        // The speech masks itself 24 dB down; the masking spreads upwards with a slope that grows with level.
        B[size_t(i)] = std::max(N[size_t(i)], E[size_t(i)] - 24);
        C[size_t(i)] = -80 + 0.6 * (B[size_t(i)] + 10 * std::log10(kCentres[size_t(i)]) - 6.353);
    }
    double sii = 0;
    for (int i = 0; i < kSiiBands; ++i) {
        double z = B[size_t(i)];
        if (i > 0) {
            double sum = std::pow(10.0, 0.1 * N[size_t(i)]);
            for (int k = 0; k < i; ++k)
                sum += std::pow(10.0, 0.1 * (B[size_t(k)] + 3.32 * C[size_t(k)] * std::log10(0.89 * kCentres[size_t(i)] / kCentres[size_t(k)])));
            z = 10 * std::log10(sum);
        }
        const double d = std::max(z, kInternalNoise[size_t(i)]);  // (normal hearing: the threshold adds nothing)
        const double level = std::min(1.0, 1 - (E[size_t(i)] - kStandardSpeech[size_t(i)] - 10) / 160);
        const double audible = std::clamp((E[size_t(i)] - d + 15) / 30, 0.0, 1.0);
        sii += kImportance[size_t(i)] * level * audible;
    }
    return std::clamp(sii, 0.0, 1.0);
}

bool intelligibilityPreset(const std::string& name, IntelligibilityLimits& out) {
    if (name == "Standard") out = {0.45, 0.6};
    else if (name == "Accessible") out = {0.6, 0.75};
    else if (name == "Lenient") out = {0.35, 0.5};
    else return false;
    return true;
}

const std::vector<std::string>& intelligibilityPresetNames() {
    static const std::vector<std::string> names = {"Standard", "Accessible", "Lenient"};
    return names;
}

std::vector<std::pair<double, double>> Intelligibility::under(double limit, double minSeconds) const {
    std::vector<std::pair<double, double>> out;
    double from = -1, to = -1;
    auto close = [&] {
        if (from >= 0 && to - from >= minSeconds - 1e-9) out.push_back({from, to});
        from = to = -1;
    };
    for (const IntelligibilityBlock& b : blocks) {
        if (b.speech && b.index < limit) {
            if (from < 0) from = b.start;
            to = b.end;
        } else if (b.speech || (from >= 0 && b.end - to > kPause)) {
            close();  // (understood again, or a pause longer than one between phrases)
        }
    }
    close();
    return out;
}

double Intelligibility::at(double seconds) const {
    for (const IntelligibilityBlock& b : blocks)
        if (seconds >= b.start && seconds < b.end) return b.speech ? b.index : -1;
    return -1;
}

Intelligibility measureIntelligibility(const Project& p, const Sequence& s, const IntelligibilityOptions& o,
                                       const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return Intelligibility{};
    };
    const double fps = s.fpsValue() > 0 ? s.fpsValue() : 30.0;
    const int rate = s.sampleRate > 0 ? s.sampleRate : 48000;
    const FrameTime out = o.out < 0 ? s.duration() : o.out;
    if (out <= o.in) return fail("Nothing to measure");
    const int64_t first = int64_t(std::llround(double(o.in) * rate / fps)), end = int64_t(std::llround(double(out) * rate / fps));

    // The dialogue: Dialogue clips heard in the range, else separated from the mix.
    const bool anySolo = std::any_of(s.audioTracks.begin(), s.audioTracks.end(), [](const Track& t) { return t.solo; });
    bool roles = false;
    if (!edit::roleMuted(s, "Dialogue"))
        for (size_t i = 0; i < s.audioTracks.size() && !roles; ++i) {
            const Track& t = s.audioTracks[i];
            if (t.muted || (anySolo && !t.solo) || (!o.tracks.empty() && (i >= o.tracks.size() || !o.tracks[i]))) continue;
            for (const Clip& c : t.clips)
                if (c.enabled && c.role == "Dialogue" && c.end() > o.in && c.start < out) roles = true;
        }
    if (o.source == DialogueSource::Roles && !roles) return fail("No Dialogue clips are heard in the range: give the dialogue the Dialogue role");
    const bool separate = !roles || o.source == DialogueSource::Separated;
    if (separate) {
        if (!stemSeparationAvailable()) return fail("Give the dialogue clips the Dialogue role (this build cannot separate a mix)");
        if (!stemModel(3).installed()) return fail("Give the dialogue clips the Dialogue role, or download the dialogue, music and effects model");
        if (rate != stemRate(3)) return fail("Separating the dialogue from the mix needs a 48 kHz sequence");
    }

    Collector col(rate, o.block);
    const int64_t chunk = rate;  // a second at a time
    std::vector<float> dialogue(size_t(chunk) * 2), rest(size_t(chunk) * 2), mix(size_t(chunk) * 2);
    AudioMixer dialogueMixer, restMixer;
    dialogueMixer.setTrackMask(o.tracks);
    restMixer.setTrackMask(o.tracks);
    if (!separate) {
        // The masker is the whole mix less the dialogue, so music ducked under the dialogue (a compressor keyed from it)
        // is measured as it is heard.
        Sequence speech = s;
        for (Track& t : speech.audioTracks)
            for (Clip& c : t.clips)
                if (c.role != "Dialogue") c.enabled = false;
        for (int64_t pos = first; pos < end;) {
            if (cancel && cancel->load()) return fail("Cancelled");
            const int n = int(std::min<int64_t>(chunk, end - pos));
            dialogueMixer.mix(p, speech, pos, n, dialogue.data());
            restMixer.mix(p, s, pos, n, rest.data());
            for (size_t i = 0; i < size_t(2 * n); ++i) rest[i] -= dialogue[i];
            col.push(dialogue.data(), rest.data(), n);
            pos += n;
            if (progress) progress(double(pos - first) / double(end - first));
        }
    } else {
        // The mix, separated as it goes; what is not dialogue is the rest of the mix.
        std::deque<float> pending;  // the mix not yet given back separated
        std::string err;
        auto splitter = StemSplitter::create(3, [&](const std::vector<std::vector<float>>& stems) {
            const std::vector<float>& d = stems.at(0);
            const int n = int(d.size() / 2);
            std::vector<float> m(d.size());
            for (size_t i = 0; i < d.size(); ++i) {
                m[i] = (pending.empty() ? 0.0f : pending.front()) - d[i];
                if (!pending.empty()) pending.pop_front();
            }
            col.push(d.data(), m.data(), n);
            return !(cancel && cancel->load());
        }, &err);
        if (!splitter) return fail(err);
        splitter->setCancel(cancel);
        for (int64_t pos = first; pos < end;) {
            if (cancel && cancel->load()) return fail("Cancelled");
            const int n = int(std::min<int64_t>(chunk, end - pos));
            restMixer.mix(p, s, pos, n, mix.data());
            pending.insert(pending.end(), mix.begin(), mix.begin() + 2 * n);
            if (!splitter->push(mix.data(), size_t(n), &err)) return fail(err.empty() ? "Cancelled" : err);
            pos += n;
            if (progress) progress(0.95 * double(pos - first) / double(end - first));
        }
        if (!splitter->finish(&err)) return fail(err.empty() ? "Cancelled" : err);
    }
    // The last part of a frame: padded with silence so the end is measured too.
    {
        const int pad = kFft;
        std::vector<float> zeros(size_t(pad) * 2, 0.0f);
        col.push(zeros.data(), zeros.data(), pad);
    }

    // Where someone speaks: blocks whose dialogue is within 20 dB of its usual level (and not near silence).
    const double toSpl = o.listeningLevel + 20;
    const size_t nb = size_t((end - first + col.blockLen - 1) / col.blockLen);
    col.blocks.resize(nb);
    std::vector<double> level(nb, -200);
    double energy = 0;
    int counted = 0;
    for (size_t b = 0; b < nb; ++b) {
        const auto& bl = col.blocks[b];
        if (bl.frames == 0) continue;
        double sum = 0;
        for (double v : bl.speech) sum += v;
        level[b] = 10 * std::log10(std::max(1e-20, sum / bl.frames));
        if (level[b] > -70) energy += sum / bl.frames, ++counted;
    }
    const double usual = counted ? 10 * std::log10(energy / counted) : -200;
    Intelligibility r;
    r.valid = true;
    r.source = separate ? DialogueSource::Separated : DialogueSource::Roles;
    double total = 0;
    int speaking = 0;
    for (size_t b = 0; b < nb; ++b) {
        IntelligibilityBlock blk;
        blk.start = (double(first) + double(b) * double(col.blockLen)) / rate;
        blk.end = std::min(double(end), double(first) + double(b + 1) * double(col.blockLen)) / rate;
        const auto& bl = col.blocks[b];
        blk.speech = bl.frames > 0 && level[b] > -60 && level[b] >= usual - 20;
        if (blk.speech) {
            std::array<double, kSiiBands> sp{}, ms{};
            for (int k = 0; k < kSiiBands; ++k) {
                sp[size_t(k)] = 10 * std::log10(std::max(1e-20, bl.speech[size_t(k)] / bl.frames)) + toSpl;
                ms[size_t(k)] = 10 * std::log10(std::max(1e-20, bl.masker[size_t(k)] / bl.frames)) + toSpl;
                blk.ratioDb[size_t(k)] = float(std::clamp(sp[size_t(k)] - ms[size_t(k)], -99.0, 99.0));
            }
            blk.index = speechIntelligibilityIndex(sp, ms);
            total += blk.index, ++speaking;
        }
        r.blocks.push_back(blk);
    }
    if (!speaking) return fail("Nobody speaks in the range");
    r.average = total / speaking;
    if (progress) progress(1.0);
    return r;
}

}  // namespace montage
