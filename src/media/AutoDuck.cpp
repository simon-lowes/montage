#include "AutoDuck.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "Decoder.h"

namespace montage {

namespace {

constexpr int kRate = 16000;
constexpr double kWindow = 0.05;

// Joins spans closer than `gap`, after sorting.
Spans merge(Spans spans, double gap) {
    std::sort(spans.begin(), spans.end());
    Spans out;
    for (const auto& sp : spans) {
        if (!out.empty() && sp.first - out.back().second < gap) out.back().second = std::max(out.back().second, sp.second);
        else out.push_back(sp);
    }
    return out;
}

}  // namespace

Spans dialogueSpans(const Project& p, const Sequence& s, const std::vector<int>& tracks, const DuckOptions& o, std::string* error,
                    const std::atomic<bool>* cancel) {
    const double fps = s.fpsValue();
    Spans words, sounds;
    std::map<std::string, AudioBufferPtr> decoded;
    for (int ti : tracks) {
        if (ti < 0 || ti >= int(s.audioTracks.size())) continue;
        const Track& t = s.audioTracks[size_t(ti)];
        if (t.muted) continue;
        for (const Clip& c : t.clips) {
            if (!c.enabled) continue;
            const MediaItem* m = p.findMedia(c.mediaId);
            if (!m || !m->hasAudio || m->path.empty()) continue;
            const double cs = double(c.start) / fps, ce = double(c.end()) / fps;
            // Words, where the media is transcribed.
            if (o.useTranscripts && m->transcript && !m->transcript->empty()) {
                for (const TranscriptSegment& seg : m->transcript->segments)
                    for (const TranscriptWord& w : seg.words) {
                        double a = (double(c.start) + c.localForSource(w.start * fps)) / fps;
                        double b = (double(c.start) + c.localForSource(w.end * fps)) / fps;
                        if (a > b) std::swap(a, b);
                        a = std::max(a, cs);
                        b = std::min(b, ce);
                        if (b > a) words.emplace_back(a, b);
                    }
                continue;
            }
            // Otherwise loudness, 50 ms at a time.
            AudioBufferPtr& buf = decoded[m->path];
            std::string err;
            if (!buf) buf = decodeAudio(m->path, kRate, &err, cancel);
            if (cancel && cancel->load()) {
                if (error) *error = "Cancelled";
                return {};
            }
            if (!buf) {
                if (error) *error = "Cannot read the sound of " + m->name + (err.empty() ? "" : ": " + err);
                return {};
            }
            const double gain = std::pow(10.0, c.audio.p("gain_db", 0) / 20.0);
            const int64_t n = buf->frames();
            double runStart = -1;
            for (double t0 = cs; t0 < ce; t0 += kWindow) {
                const double local = (t0 - cs) * fps;
                const double src = c.sourceAt(local) / fps;
                const double srcEnd = c.sourceAt(std::min(local + kWindow * fps, double(c.duration))) / fps;
                int64_t s0 = int64_t(std::min(src, srcEnd) * kRate), s1 = int64_t(std::max(src, srcEnd) * kRate);
                s1 = std::max(s1, s0 + 1);
                double sum = 0;
                int64_t count = 0;
                for (int64_t k = std::max<int64_t>(0, s0); k < std::min(s1, n); ++k, ++count) {
                    const double v = 0.5 * (buf->samples[size_t(k) * 2] + buf->samples[size_t(k) * 2 + 1]) * gain;
                    sum += v * v;
                }
                const bool loud = count > 0 && 10 * std::log10(sum / double(count) + 1e-12) > o.thresholdDb;
                if (loud && runStart < 0) runStart = t0;
                if (!loud && runStart >= 0) {
                    sounds.emplace_back(runStart, t0);
                    runStart = -1;
                }
            }
            if (runStart >= 0) sounds.emplace_back(runStart, ce);
        }
    }
    // Blips are not speech; syllables a breath apart are one sound.
    Spans speech = words;
    for (const auto& sp : merge(sounds, 0.15))
        if (sp.second - sp.first >= o.minSpeech) speech.push_back(sp);
    // Pauses too short for the music to come back up stay ducked.
    return merge(speech, std::max(o.minPause, o.fadeDown + o.fadeUp));
}

bool duckClip(Clip& c, const Sequence& s, const Spans& spans, const DuckOptions& o) {
    const double fps = s.fpsValue();
    Param& gain = c.audio.params["gain_db"];
    const double base = gain.at(0);
    const double cs = double(c.start) / fps, ce = double(c.end()) / fps;
    // How far down the music is (0 to 1): spans are far enough apart that the fades never overlap.
    auto depth = [&](double t) {
        double d = 0;
        for (const auto& [a, b] : spans) {
            if (t >= a && t <= b) return 1.0;
            if (t < a && t > a - o.fadeDown) d = std::max(d, 1 - (a - t) / std::max(1e-6, o.fadeDown));
            if (t > b && t < b + o.fadeUp) d = std::max(d, 1 - (t - b) / std::max(1e-6, o.fadeUp));
        }
        return d;
    };
    std::vector<double> times{cs, ce - 1 / fps};
    for (const auto& [a, b] : spans)
        for (double t : {a - o.fadeDown, a, b, b + o.fadeUp})
            if (t > cs && t < ce - 1 / fps) times.push_back(t);
    std::sort(times.begin(), times.end());
    Param out(base);
    bool reached = false;
    for (double t : times) {
        const FrameTime local = std::clamp<FrameTime>(FrameTime(std::llround(t * fps)) - c.start, 0, std::max<FrameTime>(0, c.duration - 1));
        const double d = depth(t);
        reached |= d > 0;
        out.addKey(local, base + o.amountDb * d);
    }
    if (!reached) out.keys.clear();
    // Keys between two equal neighbours add nothing.
    for (size_t i = 1; i + 1 < out.keys.size();)
        if (std::fabs(out.keys[i - 1].v - out.keys[i].v) < 1e-9 && std::fabs(out.keys[i + 1].v - out.keys[i].v) < 1e-9)
            out.keys.erase(out.keys.begin() + long(i));
        else
            ++i;
    if (out == gain) return false;
    gain = out;
    return true;
}

}  // namespace montage
