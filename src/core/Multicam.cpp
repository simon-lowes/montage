#include "Multicam.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "Effects.h"

namespace montage {

Id makeMulticam(Project& p, const std::vector<Id>& media, const std::vector<double>& offsets, const std::string& name,
                std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return Id(0);
    };
    if (media.empty() || offsets.size() != media.size()) return fail("Choose the cameras' clips");
    const MediaItem* first = nullptr;
    int videos = 0, sounds = 0;
    for (Id id : media) {
        const MediaItem* m = p.findMedia(id);
        if (!m || m->kind == MediaKind::Sequence) return fail("Multicam clips are made from media files");
        if (m->hasVideo || m->kind == MediaKind::Image) {
            if (!first) first = m;
            ++videos;
        }
        if (m->hasAudio) ++sounds;
    }
    if (!first) return fail("A multicam clip needs at least one camera (a clip with video)");
    const Sequence* active = p.active();
    const Rational fps = first->fps.valid() ? first->fps : active ? active->fps : Rational{30, 1};
    const int w = first->width > 0 ? first->width : active ? active->width : 1920;
    const int h = first->height > 0 ? first->height : active ? active->height : 1080;
    Sequence mc = makeSequence(p, name, w, h, fps, videos, sounds);
    mc.multicam = true;
    if (active) {
        mc.sampleRate = active->sampleRate;
        mc.colorSpace = active->colorSpace;
        mc.hdrPeakNits = active->hdrPeakNits;
    }
    const double origin = *std::min_element(offsets.begin(), offsets.end());
    int vi = 0, ai = 0;
    for (size_t i = 0; i < media.size(); ++i) {
        const MediaItem m = *p.findMedia(media[i]);
        const FrameTime at = FrameTime(std::llround((offsets[i] - origin) * mc.fpsValue()));
        if (m.hasVideo || m.kind == MediaKind::Image) {
            Clip c = makeClip(p, m, TrackKind::Video, mc);
            c.start = at;
            mc.videoTracks[size_t(vi)].name = m.name;
            mc.videoTracks[size_t(vi++)].clips.push_back(c);
        }
        if (m.hasAudio) {
            Clip c = makeClip(p, m, TrackKind::Audio, mc);
            c.start = at;
            mc.audioTracks[size_t(ai)].name = m.name;
            mc.audioTracks[size_t(ai++)].clips.push_back(c);
        }
    }
    MediaItem item;
    item.id = p.newId();
    item.kind = MediaKind::Sequence;
    item.name = name;
    item.sequenceId = mc.id;
    item.hasVideo = true;
    item.hasAudio = sounds > 0;
    item.width = w;
    item.height = h;
    item.fps = fps;
    item.duration = double(mc.duration()) / mc.fpsValue();
    item.bin = first->bin;
    p.sequences.push_back(std::move(mc));
    p.media.push_back(item);
    return item.id;
}

const Sequence* multicamSequence(const Project& p, const Clip& c) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || m->kind != MediaKind::Sequence) return nullptr;
    const Sequence* s = p.findSequence(m->sequenceId);
    return s && s->multicam ? s : nullptr;
}

std::vector<std::string> angleNames(const Sequence& mc) {
    std::vector<std::string> out;
    for (const Track& t : mc.videoTracks) out.push_back(t.name);
    return out;
}

namespace {
Id trackMedia(const Track& t) { return t.clips.empty() ? 0 : t.clips.front().mediaId; }
}  // namespace

int angleAudioTrack(const Sequence& mc, int angle) {
    if (angle < 0 || angle >= int(mc.videoTracks.size())) return -1;
    const Id m = trackMedia(mc.videoTracks[size_t(angle)]);
    for (size_t i = 0; m && i < mc.audioTracks.size(); ++i)
        if (trackMedia(mc.audioTracks[i]) == m) return int(i);
    return -1;
}

int audioTrackAngle(const Sequence& mc, int track) {
    if (track < 0 || track >= int(mc.audioTracks.size())) return -1;
    const Id m = trackMedia(mc.audioTracks[size_t(track)]);
    for (size_t i = 0; m && i < mc.videoTracks.size(); ++i)
        if (trackMedia(mc.videoTracks[i]) == m) return int(i);
    return -1;
}

bool timecodeOffsets(const Project& p, const std::vector<Id>& media, std::vector<double>& offsets) {
    offsets.clear();
    for (Id id : media) {
        const MediaItem* m = p.findMedia(id);
        if (!m || m->timecode < 0) return false;
        offsets.push_back(m->timecode);
    }
    return true;
}

