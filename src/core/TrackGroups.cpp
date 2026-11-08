// Montage — track groups, group comping and track colours (core/TrackGroups.h).
#include "TrackGroups.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

#include "core/Automation.h"
#include "core/TakeLanes.h"

namespace montage {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool switchOn(const TrackGroup& g, GroupSwitch sw) {
    switch (sw) {
    case GroupSwitch::Editing: return g.editing;
    case GroupSwitch::Mute: return g.mute;
    case GroupSwitch::Volume: return g.volume;
    case GroupSwitch::Pan: return g.pan;
    case GroupSwitch::Automation: return g.automation;
    case GroupSwitch::Record: return g.record;
    case GroupSwitch::Comp: return g.comp;
    }
    return false;
}

}  // namespace

const TrackGroup* findTrackGroup(const Sequence& s, Id group) {
    if (!group) return nullptr;
    for (const TrackGroup& g : s.trackGroups)
        if (g.id == group) return &g;
    return nullptr;
}

TrackGroup* findTrackGroup(Sequence& s, Id group) { return const_cast<TrackGroup*>(findTrackGroup(std::as_const(s), group)); }

const TrackGroup* findTrackGroup(const Sequence& s, const std::string& name) {
    const std::string want = lower(name);
    for (const TrackGroup& g : s.trackGroups)
        if (lower(g.name) == want) return &g;
    return nullptr;
}

const TrackGroup* activeGroup(const Sequence& s, const Track& t, GroupSwitch sw) {
    if (s.groupsSuspended) return nullptr;
    const TrackGroup* g = findTrackGroup(s, t.group);
    return g && !g->suspended && switchOn(*g, sw) ? g : nullptr;
}

std::vector<TrackRef> groupMembers(const Sequence& s, Id group) {
    std::vector<TrackRef> out;
    if (!group) return out;
    for (TrackRef r : allTracks(s))
        if (trackAt(s, r)->group == group) out.push_back(r);
    return out;
}

std::vector<TrackRef> groupTracks(const Sequence& s, TrackRef track, GroupSwitch sw) {
    const Track* t = trackAt(s, track);
    if (!t) return {};
    std::vector<TrackRef> out{track};
    if (const TrackGroup* g = activeGroup(s, *t, sw))
        for (TrackRef r : groupMembers(s, g->id))
            if (!(r == track)) out.push_back(r);
    return out;
}

std::string normalizeTrackColor(const std::string& color) {
    std::string c = color;
    if (!c.empty() && c[0] == '#') c.erase(0, 1);
    if (!std::all_of(c.begin(), c.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; })) return {};
    if (c.size() == 3) c = std::string{c[0], c[0], c[1], c[1], c[2], c[2]};
    if (c.size() != 6) return {};
    return "#" + lower(c);
}

bool trackColorRgb(const std::string& color, double& r, double& g, double& b) {
    const std::string c = normalizeTrackColor(color);
    if (c.empty()) return false;
    const unsigned long v = std::strtoul(c.c_str() + 1, nullptr, 16);
    r = double((v >> 16) & 0xff) / 255.0, g = double((v >> 8) & 0xff) / 255.0, b = double(v & 0xff) / 255.0;
    return true;
}

