#include "AutoMix.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <map>
#include <numeric>

#include "core/EditOps.h"
#include "media/AutoDuck.h"
#include "media/Decoder.h"
#include "media/Loudness.h"

namespace montage {

const char* audioRoleName(AudioRole r) {
    switch (r) {
        case AudioRole::Dialogue: return "Dialogue";
        case AudioRole::Music: return "Music";
        case AudioRole::Effects: return "Effects";
        case AudioRole::Silence: break;
    }
    return "Silence";
}

namespace {

constexpr int kRate = 16000;
constexpr int kFft = 512;    // 32 ms
constexpr int kHop = 160;    // 10 ms: 100 frames a second
constexpr int kMixRate = 48000;

void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = float(-2 * M_PI / double(len));
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

std::vector<float> to16k(const std::vector<float>& in, int rate) {
    if (rate == kRate || in.empty()) return in;
    const size_t n = size_t(double(in.size()) * kRate / rate);
    std::vector<float> out(n);
    const double step = double(rate) / kRate;
    const int w = std::max(1, int(step));
    for (size_t i = 0; i < n; ++i) {
        // Mean of the input samples this one stands for (a crude low-pass), then the nearest.
        const size_t a = size_t(double(i) * step);
        double acc = 0;
        int cnt = 0;
        for (int k = 0; k < w && a + size_t(k) < in.size(); ++k, ++cnt) acc += in[a + size_t(k)];
        out[i] = cnt ? float(acc / cnt) : 0.0f;
    }
    return out;
}

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

}  // namespace

RoleGuess classifyAudio(const std::vector<float>& monoIn, int rate, double wordsPerSecond) {
    RoleGuess g;
    {
        AudioBuffer b;
        b.sampleRate = rate;
        b.samples.resize(monoIn.size() * 2);
        for (size_t i = 0; i < monoIn.size(); ++i) b.samples[i * 2] = b.samples[i * 2 + 1] = monoIn[i];
        const LoudnessResult r = measureLoudness(b);
        g.loudness = r.valid ? r.integrated - 3.01 : -70;  // as one channel, not two
    }
    if (g.loudness < -60) return g;  // silence
    const std::vector<float> x = to16k(monoIn, rate);
    const int frames = int(x.size() / kHop);
    if (frames < 100) {
        g.role = AudioRole::Effects;
        return g;
    }
    // Per 10 ms: speech-band energy (300-3400 Hz) and spectral flux.
    std::vector<double> band(static_cast<size_t>(frames)), flux(static_cast<size_t>(frames), 0.0);
    std::vector<std::complex<float>> buf(kFft);
    std::vector<float> mag(kFft / 2 + 1), prev(kFft / 2 + 1, 0.0f);
    const int lo = 300 * kFft / kRate, hi = 3400 * kFft / kRate;
    for (int t = 0; t < frames; ++t) {
        const size_t s0 = size_t(t) * kHop;
        for (int i = 0; i < kFft; ++i) {
            const size_t s = s0 + size_t(i);
            const float w = float(0.5 - 0.5 * std::cos(2 * M_PI * i / kFft));
            buf[size_t(i)] = s < x.size() ? x[s] * w : 0.0f;
        }
        fft(buf);
        double e = 0, f = 0;
        for (int i = 0; i <= kFft / 2; ++i) {
            mag[size_t(i)] = std::abs(buf[size_t(i)]);
            if (i >= lo && i <= hi) e += double(mag[size_t(i)]) * mag[size_t(i)];
            f += std::max(0.0, std::log1p(1000.0 * mag[size_t(i)]) - std::log1p(1000.0 * prev[size_t(i)]));
        }
        band[size_t(t)] = e;
        flux[size_t(t)] = t > 0 ? f : 0;
        prev = mag;
    }

    // Pauses: the share of 10 ms frames well below their second's average (speech ~ 0.3+, music ~ 0.1).
    double lowShare = 0;
    int windows = 0;
    for (int w0 = 0; w0 + 100 <= frames; w0 += 50) {
        double mean = 0;
        for (int t = w0; t < w0 + 100; ++t) mean += band[size_t(t)];
        mean /= 100;
        if (mean <= 0) continue;
        int low = 0;
        for (int t = w0; t < w0 + 100; ++t) low += band[size_t(t)] < 0.1 * mean;
        lowShare += low / 100.0;
        ++windows;
    }
    lowShare = windows ? lowShare / windows : 0;

    // Syllable rhythm: the share of the loudness envelope's modulation at 2.5-8 Hz.
    double syllabic = 0;
    int mw = 0;
    constexpr int kWin = 256;  // 2.56 s
    std::vector<std::complex<float>> env(kWin);
    for (int w0 = 0; w0 + kWin <= frames; w0 += kWin / 2) {
        double mean = 0;
        for (int t = 0; t < kWin; ++t) mean += std::log10(1e-9 + band[size_t(w0 + t)]);
        mean /= kWin;
        for (int t = 0; t < kWin; ++t) {
            const float w = float(0.5 - 0.5 * std::cos(2 * M_PI * t / kWin));
            env[size_t(t)] = float(std::log10(1e-9 + band[size_t(w0 + t)]) - mean) * w;
        }
        fft(env);
        double in = 0, all = 0;
        for (int k = 1; k < kWin / 2; ++k) {
            const double hz = k * 100.0 / kWin, p = std::norm(env[size_t(k)]);
            if (hz < 0.5 || hz > 20) continue;
            all += p;
            if (hz >= 2.5 && hz <= 8) in += p;
        }
        if (all <= 0) continue;
        syllabic += in / all;
        ++mw;
    }
    syllabic = mw ? syllabic / mw : 0;

    // A steady beat: the onset envelope's autocorrelation at its best lag (40-200 BPM), against lag 0.
    double beat = 0;
    {
        const double mean = std::accumulate(flux.begin(), flux.end(), 0.0) / frames;
        std::vector<double> o(static_cast<size_t>(frames));
        for (int t = 0; t < frames; ++t) o[size_t(t)] = flux[size_t(t)] - mean;
        double r0 = 0;
        for (double v : o) r0 += v * v;
        if (r0 > 0) {
            for (int lag = 30; lag <= std::min(150, frames / 2); ++lag) {
                double r = 0;
                for (int t = lag; t < frames; ++t) r += o[size_t(t)] * o[size_t(t - lag)];
                beat = std::max(beat, r / r0);
            }
        }
    }

    g.pauses = lowShare, g.syllabic = syllabic, g.beat = beat;
    g.music = clamp01((beat - 0.15) / 0.35) * clamp01(1.0 - (lowShare - 0.15) / 0.3);
    // Pauses are the strongest sign of speech; syllable rhythm adds to it; a steady beat takes away.
    g.speech = clamp01((lowShare - 0.1) / 0.25) * (0.4 + 0.6 * clamp01((syllabic - 0.2) / 0.25)) * clamp01(1.0 - (beat - 0.3) / 0.4);
    if (wordsPerSecond >= 0) g.speech = std::max(g.speech, clamp01(wordsPerSecond / 1.5));
    if (g.speech >= 0.5 && g.speech >= g.music) g.role = AudioRole::Dialogue;
    else if (g.music >= 0.4) g.role = AudioRole::Music;
    else g.role = AudioRole::Effects;
    return g;
}

void replanClip(ClipMix& m, const MixOptions& o) {
    m.ride.clear();
    const double measured = m.guess.loudness;
    switch (m.role) {
        case AudioRole::Silence: m.gainDb = 0; return;
        case AudioRole::Dialogue: m.gainDb = o.dialogueLufs - measured; break;
        case AudioRole::Music: m.gainDb = o.musicLufs - measured; break;
        case AudioRole::Effects: m.gainDb = o.effectsLufs - measured; break;
    }
    m.gainDb = std::clamp(m.gainDb, -30.0, 24.0);
    if (m.role != AudioRole::Dialogue || !o.ride || m.shortTerm.size() < 3) return;
    // Even out the speech: each 3 s stretch towards the clip's own loudness, within the range,
    // smoothed over a few seconds; pauses hold the last value.
    std::vector<std::pair<FrameTime, double>> want;
    double last = 0;
    for (const auto& [f, st] : m.shortTerm) {
        if (st > -50) last = std::clamp(measured - st, -o.rideRangeDb, o.rideRangeDb);
        want.push_back({f, last});
    }
    std::vector<std::pair<FrameTime, double>> smooth(want.size());
    for (size_t i = 0; i < want.size(); ++i) {
        double acc = 0;
        int cnt = 0;
        for (size_t k = i >= 2 ? i - 2 : 0; k <= std::min(want.size() - 1, i + 2); ++k) acc += want[k].second, ++cnt;
        smooth[i] = {want[i].first, acc / cnt};
    }
    if (std::none_of(smooth.begin(), smooth.end(), [](auto& v) { return std::fabs(v.second) >= 1.0; })) return;
    // Keyframes only where the line bends.
    m.ride.push_back(smooth.front());
    for (size_t i = 1; i + 1 < smooth.size(); ++i) {
        const auto& a = m.ride.back();
        const auto& c = smooth[i + 1];
        const double t = double(smooth[i].first - a.first) / std::max<double>(1, double(c.first - a.first));
        const double line = a.second + (c.second - a.second) * t;
        if (std::fabs(smooth[i].second - line) > 0.5) m.ride.push_back(smooth[i]);
    }
    m.ride.push_back(smooth.back());
}

std::vector<ClipMix> planMix(const Project& p, const Sequence& s, const MixOptions& o, const std::function<void(double)>& progress,
                             const std::atomic<bool>* cancel, std::string* error) {
    std::vector<ClipMix> plan;
    std::vector<const Clip*> clips;
    for (const Track& t : s.audioTracks)
        if (!t.muted)
            for (const Clip& c : t.clips)
                if (c.enabled && c.mediaId) clips.push_back(&c);
    std::map<std::string, AudioBufferPtr> decoded;
    const double fps = s.fpsValue();
    for (size_t i = 0; i < clips.size(); ++i) {
        if (cancel && cancel->load()) return {};
        if (progress) progress(double(i) / double(clips.size()));
        const Clip& c = *clips[i];
        const MediaItem* m = p.findMedia(c.mediaId);
        if (!m || !m->hasAudio || m->path.empty()) continue;
        AudioBufferPtr& buf = decoded[m->path];
        std::string err;
        if (!buf) buf = decodeAudio(m->path, kMixRate, &err, cancel);
        if (!buf) {
            if (error && error->empty()) *error = err;
            continue;
        }
        // The stretch of the source the clip plays.
        double a = c.sourceFrameAt(c.start) / fps, b = c.sourceFrameAt(c.end() - 1) / fps;
        if (a > b) std::swap(a, b);
        b += 1 / fps;
        const int64_t first = std::clamp<int64_t>(int64_t(a * kMixRate), 0, buf->frames());
        const int64_t last = std::clamp<int64_t>(int64_t(b * kMixRate), first, buf->frames());
        if (last - first < kMixRate / 2) continue;
        ClipMix mix;
        mix.clip = c.id;
        std::vector<float> mono(size_t(last - first));
        for (int64_t k = first; k < last; ++k)
            mono[size_t(k - first)] = 0.5f * (buf->samples[size_t(k) * 2] + buf->samples[size_t(k) * 2 + 1]);
        double wps = -1;
        if (m->transcript && !m->transcript->empty()) {
            int words = 0;
            for (const auto& seg : m->transcript->segments)
                for (const auto& w : seg.words) words += w.start >= a && w.start < b;
            wps = words / std::max(1e-6, b - a);
        }
        mix.guess = classifyAudio(mono, kMixRate, wps);
        // The clip's loudness as heard (both channels), and its 3 s loudness every half second.
        const LoudnessResult whole = measureLoudness(*buf, first, last - first);
        mix.guess.loudness = whole.valid ? whole.integrated : -70;
        if (!whole.valid) mix.guess.role = AudioRole::Silence;
        LoudnessMeter meter(kMixRate);
        const int64_t step = kMixRate / 2;
        for (int64_t k = first; k < last; k += step) {
            const int64_t n = std::min(step, last - k);
            meter.add(buf->samples.data() + size_t(k) * 2, n);
            const double clipFrame = (double(k + n - first) / kMixRate - 1.5) * fps / std::max(1e-6, c.speed);
            mix.shortTerm.push_back({std::clamp<FrameTime>(FrameTime(std::llround(clipFrame)), 0, c.duration - 1), meter.shortTerm()});
        }
        mix.role = mix.guess.role;
        replanClip(mix, o);
        plan.push_back(std::move(mix));
    }
    if (progress) progress(1);
    return plan;
}

int applyMix(Project& p, Sequence& s, const std::vector<ClipMix>& plan, const MixOptions& o) {
    int changed = 0;
    std::vector<int> dialogueTracks;
    std::vector<Id> music;
    for (const ClipMix& m : plan) {
        if (m.role == AudioRole::Silence) continue;
        auto loc = edit::locate(s, m.clip);
        Clip* c = edit::clipById(s, m.clip);
        if (!loc || !c) continue;
        Param gain(m.gainDb);
        for (const auto& [f, db] : m.ride) gain.addKey(f, m.gainDb + db);
        c->audio.params["gain_db"] = gain;
        ++changed;
        if (m.role == AudioRole::Dialogue && std::find(dialogueTracks.begin(), dialogueTracks.end(), loc->track.index) == dialogueTracks.end())
            dialogueTracks.push_back(loc->track.index);
        if (m.role == AudioRole::Music) music.push_back(m.clip);
    }
    if (o.duck && !dialogueTracks.empty() && !music.empty()) {
        DuckOptions d;
        d.amountDb = o.duckDb;
        const Spans spans = dialogueSpans(p, s, dialogueTracks, d);
        for (Id id : music)
            if (Clip* c = edit::clipById(s, id)) duckClip(*c, s, spans, d);
    }
    return changed;
}

}  // namespace montage
