#include "AutoDuck.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "Decoder.h"
#include "core/AudioDescription.h"
#include "MediaPool.h"

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
            if (!c.enabled || std::find(o.skipRoles.begin(), o.skipRoles.end(), c.role) != o.skipRoles.end()) continue;
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
            AudioBufferPtr& buf = decoded[audioKey(m->path, c.channels)];
            std::string err;
            if (!buf) buf = decodeAudio(m->path, kRate, &err, cancel, c.channels);
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

Spans descriptionSpeech(const Project& p, const Sequence& s, int skipTrack, std::string* error) {
    std::vector<int> tracks;
    for (int i = 0; i < int(s.audioTracks.size()); ++i)
        if (i != skipTrack) tracks.push_back(i);
    DuckOptions o;
    o.minPause = 0.5;
    std::set<std::string> roles;
    for (const Track& t : s.audioTracks)
        for (const Clip& c : t.clips) roles.insert(c.role);
    if (roles.count("Dialogue")) {
        for (const std::string& r : roles)
            if (r != "Dialogue") o.skipRoles.push_back(r);
    } else {
        o.skipRoles = {"Music", "Effects", kDescriptionRole};
    }
    return dialogueSpans(p, s, tracks, o, error);
}

Spans clipSpans(const Sequence& s, int track, double minPause) {
    if (track < 0 || track >= int(s.audioTracks.size())) return {};
    const double fps = s.fpsValue();
    Spans spans;
    for (const Clip& c : s.audioTracks[size_t(track)].clips)
        if (c.enabled) spans.emplace_back(double(c.start) / fps, double(c.end()) / fps);
    return merge(std::move(spans), minPause);
}

bool duckClip(Clip& c, const Sequence& s, const Spans& spans, const DuckOptions& o, const std::string& lane) {
    const double fps = s.fpsValue();
    const bool own = lane != "gain_db", existed = c.audio.params.count(lane) > 0;
    if (own && !existed) {
        // Only a clip a span reaches gets the lane.
        const double cs = double(c.start) / fps, ce = double(c.end()) / fps;
        if (std::none_of(spans.begin(), spans.end(), [&](const auto& sp) { return sp.first - o.fadeDown < ce && sp.second + o.fadeUp > cs; }))
            return false;
    }
    Param& gain = c.audio.params[lane];
    const double base = own ? 0.0 : gain.at(0);
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
    if (!reached) {
        if (own) {
            c.audio.params.erase(lane);
            return existed;
        }
        out.keys.clear();
    }
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

int duckUnderDescriptions(Sequence& s, double amountDb, double fadeSeconds) {
    const double fps = s.fpsValue();
    Spans spans;
    for (const Track& t : s.audioTracks)
        for (const Clip& c : t.clips)
            if (c.enabled && c.role == kDescriptionRole) spans.emplace_back(double(c.start) / fps, double(c.end()) / fps);
    std::sort(spans.begin(), spans.end());
    spans = merge(std::move(spans), 2 * fadeSeconds);
    DuckOptions o;
    o.amountDb = amountDb;
    o.fadeDown = o.fadeUp = fadeSeconds;
    int changed = 0;
    for (Track& t : s.audioTracks)
        for (Clip& c : t.clips)
            if (c.role != kDescriptionRole && duckClip(c, s, spans, o, kDescriptionDuckParam)) ++changed;
    return changed;
}

}  // namespace montage
