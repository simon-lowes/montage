// Montage — takes on lanes (core/TakeLanes.h).
#include "TakeLanes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <tuple>

#include "core/Effects.h"

namespace montage {

std::vector<LoopPass> loopPasses(int64_t recorded, int64_t preroll, double length, int64_t minPartial) {
    std::vector<LoopPass> out;
    if (!(length >= 1)) return out;
    preroll = std::max<int64_t>(0, preroll);
    for (int64_t k = 0;; ++k) {
        const int64_t first = preroll + std::llround(double(k) * length), next = preroll + std::llround(double(k + 1) * length);
        if (first >= recorded) break;
        if (next <= recorded) out.push_back({first, next - first, true});
        else if (minPartial > 0 && recorded - first >= minPartial) out.push_back({first, recorded - first, false});
    }
    return out;
}

namespace {

std::vector<std::string> lineWords(const std::string& text) {
    std::vector<std::string> out;
    std::string w;
    auto flush = [&] {
        if (!w.empty()) out.push_back(std::move(w));
        w.clear();
    };
    for (const unsigned char ch : text) {
        if (std::isalnum(ch) || ch >= 0x80) w += char(std::tolower(ch));
        else if (ch == '\'' && !w.empty()) continue;  // (don't = dont)
        else flush();
    }
    flush();
    return out;
}

}  // namespace

int lineWordErrors(const std::string& said, const std::string& line) {
    const std::vector<std::string> a = lineWords(said), b = lineWords(line);
    std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) prev[j] = int(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        cur[0] = int(i);
        for (size_t j = 1; j <= b.size(); ++j) cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

int suggestTake(const std::vector<TakeCandidate>& takes, const std::string& line, std::string* why) {
    if (takes.empty()) return -1;
    const bool byWords = !lineWords(line).empty() && std::any_of(takes.begin(), takes.end(), [](const TakeCandidate& t) { return t.transcribed; });
    // Lower is better, in order: word errors, clipping, too quiet, rating (higher), later.
    auto key = [&](size_t i) {
        const TakeCandidate& t = takes[i];
        const int errors = byWords ? (t.transcribed ? lineWordErrors(t.said, line) : 1 << 20) : 0;
        return std::make_tuple(errors, t.clippedSeconds > 0.001 ? 1 : 0, t.rmsDb < -40 ? 1 : 0, -t.rating, -int(i));
    };
    size_t best = 0;
    for (size_t i = 1; i < takes.size(); ++i)
        if (key(i) < key(best)) best = i;
    if (why) {
        const TakeCandidate& t = takes[best];
        std::string w = t.name.empty() ? "This take" : t.name;
        if (byWords) {
            const int e = std::get<0>(key(best));
            w += e == 0 ? " says the line word for word" : " is closest to the line (" + std::to_string(e) + (e == 1 ? " word" : " words") + " off)";
        } else {
            w += " sounds best (no transcripts to compare with the line)";
        }
        if (t.clippedSeconds > 0.001) w += "; it clips";
        else if (t.rmsDb < -40) w += "; it is quiet";
        *why = w;
    }
    return int(best);
}

std::vector<LaneView> laneViews(const Project& p, const Sequence& s, TrackRef track) {
    std::vector<LaneView> out;
    const Track* t = trackAt(s, track);
    if (!t) return out;
    for (const TakeLane& l : t->lanes) {
        LaneView v;
        v.lane = l.id;
        v.name = l.name;
        v.mediaId = l.mediaId;
        v.start = l.start;
        const MediaItem* m = p.findMedia(l.mediaId);
        v.end = l.start + (m ? std::min<FrameTime>(mediaFrames(*m, s), kInfiniteFrames / 2) : 0);
        v.rating = l.rating;
        out.push_back(v);
    }
    for (const Clip& c : t->clips)
        for (size_t k = 0; k < c.takes.size(); ++k) {
            if (int(k) == c.take) continue;
            LaneView v;
            v.clip = c.id;
            v.take = int(k);
            v.mediaId = c.takes[k].mediaId;
            const MediaItem* m = p.findMedia(v.mediaId);
            v.name = !c.takes[k].name.empty() ? c.takes[k].name : m ? m->name : std::string();
            v.start = c.start;
            v.end = c.end();
            out.push_back(v);
        }
    return out;
}

Id compLaneOf(const Track& t, const Clip& c) {
    if (c.reverse || std::fabs(c.speed - 1) > 1e-9 || c.ramped()) return 0;
    for (const TakeLane& l : t.lanes)
        if (l.mediaId == c.mediaId && std::fabs(double(c.start) - c.sourceIn - double(l.start)) < 1e-6) return l.id;
    return 0;
}

namespace edit {

namespace {

Track* audioTrack(Sequence& s, TrackRef track, std::string& why) {
    Track* t = track.kind == TrackKind::Audio ? trackAt(s, track) : nullptr;
    if (!t) why = "Take lanes are on audio tracks";
    else if (t->locked) why = "Track is locked", t = nullptr;
    return t;
}

// The clips either side of the join at `at`, or false.
bool joinAt(Track& t, FrameTime at, Clip*& left, Clip*& right) {
    left = right = nullptr;
    for (Clip& c : t.clips) {
        if (c.end() == at) left = &c;
        if (c.start == at) right = &c;
    }
    return left && right;
}

// The crossfade joining clips a and b: `seconds` long (none at 0), replacing any transition or fade at that join.
// Centred on the cut, it is at most twice the shorter clip; none where b carries on a's sound (the same media, its
// next frame), since a crossfade of a sound into itself only bumps its level.
void setJoin(Project& p, Track& t, Id a, Id b, double seconds, double fps) {
    const Clip *ca = nullptr, *cb = nullptr;
    for (const Clip& c : t.clips) {
        if (c.id == a) ca = &c;
        if (c.id == b) cb = &c;
    }
    if (ca && cb) {
        seconds = std::min(seconds, 2.0 * double(std::min(ca->duration, cb->duration)) / fps);
        if (ca->mediaId == cb->mediaId && compLaneOf(t, *ca) && compLaneOf(t, *cb) == compLaneOf(t, *ca)) seconds = 0;
    }
    std::erase_if(t.transitions, [&](const Transition& tr) {
        return (tr.clipA == a && (tr.clipB == b || tr.clipB == 0)) || (tr.clipA == 0 && tr.clipB == b);
    });
    if (seconds > 0) {
        Transition tr;
        tr.id = p.newId();
        tr.type = "crossfade";
        tr.clipA = a;
        tr.clipB = b;
        tr.duration = 2;  // (what the timeline draws; the sound fades over `seconds`)
        tr.params = makeEffect(tr.type, p.newId());
        tr.params.params["seconds"] = Param(seconds);
        t.transitions.push_back(std::move(tr));
    }
    normalize(t);
}

}  // namespace

const TakeLane* findTakeLane(const Track& t, Id lane) {
    for (const TakeLane& l : t.lanes)
        if (l.id == lane) return &l;
    return nullptr;
}

Result addTakeLane(Project& p, Sequence& s, TrackRef track, Id media, FrameTime start, const std::string& name) {
    std::string why;
    Track* t = audioTrack(s, track, why);
    if (!t) return Result::fail(why);
    const MediaItem* m = p.findMedia(media);
    if (!m || !m->hasAudio) return Result::fail("A take lane needs media with sound");
    TakeLane l;
    l.id = p.newId();
    l.mediaId = media;
    l.start = start;
    l.name = name;
    if (l.name.empty()) {
        int n = 0;
        for (const TakeLane& o : t->lanes)
            if (o.name.rfind("Take ", 0) == 0) n = std::max(n, std::atoi(o.name.c_str() + 5));
        l.name = "Take " + std::to_string(std::max<int>(n, 0) + 1);
    }
    t->lanes.push_back(l);
    Result r;
    r.created.push_back(l.id);
    return r;
}

Result renameTakeLane(Sequence& s, TrackRef track, Id lane, const std::string& name) {
    std::string why;
    Track* t = audioTrack(s, track, why);
    if (!t) return Result::fail(why);
    for (TakeLane& l : t->lanes)
        if (l.id == lane) {
            if (name.empty()) return Result::fail("A take needs a name");
            l.name = name;
            return {};
        }
    return Result::fail("No such take lane");
}

Result rateTakeLane(Sequence& s, TrackRef track, Id lane, int rating) {
    std::string why;
    Track* t = audioTrack(s, track, why);
    if (!t) return Result::fail(why);
    for (TakeLane& l : t->lanes)
        if (l.id == lane) {
            l.rating = std::clamp(rating, 0, 5);
            return {};
        }
    return Result::fail("No such take lane");
}

Result removeTakeLane(Sequence& s, TrackRef track, Id lane) {
    std::string why;
    Track* t = audioTrack(s, track, why);
    if (!t) return Result::fail(why);
    const size_t before = t->lanes.size();
    std::erase_if(t->lanes, [&](const TakeLane& l) { return l.id == lane; });
    return t->lanes.size() < before ? Result{} : Result::fail("No such take lane");
}

Result showLanes(Sequence& s, TrackRef track, bool shown) {
    Track* t = trackAt(s, track);
    if (!t) return Result::fail("No such track");
    t->lanesShown = shown;
    return {};
}

Result compTake(Project& p, Sequence& s, TrackRef track, Id lane, FrameTime from, FrameTime to, double crossfade) {
    std::string why;
    Track* t = audioTrack(s, track, why);
    if (!t) return Result::fail(why);
    const TakeLane* l = findTakeLane(*t, lane);
    if (!l) return Result::fail("No such take lane");
    if (!(crossfade >= 0 && crossfade <= 10)) return Result::fail("A crossfade is 0 to 10 seconds");
    const MediaItem* m = p.findMedia(l->mediaId);
    if (!m) return Result::fail("The take's media is missing");
    // (A pass of a loop recording between whole samples may end a hair before its last frame does.)
    const FrameTime length = std::max(mediaFrames(*m, s), FrameTime(std::ceil(m->duration * s.fpsValue() - 0.01)));
    if (to <= from || from < l->start || to > l->start + length)
        return Result::fail("That range is outside the take (" + l->name + ")");
    Clip c = makeClip(p, *m, TrackKind::Audio, s);
    c.name = l->name;
    c.start = from;
    c.duration = to - from;
    c.sourceIn = double(from - l->start);
    const Id id = c.id;
    Result r = overwrite(p, s, track, std::move(c));
    if (!r.ok) return r;
    t = trackAt(s, track);
    Clip *left = nullptr, *right = nullptr, *self = nullptr;
    for (Clip& o : t->clips) {
        if (o.id == id) self = &o;
        else if (o.end() == from) left = &o;
        else if (o.start == to) right = &o;
    }
    if (!self) return Result::fail("The comp could not be placed");
    const Id leftId = left ? left->id : 0, rightId = right ? right->id : 0;
    if (leftId) setJoin(p, *t, leftId, id, crossfade, s.fpsValue());
    if (rightId) setJoin(p, *t, id, rightId, crossfade, s.fpsValue());
    Result out;
    out.created.push_back(id);
    return out;
}

Result addLoopTakes(Project& p, Sequence& s, TrackRef track, const std::vector<Id>& media, const std::vector<bool>& complete,
                    FrameTime in, FrameTime out) {
    if (media.empty()) return Result::fail("No passes to add");
    Result all;
    Id compLane = 0;
    for (size_t k = 0; k < media.size(); ++k) {
        Result r = addTakeLane(p, s, track, media[k], in);
        if (!r.ok) return r;
        all.created.push_back(r.created.front());
        if (k >= complete.size() || complete[k]) compLane = r.created.front();
    }
    if (compLane && out > in) {
        Result r = compTake(p, s, track, compLane, in, out);
        if (!r.ok) return r;
        all.created.push_back(r.created.front());
    }
    return all;
}

Result moveCompJoin(Project& p, Sequence& s, TrackRef track, FrameTime at, FrameTime delta) {
    std::string why;
    Track* t = audioTrack(s, track, why);
    if (!t) return Result::fail(why);
    Clip *left, *right;
    if (!joinAt(*t, at, left, right)) return Result::fail("No join between two clips there");
    return roll(p, s, left->id, right->id, delta);
}

Result setCompCrossfade(Project& p, Sequence& s, TrackRef track, FrameTime at, double seconds) {
    std::string why;
    Track* t = audioTrack(s, track, why);
    if (!t) return Result::fail(why);
    Clip *left, *right;
    if (!joinAt(*t, at, left, right)) return Result::fail("No join between two clips there");
    if (seconds < 0 || seconds > 10) return Result::fail("A crossfade is 0 to 10 seconds");
    setJoin(p, *t, left->id, right->id, seconds, s.fpsValue());
    return {};
}

Result newTrackVersion(Project& p, Sequence& s, TrackRef track, const std::string& name, bool copy) {
    Track* t = trackAt(s, track);
    if (!t) return Result::fail("No such track");
    if (t->locked) return Result::fail("Track is locked");
    if (t->versions.empty()) {
        t->versions.push_back({"Version 1", {}, {}});
        t->version = 0;
    }
    TrackVersion& shown = t->versions[size_t(t->version)];
    shown.clips = t->clips;
    shown.transitions = t->transitions;
    // The new one: a copy under new ids (its transitions following them), or nothing.
    std::vector<Clip> clips;
    std::vector<Transition> transitions;
    if (copy) {
        std::map<Id, Id> ids;
        for (const Clip& c : t->clips) {
            Clip d = c;
            d.id = p.newId();
            ids[c.id] = d.id;
            clips.push_back(std::move(d));
        }
        for (const Transition& tr : t->transitions) {
            Transition d = tr;
            d.id = p.newId();
            d.clipA = tr.clipA ? ids[tr.clipA] : 0;
            d.clipB = tr.clipB ? ids[tr.clipB] : 0;
            transitions.push_back(std::move(d));
        }
    }
    TrackVersion fresh;
    fresh.name = name.empty() ? "Version " + std::to_string(t->versions.size() + 1) : name;
    t->versions.push_back(std::move(fresh));
    t->version = int(t->versions.size()) - 1;
    t->clips = std::move(clips);
    t->transitions = std::move(transitions);
    normalize(*t);
    return {};
}

Result switchTrackVersion(Sequence& s, TrackRef track, int index) {
    Track* t = trackAt(s, track);
    if (!t) return Result::fail("No such track");
    if (t->locked) return Result::fail("Track is locked");
    if (index < 0 || index >= int(t->versions.size())) return Result::fail("No such version");
    if (index == t->version) return {};
    TrackVersion& shown = t->versions[size_t(t->version)];
    shown.clips = std::move(t->clips);
    shown.transitions = std::move(t->transitions);
    TrackVersion& next = t->versions[size_t(index)];
    t->clips = std::move(next.clips);
    t->transitions = std::move(next.transitions);
    next.clips.clear();
    next.transitions.clear();
    t->version = index;
    normalize(*t);
    return {};
}

Result renameTrackVersion(Sequence& s, TrackRef track, int index, const std::string& name) {
    Track* t = trackAt(s, track);
    if (!t) return Result::fail("No such track");
    if (index < 0 || index >= int(t->versions.size())) return Result::fail("No such version");
    if (name.empty()) return Result::fail("A version needs a name");
    t->versions[size_t(index)].name = name;
    return {};
}

Result deleteTrackVersion(Sequence& s, TrackRef track, int index) {
    Track* t = trackAt(s, track);
    if (!t) return Result::fail("No such track");
    if (t->locked) return Result::fail("Track is locked");
    if (index < 0 || index >= int(t->versions.size())) return Result::fail("No such version");
    if (index == t->version) return Result::fail("Switch to another version before deleting this one");
    t->versions.erase(t->versions.begin() + index);
    if (t->version > index) --t->version;
    if (t->versions.size() == 1) t->versions.clear(), t->version = 0;
    return {};
}

}  // namespace edit
}  // namespace montage
