#include "EditOps.h"

#include "ProjectIO.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

#include "Effects.h"

namespace montage::edit {

namespace {

std::vector<Track>& listFor(Sequence& s, TrackKind k) { return k == TrackKind::Video ? s.videoTracks : s.audioTracks; }

void shiftParamKeys(Effect& e, FrameTime delta) {
    for (auto& [name, param] : e.params)
        for (auto& k : param.keys) k.t += delta;
}

bool editable(const Track* t) { return t && !t->locked; }

// Source-bound helpers for a clip (in source frames).
double srcLow(const Clip& c) { return c.sourceIn; }
double srcHigh(const Clip& c) { return c.sourceIn + c.sourceExtent(); }

}  // namespace

// ---------------------------------------------------------------------------
// Lookup

std::optional<ClipLoc> locate(const Sequence& s, Id clipId) {
    for (TrackRef r : allTracks(s)) {
        const Track* t = trackAt(s, r);
        for (size_t i = 0; i < t->clips.size(); ++i)
            if (t->clips[i].id == clipId) return ClipLoc{r, i};
    }
    return std::nullopt;
}

Clip* clipById(Sequence& s, Id clipId) {
    auto loc = locate(s, clipId);
    if (!loc) return nullptr;
    return &trackAt(s, loc->track)->clips[loc->index];
}

const Clip* clipById(const Sequence& s, Id clipId) { return clipById(const_cast<Sequence&>(s), clipId); }

const Clip* clipAt(const Sequence& s, TrackRef r, FrameTime frame) {
    const Track* t = trackAt(s, r);
    if (!t) return nullptr;
    for (const auto& c : t->clips)
        if (c.contains(frame)) return &c;
    return nullptr;
}

std::vector<Id> linkedClips(const Sequence& s, Id clipId) {
    const Clip* c = clipById(s, clipId);
    if (!c) return {};
    if (c->linkGroup == 0) return {clipId};
    std::vector<Id> out;
    for (TrackRef r : allTracks(s))
        for (const auto& other : trackAt(s, r)->clips)
            if (other.linkGroup == c->linkGroup) out.push_back(other.id);
    return out;
}

std::vector<Id> expandLinks(const Sequence& s, const std::vector<Id>& ids) {
    std::vector<Id> out;
    std::set<Id> seen;
    for (Id id : ids)
        for (Id l : linkedClips(s, id))
            if (seen.insert(l).second) out.push_back(l);
    return out;
}

FrameTime sourceLimit(const Project& p, const Sequence& s, const Clip& c) {
    if (c.isGenerator()) return kInfiniteFrames;
    const MediaItem* m = p.findMedia(c.mediaId);
    if (!m) return kInfiniteFrames;
    if (m->kind == MediaKind::Sequence) {
        const Sequence* nested = p.findSequence(m->sequenceId);
        return nested ? std::max<FrameTime>(1, nested->duration()) : kInfiniteFrames;
    }
    return mediaFrames(*m, s);
}

bool trackEmpty(const Track& t, FrameTime a, FrameTime b, const std::vector<Id>& ignore) {
    for (const auto& c : t.clips) {
        if (std::find(ignore.begin(), ignore.end(), c.id) != ignore.end()) continue;
        if (c.start < b && c.end() > a) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Primitives

void shiftKeyframes(Clip& c, FrameTime delta) {
    if (delta == 0) return;
    shiftParamKeys(c.motion, delta);
    shiftParamKeys(c.audio, delta);
    shiftParamKeys(c.generator, delta);
    shiftParamKeys(c.timing, delta);
    for (auto& e : c.effects) shiftParamKeys(e, delta);
}

Clip subClip(const Clip& c, FrameTime from, FrameTime to) {
    Clip out = c;
    from = std::clamp(from, c.start, c.end());
    to = std::clamp(to, from, c.end());
    out.start = from;
    out.duration = std::max<FrameTime>(1, to - from);
    if (c.reverse)
        out.sourceIn = c.sourceIn + double(c.end() - to) * c.speed;
    else
        out.sourceIn = c.sourceIn + c.sourceOffset(double(from - c.start));  // follows a speed ramp
    shiftKeyframes(out, -(from - c.start));
    return out;
}

void normalize(Track& t) {
    std::stable_sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    auto find = [&](Id id) -> const Clip* {
        if (id == 0) return nullptr;
        for (const auto& c : t.clips)
            if (c.id == id) return &c;
        return nullptr;
    };
    std::vector<Transition> kept;
    for (auto& tr : t.transitions) {
        const Clip* a = find(tr.clipA);
        const Clip* b = find(tr.clipB);
        if (tr.clipA && !a) continue;
        if (tr.clipB && !b) continue;
        if (!a && !b) continue;
        if (a && b && a->end() != b->start) continue;
        // Keep transitions no longer than the clips they join.
        // A centred transition may reach half its length into each clip; a
        // one-sided fade lives entirely inside its clip.
        FrameTime maxDur = std::numeric_limits<FrameTime>::max();
        const FrameTime k = (a && b) ? 2 : 1;
        if (a) maxDur = std::min(maxDur, a->duration * k);
        if (b) maxDur = std::min(maxDur, b->duration * k);
        tr.duration = std::clamp<FrameTime>(tr.duration, 1, maxDur);
        // Drop duplicates for the same edit point.
        bool dup = std::any_of(kept.begin(), kept.end(),
                               [&](const Transition& k) { return k.clipA == tr.clipA && k.clipB == tr.clipB; });
        if (!dup) kept.push_back(tr);
    }
    t.transitions = std::move(kept);
}

Id splitClip(Project& p, Track& t, size_t index, FrameTime frame) {
    if (index >= t.clips.size()) return 0;
    Clip orig = t.clips[index];
    if (frame <= orig.start || frame >= orig.end()) return 0;
    Clip left = subClip(orig, orig.start, frame);
    Clip right = subClip(orig, frame, orig.end());
    right.id = p.newId();
    // Linked partners split at the same frame should stay linked to each
    // other but not to the left halves; callers splitting several tracks
    // fix up link groups (see razorAll). A single split keeps the group.
    t.clips[index] = left;
    t.clips.insert(t.clips.begin() + long(index) + 1, right);
    // The transition at the original out-edge now belongs to the right part.
    for (auto& tr : t.transitions)
        if (tr.clipA == orig.id) tr.clipA = right.id;
    return right.id;
}

void clearRange(Project& p, Track& t, FrameTime a, FrameTime b) {
    if (b <= a) return;
    std::vector<Clip> out;
    out.reserve(t.clips.size() + 1);
    for (const auto& c : t.clips) {
        if (c.end() <= a || c.start >= b) {
            out.push_back(c);
            continue;
        }
        if (c.start < a) {
            Clip left = subClip(c, c.start, a);
            out.push_back(left);
        }
        if (c.end() > b) {
            Clip right = subClip(c, b, c.end());
            if (c.start < a) {
                right.id = p.newId();  // the left part kept the id
                for (auto& tr : t.transitions)
                    if (tr.clipA == c.id) tr.clipA = right.id;
            }
            out.push_back(right);
        }
    }
    t.clips = std::move(out);
    normalize(t);
}

std::vector<TrackRef> rippleTracks(const Sequence& s, const std::vector<TrackRef>& primary) {
    std::vector<TrackRef> out = primary;
    for (TrackRef r : allTracks(s)) {
        if (std::find(out.begin(), out.end(), r) != out.end()) continue;
        const Track* t = trackAt(s, r);
        if (t && t->syncLock && !t->locked) out.push_back(r);
    }
    return out;
}

void rippleOpen(Project& p, Sequence& s, FrameTime at, FrameTime len, const std::vector<TrackRef>& tracks) {
    if (len <= 0) return;
    for (TrackRef r : tracks) {
        Track* t = trackAt(s, r);
        if (!editable(t)) continue;
        for (size_t i = 0; i < t->clips.size(); ++i) {
            if (t->clips[i].start < at && t->clips[i].end() > at) {
                splitClip(p, *t, i, at);
                break;
            }
        }
        for (auto& c : t->clips)
            if (c.start >= at) c.start += len;
        normalize(*t);
    }
}

// Shift every clip starting at or after `at` by delta on the given tracks.
// For negative deltas a track is skipped if it has content in [at+delta, at).
static void rippleShift(Sequence& s, const std::vector<TrackRef>& tracks, const std::vector<TrackRef>& primary,
                        FrameTime at, FrameTime delta, const std::vector<Id>& ignore = {}) {
    if (delta == 0) return;
    for (TrackRef r : tracks) {
        Track* t = trackAt(s, r);
        if (!editable(t)) continue;
        bool isPrimary = std::find(primary.begin(), primary.end(), r) != primary.end();
        if (delta < 0 && !isPrimary && !trackEmpty(*t, at + delta, at, ignore)) continue;
        if (delta < 0 && isPrimary) {
            // Never collide on the primary track either: limit is checked by callers.
        }
        for (auto& c : t->clips) {
            if (std::find(ignore.begin(), ignore.end(), c.id) != ignore.end()) continue;
            if (c.start >= at) c.start += delta;
        }
        normalize(*t);
    }
}

// ---------------------------------------------------------------------------
// Edits

Result overwrite(Project& p, Sequence& s, TrackRef r, Clip clip) {
    Track* t = trackAt(s, r);
    if (!t) return Result::fail("No such track");
    if (t->locked) return Result::fail("Track is locked");
    if (clip.start < 0) return Result::fail("Clip would start before 0");
    if (clip.id == 0) clip.id = p.newId();
    clearRange(p, *t, clip.start, clip.end());
    Id id = clip.id;
    t->clips.push_back(std::move(clip));
    normalize(*t);
    Result res;
    res.created.push_back(id);
    return res;
}

Result insert(Project& p, Sequence& s, TrackRef r, Clip clip) {
    Track* t = trackAt(s, r);
    if (!t) return Result::fail("No such track");
    if (t->locked) return Result::fail("Track is locked");
    rippleOpen(p, s, clip.start, clip.duration, rippleTracks(s, {r}));
    return overwrite(p, s, r, std::move(clip));
}

Result placeMedia(Project& p, Sequence& s, Id mediaId, FrameTime at, double srcIn, double srcOut,
                  TrackRef videoTrack, TrackRef audioTrack, bool insertMode) {
    const MediaItem* m = p.findMedia(mediaId);
    if (!m) return Result::fail("Unknown media");
    bool wantVideo = m->hasVideo || m->kind == MediaKind::Image || m->kind == MediaKind::Sequence;
    bool wantAudio = m->hasAudio && m->kind != MediaKind::Image;
    if (m->kind == MediaKind::Sequence) {
        const Sequence* nested = p.findSequence(m->sequenceId);
        wantAudio = nested && std::any_of(nested->audioTracks.begin(), nested->audioTracks.end(),
                                          [](const Track& t) { return !t.clips.empty(); });
    }
    Track* vt = trackAt(s, videoTrack);
    Track* at_ = trackAt(s, audioTrack);
    if (wantVideo && !editable(vt)) wantVideo = false;
    if (wantAudio && !editable(at_)) wantAudio = false;
    if (!wantVideo && !wantAudio) return Result::fail("No unlocked target track for this media");

    Clip base = makeClip(p, *m, TrackKind::Video, s);
    FrameTime limit = sourceLimit(p, s, base);
    srcIn = std::max(0.0, srcIn);
    if (srcOut < 0 || srcOut > double(limit)) srcOut = limit >= kInfiniteFrames ? srcIn + double(base.duration) : double(limit);
    FrameTime len = std::max<FrameTime>(1, FrameTime(std::llround(srcOut - srcIn)));
    base.sourceIn = srcIn;
    base.duration = len;
    base.start = std::max<FrameTime>(0, at);

    std::vector<TrackRef> targets;
    if (wantVideo) targets.push_back(videoTrack);
    if (wantAudio) targets.push_back(audioTrack);
    if (insertMode) rippleOpen(p, s, base.start, len, rippleTracks(s, targets));

    Result res;
    Id group = (wantVideo && wantAudio) ? p.newId() : 0;
    if (wantVideo) {
        Clip v = base;
        v.id = p.newId();
        v.linkGroup = group;
        Result r = overwrite(p, s, videoTrack, v);
        res.created.insert(res.created.end(), r.created.begin(), r.created.end());
    }
    if (wantAudio) {
        Clip a = base;
        a.id = p.newId();
        a.linkGroup = group;
        Result r = overwrite(p, s, audioTrack, a);
        res.created.insert(res.created.end(), r.created.begin(), r.created.end());
    }
    return res;
}

// A dry run of placing the media range on new tracks of a copy: how long the edit is and which kinds it puts down.
namespace {
struct Placed {
    Result result;
    FrameTime length = 1;
    bool video = false, audio = false;
};
Placed dryPlace(const Project& p, const Sequence& s, Id mediaId, FrameTime at, double srcIn, double srcOut) {
    Project tp = p;
    Sequence ts = s;
    const TrackRef dv = addTrack(tp, ts, TrackKind::Video), da = addTrack(tp, ts, TrackKind::Audio);
    Placed out;
    out.result = placeMedia(tp, ts, mediaId, std::max<FrameTime>(0, at), srcIn, srcOut, dv, da, false);
    for (Id id : out.result.created)
        if (const Clip* c = clipById(ts, id)) out.length = std::max(out.length, c->duration);
    out.video = !trackAt(ts, dv)->clips.empty();
    out.audio = !trackAt(ts, da)->clips.empty();
    return out;
}
}  // namespace

Result placeOnTop(Project& p, Sequence& s, Id mediaId, FrameTime at, double srcIn, double srcOut, TrackRef videoTrack,
                  TrackRef audioTrack) {
    const Placed dry = dryPlace(p, s, mediaId, at, srcIn, srcOut);
    if (!dry.result.ok) return dry.result;
    at = std::max<FrameTime>(0, at);
    auto freeTrack = [&](TrackKind kind, int from) {
        const auto& list = listFor(s, kind);
        for (int i = std::max(0, from); i < int(list.size()); ++i)
            if (!list[size_t(i)].locked && trackEmpty(list[size_t(i)], at, at + dry.length)) return TrackRef{kind, i};
        return addTrack(p, s, kind);
    };
    if (dry.video) videoTrack = freeTrack(TrackKind::Video, videoTrack.index + 1);
    if (dry.audio) audioTrack = freeTrack(TrackKind::Audio, audioTrack.index);
    return placeMedia(p, s, mediaId, at, srcIn, srcOut, videoTrack, audioTrack, false);
}

Result rippleOverwrite(Project& p, Sequence& s, Id clipId, Id mediaId, double srcIn, double srcOut, TrackRef videoTrack,
                       TrackRef audioTrack) {
    const Clip* c = clipById(s, clipId);
    if (!c) return Result::fail("Unknown clip");
    const FrameTime a = c->start, b = c->end();
    const Placed dry = dryPlace(p, s, mediaId, a, srcIn, srcOut);
    if (!dry.result.ok) return dry.result;
    // The replaced clips' tracks close up or open by the difference after the old end; other sync-locked tracks
    // follow where they can (a clip across the old end stays put).
    const std::vector<Id> replaced = linkedClips(s, clipId);
    std::vector<TrackRef> primary;
    for (Id id : replaced)
        if (auto loc = locate(s, id); loc && std::find(primary.begin(), primary.end(), loc->track) == primary.end())
            primary.push_back(loc->track);
    Result r = removeClips(p, s, replaced, false);
    if (!r.ok) return r;
    rippleShift(s, rippleTracks(s, primary), primary, b, dry.length - (b - a));
    return placeMedia(p, s, mediaId, a, srcIn, srcOut, videoTrack, audioTrack, false);
}

const std::vector<std::string> kStandardRoles{"Dialogue", "Music", "Effects"};

std::vector<std::string> sequenceRoles(const Sequence& s) {
    std::vector<std::string> out = kStandardRoles;
    for (const Track& t : s.audioTracks)
        for (const Clip& c : t.clips)
            if (!c.role.empty() && std::find(out.begin(), out.end(), c.role) == out.end()) out.push_back(c.role);
    for (const std::string& r : s.mutedRoles)
        if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
    return out;
}

int setClipRole(Sequence& s, const std::vector<Id>& clips, const std::string& role) {
    std::vector<Id> ids;
    for (Id id : clips)
        for (Id l : linkedClips(s, id))
            if (std::find(ids.begin(), ids.end(), l) == ids.end()) ids.push_back(l);
    int changed = 0;
    for (Track& t : s.audioTracks)
        for (Clip& c : t.clips)
            if (std::find(ids.begin(), ids.end(), c.id) != ids.end() && c.role != role) {
                c.role = role;
                ++changed;
            }
    return changed;
}

bool roleMuted(const Sequence& s, const std::string& role) {
    return !role.empty() && std::find(s.mutedRoles.begin(), s.mutedRoles.end(), role) != s.mutedRoles.end();
}

void setRoleMuted(Sequence& s, const std::string& role, bool muted) {
    if (role.empty() || roleMuted(s, role) == muted) return;
    if (muted)
        s.mutedRoles.push_back(role);
    else
        std::erase(s.mutedRoles, role);
}

std::vector<FrameUse> sourceFrameUses(const Sequence& s, Id mediaId, double srcFrame) {
    std::vector<FrameUse> out;
    for (TrackRef r : allTracks(s)) {
        const Track* t = trackAt(s, r);
        for (const Clip& c : t->clips) {
            if (!c.enabled || c.mediaId != mediaId || c.isGenerator()) continue;
            if (!c.ramped()) {
                // A straight clip plays a straight stretch of the source: skip it when the frame is outside.
                double a = c.sourceFrameAt(c.start), b = c.sourceFrameAt(c.end() - 1);
                if (a > b) std::swap(a, b);
                if (srcFrame < a - 1 || srcFrame > b + 1 + c.speed) continue;
            }
            FrameTime best = -1;
            double bestDist = 1e300, step = 1;
            for (FrameTime f = c.start; f < c.end(); ++f) {
                const double here = c.sourceFrameAt(f), d = std::fabs(here - srcFrame);
                if (d < bestDist) {
                    bestDist = d;
                    best = f;
                    step = std::max(1.0, std::fabs(c.sourceFrameAt(f + 1) - here));
                }
            }
            if (best >= 0 && bestDist < step) out.push_back({c.id, r, best});
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const FrameUse& a, const FrameUse& b) { return a.at < b.at; });
    return out;
}

namespace {
// `t` in frames of `s`; returns the media and its frame in frames of `s` (times `toTop` for the top sequence).
std::optional<SourceMatch> matchSourceIn(const Project& p, const Sequence& s, double t, int depth, int onlyTrack) {
    if (depth > 8) return std::nullopt;
    for (int i = int(s.videoTracks.size()) - 1; i >= 0; --i) {
        if (onlyTrack >= 0 ? i != onlyTrack : s.videoTracks[size_t(i)].muted) continue;
        const Clip* c = clipAt(s, {TrackKind::Video, i}, FrameTime(std::floor(t + 1e-6)));
        if (!c || !c->enabled || c->isGenerator()) continue;
        const MediaItem* m = p.findMedia(c->mediaId);
        if (!m) continue;
        const double src = c->sourceFrameAt(FrameTime(std::floor(t + 1e-6)));
        if (m->kind != MediaKind::Sequence) return SourceMatch{m->id, src, c->id};
        const Sequence* nested = p.findSequence(m->sequenceId);
        if (!nested || nested->id == s.id) continue;
        // Into the nested sequence, at its own frame rate; a multicam only through the angle shown.
        const double k = nested->fpsValue() / s.fpsValue();
        const int angle = nested->multicam ? std::clamp(c->angle, 0, std::max(0, int(nested->videoTracks.size()) - 1)) : -1;
        if (auto inner = matchSourceIn(p, *nested, src * k, depth + 1, angle)) {
            inner->frame /= k;
            inner->clip = c->id;
            return inner;
        }
    }
    return std::nullopt;
}
}  // namespace

std::optional<SourceMatch> matchSource(const Project& p, const Sequence& s, FrameTime t) {
    return matchSourceIn(p, s, double(t), 0, -1);
}

FrameTime nearestEdit(const Sequence& s, TrackRef t, FrameTime frame) {
    FrameTime best = frame, dist = std::numeric_limits<FrameTime>::max();
    if (const Track* tr = trackAt(s, t))
        for (const Clip& c : tr->clips)
            for (FrameTime e : {c.start, c.end()})
                if (std::llabs(e - frame) < dist) dist = std::llabs(e - frame), best = e;
    return best;
}

Result razor(Project& p, Sequence& s, TrackRef r, FrameTime frame) {
    Track* t = trackAt(s, r);
    if (!editable(t)) return Result::fail("Track is locked");
    for (size_t i = 0; i < t->clips.size(); ++i) {
        if (t->clips[i].start < frame && t->clips[i].end() > frame) {
            Id right = splitClip(p, *t, i, frame);
            normalize(*t);
            Result res;
            res.created.push_back(right);
            return res;
        }
    }
    return Result::fail("No clip under the playhead");
}

Result razorAll(Project& p, Sequence& s, FrameTime frame) {
    Result res;
    // Splitting linked clips together: right halves get a fresh shared group.
    std::map<Id, Id> regroup;
    for (TrackRef r : allTracks(s)) {
        Track* t = trackAt(s, r);
        if (!editable(t)) continue;
        for (size_t i = 0; i < t->clips.size(); ++i) {
            if (t->clips[i].start < frame && t->clips[i].end() > frame) {
                Id group = t->clips[i].linkGroup;
                Id right = splitClip(p, *t, i, frame);
                if (group) {
                    auto it = regroup.find(group);
                    if (it == regroup.end()) it = regroup.emplace(group, p.newId()).first;
                    for (auto& c : t->clips)
                        if (c.id == right) c.linkGroup = it->second;
                }
                res.created.push_back(right);
                break;
            }
        }
        normalize(*t);
    }
    if (res.created.empty()) return Result::fail("No clips under the playhead");
    return res;
}

Result removeClips(Project& p, Sequence& s, const std::vector<Id>& ids, bool ripple) {
    (void)p;
    struct Removed {
        TrackRef track;
        FrameTime a, b;
    };
    std::vector<Removed> removed;
    for (Id id : ids) {
        auto loc = locate(s, id);
        if (!loc) continue;
        Track* t = trackAt(s, loc->track);
        if (t->locked) continue;
        const Clip& c = t->clips[loc->index];
        removed.push_back({loc->track, c.start, c.end()});
        t->clips.erase(t->clips.begin() + long(loc->index));
        normalize(*t);
    }
    if (removed.empty()) return Result::fail("Nothing to delete");
    if (!ripple) return {};

    // Merge removed intervals and close each gap, right to left.
    std::vector<std::pair<FrameTime, FrameTime>> intervals;
    for (const auto& r : removed) intervals.push_back({r.a, r.b});
    std::sort(intervals.begin(), intervals.end());
    std::vector<std::pair<FrameTime, FrameTime>> merged;
    for (const auto& iv : intervals) {
        if (!merged.empty() && iv.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, iv.second);
        else
            merged.push_back(iv);
    }
    std::vector<TrackRef> primary;
    for (const auto& r : removed)
        if (std::find(primary.begin(), primary.end(), r.track) == primary.end()) primary.push_back(r.track);
    for (auto it = merged.rbegin(); it != merged.rend(); ++it) {
        auto [a, b] = *it;
        // Every affected track moves by the same amount so linked clips stay in
        // sync: the gap length, limited by the free space on each primary track
        // (linked clips of different lengths leave different gaps).
        FrameTime len = b - a;
        for (TrackRef r : primary) {
            const Track* t = trackAt(s, r);
            for (const auto& c : t->clips)
                if (c.end() > a) len = std::min(len, std::max<FrameTime>(0, c.start - a));
        }
        if (len <= 0) continue;
        for (TrackRef r : rippleTracks(s, primary)) {
            Track* t = trackAt(s, r);
            if (!editable(t) || !trackEmpty(*t, a, a + len)) continue;
            for (auto& c : t->clips)
                if (c.start >= a) c.start -= len;
            normalize(*t);
        }
    }
    return {};
}

Result moveClips(Project& p, Sequence& s, const std::vector<Id>& ids, FrameTime delta, int videoTrackDelta,
                 int audioTrackDelta, bool insertMode) {
    struct Moving {
        TrackRef from, to;
        Clip clip;
    };
    std::vector<Moving> moving;
    for (Id id : ids) {
        auto loc = locate(s, id);
        if (!loc) continue;
        Track* t = trackAt(s, loc->track);
        if (t->locked) return Result::fail("Clip is on a locked track");
        Moving m;
        m.from = loc->track;
        m.to = loc->track;
        m.to.index += loc->track.kind == TrackKind::Video ? videoTrackDelta : audioTrackDelta;
        m.clip = t->clips[loc->index];
        Track* dest = trackAt(s, m.to);
        if (!dest) return Result::fail("Target track does not exist");
        if (dest->locked) return Result::fail("Target track is locked");
        if (m.clip.start + delta < 0) delta = -m.clip.start;
        moving.push_back(std::move(m));
    }
    if (moving.empty()) return Result::fail("Nothing to move");
    for (const auto& m : moving)
        if (m.clip.start + delta < 0) return Result::fail("Clips would move before 0");

    // Carry transitions whose clips all move to the same destination track.
    std::vector<std::pair<TrackRef, Transition>> carried;
    std::set<Id> movingIds;
    for (const auto& m : moving) movingIds.insert(m.clip.id);
    for (const auto& m : moving) {
        Track* t = trackAt(s, m.from);
        for (const auto& tr : t->transitions) {
            bool aOk = tr.clipA == 0 || movingIds.count(tr.clipA);
            bool bOk = tr.clipB == 0 || movingIds.count(tr.clipB);
            bool involves = tr.clipA == m.clip.id || tr.clipB == m.clip.id;
            if (involves && aOk && bOk) {
                bool already = std::any_of(carried.begin(), carried.end(),
                                           [&](const auto& c) { return c.second.id == tr.id; });
                if (!already) carried.push_back({m.to, tr});
            }
        }
    }

    // Lift the clips out.
    for (const auto& m : moving) {
        Track* t = trackAt(s, m.from);
        t->clips.erase(std::remove_if(t->clips.begin(), t->clips.end(), [&](const Clip& c) { return c.id == m.clip.id; }),
                       t->clips.end());
        normalize(*t);
    }

    if (insertMode) {
        FrameTime lo = std::numeric_limits<FrameTime>::max(), hi = 0;
        std::vector<TrackRef> dests;
        for (const auto& m : moving) {
            lo = std::min(lo, m.clip.start + delta);
            hi = std::max(hi, m.clip.end() + delta);
            if (std::find(dests.begin(), dests.end(), m.to) == dests.end()) dests.push_back(m.to);
        }
        rippleOpen(p, s, lo, hi - lo, rippleTracks(s, dests));
    }

    for (auto& m : moving) {
        m.clip.start += delta;
        Track* dest = trackAt(s, m.to);
        clearRange(p, *dest, m.clip.start, m.clip.end());
        dest->clips.push_back(m.clip);
        normalize(*dest);
    }
    for (auto& [ref, tr] : carried) {
        Track* dest = trackAt(s, ref);
        dest->transitions.push_back(tr);
        normalize(*dest);
    }
    Result res;
    res.applied = delta;
    return res;
}

namespace {

// Clamp a trim delta so the clip stays within its source and neighbours.
FrameTime clampTrim(const Project& p, const Sequence& s, const Track& t, size_t idx, Edge edge, FrameTime delta,
                    TrimMode mode) {
    const Clip& c = t.clips[idx];
    FrameTime limit = sourceLimit(p, s, c);
    double lim = double(limit);
    if (edge == Edge::Out) {
        // New duration must be >= 1.
        delta = std::max<FrameTime>(delta, 1 - c.duration);
        if (limit < kInfiniteFrames) {
            if (!c.reverse) {
                // A held frame (0 % speed) uses no more source however long it gets.
                const double sp = c.speedAt(double(c.duration));
                if (sp > 1e-9) delta = std::min<FrameTime>(delta, FrameTime(std::floor((lim - srcHigh(c)) / sp + 1e-6)));
            } else {
                double maxExtra = srcLow(c) / c.speed;
                delta = std::min<FrameTime>(delta, FrameTime(std::floor(maxExtra + 1e-6)));
            }
        }
        if (mode == TrimMode::Normal && delta > 0 && idx + 1 < t.clips.size())
            delta = std::min<FrameTime>(delta, t.clips[idx + 1].start - c.end());
    } else {
        // In edge: positive delta shortens the clip.
        delta = std::min<FrameTime>(delta, c.duration - 1);
        if (limit < kInfiniteFrames || !c.reverse) {
            if (!c.reverse) {
                const double sp = c.speedAt(0);  // how far we can extend backwards (held frames: any way)
                if (sp > 1e-9) delta = std::max<FrameTime>(delta, -FrameTime(std::floor(srcLow(c) / sp + 1e-6)));
            } else {
                double maxBack = (lim - srcHigh(c)) / c.speed;
                delta = std::max<FrameTime>(delta, -FrameTime(std::floor(maxBack + 1e-6)));
            }
        }
        if (mode == TrimMode::Normal && delta < 0) {
            FrameTime prevEnd = idx > 0 ? t.clips[idx - 1].end() : 0;
            delta = std::max<FrameTime>(delta, prevEnd - c.start);
        }
    }
    return delta;
}

void applyTrim(Clip& c, Edge edge, FrameTime delta, TrimMode mode) {
    if (edge == Edge::Out) {
        if (c.reverse) c.sourceIn -= double(delta) * c.speed;
        c.duration += delta;
    } else {
        if (!c.reverse) c.sourceIn += c.sourceOffset(double(delta));
        c.duration -= delta;
        if (mode == TrimMode::Normal) c.start += delta;
        shiftKeyframes(c, -delta);
    }
}

}  // namespace

Result trim(Project& p, Sequence& s, Id clipId, Edge edge, FrameTime delta, TrimMode mode, bool includeLinked) {
    auto loc = locate(s, clipId);
    if (!loc) return Result::fail("Unknown clip");
    Track* t = trackAt(s, loc->track);
    if (t->locked) return Result::fail("Track is locked");
    const Clip& primaryClip = t->clips[loc->index];
    FrameTime edgePos = edge == Edge::In ? primaryClip.start : primaryClip.end();

    // Gather the clips to trim together (linked partners sharing this edge).
    std::vector<Id> group{clipId};
    if (includeLinked && primaryClip.linkGroup) {
        for (Id l : linkedClips(s, clipId)) {
            if (l == clipId) continue;
            const Clip* lc = clipById(s, l);
            FrameTime pos = edge == Edge::In ? lc->start : lc->end();
            if (pos == edgePos) group.push_back(l);
        }
    }
    // Clamp the delta across the group.
    for (Id id : group) {
        auto l = locate(s, id);
        Track* lt = trackAt(s, l->track);
        if (lt->locked) continue;
        delta = clampTrim(p, s, *lt, l->index, edge, delta, mode);
    }
    if (delta == 0) {
        Result r;
        r.applied = 0;
        return r;
    }

    std::vector<TrackRef> primary;
    std::map<TrackRef, FrameTime> oldEnd;  // per primary track: where following material starts
    const FrameTime primaryOldEnd = primaryClip.end();
    for (Id id : group) {
        auto l = locate(s, id);
        Track* lt = trackAt(s, l->track);
        if (lt->locked) continue;
        if (std::find(primary.begin(), primary.end(), l->track) == primary.end()) primary.push_back(l->track);
        oldEnd[l->track] = lt->clips[l->index].end();
        applyTrim(lt->clips[l->index], edge, delta, mode);
    }

    if (mode == TrimMode::Ripple) {
        // Out edge: following material shifts by delta. In edge: the clip
        // stays put and following material shifts by -delta. Each track
        // shifts from its own trimmed clip's old end (linked partners may end
        // at different frames); other sync-locked tracks follow the clicked clip.
        FrameTime shift = edge == Edge::Out ? delta : -delta;
        for (TrackRef r : rippleTracks(s, primary)) {
            auto it = oldEnd.find(r);
            bool isPrimary = it != oldEnd.end();
            FrameTime at = isPrimary ? it->second : primaryOldEnd;
            rippleShift(s, {r}, isPrimary ? std::vector<TrackRef>{r} : std::vector<TrackRef>{}, at, shift, group);
        }
    }
    for (TrackRef r : primary) normalize(*trackAt(s, r));
    Result res;
    res.applied = delta;
    return res;
}

Result roll(Project& p, Sequence& s, Id leftClip, Id rightClip, FrameTime delta) {
    auto la = locate(s, leftClip);
    auto lb = locate(s, rightClip);
    if (!la || !lb || !(la->track == lb->track)) return Result::fail("Roll needs two clips on one track");
    Track* t = trackAt(s, la->track);
    if (t->locked) return Result::fail("Track is locked");
    Clip& a = t->clips[la->index];
    Clip& b = t->clips[lb->index];
    if (a.end() != b.start) return Result::fail("Clips are not adjacent");
    delta = clampTrim(p, s, *t, la->index, Edge::Out, delta, TrimMode::Ripple);
    delta = clampTrim(p, s, *t, lb->index, Edge::In, delta, TrimMode::Ripple);
    applyTrim(a, Edge::Out, delta, TrimMode::Normal);
    applyTrim(b, Edge::In, delta, TrimMode::Normal);
    normalize(*t);
    Result r;
    r.applied = delta;
    return r;
}

Result slip(Project& p, Sequence& s, Id clipId, FrameTime delta) {
    auto loc = locate(s, clipId);
    if (!loc) return Result::fail("Unknown clip");
    Track* t = trackAt(s, loc->track);
    if (t->locked) return Result::fail("Track is locked");
    Result res;
    for (Id id : linkedClips(s, clipId)) {
        Clip* c = clipById(s, id);
        FrameTime limit = sourceLimit(p, s, *c);
        const double sp = c->speedAt(0) > 1e-9 ? c->speedAt(0) : 1.0;  // a held frame slips frame by frame
        double shift = double(delta) * sp;
        if (limit < kInfiniteFrames) shift = std::clamp(shift, -c->sourceIn, double(limit) - srcHigh(*c));
        else shift = std::max(shift, -c->sourceIn);
        c->sourceIn += shift;
        if (id == clipId) res.applied = FrameTime(std::llround(shift / sp));
    }
    return res;
}

Result slide(Project& p, Sequence& s, Id clipId, FrameTime delta) {
    auto loc = locate(s, clipId);
    if (!loc) return Result::fail("Unknown clip");
    Track* t = trackAt(s, loc->track);
    if (t->locked) return Result::fail("Track is locked");
    size_t i = loc->index;
    Clip& c = t->clips[i];
    bool hasPrev = i > 0 && t->clips[i - 1].end() == c.start;
    bool hasNext = i + 1 < t->clips.size() && t->clips[i + 1].start == c.end();
    if (hasPrev)
        delta = clampTrim(p, s, *t, i - 1, Edge::Out, delta, TrimMode::Ripple);
    else {
        FrameTime prevEnd = i > 0 ? t->clips[i - 1].end() : 0;
        delta = std::max(delta, prevEnd - c.start);
    }
    if (hasNext)
        delta = clampTrim(p, s, *t, i + 1, Edge::In, delta, TrimMode::Ripple);
    else {
        FrameTime nextStart = i + 1 < t->clips.size() ? t->clips[i + 1].start : std::numeric_limits<FrameTime>::max() / 4;
        delta = std::min(delta, nextStart - c.end());
    }
    if (hasPrev) applyTrim(t->clips[i - 1], Edge::Out, delta, TrimMode::Normal);
    if (hasNext) applyTrim(t->clips[i + 1], Edge::In, delta, TrimMode::Normal);
    t->clips[i].start += delta;
    normalize(*t);
    Result r;
    r.applied = delta;
    return r;
}

Result setSpeed(Project& p, Sequence& s, Id clipId, double speed, bool ripple, bool reverse, bool includeLinked) {
    if (!(speed > 0.001) || speed > 100) return Result::fail("Speed out of range");
    auto loc = locate(s, clipId);
    if (!loc) return Result::fail("Unknown clip");
    if (trackAt(s, loc->track)->locked) return Result::fail("Track is locked");
    const Clip& primaryClip = trackAt(s, loc->track)->clips[loc->index];
    // Linked partners starting with the clip change speed with it, and the
    // ripple is applied once per track (not once per partner).
    std::vector<Id> group{clipId};
    if (includeLinked && primaryClip.linkGroup)
        for (Id l : linkedClips(s, clipId))
            if (l != clipId)
                if (const Clip* lc = clipById(s, l); lc && lc->start == primaryClip.start) group.push_back(l);
    struct Change {
        TrackRef track;
        Id id;
        FrameTime oldEnd, newDur;
    };
    std::vector<Change> changes;
    for (Id id : group) {
        auto l = locate(s, id);
        Track* t = trackAt(s, l->track);
        if (t->locked) continue;
        const Clip& c = t->clips[l->index];
        FrameTime newDur = std::max<FrameTime>(1, FrameTime(std::llround(c.sourceExtent() / speed)));
        FrameTime limit = sourceLimit(p, s, c);
        if (limit < kInfiniteFrames)
            newDur = std::min<FrameTime>(newDur, std::max<FrameTime>(1, FrameTime((double(limit) - c.sourceIn) / speed)));
        if (!ripple && l->index + 1 < t->clips.size())
            newDur = std::min<FrameTime>(newDur, t->clips[l->index + 1].start - c.start);
        changes.push_back({l->track, id, c.end(), newDur});
    }
    std::map<TrackRef, FrameTime> growBy, endBy;
    std::vector<TrackRef> primary;
    for (const auto& ch : changes) {
        Clip* c = clipById(s, ch.id);
        growBy[ch.track] = ch.newDur - c->duration;
        endBy[ch.track] = ch.oldEnd;
        primary.push_back(ch.track);
        c->speed = speed;
        c->reverse = reverse;
        c->duration = ch.newDur;
    }
    if (ripple) {
        FrameTime primaryGrow = growBy.count(loc->track) ? growBy[loc->track] : 0;
        for (TrackRef r : rippleTracks(s, primary)) {
            bool isPrimary = growBy.count(r) > 0;
            FrameTime grow = isPrimary ? growBy[r] : primaryGrow;
            FrameTime at = isPrimary ? endBy[r] : (endBy.count(loc->track) ? endBy[loc->track] : 0);
            if (grow != 0)
                rippleShift(s, {r}, isPrimary ? std::vector<TrackRef>{r} : std::vector<TrackRef>{}, at, grow, group);
        }
    }
    for (TrackRef r : primary) normalize(*trackAt(s, r));
    return {};
}

bool setMaintainPitch(Project& p, Sequence& s, Id clipId, bool on) {
    if (!clipById(s, clipId)) return false;
    bool changed = false;
    for (Id id : linkedClips(s, clipId)) {
        Clip* c = clipById(s, id);
        if (!c || c->isGenerator()) continue;
        if (c->timing.empty()) c->timing = makeEffect(p, "time");
        const double v = on ? 1.0 : 0.0;
        if (c->timing.p("maintain_pitch", 0) == v) continue;
        c->timing.params["maintain_pitch"] = Param(v);
        changed = true;
    }
    return changed;
}

Result closeGap(Project& p, Sequence& s, TrackRef r, FrameTime frame) {
    (void)p;
    Track* t = trackAt(s, r);
    if (!editable(t)) return Result::fail("Track is locked");
    FrameTime gapStart = 0;
    FrameTime gapEnd = -1;
    for (const auto& c : t->clips) {
        if (c.start > frame) {
            gapEnd = c.start;
            break;
        }
        gapStart = std::max(gapStart, c.end());
    }
    if (gapEnd < 0 || frame < gapStart || gapEnd <= gapStart) return Result::fail("No gap here");
    FrameTime len = gapEnd - gapStart;
    for (TrackRef tr : rippleTracks(s, {r})) {
        Track* other = trackAt(s, tr);
        if (!editable(other)) continue;
        if (!(tr == r) && !trackEmpty(*other, gapStart, gapEnd)) continue;
        for (auto& c : other->clips)
            if (c.start >= gapEnd) c.start -= len;
        normalize(*other);
    }
    return {};
}

Result insertGap(Project& p, Sequence& s, TrackRef r, FrameTime at, FrameTime length) {
    const Track* t = trackAt(s, r);
    if (!t) return Result::fail("No such track");
    if (!editable(t)) return Result::fail("Track is locked");
    if (length <= 0) return Result::fail("No gap to paste");
    rippleOpen(p, s, std::max<FrameTime>(0, at), length, rippleTracks(s, {r}));
    return {};
}

std::vector<Id> soloedTracks(const Sequence& s) {
    std::vector<Id> out;
    for (TrackRef r : allTracks(s))
        if (const Track* t = trackAt(s, r); t && t->solo) out.push_back(t->id);
    return out;
}

Result setSoloedTracks(Sequence& s, const std::vector<Id>& tracks) {
    bool changed = false;
    for (TrackRef r : allTracks(s)) {
        Track* t = trackAt(s, r);
        const bool on = std::find(tracks.begin(), tracks.end(), t->id) != tracks.end();
        if (t->solo != on) t->solo = on, changed = true;
    }
    if (!changed) return Result::fail(tracks.empty() ? "No track is soloed" : "Those tracks are soloed already");
    return {};
}

Result liftRange(Project& p, Sequence& s, FrameTime a, FrameTime b, const std::vector<TrackRef>& tracks) {
    if (b <= a) return Result::fail("Set In and Out points first");
    for (TrackRef r : tracks) {
        Track* t = trackAt(s, r);
        if (editable(t)) clearRange(p, *t, a, b);
    }
    return {};
}

Result extractRange(Project& p, Sequence& s, FrameTime a, FrameTime b, const std::vector<TrackRef>& tracks) {
    if (b <= a) return Result::fail("Set In and Out points first");
    for (TrackRef r : tracks) {
        Track* t = trackAt(s, r);
        if (!editable(t)) continue;
        clearRange(p, *t, a, b);
        for (auto& c : t->clips)
            if (c.start >= b) c.start -= (b - a);
        normalize(*t);
    }
    return {};
}

Result linkClips(Project& p, Sequence& s, const std::vector<Id>& ids) {
    if (ids.size() < 2) return Result::fail("Select at least two clips");
    Id group = p.newId();
    for (Id id : ids)
        if (Clip* c = clipById(s, id)) c->linkGroup = group;
    return {};
}

Result unlinkClips(Sequence& s, const std::vector<Id>& ids) {
    for (Id id : expandLinks(s, ids))
        if (Clip* c = clipById(s, id)) c->linkGroup = 0;
    return {};
}

std::vector<ClipboardItem> copyClips(const Sequence& s, const std::vector<Id>& ids) {
    std::vector<ClipboardItem> items;
    FrameTime minStart = std::numeric_limits<FrameTime>::max();
    for (Id id : ids) {
        auto loc = locate(s, id);
        if (!loc) continue;
        const Clip& c = trackAt(s, loc->track)->clips[loc->index];
        items.push_back({loc->track, c});
        minStart = std::min(minStart, c.start);
    }
    for (auto& it : items) it.clip.start -= minStart;
    return items;
}

Result pasteClips(Project& p, Sequence& s, const std::vector<ClipboardItem>& items, FrameTime at, bool insertMode) {
    if (items.empty()) return Result::fail("Clipboard is empty");
    std::map<Id, Id> groups;
    FrameTime len = 0;
    std::vector<TrackRef> targets;
    for (const auto& it : items) {
        len = std::max(len, it.clip.end());
        if (!trackAt(s, it.track)) return Result::fail("Target track missing");
        if (std::find(targets.begin(), targets.end(), it.track) == targets.end()) targets.push_back(it.track);
    }
    if (insertMode) rippleOpen(p, s, at, len, rippleTracks(s, targets));
    Result res;
    for (const auto& it : items) {
        Clip c = it.clip;
        c.id = p.newId();
        c.start += at;
        if (c.linkGroup) {
            auto g = groups.find(c.linkGroup);
            if (g == groups.end()) g = groups.emplace(c.linkGroup, p.newId()).first;
            c.linkGroup = g->second;
        }
        for (auto& e : c.effects) e.id = p.newId();
        Result r = overwrite(p, s, it.track, c);
        res.created.insert(res.created.end(), r.created.begin(), r.created.end());
    }
    return res;
}

Result duplicateClips(Project& p, Sequence& s, const std::vector<Id>& ids, FrameTime at) {
    return pasteClips(p, s, copyClips(s, expandLinks(s, ids)), at, false);
}

// ---------------------------------------------------------------------------
// Transitions

Result addTransition(Project& p, Sequence& s, Id clipId, Edge edge, const std::string& type, FrameTime duration) {
    auto loc = locate(s, clipId);
    if (!loc) return Result::fail("Unknown clip");
    Track* t = trackAt(s, loc->track);
    if (t->locked) return Result::fail("Track is locked");
    const Clip& c = t->clips[loc->index];
    Transition tr;
    tr.id = p.newId();
    tr.duration = std::max<FrameTime>(2, duration);
    std::string ty = type;
    const EffectInfo* info = findEffectInfo(ty);
    bool audio = loc->track.kind == TrackKind::Audio;
    if (audio && (!info || info->category != EffectCategory::AudioTransition)) ty = "crossfade";
    if (!audio && (!info || info->category != EffectCategory::VideoTransition)) ty = "cross_dissolve";
    tr.type = ty;
    tr.params = makeEffect(ty, p.newId());
    if (edge == Edge::Out) {
        tr.clipA = c.id;
        if (loc->index + 1 < t->clips.size() && t->clips[loc->index + 1].start == c.end())
            tr.clipB = t->clips[loc->index + 1].id;
    } else {
        tr.clipB = c.id;
        if (loc->index > 0 && t->clips[loc->index - 1].end() == c.start) tr.clipA = t->clips[loc->index - 1].id;
    }
    // Replace an existing transition at the same edit point.
    t->transitions.erase(std::remove_if(t->transitions.begin(), t->transitions.end(),
                                        [&](const Transition& o) { return o.clipA == tr.clipA && o.clipB == tr.clipB; }),
                         t->transitions.end());
    Id id = tr.id;
    t->transitions.push_back(std::move(tr));
    normalize(*t);
    Result r;
    r.created.push_back(id);
    return r;
}

FrameTime previousClipEdge(const Sequence& s, FrameTime frame) {
    FrameTime best = -1;
    for (const auto* list : {&s.videoTracks, &s.audioTracks})
        for (const Track& t : *list)
            for (const Clip& c : t.clips)
                for (FrameTime e : {c.start, c.end()})
                    if (e < frame) best = std::max(best, e);
    return best;
}

FrameTime nextClipEdge(const Sequence& s, FrameTime frame) {
    FrameTime best = -1;
    for (const auto* list : {&s.videoTracks, &s.audioTracks})
        for (const Track& t : *list)
            for (const Clip& c : t.clips)
                for (FrameTime e : {c.start, c.end()})
                    if (e > frame && (best < 0 || e < best)) best = e;
    return best;
}

Result rippleTrimToPlayhead(Project& p, Sequence& s, FrameTime frame, bool previous) {
    const FrameTime edit = previous ? previousClipEdge(s, frame) : nextClipEdge(s, frame);
    if (edit < 0) return Result::fail(previous ? "No edit before the playhead" : "No edit after the playhead");
    const FrameTime a = previous ? edit : frame, b = previous ? frame : edit;
    std::vector<TrackRef> tracks;
    for (TrackRef r : allTracks(s))
        if (editable(trackAt(s, r))) tracks.push_back(r);
    Result r = extractRange(p, s, a, b, tracks);
    if (r.ok) r.applied = b - a;
    return r;
}

namespace {

// Transform parameters that belong to Motion (not Opacity) in Paste Attributes.
bool isOpacityParam(const std::string& name) { return name == "opacity"; }

void copyEffectsOnto(Project& p, const std::vector<Effect>& from, std::vector<Effect>& to) {
    for (Effect e : from) {
        e.id = p.newId();
        to.push_back(std::move(e));
    }
}

}  // namespace

Result pasteAttributes(Project& p, Sequence& s, const Clip& from, TrackKind fromKind, const std::vector<Id>& to,
                       unsigned what) {
    Result res;
    for (Id id : to) {
        auto loc = locate(s, id);
        if (!loc || id == from.id) continue;
        Track* t = trackAt(s, loc->track);
        if (!editable(t)) continue;
        Clip& c = t->clips[loc->index];
        const bool video = loc->track.kind == TrackKind::Video;
        bool changed = false;
        if (video && (what & AttrMotion)) {
            const Id keep = c.motion.id;
            for (const auto& [name, prm] : from.motion.params)
                if (!isOpacityParam(name)) c.motion.params[name] = prm;
            c.motion.id = keep;
            changed = true;
        }
        if (video && (what & AttrOpacity)) {
            auto it = from.motion.params.find("opacity");
            if (it != from.motion.params.end()) c.motion.params["opacity"] = it->second;
            else c.motion.params.erase("opacity");
            c.blendMode = from.blendMode;
            changed = true;
        }
        if (what & AttrTimeRemap) {
            const Id keep = c.timing.id ? c.timing.id : p.newId();
            c.timing = from.timing;
            c.timing.id = keep;
            changed = true;
        }
        if (!video && (what & AttrVolume)) {
            const Id keep = c.audio.id;
            c.audio.params = from.audio.params;
            c.audio.id = keep;
            changed = true;
        }
        if ((what & AttrEffects) && loc->track.kind == fromKind && !from.effects.empty()) {
            copyEffectsOnto(p, from.effects, c.effects);
            changed = true;
        }
        if (changed) res.created.push_back(id);
    }
    if (res.created.empty()) return Result::fail("Nothing to paste onto");
    res.applied = FrameTime(res.created.size());
    res.created.clear();
    return res;
}

Result removeAttributes(Project& p, Sequence& s, const std::vector<Id>& ids, unsigned what) {
    Result res;
    for (Id id : ids) {
        auto loc = locate(s, id);
        if (!loc) continue;
        Track* t = trackAt(s, loc->track);
        if (!editable(t)) continue;
        Clip& c = t->clips[loc->index];
        const bool video = loc->track.kind == TrackKind::Video;
        if (video && (what & AttrMotion)) {
            const Effect fresh = makeEffect("transform", c.motion.id);
            for (auto it = c.motion.params.begin(); it != c.motion.params.end();)
                if (isOpacityParam(it->first)) ++it;
                else it = c.motion.params.erase(it);
            for (const auto& [name, prm] : fresh.params)
                if (!isOpacityParam(name)) c.motion.params[name] = prm;
        }
        if (video && (what & AttrOpacity)) {
            const Effect fresh = makeEffect("transform", 0);
            auto it = fresh.params.find("opacity");
            if (it != fresh.params.end()) c.motion.params["opacity"] = it->second;
            else c.motion.params.erase("opacity");
            c.blendMode = "normal";
        }
        if (what & AttrTimeRemap) c.timing = makeEffect("time", c.timing.id ? c.timing.id : p.newId());
        if (!video && (what & AttrVolume)) c.audio = makeEffect("volume", c.audio.id);
        if (what & AttrEffects) c.effects.clear();
        ++res.applied;
    }
    if (res.applied == 0) return Result::fail("No clips to change");
    return res;
}

Result addFrameHold(Project& p, Sequence& s, Id clipId, FrameTime frame) {
    auto loc = locate(s, clipId);
    if (!loc || loc->track.kind != TrackKind::Video) return Result::fail("Frame Hold needs a video clip");
    Track* t = trackAt(s, loc->track);
    if (!editable(t)) return Result::fail("Track is locked");
    const Clip& c = t->clips[loc->index];
    if (!c.contains(frame)) return Result::fail("The playhead is not over the clip");
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || m->kind != MediaKind::Video) return Result::fail("Frame Hold needs a video clip");
    Id held = clipId;
    if (frame > c.start) {
        held = splitClip(p, *t, loc->index, frame);
        normalize(*t);
    }
    Clip* h = clipById(s, held);
    if (!h) return Result::fail("Could not split the clip");
    // The frame on screen at the split, held: Time Remapping at 0 % from there.
    if (h->timing.empty()) h->timing = makeEffect("time", p.newId());
    h->reverse = false;
    h->timing.params["speed"] = Param(0.0);
    Result r;
    r.created.push_back(held);
    return r;
}

const std::vector<SpeedRampPreset>& speedRampPresets() {
    static const std::vector<SpeedRampPreset> presets = {
        {"montage", "Montage", "Quick, slow, quick, slow, quick: a rhythmic run of moments", {{0, 1.6}, {0.3, 0.4}, {0.55, 2.2}, {0.8, 0.4}, {1, 1.6}}},
        {"hero", "Hero", "Speeds up, lingers on the moment in the middle, speeds away", {{0, 1}, {0.3, 2.5}, {0.5, 0.2}, {0.7, 2.5}, {1, 1}}},
        {"bullet", "Bullet", "Fast in, almost stopped through the middle, fast out", {{0, 2.5}, {0.4, 0.12}, {0.6, 0.12}, {1, 2.5}}},
        {"jump_cut", "Jump Cut", "A sudden burst of speed in the middle", {{0, 1}, {0.4, 1}, {0.5, 5}, {0.6, 1}, {1, 1}}},
        {"flash_in", "Flash In", "Rushes in, then plays at an even pace", {{0, 5}, {0.35, 1}, {1, 1}}},
        {"flash_out", "Flash Out", "Plays at an even pace, then rushes away", {{0, 1}, {0.65, 1}, {1, 5}}},
        {"slow_in", "Ease into Slow Motion", "Slows gradually to a third of the pace", {{0, 1.6}, {1, 0.35}}},
        {"fast_out", "Ease out of Slow Motion", "Starts in slow motion and gathers pace", {{0, 0.35}, {1, 1.6}}},
    };
    return presets;
}

Result applySpeedRamp(Project& p, Sequence& s, Id clipId, const std::string& preset) {
    auto loc = locate(s, clipId);
    if (!loc) return Result::fail("Unknown clip");
    if (!editable(trackAt(s, loc->track))) return Result::fail("Track is locked");
    const Clip base = trackAt(s, loc->track)->clips[loc->index];
    if (base.isGenerator()) return Result::fail("Speed ramps need footage");
    if (base.reverse) return Result::fail("Speed ramps need a clip that plays forwards");
    Param curve(100.0);
    if (preset != "none") {
        const auto& all = speedRampPresets();
        auto it = std::find_if(all.begin(), all.end(), [&](const SpeedRampPreset& r) { return r.id == preset; });
        if (it == all.end()) return Result::fail("Unknown speed ramp \"" + preset + "\"");
        if (base.duration < 8) return Result::fail("The clip is too short to ramp");
        for (const auto& [u, v] : it->shape)
            curve.addKey(FrameTime(std::llround(u * double(base.duration - 1))), v * 100, Interp::Smooth);
        // The same footage in the same length: the curve scaled to play what the clip played before.
        Clip trial = base;
        if (trial.timing.empty()) trial.timing = makeEffect("time", 0);
        trial.timing.params["speed"] = curve;
        const double before = base.sourceExtent(), after = trial.sourceExtent();
        if (after <= 0 || before <= 0) return Result::fail("The clip has no footage to ramp");
        for (Keyframe& k : curve.keys) k.v *= before / after;
    }
    bool any = false;
    for (Id id : linkedClips(s, clipId)) {
        auto l = locate(s, id);
        if (!l || !editable(trackAt(s, l->track))) continue;
        Clip& c = trackAt(s, l->track)->clips[l->index];
        if (c.start != base.start || c.duration != base.duration || c.reverse) continue;
        if (c.timing.empty()) c.timing = makeEffect("time", p.newId());
        c.timing.params["speed"] = curve;
        any = true;
    }
    return any ? Result{} : Result::fail("Nothing to ramp");
}

Result replaceClip(Project& p, Sequence& s, Id clipId, Id mediaId, double srcAlign, FrameTime at) {
    const MediaItem* m = p.findMedia(mediaId);
    if (!m) return Result::fail("Unknown media");
    auto loc = locate(s, clipId);
    if (!loc) return Result::fail("Unknown clip");
    const Clip& base = trackAt(s, loc->track)->clips[loc->index];
    // The new in-point: srcAlign at `at` (at the clip's speed).
    const double sourceIn = srcAlign - double(at - base.start) * base.speed;
    if (sourceIn < -1e-6) return Result::fail("The source does not reach back to the clip's start");
    Result res;
    for (Id id : linkedClips(s, clipId)) {
        auto l = locate(s, id);
        if (!l) continue;
        Track* t = trackAt(s, l->track);
        if (!editable(t)) continue;
        Clip& c = t->clips[l->index];
        const bool video = l->track.kind == TrackKind::Video;
        if (video ? !(m->hasVideo || m->kind == MediaKind::Image) : !m->hasAudio) continue;
        Clip fresh = makeClip(p, *m, l->track.kind, s);
        c.mediaId = mediaId;
        c.name = fresh.name;
        c.sourceIn = std::max(0.0, sourceIn);
        c.reverse = false;
        c.unrendered.clear();
        const FrameTime limit = sourceLimit(p, s, c);
        if (limit < kInfiniteFrames && srcHigh(c) > double(limit) + 1e-6)
            return Result::fail("The source is too short for the clip");
        res.created.push_back(id);
    }
    if (res.created.empty()) return Result::fail("The source has nothing for this clip's tracks");
    return res;
}

Result fitToFill(Project& p, Sequence& s, Id mediaId, double srcIn, double srcOut, FrameTime tlIn, FrameTime tlOut,
                 TrackRef videoTrack, TrackRef audioTrack) {
    const MediaItem* m = p.findMedia(mediaId);
    if (!m) return Result::fail("Unknown media");
    if (srcOut < srcIn || tlOut < tlIn) return Result::fail("Mark In and Out in the source and the timeline");
    const FrameTime len = tlOut - tlIn + 1;
    const double speed = (srcOut - srcIn + 1) / double(len);
    // The range is cleared on the tracks the media goes to, then it is placed at normal
    // speed and sped up or slowed down to fill it.
    std::vector<TrackRef> targets;
    if (m->hasVideo || m->kind == MediaKind::Image) targets.push_back(videoTrack);
    if (m->hasAudio && m->kind != MediaKind::Image) targets.push_back(audioTrack);
    liftRange(p, s, tlIn, tlOut + 1, targets);
    Result r = placeMedia(p, s, mediaId, tlIn, srcIn, srcIn + double(len), videoTrack, audioTrack, false);
    if (!r.ok) return r;
    for (Id id : r.created)
        if (Clip* c = clipById(s, id)) {
            c->speed = speed;
            c->duration = len;
        }
    r.applied = len;
    return r;
}

Result swapClip(Project& p, Sequence& s, Id clipId, bool withNext) {
    (void)p;
    const auto loc = locate(s, clipId);
    if (!loc) return Result::fail("No such clip");
    const Track* track = trackAt(s, loc->track);
    if (track->locked) return Result::fail("Clip is on a locked track");
    const size_t i = loc->index;
    if (withNext ? i + 1 >= track->clips.size() : i == 0)
        return Result::fail(withNext ? "There is no clip after it" : "There is no clip before it");
    const Clip a = track->clips[withNext ? i : i - 1], b = track->clips[withNext ? i + 1 : i];  // a is the earlier
    const bool adjacent = a.end() == b.start;
    const FrameTime da = b.end() - a.duration - a.start, db = a.start - b.start;
    // Each moves with the clips linked to it.
    struct Moving {
        TrackRef track;
        Clip clip;
    };
    std::vector<Moving> moving;
    std::set<Id> ids;
    for (const auto& [group, delta] : {std::pair{linkedClips(s, a.id), da}, std::pair{linkedClips(s, b.id), db}})
        for (Id id : group) {
            const auto l = locate(s, id);
            if (!l || ids.count(id)) continue;
            const Track* t = trackAt(s, l->track);
            if (t->locked) return Result::fail("A linked clip is on a locked track");
            Clip c = t->clips[l->index];
            c.start += delta;
            if (c.start < 0) return Result::fail("Clips would move before 0");
            moving.push_back({l->track, c});
            ids.insert(id);
        }
    // Room for each where it goes, with all of them lifted out.
    const std::vector<Id> ignore(ids.begin(), ids.end());
    for (size_t k = 0; k < moving.size(); ++k) {
        const Moving& m = moving[k];
        if (!trackEmpty(*trackAt(s, m.track), m.clip.start, m.clip.end(), ignore))
            return Result::fail("The clips linked to them would overlap other clips");
        for (size_t o = k + 1; o < moving.size(); ++o)
            if (moving[o].track == m.track && moving[o].clip.start < m.clip.end() && m.clip.start < moving[o].clip.end())
                return Result::fail("The clips linked to them would overlap each other");
    }
    std::set<TrackRef> tracksDone;
    for (const Moving& m : moving) {
        Track* t = trackAt(s, m.track);
        t->clips.erase(std::remove_if(t->clips.begin(), t->clips.end(), [&](const Clip& c) { return c.id == m.clip.id; }), t->clips.end());
        if (!tracksDone.insert(m.track).second) continue;
        // The dissolve between the two goes to their new edit (b now first); others on their edges go.
        std::vector<Transition> kept;
        for (Transition tr : t->transitions) {
            const bool aIn = ids.count(tr.clipA) > 0, bIn = ids.count(tr.clipB) > 0;
            if (!aIn && !bIn) kept.push_back(tr);
            else if (adjacent && aIn && bIn && tr.clipA != 0 && tr.clipB != 0) {
                std::swap(tr.clipA, tr.clipB);
                kept.push_back(tr);
            }
        }
        t->transitions = std::move(kept);
    }
    for (const Moving& m : moving) trackAt(s, m.track)->clips.push_back(m.clip);
    for (const Moving& m : moving) normalize(*trackAt(s, m.track));
    Result r;
    r.applied = withNext ? da : db;
    return r;
}

std::vector<Id> clipsFrom(const Sequence& s, FrameTime frame, std::optional<TrackRef> track) {
    std::vector<Id> out;
    for (TrackRef r : allTracks(s)) {
        if (track && !(*track == r)) continue;
        for (const Clip& c : trackAt(s, r)->clips)
            if (c.start >= frame) out.push_back(c.id);
    }
    return out;
}

Id duplicateSequence(Project& p, Id id, const std::string& name, std::map<Id, Id>* clipIds) {
    const Sequence* src = p.findSequence(id);
    if (!src) return 0;
    Sequence s = *src;
    s.id = p.newId();
    s.name = name.empty() ? src->name + " Copy" : name;
    std::map<Id, Id> clips, groups, buses;
    auto renew = [&](Effect& e) {
        if (e.id) e.id = p.newId();
    };
    for (Bus& b : s.buses) {
        const Id fresh = p.newId();
        buses[b.id] = fresh;
        b.id = fresh;
        for (Effect& e : b.effects) renew(e);
    }
    for (Effect& e : s.masterEffects) renew(e);
    for (CaptionTrack& ct : s.captionTracks) ct.id = p.newId();
    for (auto* tracks : {&s.videoTracks, &s.audioTracks})
        for (Track& t : *tracks) {
            t.id = p.newId();
            if (t.output) t.output = buses.count(t.output) ? buses[t.output] : 0;
            for (Effect& e : t.effects) renew(e);
            for (Clip& c : t.clips) {
                const Id fresh = p.newId();
                clips[c.id] = fresh;
                c.id = fresh;
                if (c.linkGroup) {
                    auto [it, added] = groups.try_emplace(c.linkGroup, 0);
                    if (added) it->second = p.newId();
                    c.linkGroup = it->second;
                }
                for (Effect* e : {&c.generator, &c.motion, &c.audio, &c.timing}) renew(*e);
                for (Effect& e : c.effects) renew(e);
            }
            for (Transition& tr : t.transitions) {
                tr.id = p.newId();
                renew(tr.params);
                if (tr.clipA) tr.clipA = clips.count(tr.clipA) ? clips[tr.clipA] : 0;
                if (tr.clipB) tr.clipB = clips.count(tr.clipB) ? clips[tr.clipB] : 0;
            }
        }
    const Id out = s.id;
    // Its own item in the media bin, beside the original's.
    MediaItem m;
    m.id = p.newId();
    m.kind = MediaKind::Sequence;
    m.name = s.name;
    m.sequenceId = out;
    m.hasVideo = m.hasAudio = true;
    m.width = s.width;
    m.height = s.height;
    m.fps = s.fps;
    for (const MediaItem& o : p.media)
        if (o.kind == MediaKind::Sequence && o.sequenceId == id) m.bin = o.bin;
    p.sequences.push_back(std::move(s));
    p.media.push_back(std::move(m));
    if (clipIds) *clipIds = std::move(clips);
    return out;
}

Result removeTransition(Sequence& s, Id transitionId) {
    for (TrackRef r : allTracks(s)) {
        Track* t = trackAt(s, r);
        auto it = std::find_if(t->transitions.begin(), t->transitions.end(),
                               [&](const Transition& tr) { return tr.id == transitionId; });
        if (it != t->transitions.end()) {
            if (t->locked) return Result::fail("Track is locked");
            t->transitions.erase(it);
            return {};
        }
    }
    return Result::fail("Unknown transition");
}

Transition* transitionById(Sequence& s, Id id, TrackRef* where) {
    for (TrackRef r : allTracks(s)) {
        Track* t = trackAt(s, r);
        for (auto& tr : t->transitions)
            if (tr.id == id) {
                if (where) *where = r;
                return &tr;
            }
    }
    return nullptr;
}

Result setTransitionDuration(Sequence& s, Id transitionId, FrameTime duration) {
    TrackRef where;
    Transition* tr = transitionById(s, transitionId, &where);
    if (!tr) return Result::fail("Unknown transition");
    Track* t = trackAt(s, where);
    if (t->locked) return Result::fail("Track is locked");
    // The room it has: half of each clip for a dissolve (it is centred on the cut), the whole clip for a fade.
    FrameTime room = std::numeric_limits<FrameTime>::max();
    for (const Clip& c : t->clips) {
        if (c.id != tr->clipA && c.id != tr->clipB) continue;
        room = std::min(room, tr->clipA && tr->clipB ? 2 * c.duration : c.duration);
    }
    if (room == std::numeric_limits<FrameTime>::max()) return Result::fail("The transition's clips are gone");
    tr->duration = std::clamp<FrameTime>(duration, 2, std::max<FrameTime>(2, room));
    Result r;
    r.applied = tr->duration;
    return r;
}

bool transitionRange(const Track& t, const Transition& tr, FrameTime& from, FrameTime& to) {
    FrameTime cut = -1;
    for (const auto& c : t.clips) {
        if (tr.clipA && c.id == tr.clipA) cut = c.end();
        if (!tr.clipA && tr.clipB && c.id == tr.clipB) cut = c.start;
    }
    if (cut < 0) return false;
    if (tr.clipA && tr.clipB) {
        from = cut - tr.duration / 2;
        to = from + tr.duration;
    } else if (tr.clipA) {  // fade out: lives inside the clip's tail
        from = cut - tr.duration;
        to = cut;
    } else {  // fade in: lives inside the clip's head
        from = cut;
        to = cut + tr.duration;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Tracks & markers

TrackRef addTrack(Project& p, Sequence& s, TrackKind kind) {
    auto& list = listFor(s, kind);
    std::string name = (kind == TrackKind::Video ? "V" : "A") + std::to_string(list.size() + 1);
    list.push_back(makeTrack(p, kind, name));
    return {kind, int(list.size()) - 1};
}

Result removeTrack(Sequence& s, TrackRef r) {
    auto& list = listFor(s, r.kind);
    if (r.index < 0 || r.index >= int(list.size())) return Result::fail("No such track");
    if (list.size() <= 1) return Result::fail("A sequence needs at least one track of each kind");
    list.erase(list.begin() + r.index);
    return {};
}

void addMarker(Sequence& s, Marker m) {
    removeMarkerAt(s, m.t);
    s.markers.push_back(std::move(m));
    std::sort(s.markers.begin(), s.markers.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
}

bool removeMarkerAt(Sequence& s, FrameTime frame) {
    auto it = std::find_if(s.markers.begin(), s.markers.end(), [&](const Marker& m) { return m.t == frame; });
    if (it == s.markers.end()) return false;
    s.markers.erase(it);
    return true;
}

bool addClipMarker(Sequence& s, Id id, FrameTime at, Marker m) {
    Clip* c = clipById(s, id);
    if (!c || !c->contains(at)) return false;
    m.t = FrameTime(std::llround(c->sourceFrameAt(at)));
    std::erase_if(c->markers, [&](const Marker& x) { return x.t == m.t; });
    c->markers.push_back(std::move(m));
    std::sort(c->markers.begin(), c->markers.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
    return true;
}

bool removeClipMarkerAt(Sequence& s, Id id, FrameTime at) {
    Clip* c = clipById(s, id);
    if (!c) return false;
    const size_t before = c->markers.size();
    std::erase_if(c->markers, [&](const Marker& m) { return c->markerFrame(m) == at; });
    return c->markers.size() != before;
}

// ---------------------------------------------------------------------------
// Snapping & navigation

std::vector<FrameTime> snapPoints(const Sequence& s, const std::vector<Id>& exclude, bool includePlayhead) {
    std::vector<FrameTime> pts{0};
    for (TrackRef r : allTracks(s))
        for (const auto& c : trackAt(s, r)->clips) {
            if (std::find(exclude.begin(), exclude.end(), c.id) != exclude.end()) continue;
            pts.push_back(c.start);
            pts.push_back(c.end());
        }
    for (const auto& m : s.markers) pts.push_back(m.t);
    if (includePlayhead) pts.push_back(s.playhead);
    if (s.inPoint >= 0) pts.push_back(s.inPoint);
    if (s.outPoint >= 0) pts.push_back(s.outPoint);
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    return pts;
}

FrameTime snap(const std::vector<FrameTime>& points, FrameTime frame, FrameTime tolerance, bool* snapped) {
    FrameTime best = frame;
    FrameTime bestDist = tolerance + 1;
    for (FrameTime pt : points) {
        FrameTime d = std::llabs(pt - frame);
        if (d < bestDist) {
            bestDist = d;
            best = pt;
        }
    }
    if (snapped) *snapped = bestDist <= tolerance;
    return bestDist <= tolerance ? best : frame;
}

FrameTime nextEdit(const Sequence& s, FrameTime frame) {
    FrameTime best = -1;
    for (FrameTime pt : snapPoints(s, {}, false))
        if (pt > frame && (best < 0 || pt < best)) best = pt;
    return best < 0 ? frame : best;
}

FrameTime prevEdit(const Sequence& s, FrameTime frame) {
    FrameTime best = -1;
    for (FrameTime pt : snapPoints(s, {}, false))
        if (pt < frame && pt > best) best = pt;
    return best < 0 ? 0 : best;
}

bool matchSequenceToMedia(Sequence& s, const MediaItem& m) {
    if (s.duration() > 0 || m.kind != MediaKind::Video || !m.hasVideo) return false;
    if (m.width <= 0 || m.height <= 0) return false;
    bool changed = false;
    int w = m.width + (m.width & 1), h = m.height + (m.height & 1);
    if (s.width != w || s.height != h) {
        s.width = w;
        s.height = h;
        changed = true;
    }
    if (m.fps.valid() && m.fps.toDouble() >= 1 && m.fps.toDouble() <= 240 && !(s.fps == m.fps)) {
        s.fps = m.fps;
        changed = true;
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Nesting

Result replaceWithRender(Sequence& s, Id clipId, Id media) {
    Clip* c = clipById(s, clipId);
    if (!c) return Result::fail("No such clip");
    if (c->unrendered.empty()) c->unrendered = clipToJsonString(*c);
    c->mediaId = media;
    c->sourceIn = 0;
    c->speed = 1;
    c->reverse = false;
    c->effects.clear();
    c->timing.params.clear();  // a speed ramp is in the render too
    c->generator = Effect{};      // a title or matte is now footage
    return {};
}

Result restoreUnrendered(Sequence& s, Id clipId) {
    Clip* c = clipById(s, clipId);
    if (!c || c->unrendered.empty()) return Result::fail("This clip was not rendered");
    Clip orig;
    if (!clipFromJsonString(c->unrendered, orig)) return Result::fail("The original clip could not be read");
    // Trims since rendering moved sourceIn along the rendered file, which starts where the original did.
    const double trimmed = c->sourceIn;
    c->mediaId = orig.mediaId;
    c->speed = orig.speed;
    c->reverse = orig.reverse;
    c->sourceIn = orig.reverse ? orig.sourceIn + (double(orig.duration) - double(c->duration) - trimmed) * orig.speed
                               : orig.sourceIn + trimmed * orig.speed;
    c->effects = orig.effects;
    c->timing = orig.timing;
    c->generator = orig.generator;
    c->unrendered.clear();
    return {};
}

std::vector<Effect>* effectChain(Sequence& s, Id owner, FrameTime* origin) {
    if (origin) *origin = 0;
    if (!owner) return nullptr;
    if (owner == s.id) return &s.masterEffects;
    for (auto& t : s.audioTracks)
        if (t.id == owner) return &t.effects;
    for (auto& b : s.buses)
        if (b.id == owner) return &b.effects;
    if (Clip* c = clipById(s, owner)) {
        if (origin) *origin = c->start;
        return &c->effects;
    }
    return nullptr;
}

Effect* ownedEffect(Sequence& s, Id owner, Id effect, FrameTime* origin) {
    std::vector<Effect>* chain = effectChain(s, owner, origin);
    if (!chain) return nullptr;
    for (Effect& e : *chain)
        if (e.id == effect) return &e;
    return nullptr;
}

Result makeCompound(Project& p, Sequence& s, const std::vector<Id>& ids, const std::string& name) {
    std::vector<Id> all = expandLinks(s, ids);
    if (all.empty()) return Result::fail("Select clips to nest");
    const Id outerId = s.id;  // `s` lives in p.sequences, which we append to below
    FrameTime lo = std::numeric_limits<FrameTime>::max(), hi = 0;
    for (Id id : all) {
        const Clip* c = clipById(s, id);
        lo = std::min(lo, c->start);
        hi = std::max(hi, c->end());
    }
    Sequence nested = makeSequence(p, name, s.width, s.height, s.fps, int(s.videoTracks.size()),
                                   int(s.audioTracks.size()));
    nested.sampleRate = s.sampleRate;
    nested.colorSpace = s.colorSpace;  // same working space: the clips are not converted twice
    nested.hdrPeakNits = s.hdrPeakNits;
    bool hasAudio = false;
    for (Id id : all) {
        auto loc = locate(s, id);
        Track* src = trackAt(s, loc->track);
        Clip c = src->clips[loc->index];
        c.start -= lo;
        Track* dst = trackAt(nested, loc->track);
        dst->clips.push_back(c);
        if (loc->track.kind == TrackKind::Audio) hasAudio = true;
        // Carry transitions between nested clips.
        for (const auto& tr : src->transitions) {
            bool aIn = tr.clipA == 0 || std::find(all.begin(), all.end(), tr.clipA) != all.end();
            bool bIn = tr.clipB == 0 || std::find(all.begin(), all.end(), tr.clipB) != all.end();
            bool involves = tr.clipA == c.id || tr.clipB == c.id;
            if (involves && aIn && bIn &&
                std::none_of(dst->transitions.begin(), dst->transitions.end(), [&](const Transition& o) { return o.id == tr.id; }))
                dst->transitions.push_back(tr);
        }
    }
    for (auto* list : {&nested.videoTracks, &nested.audioTracks})
        for (auto& t : *list) normalize(t);

    // Lowest video / audio track used by the selection.
    int vIdx = int(s.videoTracks.size()), aIdx = int(s.audioTracks.size());
    for (Id id : all) {
        auto loc = locate(s, id);
        if (loc->track.kind == TrackKind::Video) vIdx = std::min(vIdx, loc->track.index);
        else aIdx = std::min(aIdx, loc->track.index);
    }
    removeClips(p, s, all, false);

    MediaItem m;
    m.id = p.newId();
    m.kind = MediaKind::Sequence;
    m.name = name;
    m.sequenceId = nested.id;
    m.hasVideo = true;
    m.hasAudio = hasAudio;
    m.width = s.width;
    m.height = s.height;
    m.fps = s.fps;
    m.duration = double(hi - lo) / s.fpsValue();
    p.sequences.push_back(std::move(nested));
    p.media.push_back(m);
    Sequence& sq = *p.findSequence(outerId);
    if (vIdx >= int(sq.videoTracks.size())) vIdx = 0;
    if (aIdx >= int(sq.audioTracks.size())) aIdx = 0;
    return placeMedia(p, sq, m.id, lo, 0, double(hi - lo), {TrackKind::Video, vIdx}, {TrackKind::Audio, aIdx}, false);
}

// ---- Duplicate frames -------------------------------------------------------------

std::map<Id, std::vector<DuplicateSpan>> duplicateFrames(const Sequence& s) {
    struct Use {
        const Clip* clip;
        double a, b;  // source frames shown, [a, b)
    };
    std::map<Id, std::vector<Use>> byMedia;
    std::vector<Id> order;  // media in order of first use, for stable groups
    for (const Track& t : s.videoTracks)
        for (const Clip& c : t.clips) {
            if (!c.mediaId || c.isGenerator()) continue;
            const double x = c.sourceAt(0), y = c.sourceAt(double(c.duration));
            if (!byMedia.count(c.mediaId)) order.push_back(c.mediaId);
            byMedia[c.mediaId].push_back({&c, std::min(x, y), std::max(x, y)});
        }
    std::map<Id, std::vector<DuplicateSpan>> out;
    for (size_t g = 0; g < order.size(); ++g) {
        const std::vector<Use>& uses = byMedia[order[g]];
        for (size_t i = 0; i < uses.size(); ++i)
            for (size_t j = 0; j < uses.size(); ++j) {
                if (i == j) continue;
                const double lo = std::max(uses[i].a, uses[j].a), hi = std::min(uses[i].b, uses[j].b);
                if (hi - lo < 0.5) continue;  // less than a frame in common
                // The shared source frames back on clip i's timeline.
                const Clip& c = *uses[i].clip;
                const double l0 = c.localForSource(lo), l1 = c.localForSource(hi);
                FrameTime from = c.start + FrameTime(std::floor(std::min(l0, l1) + 1e-6));
                FrameTime to = c.start + FrameTime(std::ceil(std::max(l0, l1) - 1e-6));
                from = std::clamp(from, c.start, c.end());
                to = std::clamp(to, c.start, c.end());
                if (to > from) out[c.id].push_back({from, to, int(g)});
            }
    }
    for (auto& [id, spans] : out) {  // merged, in order
        std::sort(spans.begin(), spans.end(), [](const DuplicateSpan& a, const DuplicateSpan& b) { return a.from < b.from; });
        std::vector<DuplicateSpan> merged;
        for (const DuplicateSpan& d : spans) {
            if (!merged.empty() && d.from <= merged.back().to) merged.back().to = std::max(merged.back().to, d.to);
            else merged.push_back(d);
        }
        spans = std::move(merged);
    }
    return out;
}

// ---- Video layouts ------------------------------------------------------------------------

std::vector<Cell> layoutCells(Layout layout, int n, int width, int height, const LayoutOptions& o) {
    const double W = width, H = height, g = std::max(0.0, o.gap);
    auto grid = [&](int cols, int rows) {
        std::vector<Cell> out;
        const double cw = (W - g * (cols + 1)) / cols, ch = (H - g * (rows + 1)) / rows;
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < cols; ++c) out.push_back({g + c * (cw + g), g + r * (ch + g), cw, ch});
        return out;
    };
    switch (layout) {
        case Layout::FullFrame: return std::vector<Cell>(size_t(std::max(1, n)), Cell{0, 0, W, H});
        case Layout::SideBySide: return grid(2, 1);
        case Layout::TopAndBottom: return grid(1, 2);
        case Layout::ThreeAcross: return grid(3, 1);
        case Layout::Grid: return grid(2, 2);
        case Layout::PictureInPicture: {
            std::vector<Cell> out{{0, 0, W, H}};
            const double margin = std::max(g, 0.04 * std::min(W, H)), w = std::clamp(o.pipSize, 0.05, 1.0) * W;
            // From the chosen corner, round the others: bottom right, bottom left, top right, top left.
            static constexpr int kOrder[4] = {3, 2, 1, 0};
            const int first = int(std::find(kOrder, kOrder + 4, std::clamp(o.corner, 0, 3)) - kOrder);
            for (int i = 1; i < std::max(2, n); ++i) {
                const int corner = kOrder[(first + i - 1) % 4];
                // Height comes from the clip; the corner is kept by its edge, so y here marks top or bottom.
                out.push_back({corner % 2 ? W - margin - w : margin, corner < 2 ? margin : H - margin, w, 0});
            }
            return out;
        }
    }
    return {};
}

Result arrangeLayout(const Project& p, Sequence& s, const std::vector<Id>& clips, Layout layout, const LayoutOptions& o) {
    // The video clips, from the lowest track up.
    struct Item {
        Clip* clip;
        int track;
    };
    std::vector<Item> items;
    for (Id id : clips)
        if (auto loc = locate(s, id); loc && loc->track.kind == TrackKind::Video)
            if (Clip* c = clipById(s, id); c && std::none_of(items.begin(), items.end(), [c](const Item& it) { return it.clip == c; }))
                items.push_back({c, loc->track.index});
    if (items.empty()) return Result::fail("Select the video clips to arrange");
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.track != b.track ? a.track < b.track : a.clip->start < b.clip->start;
    });
    const double W = s.width, H = s.height;
    const std::vector<Cell> cells = layoutCells(layout, int(items.size()), s.width, s.height, o);
    for (size_t i = 0; i < items.size(); ++i) {
        Clip& c = *items[i].clip;
        // The picture's own size, as the compositor sees it.
        double mw = W, mh = H;
        if (!c.isGenerator())
            if (const MediaItem* m = p.findMedia(c.mediaId)) {
                if (m->kind == MediaKind::Sequence) {
                    if (const Sequence* n = p.findSequence(m->sequenceId)) mw = n->width, mh = n->height;
                } else if (m->width > 0 && m->height > 0) {
                    mw = m->width, mh = m->height;
                }
            }
        const double fit = [&] {
            switch (int(c.motion.p("fit", 0, 0))) {
                case 1: return std::max(W / mw, H / mh);
                case 2: return W / mw;  // Stretch: the width's factor (the height follows its own)
                case 3: return 1.0;
                default: return std::min(W / mw, H / mh);
            }
        }();
        const double fitY = int(c.motion.p("fit", 0, 0)) == 2 ? H / mh : fit;
        // Cells past the layout's count share the last one; picture in picture gives each corner the clip's shape.
        Cell cell = cells[std::min(i, cells.size() - 1)];
        double scale, cropX = 0, cropY = 0;
        const double bw = mw * fit, bh = mh * fitY;  // its size at 100 %
        if (layout == Layout::PictureInPicture && i > 0) {
            scale = cell.w / bw;
            cell.h = bh * scale;
            if (cell.y >= H / 2) cell.y -= cell.h;  // a bottom corner: up from the margin
        } else if (layout == Layout::FullFrame) {
            scale = 1;
        } else {
            scale = std::max(cell.w / bw, cell.h / bh);  // cover the cell, then crop the overflow
            cropX = std::max(0.0, (bw * scale - cell.w) / 2 / (bw * scale));
            cropY = std::max(0.0, (bh * scale - cell.h) / 2 / (bh * scale));
        }
        auto set = [&](const char* k, double v) {
            Param& prm = c.motion.params[k];
            prm.keys.clear();
            prm.value = v;
        };
        set("scale", scale * 100);
        set("scale_x", 100);
        set("scale_y", 100);
        set("anchor_x", 0);
        set("anchor_y", 0);
        set("pos_x", layout == Layout::FullFrame ? 0 : cell.x + cell.w / 2 - W / 2);
        set("pos_y", layout == Layout::FullFrame ? 0 : cell.y + cell.h / 2 - H / 2);
        set("crop_left", cropX * 100);
        set("crop_right", cropX * 100);
        set("crop_top", cropY * 100);
        set("crop_bottom", cropY * 100);
    }
    return {};
}

// ---- Close Up -------------------------------------------------------------------------

Result closeUp(Project& p, Sequence& s, Id clipId, FrameTime from, FrameTime to, double zoom, double u, double v) {
    const auto loc = locate(s, clipId);
    if (!loc || loc->track.kind != TrackKind::Video) return Result::fail("Choose a video clip for the close-up");
    const Clip src = s.videoTracks[size_t(loc->track.index)].clips[loc->index];
    const MediaItem* m = p.findMedia(src.mediaId);
    if (!m || src.isGenerator() || m->width <= 0 || m->height <= 0) return Result::fail("A close-up needs a picture from a file");
    from = std::max(from, src.start);
    to = std::min(to, src.end());
    if (to <= from) return Result::fail("The close-up must be within the clip");
    zoom = std::clamp(zoom, 1.05, 4.0);
    Clip c = subClip(src, from, to);
    c.id = p.newId();
    c.linkGroup = 0;
    c.name = src.name + " (Close Up)";
    // Its framing, at the clip's own scale times `zoom`, keyed nowhere: a fixed punch-in.
    const double fit = [&] {
        switch (int(src.motion.p("fit", 0, 0))) {
            case 1: return std::max(double(s.width) / m->width, double(s.height) / m->height);
            case 2: return double(s.width) / m->width;
            default: return std::min(double(s.width) / m->width, double(s.height) / m->height);
        }
    }();
    const double scale = src.motion.p("scale", 0, 100) * zoom;
    const double w = m->width * fit * scale / 100.0, h = m->height * fit * scale / 100.0;
    double px = -(std::clamp(u, 0.0, 1.0) - 0.5) * w, py = -(std::clamp(v, 0.0, 1.0) - 0.45) * h;
    // Keep the picture over the whole frame where it is big enough to.
    if (w >= s.width) px = std::clamp(px, -(w - s.width) / 2, (w - s.width) / 2);
    if (h >= s.height) py = std::clamp(py, -(h - s.height) / 2, (h - s.height) / 2);
    for (const char* k : {"scale", "pos_x", "pos_y"}) c.motion.params[k].keys.clear();
    c.motion.params["scale"] = Param(scale);
    c.motion.params["pos_x"] = Param(px);
    c.motion.params["pos_y"] = Param(py);
    // On the track above, a new one if it is the top track or that stretch is taken.
    int above = loc->track.index + 1;
    if (above >= int(s.videoTracks.size()) || !trackEmpty(s.videoTracks[size_t(above)], from, to)) {
        addTrack(p, s, TrackKind::Video);
        above = int(s.videoTracks.size()) - 1;
        if (!trackEmpty(s.videoTracks[size_t(above)], from, to)) return Result::fail("No room above the clip");
    }
    c.start = from;
    Result r = overwrite(p, s, {TrackKind::Video, above}, c);
    if (r.ok && r.created.empty()) r.created.push_back(c.id);
    return r;
}

// ---- Track folders ------------------------------------------------------------------

namespace {
std::string folderKey(TrackKind kind, const std::string& folder) { return (kind == TrackKind::Video ? "V/" : "A/") + folder; }
}  // namespace

Result setTrackFolder(Sequence& s, const std::vector<TrackRef>& tracks, const std::string& folder) {
    if (tracks.empty()) return Result::fail("No tracks");
    if (folder.find('/') != std::string::npos) return Result::fail("Folder names cannot have a slash");
    for (TrackRef r : tracks)
        if (!trackAt(s, r)) return Result::fail("No such track");
    for (TrackRef r : tracks) trackAt(s, r)->folder = folder;
    // Forget the settings of folders that no longer have tracks.
    auto gone = [&](const std::string& key) {
        const TrackKind kind = key.rfind("V/", 0) == 0 ? TrackKind::Video : TrackKind::Audio;
        return folderTracks(s, kind, key.substr(2)).empty();
    };
    std::erase_if(s.collapsedFolders, gone);
    std::erase_if(s.folderGains, [&](const auto& kv) { return gone(kv.first); });
    return {};
}

std::vector<int> folderTracks(const Sequence& s, TrackKind kind, const std::string& folder) {
    std::vector<int> out;
    if (folder.empty()) return out;
    const auto& tracks = kind == TrackKind::Video ? s.videoTracks : s.audioTracks;
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].folder == folder) out.push_back(int(i));
    return out;
}

bool folderCollapsed(const Sequence& s, TrackKind kind, const std::string& folder) {
    const std::string key = folderKey(kind, folder);
    return std::find(s.collapsedFolders.begin(), s.collapsedFolders.end(), key) != s.collapsedFolders.end();
}

void setFolderCollapsed(Sequence& s, TrackKind kind, const std::string& folder, bool collapsed) {
    const std::string key = folderKey(kind, folder);
    std::erase(s.collapsedFolders, key);
    if (collapsed && !folder.empty()) s.collapsedFolders.push_back(key);
}

double folderGain(const Sequence& s, TrackKind kind, const std::string& folder) {
    if (folder.empty()) return 0;
    const auto it = s.folderGains.find(folderKey(kind, folder));
    return it == s.folderGains.end() ? 0.0 : it->second;
}

void setFolderGain(Sequence& s, TrackKind kind, const std::string& folder, double db) {
    if (folder.empty()) return;
    if (std::fabs(db) < 1e-9) s.folderGains.erase(folderKey(kind, folder));
    else s.folderGains[folderKey(kind, folder)] = db;
}

Result renameFolder(Sequence& s, TrackKind kind, const std::string& from, const std::string& to) {
    if (to.empty() || to.find('/') != std::string::npos) return Result::fail("Give the folder a name without a slash");
    if (to != from && !folderTracks(s, kind, to).empty()) return Result::fail("There is already a folder called " + to);
    const std::vector<int> members = folderTracks(s, kind, from);
    if (members.empty()) return Result::fail("No such folder");
    const bool collapsed = folderCollapsed(s, kind, from);
    const double gain = folderGain(s, kind, from);
    for (int i : members) trackAt(s, {kind, i})->folder = to;
    setFolderCollapsed(s, kind, from, false);
    setFolderCollapsed(s, kind, to, collapsed);
    setFolderGain(s, kind, from, 0);
    setFolderGain(s, kind, to, gain);
    return {};
}

// ---- Through edits -----------------------------------------------------------------

namespace {
bool isThrough(const Track& t, size_t i) {
    if (i + 1 >= t.clips.size()) return false;
    const Clip& a = t.clips[i];
    const Clip& b = t.clips[i + 1];
    if (a.end() != b.start || !a.mediaId || a.mediaId != b.mediaId || a.isGenerator() || b.isGenerator()) return false;
    if (a.speed != b.speed || a.reverse || b.reverse || a.ramped() || b.ramped()) return false;
    if (!a.takes.empty() || !b.takes.empty() || !a.unrendered.empty() || !b.unrendered.empty()) return false;
    if (a.enabled != b.enabled || a.blendMode != b.blendMode || a.angle != b.angle || a.audioAngle != b.audioAngle) return false;
    if (a.effects.size() != b.effects.size()) return false;
    for (size_t k = 0; k < a.effects.size(); ++k)
        if (a.effects[k].type != b.effects[k].type) return false;
    if (std::fabs(b.sourceIn - (a.sourceIn + a.sourceExtent())) > 1e-3) return false;
    for (const Transition& tr : t.transitions)
        if (tr.clipA == a.id && tr.clipB == b.id) return false;
    return true;
}

// Merges clip i of the track with the one after it.
void mergeWithNext(Track& t, size_t i) {
    Clip& a = t.clips[i];
    const Clip b = t.clips[i + 1];
    a.duration += b.duration;
    for (const Marker& m : b.markers)
        if (std::find(a.markers.begin(), a.markers.end(), m) == a.markers.end()) a.markers.push_back(m);
    t.clips.erase(t.clips.begin() + long(i) + 1);
    for (Transition& tr : t.transitions)
        if (tr.clipA == b.id) tr.clipA = a.id;
}
}  // namespace

std::vector<Id> throughEdits(const Sequence& s) {
    std::vector<Id> out;
    for (const std::vector<Track>* tracks : {&s.videoTracks, &s.audioTracks})
        for (const Track& t : *tracks)
            for (size_t i = 0; i + 1 < t.clips.size(); ++i)
                if (isThrough(t, i)) out.push_back(t.clips[i].id);
    return out;
}

Result joinThroughEdit(Project&, Sequence& s, Id clipId) {
    const auto loc = locate(s, clipId);
    if (!loc) return Result::fail("No such clip");
    Track* t = trackAt(s, loc->track);
    if (t->locked) return Result::fail("The track is locked");
    if (!isThrough(*t, loc->index)) return Result::fail("There is no through edit after that clip");
    const Id next = t->clips[loc->index + 1].id;
    // Linked clips on other tracks that also run through, into the clips linked to the next one.
    std::vector<std::pair<Track*, size_t>> joins{{t, loc->index}};
    const std::vector<Id> after = linkedClips(s, next);
    for (Id x : linkedClips(s, clipId)) {
        if (x == clipId || x == next) continue;
        const auto lx = locate(s, x);
        Track* tx = lx ? trackAt(s, lx->track) : nullptr;
        if (!tx || tx == t || tx->locked || lx->index + 1 >= tx->clips.size()) continue;
        const Id y = tx->clips[lx->index + 1].id;
        if (y != x && std::find(after.begin(), after.end(), y) != after.end() && isThrough(*tx, lx->index)) joins.push_back({tx, lx->index});
    }
    for (auto& [track, index] : joins) mergeWithNext(*track, index);
    return {};
}

int joinThroughEdits(Project& p, Sequence& s, const std::vector<Id>& ids) {
    auto wanted = [&](const Track& t, size_t i) {
        if (ids.empty()) return true;
        return std::find(ids.begin(), ids.end(), t.clips[i].id) != ids.end() ||
               std::find(ids.begin(), ids.end(), t.clips[i + 1].id) != ids.end();
    };
    int joined = 0;
    for (bool again = true; again;) {
        again = false;
        for (std::vector<Track>* tracks : {&s.videoTracks, &s.audioTracks})
            for (Track& t : *tracks)
                for (size_t i = 0; i + 1 < t.clips.size() && !again; ++i)
                    if (!t.locked && isThrough(t, i) && wanted(t, i) && joinThroughEdit(p, s, t.clips[i].id).ok) {
                        ++joined;
                        again = true;  // indices moved: look again
                    }
    }
    return joined;
}

// ---- Auditions -----------------------------------------------------------------

Result addTakes(Project& p, Sequence& s, Id clipId, const std::vector<std::pair<Id, double>>& media) {
    const auto loc = locate(s, clipId);
    Clip* c = clipById(s, clipId);
    if (!loc || !c) return Result::fail("No such clip");
    if (c->isGenerator() || !c->mediaId) return Result::fail("Titles and generators have no takes");
    const bool video = loc->track.kind == TrackKind::Video;
    std::vector<Take> added;
    for (const auto& [id, in] : media) {
        const MediaItem* m = p.findMedia(id);
        if (!m || m->kind == MediaKind::Sequence) return Result::fail("Takes are media files");
        if (video ? !(m->hasVideo || m->kind == MediaKind::Image) : !m->hasAudio)
            return Result::fail(video ? m->name + " has no picture" : m->name + " has no sound");
        added.push_back({id, in - c->sourceIn, m->name});
    }
    if (added.empty()) return Result::fail("No media to add as takes");
    if (c->takes.empty()) {
        const MediaItem* own = p.findMedia(c->mediaId);
        c->takes.push_back({c->mediaId, 0, c->name.empty() && own ? own->name : c->name});
        c->take = 0;
    }
    c->takes.insert(c->takes.end(), added.begin(), added.end());
    return {};
}

Result pickTake(Project& p, Sequence& s, Id clipId, int index) {
    Clip* c = clipById(s, clipId);
    if (!c || c->takes.empty()) return Result::fail("That clip is not an audition");
    if (index < 0 || index >= int(c->takes.size())) return Result::fail("No such take");
    if (index == c->take) return {};
    if (!p.findMedia(c->takes[size_t(index)].mediaId)) return Result::fail("That take's media is no longer in the project");
    // Re-based on the new pick: every take's offset from the clip's new in-point.
    const Take next = c->takes[size_t(index)];
    const Id was = c->mediaId;
    c->takes[size_t(c->take)].name = c->name;  // a rename stays with its take
    for (Take& t : c->takes) t.offset -= next.offset;
    for (Id id : linkedClips(s, clipId)) {
        Clip* l = clipById(s, id);
        if (l && id != clipId && l->mediaId == was && l->takes.empty()) {
            l->mediaId = next.mediaId;
            l->sourceIn += next.offset;
        }
    }
    c->mediaId = next.mediaId;
    c->sourceIn += next.offset;
    c->name = next.name;
    c->take = index;
    return {};
}

Result cycleTake(Project& p, Sequence& s, Id clipId, int step) {
    const Clip* c = clipById(s, clipId);
    if (!c || c->takes.empty()) return Result::fail("That clip is not an audition");
    const int n = int(c->takes.size());
    return pickTake(p, s, clipId, ((c->take + step) % n + n) % n);
}

Result finalizeAudition(Project&, Sequence& s, Id clipId) {
    Clip* c = clipById(s, clipId);
    if (!c || c->takes.empty()) return Result::fail("That clip is not an audition");
    c->takes.clear();
    c->take = 0;
    return {};
}

}  // namespace montage::edit
