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
        out.sourceIn = c.sourceIn + double(from - c.start) * c.speed;
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
                double maxExtra = (lim - srcHigh(c)) / c.speed;
                delta = std::min<FrameTime>(delta, FrameTime(std::floor(maxExtra + 1e-6)));
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
                double maxBack = srcLow(c) / c.speed;  // how far we can extend backwards
                delta = std::max<FrameTime>(delta, -FrameTime(std::floor(maxBack + 1e-6)));
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
        if (!c.reverse) c.sourceIn += double(delta) * c.speed;
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
        double shift = double(delta) * c->speed;
        if (limit < kInfiniteFrames) shift = std::clamp(shift, -c->sourceIn, double(limit) - srcHigh(*c));
        else shift = std::max(shift, -c->sourceIn);
        c->sourceIn += shift;
        if (id == clipId) res.applied = FrameTime(std::llround(shift / c->speed));
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

}  // namespace montage::edit
