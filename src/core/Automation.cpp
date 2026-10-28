#include "Automation.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "Effects.h"

namespace montage {

namespace {

// A lane's value at a fractional frame: whole frames as the lane gives them, linear between.
double laneAt(const Param& p, double frame) {
    // Keys between frames are met where they sit; otherwise a straight line between the frames' values, as always.
    if (p.expr.empty() && std::any_of(p.keys.begin(), p.keys.end(), [](const Keyframe& k) { return k.sub != 0; })) return p.atTime(frame);
    const FrameTime f0 = FrameTime(std::floor(frame));
    const double u = frame - double(f0), a = p.at(f0);
    return u > 1e-9 ? a + (p.at(f0 + 1) - a) * u : a;
}

bool reads(const Track& t) { return automationReads(trackAutomation(t)); }

// A trim's offset at a fractional frame (as laneAt; its static value without keys).
double trimLaneAt(const Param& trim, double frame) { return trim.animated() ? laneAt(trim, frame) : trim.value; }
bool trims(const Param& trim) { return trim.animated() || trim.value != 0; }

}  // namespace

const char* automationModeName(AutomationMode m) {
    switch (m) {
        case AutomationMode::Off: return "Off";
        case AutomationMode::Read: return "Read";
        case AutomationMode::Write: return "Write";
        case AutomationMode::Latch: return "Latch";
        case AutomationMode::Touch: return "Touch";
        case AutomationMode::TrimWrite: return "Trim Write";
        case AutomationMode::TrimLatch: return "Trim Latch";
        case AutomationMode::TrimTouch: return "Trim Touch";
    }
    return "Read";
}

bool automationTrims(AutomationMode m) { return m == AutomationMode::TrimWrite || m == AutomationMode::TrimLatch || m == AutomationMode::TrimTouch; }

AutomationMode untrimmed(AutomationMode m) {
    switch (m) {
        case AutomationMode::TrimWrite: return AutomationMode::Write;
        case AutomationMode::TrimLatch: return AutomationMode::Latch;
        case AutomationMode::TrimTouch: return AutomationMode::Touch;
        default: return m;
    }
}

AutomationMode trimmed(AutomationMode m) {
    switch (m) {
        case AutomationMode::Write: return AutomationMode::TrimWrite;
        case AutomationMode::Latch: return AutomationMode::TrimLatch;
        case AutomationMode::Off:
        case AutomationMode::Read:
        case AutomationMode::Touch: return AutomationMode::TrimTouch;
        default: return m;
    }
}

bool automationReads(AutomationMode m) { return m != AutomationMode::Off && m != AutomationMode::Write; }
bool automationWrites(AutomationMode m) { return m != AutomationMode::Off && m != AutomationMode::Read; }

AutomationMode trackAutomation(const Track& t) { return AutomationMode(std::clamp(t.automation, 0, kAutomationModes - 1)); }

double trimAt(const Param& trim, double frame) { return trimLaneAt(trim, frame); }

double trackVolumeAt(const Track& t, double frame) {
    if (!reads(t)) return t.volumeDb;
    return (t.volumeAuto.animated() ? laneAt(t.volumeAuto, frame) : t.volumeDb) + trimLaneAt(t.volumeTrim, frame);
}

bool trackVolumeAnimated(const Track& t) { return reads(t) && (t.volumeAuto.animated() || trims(t.volumeTrim)); }

double trackPanAt(const Track& t, double frame) {
    return reads(t) && t.panAuto.animated() ? std::clamp(laneAt(t.panAuto, frame), -1.0, 1.0) : t.pan;
}

bool laneMuted(const Param& lane, bool still, double frame) {
    if (lane.keys.empty()) return still;
    const auto after = std::upper_bound(lane.keys.begin(), lane.keys.end(), frame, [](double f, const Keyframe& k) { return f < k.time(); });
    const Keyframe& k = after == lane.keys.begin() ? lane.keys.front() : *std::prev(after);
    return k.v >= 0.5;
}

bool trackMutedAt(const Track& t, double frame) { return t.muted || (reads(t) && laneMuted(t.muteAuto, false, frame)); }

double sendLevelAt(const Track& t, const Send& send, double frame) {
    if (!reads(t)) return send.levelDb;
    return (send.levelAuto.animated() ? laneAt(send.levelAuto, frame) : send.levelDb) + trimLaneAt(send.levelTrim, frame);
}

bool sendLevelAnimated(const Track& t, const Send& send) { return reads(t) && (send.levelAuto.animated() || trims(send.levelTrim)); }

AutomationMode busAutomation(const Bus& b) { return AutomationMode(std::clamp(b.automation, 0, kAutomationModes - 1)); }

namespace {
bool reads(const Bus& b) { return automationReads(busAutomation(b)); }
}  // namespace

double busVolumeAt(const Bus& b, double frame) {
    if (!reads(b)) return b.volumeDb;
    return (b.volumeAuto.animated() ? laneAt(b.volumeAuto, frame) : b.volumeDb) + trimLaneAt(b.volumeTrim, frame);
}

bool busVolumeAnimated(const Bus& b) { return reads(b) && (b.volumeAuto.animated() || trims(b.volumeTrim)); }

double busPanAt(const Bus& b, double frame) { return reads(b) && b.panAuto.animated() ? std::clamp(laneAt(b.panAuto, frame), -1.0, 1.0) : b.pan; }

bool busMutedAt(const Bus& b, double frame) { return b.muted || (reads(b) && laneMuted(b.muteAuto, false, frame)); }

bool surroundAnimated(const Track& t) { return t.surroundXAuto.animated() || t.surroundYAuto.animated() || t.surroundZAuto.animated(); }

SurroundPan trackSurroundAt(const Track& t, double frame) {
    SurroundPan p = t.surround;
    if (!reads(t)) return p;
    if (t.surroundXAuto.animated()) p.x = std::clamp(laneAt(t.surroundXAuto, frame), -1.0, 1.0);
    if (t.surroundYAuto.animated()) p.y = std::clamp(laneAt(t.surroundYAuto, frame), -1.0, 1.0);
    if (t.surroundZAuto.animated()) p.z = std::clamp(laneAt(t.surroundZAuto, frame), 0.0, 1.0);
    return p;
}

void thinKeys(std::vector<Keyframe>& keys, double tolerance) {
    if (keys.size() < 3) return;
    std::vector<char> keep(keys.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> stack{{0, keys.size() - 1}};
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        if (b <= a + 1) continue;
        double worst = -1;
        size_t at = a;
        const double span = double(keys[b].t - keys[a].t);
        for (size_t i = a + 1; i < b; ++i) {
            const double u = span > 0 ? double(keys[i].t - keys[a].t) / span : 0;
            const double d = std::fabs(keys[i].v - (keys[a].v + (keys[b].v - keys[a].v) * u));
            if (d > worst) worst = d, at = i;
        }
        if (worst > tolerance) {
            keep[at] = 1;
            stack.push_back({a, at});
            stack.push_back({at, b});
        }
    }
    std::vector<Keyframe> out;
    for (size_t i = 0; i < keys.size(); ++i)
        if (keep[i]) out.push_back(keys[i]);
    keys = std::move(out);
}

AutomationRecorder::AutomationRecorder(AutomationMode mode, Param lane, double still, FrameTime start, FrameTime glide)
    : mode_(untrimmed(mode)), lane_(std::move(lane)), still_(still), glide_(std::max<FrameTime>(1, glide)), passStart_(start), last_(start - 1),
      lastValue_(still) {
    if (mode_ == AutomationMode::Write) writing_ = true;
}

double AutomationRecorder::tick(FrameTime t, double fader, bool held) {
    if (t <= last_) return writing_ ? fader : (mode_ == AutomationMode::Off ? fader : original(t));  // playback only runs forward
    last_ = t;
    auto record = [&] {
        current_.push_back({t, fader});
        lastValue_ = fader;
        return fader;
    };
    switch (mode_) {
        case AutomationMode::Off: return fader;
        case AutomationMode::Read: return original(t);
        case AutomationMode::Write: return record();
        case AutomationMode::Latch:
            if (!writing_ && held) writing_ = true, passStart_ = t;
            return writing_ ? record() : original(t);
        case AutomationMode::Touch:
            if (held) {
                if (!writing_) writing_ = true, passStart_ = t;
                return record();
            }
            if (writing_) endPass(t - 1);
            return original(t);
        default: break;
    }
    return fader;
}

void AutomationRecorder::endPass(FrameTime t) {
    if (!current_.empty()) {
        Pass p;
        p.from = passStart_;
        p.to = std::max(t, current_.back().t);
        p.keys = std::move(current_);
        p.glide = mode_ == AutomationMode::Touch;
        passes_.push_back(std::move(p));
    }
    current_.clear();
    writing_ = false;
}

Param AutomationRecorder::finish(FrameTime stop, double tolerance) {
    if (writing_) {
        // Write and Latch hold the last value to where playback stopped; Touch ends where it was let go.
        if (mode_ != AutomationMode::Touch && !current_.empty() && current_.back().t < stop) current_.push_back({stop, lastValue_});
        endPass(stop);
    }
    Param out = lane_;
    if (!out.animated()) out.value = still_;
    for (Pass& p : passes_) {
        const FrameTime from = p.from, to = p.keys.back().t;
        const FrameTime resume = p.glide ? to + glide_ : to + 1;
        const double before = original(from - 1), after = original(resume);
        std::erase_if(out.keys, [&](const Keyframe& k) { return k.t >= from && k.t <= resume; });
        thinKeys(p.keys, tolerance);
        // The level either side of the pass stays as it was.
        if (from > 0 && !out.keyAt(from - 1)) out.keys.push_back({from - 1, before});
        for (const Keyframe& k : p.keys) out.keys.push_back(k);
        out.keys.push_back({resume, after});
    }
    std::sort(out.keys.begin(), out.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
    passes_.clear();
    // A pass that left a flat lane at the fader's level (pan never touched in Write) adds nothing.
    if (!lane_.animated() && std::all_of(out.keys.begin(), out.keys.end(), [&](const Keyframe& k) { return std::fabs(k.v - still_) <= tolerance; }))
        return Param(still_);
    return out;
}


// ---- Trim ----

namespace {

// Whether a lane is straight between whole-frame points (so a sum of two is exact at the union of their points).
bool straight(const Param& p) {
    return p.expr.empty() && (p.keys.size() < 2 || p.repeat == Repeat::Hold) &&
           std::all_of(p.keys.begin(), p.keys.end(), [](const Keyframe& k) { return k.interp == Interp::Linear && k.sub == 0; });
}

void sortKeys(std::vector<Keyframe>& keys) {
    std::stable_sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
}

bool hasKey(const std::vector<Keyframe>& keys, FrameTime t) {
    return std::any_of(keys.begin(), keys.end(), [t](const Keyframe& k) { return k.t == t; });
}

// `keys` put in place of a lane's over frames from..to (to < 0: to the end and on), the level before `from` kept with a
// point a frame before it, and the level after `to` with one a frame after it (unless, with `runOn`, `to` was one of
// the lane's points, which `keys` replace: the lane then runs on from there to its next).
void replaceRange(Param& lane, double still, FrameTime from, FrameTime to, std::vector<Keyframe> keys, bool hold, bool runOn = false) {
    const Param old = lane;
    auto oldAt = [&](FrameTime t) { return old.animated() ? old.at(t) : still; };
    const bool toEnd = to < 0;
    if (!toEnd && to < from) to = from;
    std::erase_if(lane.keys, [&](const Keyframe& k) { return k.t >= from && (toEnd || k.t <= to); });
    if (from > 0 && !hasKey(lane.keys, from - 1)) {
        Keyframe before{from - 1, oldAt(from - 1)};
        if (hold) before.interp = Interp::Hold;
        lane.keys.push_back(before);
    }
    if (!toEnd && !(runOn && hasKey(old.keys, to)) && !hasKey(lane.keys, to + 1)) {
        Keyframe after{to + 1, oldAt(to + 1)};
        if (hold) after.interp = Interp::Hold;
        lane.keys.push_back(after);
    }
    for (Keyframe& k : keys) {
        if (hold) k.interp = Interp::Hold;
        std::erase_if(lane.keys, [&](const Keyframe& x) { return x.t == k.t; });
        lane.keys.push_back(k);
    }
    sortKeys(lane.keys);
}

}  // namespace

bool coalesceTrim(Param& lane, double& still, Param& trim) {
    if (!trims(trim)) return false;
    // A lane worked out by an expression, or repeating after its last point, cannot take the trim as points.
    for (const Param* p : {&lane, &trim})
        if (!p->expr.empty() || (p->keys.size() >= 2 && p->repeat != Repeat::Hold)) return false;
    if (!trim.animated()) {
        if (lane.animated())
            for (Keyframe& k : lane.keys) k.v += trim.value;
        else
            still += trim.value;
        trim = Param();
        return true;
    }
    auto base = [&](FrameTime t) { return lane.animated() ? lane.at(t) : still; };
    std::set<FrameTime> times;
    const bool exact = straight(lane) && straight(trim);
    for (const Param* p : {&lane, &trim})
        for (const Keyframe& k : p->keys) times.insert(k.t);
    const std::vector<FrameTime> at(times.begin(), times.end());
    auto key = [&](FrameTime t) { return Keyframe{t, base(t) + trim.at(t)}; };
    std::vector<Keyframe> keys;
    for (size_t i = 0; i < at.size(); ++i) {
        keys.push_back(key(at[i]));
        if (exact || i + 1 == at.size() || at[i + 1] - at[i] < 2) continue;
        // Between two points both curves are smooth (or held): up to 64 points along it, the frame before the next
        // point among them (so a held value still steps there), thinned on their own (a long pass with eased points
        // costs no more than its points).
        const FrameTime span = at[i + 1] - at[i];
        const int n = int(std::min<FrameTime>(span - 1, 63));
        std::vector<Keyframe> seg{keys.back()};
        for (int j = 1; j < n; ++j) seg.push_back(key(at[i] + FrameTime(double(span) * j / n)));
        seg.push_back(key(at[i + 1] - 1));
        seg.push_back(key(at[i + 1]));
        thinKeys(seg, 1e-4);
        keys.insert(keys.end(), seg.begin() + 1, seg.end() - 1);
    }
    lane.keys = std::move(keys);
    trim = Param();
    return true;
}

bool hasTrim(const Track& t) {
    return trims(t.volumeTrim) || std::any_of(t.sends.begin(), t.sends.end(), [](const Send& s) { return trims(s.levelTrim); });
}
bool hasTrim(const Bus& b) { return trims(b.volumeTrim); }

bool coalesceTrim(Track& t) {
    if (!reads(t)) return false;  // (a trim not heard: folding it in would change what plays)
    bool changed = coalesceTrim(t.volumeAuto, t.volumeDb, t.volumeTrim);
    for (Send& s : t.sends) changed = coalesceTrim(s.levelAuto, s.levelDb, s.levelTrim) || changed;
    return changed;
}

bool coalesceTrim(Bus& b) {
    if (!reads(b)) return false;
    return coalesceTrim(b.volumeAuto, b.volumeDb, b.volumeTrim);
}

// ---- Values over a range ----

std::pair<FrameTime, FrameTime> writeToRange(WriteTo w, const Param& lane, FrameTime passFrom, FrameTime passTo, FrameTime in,
                                             FrameTime out) {
    switch (w) {
        case WriteTo::Start: return {0, std::max<FrameTime>(0, passTo)};
        case WriteTo::End: return {std::max<FrameTime>(0, passFrom), -1};
        case WriteTo::InOut:
            if (in < 0 || out < in) return {-1, -1};  // (no In to Out)
            return {in, out};
        case WriteTo::All: return {0, -1};
        case WriteTo::Next:
            for (const Keyframe& k : lane.keys)
                if (k.t > passTo) return {std::max<FrameTime>(0, passFrom), k.t};
            return {std::max<FrameTime>(0, passFrom), -1};
    }
    return {passFrom, passTo};
}

void writeLaneValue(Param& lane, double still, FrameTime from, FrameTime to, double value, bool hold) {
    from = std::max<FrameTime>(0, from);
    std::vector<Keyframe> keys{{from, value}};
    if (to > from) keys.push_back({to, value});
    replaceRange(lane, still, from, to, std::move(keys), hold, true);
}

void glideLane(Param& lane, double still, FrameTime from, FrameTime to, double value, bool hold) {
    from = std::max<FrameTime>(0, from);
    to = std::max(to, from + 1);
    const double start = lane.animated() ? lane.at(from) : still;
    replaceRange(lane, still, from, hold ? -1 : to, {{from, start}, {to, value}}, false);
}

// ---- Lanes by name ----

namespace {

// "insert:<index>:<type>:<param>" on a chain.
LaneRef insertLane(std::vector<Effect>& chain, const std::string& name) {
    if (name.rfind("insert:", 0) != 0) return {};
    const size_t a = name.find(':', 7), b = a == std::string::npos ? a : name.find(':', a + 1);
    if (b == std::string::npos) return {};
    size_t index = 0;
    try {
        index = size_t(std::stoul(name.substr(7, a - 7)));
    } catch (...) {
        return {};
    }
    if (index >= chain.size() || chain[index].type != name.substr(a + 1, b - a - 1)) return {};
    Effect& e = chain[index];
    const std::string param = name.substr(b + 1);
    LaneRef r;
    bool known = false;
    for (const ParamInfo& pi : effectParams(e))
        if (pi.name == param && pi.kind != ParamKind::Color) {
            known = true;
            r.hold = pi.kind == ParamKind::Bool || pi.kind == ParamKind::Choice;
            r.lo = pi.min, r.hi = pi.max;
            if (!e.params.count(param)) e.params[param] = Param(pi.def);
        }
    const auto it = e.params.find(param);
    if (it == e.params.end() || (!known && e.type != "plugin")) return {};
    r.lane = &it->second;
    r.still = &it->second.value;
    return r;
}

std::vector<std::string> insertLaneNames(const std::vector<Effect>& chain) {
    std::vector<std::string> out;
    for (size_t i = 0; i < chain.size(); ++i)
        for (const auto& [param, value] : chain[i].params)
            if (value.expr.empty()) out.push_back("insert:" + std::to_string(i) + ":" + chain[i].type + ":" + param);
    return out;
}

std::optional<Id> busIdIn(const std::string& name, const char* prefix) {
    const std::string p(prefix);
    if (name.rfind(p, 0) != 0) return std::nullopt;
    try {
        return Id(std::stoull(name.substr(p.size())));
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

LaneRef trackLane(Track& t, const std::string& name) {
    if (name == "volume") return {&t.volumeAuto, &t.volumeDb, false, -96, 12};
    if (name == "trim") return {&t.volumeTrim, &t.volumeTrim.value, false, -96, 24};
    if (name == "pan") return {&t.panAuto, &t.pan, false, -1, 1};
    if (name == "mute") return {&t.muteAuto, &t.muteAuto.value, true, 0, 1};
    if (name == "x") return {&t.surroundXAuto, &t.surround.x, false, -1, 1};
    if (name == "y") return {&t.surroundYAuto, &t.surround.y, false, -1, 1};
    if (name == "z") return {&t.surroundZAuto, &t.surround.z, false, 0, 1};
    for (const char* prefix : {"send:", "send-trim:"})
        if (const auto bus = busIdIn(name, prefix))
            for (Send& sd : t.sends)
                if (sd.bus == *bus) {
                    if (prefix[4] == ':') return {&sd.levelAuto, &sd.levelDb, false, -96, 12};
                    return {&sd.levelTrim, &sd.levelTrim.value, false, -96, 24};
                }
    return insertLane(t.effects, name);
}

LaneRef busLane(Bus& b, const std::string& name) {
    if (name == "volume") return {&b.volumeAuto, &b.volumeDb, false, -96, 12};
    if (name == "trim") return {&b.volumeTrim, &b.volumeTrim.value, false, -96, 24};
    if (name == "pan") return {&b.panAuto, &b.pan, false, -1, 1};
    if (name == "mute") return {&b.muteAuto, &b.muteAuto.value, true, 0, 1};
    return insertLane(b.effects, name);
}

std::vector<std::string> trackLaneNames(const Track& t) {
    std::vector<std::string> out{"volume", "trim", "pan", "mute", "x", "y", "z"};
    for (const Send& sd : t.sends) {
        out.push_back("send:" + std::to_string(sd.bus));
        out.push_back("send-trim:" + std::to_string(sd.bus));
    }
    for (std::string& n : insertLaneNames(t.effects)) out.push_back(std::move(n));
    return out;
}

std::vector<std::string> busLaneNames(const Bus& b) {
    std::vector<std::string> out{"volume", "trim", "pan", "mute"};
    for (std::string& n : insertLaneNames(b.effects)) out.push_back(std::move(n));
    return out;
}

namespace {

// Each of a sequence's audio tracks and buses named in `owners` (all with none), in that order, with a lane lookup.
struct Owner {
    Track* track = nullptr;
    Bus* bus = nullptr;
    Id id() const { return track ? track->id : bus->id; }
    LaneRef lane(const std::string& name) const { return track ? trackLane(*track, name) : busLane(*bus, name); }
    std::vector<std::string> names() const { return track ? trackLaneNames(*track) : busLaneNames(*bus); }
};

std::vector<Owner> owners(Sequence& s, const std::vector<Id>& ids) {
    std::vector<Owner> out;
    auto find = [&](Id id) -> Owner {
        for (Track& t : s.audioTracks)
            if (t.id == id) return {&t, nullptr};
        for (Bus& b : s.buses)
            if (b.id == id) return {nullptr, &b};
        return {};
    };
    if (ids.empty()) {
        for (Track& t : s.audioTracks) out.push_back({&t, nullptr});
        for (Bus& b : s.buses) out.push_back({nullptr, &b});
        return out;
    }
    for (Id id : ids)
        if (const Owner o = find(id); o.track || o.bus) out.push_back(o);
    return out;
}

}  // namespace

// ---- Capture ----

MixSnapshot captureMix(const Sequence& seq, const std::vector<Id>& ids, FrameTime at) {
    Sequence s = seq;  // (a lane looked up for the first time may be added at its default)
    MixSnapshot snap;
    snap.at = at;
    for (const Owner& o : owners(s, ids)) {
        MixSnapshot::Strip strip{o.id(), o.bus != nullptr, {}};
        for (const std::string& name : o.names())
            if (const LaneRef r = o.lane(name); r.lane) strip.values[name] = r.at(at);
        // A static mute is captured as muted.
        if (o.track ? o.track->muted : o.bus->muted) strip.values["mute"] = 1;
        snap.strips.push_back(std::move(strip));
    }
    return snap;
}

int pasteMix(Sequence& s, const MixSnapshot& snap, FrameTime from, FrameTime to) {
    int written = 0;
    for (const MixSnapshot::Strip& strip : snap.strips)
        for (const Owner& o : owners(s, {strip.id})) {
            if ((o.bus != nullptr) != strip.bus) continue;
            for (const auto& [name, value] : strip.values) {
                const LaneRef r = o.lane(name);
                if (!r.lane) continue;
                if (!r.lane->animated() && std::fabs(*r.still - value) < 1e-12) continue;  // already there, not moving
                // A point paste reaches to the lane's next point (to the end with none).
                FrameTime until = to;
                if (to < from) until = writeToRange(WriteTo::Next, *r.lane, from, from, 0, 0).second;
                writeLaneValue(*r.lane, *r.still, from, until, value, r.hold);
                ++written;
            }
        }
    return written;
}

// ---- Copy and paste ----

LaneClip copyLane(const Param& lane, double still, FrameTime from, FrameTime to) {
    if (to < from) std::swap(from, to);
    LaneClip c;
    c.length = to - from;
    auto at = [&](FrameTime t) { return lane.animated() ? lane.at(t) : still; };
    c.keys.push_back({0, at(from)});
    for (const Keyframe& k : lane.keys)
        if (k.t > from && k.t < to) {
            Keyframe x = k;
            x.t -= from;
            c.keys.push_back(x);
        }
    if (to > from) c.keys.push_back({c.length, at(to)});
    // The first point keeps the interpolation the lane had there.
    for (const Keyframe& k : lane.keys)
        if (k.t <= from) c.keys.front().interp = k.interp;
    return c;
}

void pasteLane(Param& lane, double still, const LaneClip& clip, FrameTime at, bool relative) {
    if (clip.keys.empty()) return;
    at = std::max<FrameTime>(0, at);
    std::vector<Keyframe> keys;
    if (!relative || clip.hold) {
        for (Keyframe k : clip.keys) {
            k.t += at;
            keys.push_back(k);
        }
    } else {
        // What was there, moved as the copied lane moved: exact at the points of both where both are straight.
        Param shape;
        shape.keys = clip.keys;
        const double first = clip.keys.front().v;
        auto oldAt = [&](FrameTime t) { return lane.animated() ? lane.at(t) : still; };
        std::set<FrameTime> times{at, at + clip.length};
        for (const Keyframe& k : clip.keys) times.insert(k.t + at);
        for (const Keyframe& k : lane.keys)
            if (k.t > at && k.t < at + clip.length) times.insert(k.t);
        if (!straight(shape) || !straight(lane))
            for (FrameTime t = at; t <= at + clip.length; ++t) times.insert(t);
        for (FrameTime t : times) keys.push_back({t, oldAt(t) + shape.at(t - at) - first});
    }
    replaceRange(lane, still, at, at + clip.length, std::move(keys), clip.hold);
}

AutomationCopy copyAutomation(const Sequence& seq, const std::vector<Id>& ids, FrameTime from, FrameTime to,
                              const std::vector<std::string>& lanes) {
    Sequence s = seq;
    AutomationCopy c;
    c.length = std::abs(to - from);
    for (const Owner& o : owners(s, ids)) {
        std::map<std::string, LaneClip> strip;
        for (const std::string& name : lanes.empty() ? o.names() : lanes) {
            const LaneRef r = o.lane(name);
            if (!r.lane || (lanes.empty() && !r.lane->animated())) continue;
            LaneClip clip = copyLane(*r.lane, *r.still, from, to);
            clip.hold = r.hold;
            strip[name] = std::move(clip);
        }
        c.strips.push_back(std::move(strip));
    }
    return c;
}

int pasteAutomation(Sequence& s, const AutomationCopy& c, const std::vector<Id>& ids, FrameTime at, bool relative) {
    int written = 0;
    const std::vector<Owner> to = owners(s, ids);
    for (size_t i = 0; i < to.size() && i < c.strips.size(); ++i)
        for (const auto& [name, clip] : c.strips[i]) {
            const LaneRef r = to[i].lane(name);
            if (!r.lane) continue;
            pasteLane(*r.lane, *r.still, clip, at, relative);
            ++written;
        }
    return written;
}

}  // namespace montage