Sequence multicamPart(const Sequence& mc, bool picture, int angle, int audioAngle) {
    Sequence part = mc;
    part.multicam = false;
    if (picture) {
        part.audioTracks.clear();
        if (mc.videoTracks.empty()) return part;
        part.videoTracks = {mc.videoTracks[size_t(std::clamp(angle, 0, int(mc.videoTracks.size()) - 1))]};
    } else {
        part.videoTracks.clear();
        if (audioAngle >= 0 && audioAngle < int(mc.audioTracks.size())) part.audioTracks = {mc.audioTracks[size_t(audioAngle)]};
    }
    return part;
}

Sequence flattenedMulticam(const Project& p, const Sequence& in) {
    Sequence s = in;
    std::vector<Id> clips;
    for (const auto* list : {&s.videoTracks, &s.audioTracks})
        for (const Track& t : *list)
            for (const Clip& c : t.clips)
                if (multicamSequence(p, c)) clips.push_back(c.id);
    if (clips.empty()) return s;
    // The mix of every source becomes the sound of the angle seen with it (else the first source's).
    for (Track& t : s.audioTracks)
        for (Clip& a : t.clips) {
            const Sequence* mc = multicamSequence(p, a);
            if (!mc || a.audioAngle >= 0) continue;
            int angle = -1;
            for (Id other : edit::linkedClips(s, a.id))
                if (const Clip* v = edit::clipById(s, other); v && v->mediaId == a.mediaId && edit::locate(s, other)->track.kind == TrackKind::Video)
                    angle = v->angle;
            const int track = angle >= 0 ? angleAudioTrack(*mc, angle) : -1;
            a.audioAngle = track >= 0 ? track : mc->audioTracks.empty() ? -1 : 0;
        }
    const Sequence unflattened = s;
    // Transitions touching a multicam clip, to be put back between the pieces that replace it.
    struct Kept {
        TrackRef track;
        Transition tr;
        FrameTime aEnd = -1, bStart = -1;  // where the multicam clip on either side ended or started
    };
    std::vector<Kept> kept;
    const auto isMulticam = [&](Id id) { return std::find(clips.begin(), clips.end(), id) != clips.end(); };
    for (const TrackRef ref : allTracks(s)) {
        const Track& t = *trackAt(s, ref);
        for (const Transition& tr : t.transitions) {
            if (!isMulticam(tr.clipA) && !isMulticam(tr.clipB)) continue;
            Kept k{ref, tr};
            if (const Clip* a = tr.clipA ? edit::clipById(s, tr.clipA) : nullptr; a && isMulticam(a->id)) k.aEnd = a->end();
            if (const Clip* b = tr.clipB ? edit::clipById(s, tr.clipB) : nullptr; b && isMulticam(b->id)) k.bStart = b->start;
            kept.push_back(k);
        }
    }
    // Locks only guard editing by hand: the copy is flattened whatever is locked.
    for (auto* list : {&s.videoTracks, &s.audioTracks})
        for (Track& t : *list) t.locked = false;
    Project scratch = p;  // (new ids only; the media and sequences are the project's)
    const edit::Result r = edit::flattenMulticam(scratch, s, clips);
    if (!r.ok) return unflattened;
    for (auto* list : {&s.videoTracks, &s.audioTracks})
        for (size_t i = 0; i < list->size(); ++i)
            (*list)[i].locked = (list == &s.videoTracks ? unflattened.videoTracks : unflattened.audioTracks)[i].locked;
    std::vector<Clip*> made;
    for (auto* list : {&s.videoTracks, &s.audioTracks})
        for (Track& t : *list)
            for (Clip& c : t.clips)
                if (std::find(r.created.begin(), r.created.end(), c.id) != r.created.end()) made.push_back(&c);
    // Picture and sound cut from one source together stay linked.
    for (Clip* v : made)
        for (Clip* a : made)
            if (v != a && !v->linkGroup && !a->linkGroup && v->mediaId == a->mediaId && v->start == a->start &&
                v->duration == a->duration && edit::locate(s, v->id)->track.kind == TrackKind::Video &&
                edit::locate(s, a->id)->track.kind == TrackKind::Audio)
                v->linkGroup = a->linkGroup = scratch.newId();
    // Dissolves and fades back on: the piece ending where the old clip ended, the piece starting where it started.
    for (Kept& k : kept) {
        Track* t = trackAt(s, k.track);
        if (!t) continue;
        auto pieceAt = [&](FrameTime edge, bool ending) -> Id {
            for (const Clip& c : t->clips)
                if ((ending ? c.end() : c.start) == edge) return c.id;
            return 0;
        };
        if (k.aEnd >= 0) k.tr.clipA = pieceAt(k.aEnd, true);
        if (k.bStart >= 0) k.tr.clipB = pieceAt(k.bStart, false);
        if ((k.aEnd >= 0 && !k.tr.clipA) || (k.bStart >= 0 && !k.tr.clipB)) continue;  // (an angle with nothing there)
        if (std::none_of(t->transitions.begin(), t->transitions.end(), [&](const Transition& o) { return o.id == k.tr.id; }))
            t->transitions.push_back(k.tr);
    }
    return s;
}

