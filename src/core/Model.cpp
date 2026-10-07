#include "Model.h"

#include <algorithm>
#include <cmath>

#include "Effects.h"

namespace montage {

// ---------------------------------------------------------------------------
// Param

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

double Clip::sourceFrameAt(FrameTime t) const {
    double local = double(t - start);
    if (reverse) local = double(duration - 1) - local;
    return double(sourceIn) + local * speed;
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