std::string trackColorFromRgb(double r, double g, double b) {
    static const char* hex = "0123456789abcdef";
    std::string out = "#";
    for (double v : {r, g, b}) {
        const int n = int(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
        out += hex[n >> 4];
        out += hex[n & 15];
    }
    return out;
}

int takeNumber(const TakeLane& lane) {
    if (lane.number > 0) return lane.number;
    if (lane.name.rfind("Take ", 0) == 0) {
        const char* digits = lane.name.c_str() + 5;
        char* end = nullptr;
        const long n = std::strtol(digits, &end, 10);
        if (end != digits && *end == 0 && n > 0) return int(n);
    }
    return 0;
}

std::string takeColor(const TakeLane& lane) {
    // Eight hues far enough apart to tell side by side on the dark timeline, in turn.
    static const char* palette[] = {"#4fb3a9", "#d9905a", "#8a7ad6", "#c7c44f", "#d56a8c", "#5a9ad9", "#7fbf5f", "#b97ad0"};
    const int n = takeNumber(lane);
    return n > 0 ? palette[(n - 1) % 8] : std::string();
}

namespace edit {

std::vector<Id> groupMates(const Sequence& s, Id clip) {
    std::vector<Id> out;
    const auto loc = locate(s, clip);
    if (!loc) return out;
    const Clip& c = trackAt(s, loc->track)->clips[loc->index];
    const std::vector<TrackRef> tracks = groupTracks(s, loc->track, GroupSwitch::Editing);
    for (size_t k = 1; k < tracks.size(); ++k)
        for (const Clip& o : trackAt(s, tracks[k])->clips)
            if (o.start < c.end() && o.end() > c.start) out.push_back(o.id);
    return out;
}

std::vector<Id> expandGroups(const Sequence& s, const std::vector<Id>& ids) {
    std::vector<Id> out;
    std::set<Id> seen;
    auto add = [&](Id id) {
        if (seen.insert(id).second) out.push_back(id);
    };
    const std::vector<Id> linked = expandLinks(s, ids);
    for (Id id : linked) add(id);
    for (Id id : linked)
        for (Id m : groupMates(s, id))
            for (Id l : linkedClips(s, m)) add(l);
    return out;
}

std::vector<Id> groupEdgeMates(const Sequence& s, Id clip, Edge edge) {
    std::vector<Id> out;
    const Clip* c = clipById(s, clip);
    if (!c) return out;
    const FrameTime at = edge == Edge::In ? c->start : c->end();
    for (Id m : groupMates(s, clip))
        if (const Clip* o = clipById(s, m); o && (edge == Edge::In ? o->start : o->end()) == at) out.push_back(m);
    return out;
}

Result createTrackGroup(Project& p, Sequence& s, const std::string& name, const std::vector<TrackRef>& tracks) {
    if (name.empty()) return Result::fail("A group needs a name");
    if (findTrackGroup(s, name)) return Result::fail("There is already a group called " + name);
    if (tracks.empty()) return Result::fail("Choose the tracks to group");
    for (TrackRef r : tracks)
        if (!trackAt(s, r)) return Result::fail("No such track");
    TrackGroup g;
    g.id = p.newId();
    g.name = name;
    s.trackGroups.push_back(g);
    for (TrackRef r : tracks) trackAt(s, r)->group = g.id;
    Result r;
    r.created.push_back(g.id);
    return r;
}

Result setTrackGroup(Sequence& s, const std::vector<TrackRef>& tracks, Id group) {
    if (tracks.empty()) return Result::fail("No tracks");
    if (group && !findTrackGroup(s, group)) return Result::fail("No such group");
    bool changed = false;
    for (TrackRef r : tracks) {
        Track* t = trackAt(s, r);
        if (!t) return Result::fail("No such track");
        changed |= t->group != group;
        t->group = group;
    }
    return changed ? Result{} : Result::fail("");
}

Result renameTrackGroup(Sequence& s, Id group, const std::string& name) {
    TrackGroup* g = findTrackGroup(s, group);
    if (!g) return Result::fail("No such group");
    if (name.empty()) return Result::fail("A group needs a name");
    if (const TrackGroup* other = findTrackGroup(s, name); other && other != g) return Result::fail("There is already a group called " + name);
    g->name = name;
    return {};
}

Result removeTrackGroup(Sequence& s, Id group) {
    if (!findTrackGroup(s, group)) return Result::fail("No such group");
    for (TrackRef r : allTracks(s))
        if (Track* t = trackAt(s, r); t->group == group) t->group = 0;
    std::erase_if(s.trackGroups, [&](const TrackGroup& g) { return g.id == group; });
    return {};
}

Result setTrackGroupSwitches(Sequence& s, Id group, const TrackGroup& settings) {
    TrackGroup* g = findTrackGroup(s, group);
    if (!g) return Result::fail("No such group");
    TrackGroup next = settings;
    next.id = g->id;
    next.name = g->name;
    if (next == *g) return Result::fail("");
    *g = next;
    return {};
}

Result suspendTrackGroup(Sequence& s, Id group, bool suspended) {
    TrackGroup* g = findTrackGroup(s, group);
    if (!g) return Result::fail("No such group");
    if (g->suspended == suspended) return Result::fail("");
    g->suspended = suspended;
    return {};
}

Result setTrackColor(Sequence& s, const std::vector<TrackRef>& tracks, const std::string& color) {
    const std::string c = normalizeTrackColor(color);
    if (!color.empty() && c.empty()) return Result::fail("A colour is written #rrggbb");
    if (tracks.empty()) return Result::fail("No tracks");
    bool changed = false;
    for (TrackRef r : tracks) {
        Track* t = trackAt(s, r);
        if (!t) return Result::fail("No such track");
        changed |= t->color != c;
        t->color = c;
    }
    return changed ? Result{} : Result::fail("");
}

Result setTrackColorClips(Sequence& s, const std::vector<TrackRef>& tracks, bool on) {
    if (tracks.empty()) return Result::fail("No tracks");
    bool changed = false;
    for (TrackRef r : tracks) {
        Track* t = trackAt(s, r);
        if (!t) return Result::fail("No such track");
        if (t->colorClips == on) continue;
        t->colorClips = on;
        changed = true;
    }
    return changed ? Result{} : Result::fail("");
}

Result setTrackMuted(Sequence& s, TrackRef track, bool muted) {
    bool changed = false;
    for (TrackRef r : groupTracks(s, track, GroupSwitch::Mute)) {
        Track* t = trackAt(s, r);
        changed |= t->muted != muted;
        t->muted = muted;
    }
    return changed ? Result{} : Result::fail("");
}

Result setTrackSoloed(Sequence& s, TrackRef track, bool soloed) {
    bool changed = false;
    for (TrackRef r : groupTracks(s, track, GroupSwitch::Mute)) {
        Track* t = trackAt(s, r);
        changed |= t->solo != soloed;
        t->solo = soloed;
    }
    return changed ? Result{} : Result::fail("");
}

Result setTrackAutomationMode(Sequence& s, TrackRef track, int mode) {
    if (track.kind != TrackKind::Audio || !trackAt(s, track)) return Result::fail("Automation is on audio tracks");
    if (mode < 0 || mode >= kAutomationModes) return Result::fail("No such automation mode");
    bool changed = false;
    for (TrackRef r : groupTracks(s, track, GroupSwitch::Automation)) {
        Track* t = trackAt(s, r);
        if (r.kind != TrackKind::Audio) continue;
        changed |= t->automation != mode;
        t->automation = mode;
    }
    return changed ? Result{} : Result::fail("");
}

Result razorTogether(Project& p, Sequence& s, const std::vector<TrackRef>& tracks, FrameTime frame) {
    std::vector<TrackRef> all;
    auto add = [&](TrackRef r) {
        if (std::find(all.begin(), all.end(), r) == all.end()) all.push_back(r);
    };
    auto addLinks = [&](TrackRef r) {
        add(r);
        if (const Clip* c = clipAt(s, r, frame))
            for (Id l : linkedClips(s, c->id))
                if (const auto loc = locate(s, l)) add(loc->track);
    };
    for (TrackRef r : tracks) {
        for (TrackRef g : groupTracks(s, r, GroupSwitch::Editing)) addLinks(g);
    }
    // (The tracks of links found through a grouped track bring their own groups' tracks too.)
    for (size_t k = 0; k < all.size(); ++k)
        for (TrackRef g : groupTracks(s, all[k], GroupSwitch::Editing)) addLinks(g);
    Result out = Result::fail("No clip under the playhead");
    std::map<Id, Id> regroup;
    for (TrackRef r : all) {
        const Clip* under = clipAt(s, r, frame);
        if (!under || under->start == frame) continue;
        const Id group = under->linkGroup;
        Result one = razor(p, s, r, frame);
        if (!one.ok) {
            if (out.created.empty()) out = one;
            continue;
        }
        if (!out.ok) out = Result{};
        if (group && !one.created.empty()) {
            auto it = regroup.find(group);
            if (it == regroup.end()) it = regroup.emplace(group, p.newId()).first;
            if (Clip* rc = clipById(s, one.created[0])) rc->linkGroup = it->second;
        }
        out.created.insert(out.created.end(), one.created.begin(), one.created.end());
    }
    return out;
}

std::vector<std::pair<TrackRef, Id>> passLanes(const Sequence& s, TrackRef track, Id lane) {
    std::vector<std::pair<TrackRef, Id>> out;
    const Track* t = trackAt(s, track);
    const TakeLane* l = t ? findTakeLane(*t, lane) : nullptr;
    if (!l) return out;
    out.push_back({track, lane});
    if (!l->pass) return out;
    const std::vector<TrackRef> tracks = groupTracks(s, track, GroupSwitch::Comp);
    for (size_t k = 1; k < tracks.size(); ++k)
        for (const TakeLane& o : trackAt(s, tracks[k])->lanes)
            if (o.pass == l->pass) {
                out.push_back({tracks[k], o.id});
                break;
            }
    return out;
}

Result compGroup(Project& p, Sequence& s, TrackRef track, Id lane, FrameTime from, FrameTime to, double crossfade) {
    const std::vector<std::pair<TrackRef, Id>> lanes = passLanes(s, track, lane);
    Result first = compTake(p, s, track, lane, from, to, crossfade);
    if (!first.ok) return first;
    for (size_t k = 1; k < lanes.size(); ++k) {
        const Track* t = trackAt(s, lanes[k].first);
        if (t->locked) continue;
        Result r = compTake(p, s, lanes[k].first, lanes[k].second, from, to, crossfade);
        if (r.ok) first.created.insert(first.created.end(), r.created.begin(), r.created.end());
    }
    return first;
}

Result rateGroupLane(Sequence& s, TrackRef track, Id lane, int rating) {
    const std::vector<std::pair<TrackRef, Id>> lanes = passLanes(s, track, lane);
    if (lanes.empty()) return Result::fail("No such take lane");
    for (const auto& [r, id] : lanes) {
        Result one = rateTakeLane(s, r, id, rating);
        if (!one.ok && r == track) return one;
    }
    return {};
}

Result addGroupLoopTakes(Project& p, Sequence& s, const std::vector<TrackRef>& tracks, const std::vector<std::vector<Id>>& media,
                         const std::vector<bool>& complete, FrameTime in, FrameTime out) {
    if (tracks.empty() || media.size() != tracks.size()) return Result::fail("A pass for each track");
    const size_t passes = media.front().size();
    if (passes == 0) return Result::fail("No passes to add");
    for (const auto& m : media)
        if (m.size() != passes) return Result::fail("The tracks have different numbers of passes");
    // The take numbers: after the highest on any of the tracks, so they match.
    int next = 1;
    for (TrackRef r : tracks) {
        const Track* t = trackAt(s, r);
        if (!t || r.kind != TrackKind::Audio) return Result::fail("Take lanes are on audio tracks");
        for (const TakeLane& l : t->lanes) next = std::max(next, takeNumber(l) + 1);
    }
    std::vector<Id> passIds;
    for (size_t k = 0; k < passes; ++k) passIds.push_back(p.newId());
    Result all;
    std::vector<std::pair<TrackRef, Id>> compLanes;
    for (size_t t = 0; t < tracks.size(); ++t) {
        Id compLane = 0;
        for (size_t k = 0; k < passes; ++k) {
            const int number = next + int(k);
            Result r = addTakeLane(p, s, tracks[t], media[t][k], in, "Take " + std::to_string(number));
            if (!r.ok) return r;
            Track* tr = trackAt(s, tracks[t]);
            for (TakeLane& l : tr->lanes)
                if (l.id == r.created.front()) l.pass = passIds[k], l.number = number;
            all.created.push_back(r.created.front());
            if (k >= complete.size() || complete[k]) compLane = r.created.front();
        }
        if (compLane) compLanes.push_back({tracks[t], compLane});
    }
    if (out > in)
        for (const auto& [r, lane] : compLanes) {
            Result c = compTake(p, s, r, lane, in, out);
            if (!c.ok) return c;
            all.created.push_back(c.created.front());
        }
    return all;
}

namespace {

// The auditions on the grouped tracks (comp switch) starting where `clip` does.
std::vector<Id> auditionMates(const Sequence& s, Id clip) {
    std::vector<Id> out;
    const auto loc = locate(s, clip);
    if (!loc) return out;
    const Clip& c = trackAt(s, loc->track)->clips[loc->index];
    const std::vector<TrackRef> tracks = groupTracks(s, loc->track, GroupSwitch::Comp);
    for (size_t k = 1; k < tracks.size(); ++k)
        for (const Clip& o : trackAt(s, tracks[k])->clips)
            if (o.start == c.start && !o.takes.empty()) out.push_back(o.id);
    return out;
}

}  // namespace

Result pickGroupTake(Project& p, Sequence& s, Id clip, int take) {
    const std::vector<Id> mates = auditionMates(s, clip);
    Result r = pickTake(p, s, clip, take);
    if (!r.ok) return r;
    for (Id m : mates)
        if (const Clip* c = clipById(s, m); c && take < int(c->takes.size())) pickTake(p, s, m, take);
    return r;
}

Result cycleGroupTake(Project& p, Sequence& s, Id clip, int step) {
    const Clip* c = clipById(s, clip);
    if (!c || c->takes.empty()) return Result::fail("That clip is not an audition");
    const int n = int(c->takes.size());
    return pickGroupTake(p, s, clip, ((c->take + step) % n + n) % n);
}

}  // namespace edit
}  // namespace montage
