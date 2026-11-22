#include "AutoDuck.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "Decoder.h"
#include "core/AudioDescription.h"
#include "MediaPool.h"

namespace montage {

namespace {

constexpr int kRate = 16000;
constexpr double kWindow = 0.05;
constexpr int kBlock = 80;  // samples of a heard file's sound summed into one energy (5 ms)

// How loud a file is, as heard once: the sum of its squared samples (both channels' mean) in each 5 ms.
struct Heard {
    std::vector<float> energy;
};
std::mutex gHeardLock;
std::map<std::string, std::shared_ptr<const Heard>> gHeard;

std::shared_ptr<const Heard> heard(const std::string& key) {
    std::lock_guard l(gHeardLock);
    const auto it = gHeard.find(key);
    return it == gHeard.end() ? nullptr : it->second;
}

std::shared_ptr<const Heard> hear(const std::string& key, const AudioBuffer& buf) {
    auto h = std::make_shared<Heard>();
    const int64_t n = buf.frames();
    h->energy.assign(size_t((n + kBlock - 1) / kBlock), 0.f);
    for (int64_t k = 0; k < n; ++k) {
        const double v = 0.5 * (buf.samples[size_t(k) * 2] + buf.samples[size_t(k) * 2 + 1]);
        h->energy[size_t(k / kBlock)] += float(v * v);
    }
    std::lock_guard l(gHeardLock);
    gHeard[key] = h;
    return h;
}

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
    return dialogueSpans(p, s, tracks, o, error, cancel, false, nullptr);
}

void forgetHeardSound() {
    std::lock_guard l(gHeardLock);
    gHeard.clear();
}

Spans dialogueSpans(const Project& p, const Sequence& s, const std::vector<int>& tracks, const DuckOptions& o, std::string* error,
                    const std::atomic<bool>* cancel, bool heardOnly, bool* unheard) {
    const double fps = s.fpsValue();
    Spans words, sounds;
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
            // Otherwise loudness, 50 ms at a time, from the file's energy every 5 ms (heard once, then kept).
            const std::string key = audioKey(m->path, c.channels);
            std::shared_ptr<const Heard> sound = heard(key);
            if (!sound) {
                if (heardOnly) {
                    if (unheard) *unheard = true;
                    return {};
                }
                std::string err;
                const AudioBufferPtr buf = decodeAudio(m->path, kRate, &err, cancel, c.channels);
                if (cancel && cancel->load()) {
                    if (error) *error = "Cancelled";
                    return {};
                }
                if (!buf) {
                    if (error) *error = "Cannot read the sound of " + m->name + (err.empty() ? "" : ": " + err);
                    return {};
                }
                sound = hear(key, *buf);
            }
            const double gain = std::pow(10.0, c.audio.p("gain_db", 0) / 20.0);
            const int64_t n = int64_t(sound->energy.size());
            double runStart = -1;
            for (double t0 = cs; t0 < ce; t0 += kWindow) {
                const double local = (t0 - cs) * fps;
                const double src = c.sourceAt(local) / fps;
                const double srcEnd = c.sourceAt(std::min(local + kWindow * fps, double(c.duration))) / fps;
                int64_t s0 = int64_t(std::min(src, srcEnd) * kRate) / kBlock, s1 = (int64_t(std::max(src, srcEnd) * kRate) + kBlock - 1) / kBlock;
                s1 = std::max(s1, s0 + 1);
                double sum = 0;
                int64_t count = 0;
                for (int64_t k = std::max<int64_t>(0, s0); k < std::min(s1, n); ++k, count += kBlock) sum += double(sound->energy[size_t(k)]) * gain * gain;
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

Spans descriptionSpeech(const Project& p, const Sequence& s, int skipTrack, std::string* error, const std::atomic<bool>* cancel) {
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
    return dialogueSpans(p, s, tracks, o, error, cancel);
}

Spans clipSpans(const Sequence& s, int track, double minPause) {
    if (track < 0 || track >= int(s.audioTracks.size())) return {};
    const double fps = s.fpsValue();
    Spans spans;
    for (const Clip& c : s.audioTracks[size_t(track)].clips)
        if (c.enabled) spans.emplace_back(double(c.start) / fps, double(c.end()) / fps);
    return merge(std::move(spans), minPause);
}

namespace {

// The duck's keyframes for `c` over a volume of `base` dB: none where no span reaches it (`reached` false).
Param duckKeys(const Clip& c, const Sequence& s, const Spans& spans, const DuckOptions& o, double base, bool* reached);

}  // namespace

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
    bool reached = false;
    Param out = duckKeys(c, s, spans, o, base, &reached);
    if (!reached && own) {
        c.audio.params.erase(lane);
        return existed;
    }
    if (out == gain) return false;
    gain = out;
    return true;
}

namespace {

Param duckKeys(const Clip& c, const Sequence& s, const Spans& spans, const DuckOptions& o, double base, bool* reachedOut) {
    const double fps = s.fpsValue();
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
    if (reachedOut) *reachedOut = reached;
    if (!reached) out.keys.clear();
    // Keys between two equal neighbours add nothing.
    for (size_t i = 1; i + 1 < out.keys.size();)
        if (std::fabs(out.keys[i - 1].v - out.keys[i].v) < 1e-9 && std::fabs(out.keys[i + 1].v - out.keys[i].v) < 1e-9)
            out.keys.erase(out.keys.begin() + long(i));
        else
            ++i;
    return out;
}

}  // namespace

DuckOptions duckOptions(const DuckLink& link) {
    DuckOptions o;
    o.amountDb = link.amountDb;
    o.fadeDown = link.fadeDown;
    o.fadeUp = link.fadeUp;
    o.minPause = link.minPause;
    o.minSpeech = link.minSpeech;
    o.thresholdDb = link.thresholdDb;
    o.useTranscripts = link.useTranscripts;
    return o;
}

std::vector<int> duckTracks(const Sequence& s, const DuckLink& link) {
    std::vector<int> out;
    for (int i = 0; i < int(s.audioTracks.size()); ++i)
        if (std::find(link.tracks.begin(), link.tracks.end(), s.audioTracks[size_t(i)].id) != link.tracks.end()) out.push_back(i);
    return out;
}

bool duckClipKept(Clip& c, const Sequence& s, const Spans& spans, const DuckOptions& o, const std::vector<Id>& tracks) {
    const Clip was = c;
    Param& gain = c.audio.params["gain_db"];
    DuckLink link;
    std::vector<Keyframe> byHand;
    if (c.duck) {
        link = *c.duck;
        // The keyframes this duck wrote, found again among the clip's: where they were, or all moved by the same
        // number of frames (the clip cut or trimmed at its head since). The others were set or moved by hand.
        auto own = [&](const Keyframe& k, FrameTime shift) {
            return std::any_of(link.keys.begin(), link.keys.end(), [&](const auto& w) { return w.first + shift == k.t && std::fabs(w.second - k.v) < 1e-9; });
        };
        auto matches = [&](FrameTime shift) { return int(std::count_if(gain.keys.begin(), gain.keys.end(), [&](const Keyframe& k) { return own(k, shift); })); };
        FrameTime shift = 0;
        int best = matches(0);
        if (best < int(std::min(link.keys.size(), gain.keys.size())))
            for (const Keyframe& k : gain.keys)
                for (const auto& w : link.keys) {
                    if (std::fabs(w.second - k.v) > 1e-9 || k.t - w.first == shift) continue;
                    if (const int n = matches(k.t - w.first); n > best) best = n, shift = k.t - w.first;
                }
        for (const Keyframe& k : gain.keys)
            if (!own(k, shift)) byHand.push_back(k);
    } else {
        link.base = gain.at(0);
    }
    link.tracks = tracks;
    link.amountDb = o.amountDb;
    link.fadeDown = o.fadeDown;
    link.fadeUp = o.fadeUp;
    link.minPause = o.minPause;
    link.minSpeech = o.minSpeech;
    link.thresholdDb = o.thresholdDb;
    link.useTranscripts = o.useTranscripts;
    Param out = duckKeys(c, s, spans, o, link.base, nullptr);
    link.keys.clear();
    for (const Keyframe& k : out.keys)
        if (std::none_of(byHand.begin(), byHand.end(), [&](const Keyframe& h) { return h.t == k.t; })) link.keys.emplace_back(k.t, k.v);
    for (const Keyframe& h : byHand) {
        if (h.t < 0 || h.t >= std::max<FrameTime>(1, c.duration)) continue;
        std::erase_if(out.keys, [&](const Keyframe& k) { return k.t == h.t; });
        out.keys.push_back(h);
    }
    std::sort(out.keys.begin(), out.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
    link.kept = int(std::count_if(out.keys.begin(), out.keys.end(), [&](const Keyframe& k) {
        return std::any_of(byHand.begin(), byHand.end(), [&](const Keyframe& h) { return h.t == k.t; });
    }));
    gain = out;
    c.duck = std::move(link);
    return !(c == was);
}

bool hasKeptDucks(const Sequence& s) {
    for (const Track& t : s.audioTracks)
        for (const Clip& c : t.clips)
            if (c.duck) return true;
    return false;
}

DuckRefresh refreshKeptDucks(const Project& p, Sequence& s, const Sequence* before) {
    DuckRefresh out;
    auto trackById = [](const Sequence& in, Id id) -> const Track* {
        for (const Track& t : in.audioTracks)
            if (t.id == id) return &t;
        return nullptr;
    };
    auto clipById = [](const Sequence& in, Id id) -> const Clip* {
        for (const Track& t : in.audioTracks)
            for (const Clip& c : t.clips)
                if (c.id == id) return &c;
        return nullptr;
    };
    // (ducks with the same tracks and settings share one reading of the dialogue)
    std::vector<std::pair<DuckLink, Spans>> found;
    for (Track& t : s.audioTracks)
        for (Clip& c : t.clips) {
            if (!c.duck) continue;
            bool stale = !before;
            if (before) {
                const Clip* was = clipById(*before, c.id);
                stale = !was || !was->duck || was->start != c.start || was->duration != c.duration || was->sourceIn != c.sourceIn ||
                        was->duck->tracks != c.duck->tracks;
                for (Id id : c.duck->tracks) {
                    const Track* a = trackById(*before, id);
                    const Track* b = trackById(s, id);
                    stale |= bool(a) != bool(b) || (a && b && !(*a == *b));
                }
            }
            if (!stale) continue;
            const DuckOptions o = duckOptions(*c.duck);
            const Spans* spans = nullptr;
            for (const auto& [link, sp] : found)
                if (link.tracks == c.duck->tracks && link.thresholdDb == c.duck->thresholdDb && link.minPause == c.duck->minPause &&
                    link.minSpeech == c.duck->minSpeech && link.useTranscripts == c.duck->useTranscripts && link.fadeDown == c.duck->fadeDown &&
                    link.fadeUp == c.duck->fadeUp)
                    spans = &sp;
            if (!spans) {
                bool unheard = false;
                Spans sp = dialogueSpans(p, s, duckTracks(s, *c.duck), o, nullptr, nullptr, true, &unheard);
                if (unheard) {
                    out.unheard = true;
                    continue;
                }
                found.emplace_back(*c.duck, std::move(sp));
                spans = &found.back().second;
            }
            const std::vector<Id> tracks = c.duck->tracks;
            if (duckClipKept(c, s, *spans, o, tracks)) ++out.clips;
            out.kept += c.duck->kept;
        }
    return out;
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
