#include "Model.h"

#include <algorithm>
#include <cmath>

#include "Effects.h"

namespace montage {

// ---------------------------------------------------------------------------
// Param

void keyHandles(const std::vector<Keyframe>& keys, size_t i, double& inDt, double& inDv, double& outDt, double& outDv) {
    inDt = inDv = outDt = outDv = 0;
    if (i >= keys.size()) return;
    const Keyframe& k = keys[i];
    // Automatic: the slope through the neighbours, flat where the curve turns and at the ends.
    double slope = 0;
    if (i > 0 && i + 1 < keys.size()) {
        const Keyframe& a = keys[i - 1];
        const Keyframe& b = keys[i + 1];
        if ((k.v - a.v) * (b.v - k.v) > 0 && b.t > a.t) slope = (b.v - a.v) / double(b.t - a.t);
    }
    if (k.inDt != 0 || k.inDv != 0) {
        inDt = k.inDt, inDv = k.inDv;
    } else if (i > 0) {
        inDt = -double(k.t - keys[i - 1].t) / 3;
        inDv = slope * inDt;
    }
    if (k.outDt != 0 || k.outDv != 0) {
        outDt = k.outDt, outDv = k.outDv;
    } else if (i + 1 < keys.size()) {
        outDt = double(keys[i + 1].t - k.t) / 3;
        outDv = slope * outDt;
    }
}

double Param::at(FrameTime t) const {
    if (keys.empty()) return value;
    if (t <= keys.front().t) return keys.front().v;
    if (t >= keys.back().t) return keys.back().v;
    auto it = std::upper_bound(keys.begin(), keys.end(), t,
                               [](FrameTime tt, const Keyframe& k) { return tt < k.t; });
    const Keyframe& b = *it;
    const Keyframe& a = *(it - 1);
    if (a.interp == Interp::Hold || b.t == a.t) return a.v;
    double u = double(t - a.t) / double(b.t - a.t);
    if (a.interp == Interp::Bezier) {
        // A cubic from a to b through a's out handle and b's in handle, their times kept
        // inside the segment so the curve is a function of time; solved for t by bisection.
        const size_t ia = size_t(it - keys.begin()) - 1;
        double ai, av, aOutDt, aOutDv, bInDt, bInDv, bo1, bo2;
        keyHandles(keys, ia, ai, av, aOutDt, aOutDv);
        keyHandles(keys, ia + 1, bInDt, bInDv, bo1, bo2);
        const double seg = double(b.t - a.t);
        const double x1 = std::clamp(aOutDt, 0.0, seg) / seg, x2 = 1 + std::clamp(bInDt, -seg, 0.0) / seg;
        const double y0 = a.v, y1 = a.v + aOutDv, y2 = b.v + bInDv, y3 = b.v;
        auto bez = [](double p0, double p1, double p2, double p3, double s) {
            const double r = 1 - s;
            return r * r * r * p0 + 3 * r * r * s * p1 + 3 * r * s * s * p2 + s * s * s * p3;
        };
        double lo = 0, hi = 1;
        for (int k = 0; k < 50; ++k) {
            const double mid = 0.5 * (lo + hi);
            (bez(0, x1, x2, 1, mid) < u ? lo : hi) = mid;
        }
        return bez(y0, y1, y2, y3, 0.5 * (lo + hi));
    }
    if (a.interp == Interp::Smooth) u = u * u * (3.0 - 2.0 * u);
    return a.v + (b.v - a.v) * u;
}

void Param::addKey(FrameTime t, double v, Interp interp) {
    auto it = std::lower_bound(keys.begin(), keys.end(), t,
                               [](const Keyframe& k, FrameTime tt) { return k.t < tt; });
    if (it != keys.end() && it->t == t) {
        it->v = v;
        it->interp = interp;
    } else {
        keys.insert(it, Keyframe{t, v, interp});
    }
}

void Param::set(FrameTime t, double v) {
    if (keys.empty()) {
        value = v;
        return;
    }
    Interp interp = Interp::Linear;
    if (const Keyframe* k = keyAt(t)) interp = k->interp;
    addKey(t, v, interp);
}

bool Param::removeKey(FrameTime t) {
    auto it = std::find_if(keys.begin(), keys.end(), [t](const Keyframe& k) { return k.t == t; });
    if (it == keys.end()) return false;
    if (keys.size() == 1) value = it->v;
    keys.erase(it);
    return true;
}

const Keyframe* Param::keyAt(FrameTime t) const {
    for (const auto& k : keys)
        if (k.t == t) return &k;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Effect

bool Effect::operator==(const Effect& o) const {
    if (id != o.id || type != o.type || enabled != o.enabled || params != o.params || strings != o.strings) return false;
    if (object == o.object) return true;
    return object && o.object && *object == *o.object;
}

double Effect::p(const std::string& name, FrameTime t, double def) const {
    auto it = params.find(name);
    if (it == params.end()) return def;
    return it->second.at(t);
}

std::string Effect::s(const std::string& name, const std::string& def) const {
    auto it = strings.find(name);
    return it == strings.end() ? def : it->second;
}

// ---------------------------------------------------------------------------
// Clip / Sequence / Project

namespace {
// The integral of a parameter's curve from x0 to x1 (continuous form of Param::at).
double integrate(const Param& p, double x0, double x1) {
    const auto& k = p.keys;
    if (k.empty()) return p.value * (x1 - x0);
    // Antiderivative with G(first key) = 0.
    auto G = [&](double x) {
        if (x <= double(k.front().t)) return k.front().v * (x - double(k.front().t));
        double cum = 0;
        for (size_t i = 0; i + 1 < k.size(); ++i) {
            const Keyframe& a = k[i];
            const Keyframe& b = k[i + 1];
            const double dt = double(b.t - a.t);
            if (dt <= 0) continue;
            const double u = std::min(1.0, (x - double(a.t)) / dt);
            double part;
            if (a.interp == Interp::Hold) part = a.v * u;
            else if (a.interp == Interp::Smooth) part = a.v * u + (b.v - a.v) * (u * u * u - u * u * u * u / 2);
            else part = a.v * u + (b.v - a.v) * u * u / 2;
            if (x <= double(b.t)) return cum + part * dt;
            cum += part * dt;
        }
        return cum + k.back().v * (x - double(k.back().t));
    };
    return G(x1) - G(x0);
}
}  // namespace

bool Clip::ramped() const {
    if (reverse || timing.empty()) return false;
    auto it = timing.params.find("speed");
    return it != timing.params.end() && (it->second.animated() || it->second.value != 100);
}

double Clip::speedAt(double local) const {
    if (!ramped()) return speed;
    const Param& p = timing.params.at("speed");
    if (!p.animated()) return speed * p.value / 100;
    return speed * std::max(0.0, integrate(p, local, local + 1e-6) / 1e-6) / 100;
}

double Clip::sourceOffset(double local) const {
    if (!ramped()) return local * speed;
    return speed * integrate(timing.params.at("speed"), 0, local) / 100;
}

double Clip::sourceAt(double local) const {
    if (reverse) return double(sourceIn) + (double(duration - 1) - local) * speed;
    return double(sourceIn) + sourceOffset(local);
}

double Clip::sourceFrameAt(FrameTime t) const { return sourceAt(double(t - start)); }

double Clip::localForSource(double source) const {
    if (reverse) return double(duration - 1) - (source - sourceIn) / speed;
    const double want = source - sourceIn;
    if (!ramped()) return want / speed;
    // The offset grows with time (speeds are positive): bisect.
    double lo = 0, hi = 1;
    if (want < 0) {
        lo = want / std::max(1e-6, speedAt(0));
        hi = 0;
    } else {
        while (sourceOffset(hi) < want && hi < 1e9) hi *= 2;
    }
    for (int i = 0; i < 60; ++i) {
        const double mid = (lo + hi) / 2;
        (sourceOffset(mid) < want ? lo : hi) = mid;
    }
    return (lo + hi) / 2;
}

FrameTime Sequence::duration() const {
    FrameTime d = 0;
    for (const auto* list : {&videoTracks, &audioTracks})
        for (const auto& tr : *list)
            if (!tr.clips.empty()) d = std::max(d, tr.clips.back().end());
    return d;
}

MediaItem* Project::findMedia(Id id) {
    for (auto& m : media)
        if (m.id == id) return &m;
    return nullptr;
}
const MediaItem* Project::findMedia(Id id) const { return const_cast<Project*>(this)->findMedia(id); }

Sequence* Project::findSequence(Id id) {
    for (auto& s : sequences)
        if (s.id == id) return &s;
    return nullptr;
}
const Sequence* Project::findSequence(Id id) const { return const_cast<Project*>(this)->findSequence(id); }

Track* trackAt(Sequence& s, TrackRef r) {
    auto& list = r.kind == TrackKind::Video ? s.videoTracks : s.audioTracks;
    if (r.index < 0 || r.index >= int(list.size())) return nullptr;
    return &list[size_t(r.index)];
}

const Track* trackAt(const Sequence& s, TrackRef r) { return trackAt(const_cast<Sequence&>(s), r); }

std::vector<TrackRef> allTracks(const Sequence& s) {
    std::vector<TrackRef> out;
    for (int i = 0; i < int(s.videoTracks.size()); ++i) out.push_back({TrackKind::Video, i});
    for (int i = 0; i < int(s.audioTracks.size()); ++i) out.push_back({TrackKind::Audio, i});
    return out;
}

Track makeTrack(Project& p, TrackKind kind, const std::string& name) {
    Track t;
    t.id = p.newId();
    t.kind = kind;
    t.name = name;
    return t;
}

Sequence makeSequence(Project& p, const std::string& name, int w, int h, Rational fps, int videoTracks,
                      int audioTracks) {
    Sequence s;
    s.id = p.newId();
    s.name = name;
    s.width = w;
    s.height = h;
    s.fps = fps;
    for (int i = 0; i < videoTracks; ++i)
        s.videoTracks.push_back(makeTrack(p, TrackKind::Video, "V" + std::to_string(i + 1)));
    for (int i = 0; i < audioTracks; ++i)
        s.audioTracks.push_back(makeTrack(p, TrackKind::Audio, "A" + std::to_string(i + 1)));
    return s;
}

FrameTime mediaFrames(const MediaItem& m, const Sequence& seq) {
    if (m.kind == MediaKind::Image) return kInfiniteFrames;
    if (m.kind == MediaKind::Sequence) return kInfiniteFrames;  // resolved by callers that know the project
    return std::max<FrameTime>(1, FrameTime(std::floor(m.duration * seq.fpsValue() + 1e-6)));
}

Clip makeClip(Project& p, const MediaItem& media, TrackKind kind, const Sequence& seq) {
    Clip c;
    c.id = p.newId();
    c.mediaId = media.id;
    c.name = media.name;
    FrameTime len = mediaFrames(media, seq);
    if (media.kind == MediaKind::Sequence) {
        if (const Sequence* nested = p.findSequence(media.sequenceId)) len = std::max<FrameTime>(1, nested->duration());
    }
    if (len >= kInfiniteFrames) len = FrameTime(std::llround(5.0 * seq.fpsValue()));  // stills default to 5 s
    c.duration = len;
    c.motion = makeEffect(p, "transform");
    c.audio = makeEffect(p, "volume");
    c.timing = makeEffect(p, "time");
    (void)kind;
    return c;
}

Clip makeGeneratorClip(Project& p, const std::string& generatorType, FrameTime duration) {
    Clip c;
    c.id = p.newId();
    c.generator = makeEffect(p, generatorType);
    if (const EffectInfo* info = findEffectInfo(generatorType)) c.name = info->displayName;
    c.duration = std::max<FrameTime>(1, duration);
    c.motion = makeEffect(p, "transform");
    c.audio = makeEffect(p, "volume");
    c.timing = makeEffect(p, "time");
    return c;
}

Project makeDefaultProject() {
    Project p;
    Sequence s = makeSequence(p, "Sequence 1", 1920, 1080, Rational{30, 1});
    p.activeSequence = s.id;
    p.sequences.push_back(std::move(s));
    return p;
}

}  // namespace montage