namespace edit {

namespace {

// Cuts clip `id` at `at` if that is strictly inside it; returns the id of the
// part from `at` on (the clip itself if no cut was needed). Right halves of
// linked clips cut together share a fresh link group through `regroup`.
Id cutAt(Project& p, Sequence& s, Id id, FrameTime at, std::map<Id, Id>& regroup, Result& res) {
    auto loc = locate(s, id);
    if (!loc) return 0;
    Track* t = trackAt(s, loc->track);
    const Clip& c = t->clips[loc->index];
    if (at <= c.start || at >= c.end()) return id;
    const Id group = c.linkGroup;
    const Id right = splitClip(p, *t, loc->index, at);
    if (group) {
        auto it = regroup.find(group);
        if (it == regroup.end()) it = regroup.emplace(group, p.newId()).first;
        for (auto& k : t->clips)
            if (k.id == right) k.linkGroup = it->second;
    }
    normalize(*t);
    res.created.push_back(right);
    return right;
}

// Multicam audio clips linked to `id` that show the same multicam sequence.
std::vector<Id> linkedMulticamAudio(const Project& p, const Sequence& s, Id id, const Sequence* mc) {
    std::vector<Id> out;
    for (Id other : linkedClips(s, id)) {
        auto loc = locate(s, other);
        if (other == id || !loc || loc->track.kind != TrackKind::Audio) continue;
        if (multicamSequence(p, trackAt(s, loc->track)->clips[loc->index]) == mc) out.push_back(other);
    }
    return out;
}

bool defaultParams(const Effect& e) {
    const Effect fresh = makeEffect(e.type, e.id);
    return e.params == fresh.params && e.strings == fresh.strings;
}

}  // namespace

Result switchAngle(Project& p, Sequence& s, Id clipId, int angle, FrameTime at, bool cut, bool audioFollows) {
    const Clip* c = clipById(s, clipId);
    if (!c) return Result::fail("The clip no longer exists");
    const Sequence* mc = multicamSequence(p, *c);
    if (!mc) return Result::fail("Not a multicam clip");
    if (angle < 0 || angle >= int(mc->videoTracks.size())) return Result::fail("The multicam clip has no angle " + std::to_string(angle + 1));
    auto loc = locate(s, clipId);
    if (!loc || loc->track.kind != TrackKind::Video) return Result::fail("Switch angles on the video clip");
    if (Track* t = trackAt(s, loc->track); t && t->locked) return Result::fail("Track is locked");
    // Linked multicam audio is cut with the picture, so the pieces stay linked
    // and aligned; it changes to the angle's sound only when audio follows video.
    const std::vector<Id> audioClips = linkedMulticamAudio(p, s, clipId, mc);
    const int audio = angleAudioTrack(*mc, angle);
    Result res;
    std::map<Id, Id> regroup;
    const Id videoPart = cut ? cutAt(p, s, clipId, at, regroup, res) : clipId;
    if (Clip* k = clipById(s, videoPart)) k->angle = angle;
    for (Id a : audioClips) {
        const Id part = cut ? cutAt(p, s, a, at, regroup, res) : a;
        if (audioFollows && audio >= 0)
            if (Clip* k = clipById(s, part)) k->audioAngle = audio;
    }
    return res;
}

Result setAudioAngle(Project& p, Sequence& s, Id clipId, int audioAngle) {
    Clip* c = clipById(s, clipId);
    if (!c) return Result::fail("The clip no longer exists");
    const Sequence* mc = multicamSequence(p, *c);
    if (!mc) return Result::fail("Not a multicam clip");
    if (audioAngle < -1 || audioAngle >= int(mc->audioTracks.size())) return Result::fail("No such audio track");
    c->audioAngle = audioAngle;
    return {};
}

Result applyAngleChanges(Project& p, Sequence& s, Id clipId, const std::vector<std::pair<FrameTime, int>>& changes,
                         bool audioFollows) {
    const Clip* c = clipById(s, clipId);
    if (!c) return Result::fail("The clip no longer exists");
    const Sequence* mc = multicamSequence(p, *c);
    if (!mc) return Result::fail("Not a multicam clip");
    if (c->reverse) return Result::fail("Reversed multicam clips cannot be switched automatically");
    if (changes.empty()) return {};
    // Multicam frame f shows at timeline frame start + (f * outer/inner fps - sourceIn) / speed.
    const double ratio = s.fpsValue() / mc->fpsValue();
    const Clip clip = *c;
    auto toTimeline = [&](FrameTime f) { return clip.start + FrameTime(std::llround(clip.localForSource(double(f) * ratio))); };
    // The angle in effect at the clip's first frame, then every change inside it.
    int initial = changes.front().second;
    for (const auto& [f, a] : changes)
        if (toTimeline(f) <= clip.start) initial = a;
    Result res = switchAngle(p, s, clipId, initial, clip.start, false, audioFollows);
    if (!res.ok) return res;
    Id current = clipId;
    int shown = initial;
    for (const auto& [f, a] : changes) {
        const FrameTime t = toTimeline(f);
        if (t <= clip.start || t >= clip.end() || a == shown) continue;
        Result r = switchAngle(p, s, current, a, t, true, audioFollows);
        if (!r.ok) return r;
        for (Id id : r.created)  // the video part from t on
            if (auto loc = locate(s, id); loc && loc->track.kind == TrackKind::Video) {
                current = id;
                break;
            }
        res.created.insert(res.created.end(), r.created.begin(), r.created.end());
        shown = a;
    }
    return res;
}

Result flattenMulticam(Project& p, Sequence& s, const std::vector<Id>& clips) {
    Result res;
    std::vector<Id> all = expandLinks(s, clips);
    struct Piece {
        TrackRef track;
        Clip clip;
    };
    std::vector<Piece> pieces;
    std::vector<Id> replaced;
    for (Id id : all) {
        auto loc = locate(s, id);
        if (!loc) continue;
        const Clip c = trackAt(s, loc->track)->clips[loc->index];
        const Sequence* mc = multicamSequence(p, c);
        if (!mc || c.speed != 1.0 || c.reverse || c.ramped()) continue;
        const bool video = loc->track.kind == TrackKind::Video;
        const Track* src = nullptr;
        if (video && c.angle >= 0 && c.angle < int(mc->videoTracks.size())) src = &mc->videoTracks[size_t(c.angle)];
        if (!video) {
            const int a = c.audioAngle >= 0 ? c.audioAngle : (mc->audioTracks.size() == 1 ? 0 : -1);
            if (a >= 0 && a < int(mc->audioTracks.size())) src = &mc->audioTracks[size_t(a)];
        }
        if (!src) continue;  // the whole mix stays a multicam clip
        // The clip shows multicam frames [a, b) (same rate: outer frames are inner frames scaled).
        const double ratio = mc->fpsValue() / s.fpsValue();
        const double a = c.sourceIn * ratio, b = (c.sourceIn + double(c.duration)) * ratio;
        for (const Clip& k : src->clips) {
            const double x0 = std::max<double>(double(k.start), a), x1 = std::min<double>(double(k.end()), b);
            if (x1 <= x0) continue;
            const FrameTime t0 = c.start + FrameTime(std::llround((x0 - a) / ratio));
            const FrameTime t1 = c.start + FrameTime(std::llround((x1 - a) / ratio));
            if (t1 <= t0) continue;
            Clip piece = subClip(k, FrameTime(std::floor(x0)), FrameTime(std::ceil(x1)));
            piece.id = p.newId();
            piece.start = t0;
            piece.duration = t1 - t0;
            piece.sourceIn = (k.sourceIn + (x0 - double(k.start)) * k.speed) / ratio;
            piece.linkGroup = 0;
            piece.enabled = c.enabled;
            piece.colorLabel = c.colorLabel;
            if (video) piece.blendMode = c.blendMode;
            // The multicam clip's own settings, from this piece's first frame.
            Clip own = c;
            shiftKeyframes(own, -(t0 - c.start));
            if (!defaultParams(own.motion)) piece.motion = own.motion;
            if (!defaultParams(own.audio)) piece.audio = own.audio;
            for (Effect e : own.effects) {
                e.id = p.newId();
                piece.effects.push_back(e);
            }
            pieces.push_back({loc->track, piece});
        }
        replaced.push_back(id);
    }
    if (replaced.empty()) return Result::fail("Select multicam clips at normal speed to flatten");
    removeClips(p, s, replaced, false);
    for (Piece& pc : pieces) {
        Result r = overwrite(p, s, pc.track, pc.clip);
        if (!r.ok) return r;
        res.created.push_back(pc.clip.id);
    }
    return res;
}

}  // namespace edit

}  // namespace montage
