#include "ClipAnalysis.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "Compositor.h"
#include "core/Automation.h"
#include "core/EditOps.h"
#include "core/Surround.h"
#include "media/Decoder.h"
#include "media/Reframe.h"
#include "media/Segmenter.h"

namespace montage {

namespace {

// Media seconds shown at clip-local frame lt.
double sourceSeconds(const Sequence& s, const Clip& c, FrameTime lt) { return c.sourceFrameAt(c.start + lt) / s.fpsValue(); }

// The clip-local frame showing media time `sec` (nearest).
double localFrame(const Sequence& s, const Clip& c, double sec) { return c.localForSource(sec * s.fpsValue()); }

const MediaItem* videoMedia(const Project& p, const Clip& c, std::string* error) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || m->kind != MediaKind::Video || !m->hasVideo || m->path.empty()) {
        if (error) *error = "Tracking needs a video clip";
        return nullptr;
    }
    return m;
}

}  // namespace

bool analyzeClipStabilization(const Project& p, const Sequence& s, const Clip& c, std::string& motion,
                              const TrackProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    const MediaItem* m = videoMedia(p, c, error);
    if (!m) return false;
    const double a = sourceSeconds(s, c, 0), b = sourceSeconds(s, c, c.duration - 1);
    const double from = std::max(0.0, std::min(a, b) - 1.0);
    const double to = std::min(m->duration, std::max(a, b) + 1.0);
    CameraMotion cm = analyzeCameraMotion(m->path, from, to, progress, cancel, error);
    if (cm.steps.empty()) return false;
    motion = cameraMotionToString(cm);
    return true;
}

bool trackClipMask(const Project& p, const Sequence& s, const Clip& c, const Effect& e, FrameTime fromLocal, bool forward,
                   MotionModel model, std::vector<std::pair<FrameTime, TrackRegion>>& keys, const TrackProgress& progress,
                   const std::atomic<bool>* cancel, std::string* error) {
    keys.clear();
    const MediaItem* m = videoMedia(p, c, error);
    if (!m) return false;
    fromLocal = std::clamp<FrameTime>(fromLocal, 0, c.duration - 1);
    const FrameTime toLocal = forward ? c.duration - 1 : 0;
    if (fromLocal == toLocal) {
        if (error) *error = forward ? "Already at the clip's last frame" : "Already at the clip's first frame";
        return false;
    }
    TrackRegion start{e.p("mask.x", fromLocal, 0.5), e.p("mask.y", fromLocal, 0.5), e.p("mask.w", fromLocal, 0.4),
                      e.p("mask.h", fromLocal, 0.4), e.p("mask.rotation", fromLocal, 0)};
    const double fromSec = sourceSeconds(s, c, fromLocal), toSec = sourceSeconds(s, c, toLocal);
    const auto regions = trackRegion(m->path, fromSec, toSec, start, model, progress, cancel, error);
    if (regions.size() < 2) {
        if (error && error->empty()) *error = "Nothing to track in the mask: draw it around something with detail";
        return false;
    }
    const double fps = m->fps.valid() ? m->fps.toDouble() : s.fpsValue();
    const double dir = toSec >= fromSec ? 1 : -1;
    for (size_t k = 0; k < regions.size(); ++k) {
        const FrameTime lt = FrameTime(std::llround(localFrame(s, c, fromSec + dir * double(k) / fps)));
        if (lt < 0 || lt >= c.duration) continue;
        if (!keys.empty() && keys.back().first == lt) keys.back().second = regions[k];  // several media frames per sequence frame
        else keys.push_back({lt, regions[k]});
    }
    return keys.size() >= 2;
}

void applyMaskTrack(Effect& e, const std::vector<std::pair<FrameTime, TrackRegion>>& keys) {
    if (keys.empty()) return;
    FrameTime lo = keys.front().first, hi = lo;
    for (const auto& [t, r] : keys) {
        lo = std::min(lo, t);
        hi = std::max(hi, t);
    }
    auto write = [&](const char* name, double TrackRegion::*field) {
        Param& prm = e.params[name];
        prm.keys.erase(std::remove_if(prm.keys.begin(), prm.keys.end(), [&](const Keyframe& k) { return k.t >= lo && k.t <= hi; }),
                       prm.keys.end());
        for (const auto& [t, r] : keys) prm.addKey(t, r.*field, Interp::Linear);
    };
    write("mask.x", &TrackRegion::x);
    write("mask.y", &TrackRegion::y);
    write("mask.w", &TrackRegion::w);
    write("mask.h", &TrackRegion::h);
    write("mask.rotation", &TrackRegion::rotation);
}

// ---- Corner pins -------------------------------------------------------------------

namespace {

const char* const kCornerParams[4][2] = {{"tl_x", "tl_y"}, {"tr_x", "tr_y"}, {"br_x", "br_y"}, {"bl_x", "bl_y"}};

bool hasPicture(const Project& p, const Clip& c) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    return c.enabled && m && m->kind == MediaKind::Video && m->hasVideo && !m->path.empty();
}

// Corners from one clip's frame into another's through the sequence frame at timeline frame t.
bool mapQuad(const Project& p, const Sequence& s, const Clip& from, const Clip& to, FrameTime t, TrackQuad& q) {
    if (from.id == to.id) return true;
    for (Point2& pt : q.p) {
        double x, y;
        if (!clipFrameToSequence(p, s, from, t, pt.x, pt.y, x, y) || !sequenceToClipFrame(p, s, to, t, x, y, pt.x, pt.y))
            return false;
    }
    return true;
}

}  // namespace

const Clip* footageBeneath(const Project& p, const Sequence& s, const Clip& c, FrameTime local) {
    const auto loc = edit::locate(s, c.id);
    const FrameTime t = c.start + local;
    if (loc && loc->track.kind == TrackKind::Video)
        for (int i = loc->track.index - 1; i >= 0; --i) {
            const Track& tr = s.videoTracks[size_t(i)];
            if (tr.muted) continue;
            const Clip* below = edit::clipAt(s, TrackRef{TrackKind::Video, i}, t);
            if (below && hasPicture(p, *below)) return below;
        }
    return nullptr;
}

const Clip* cornerTrackSource(const Project& p, const Sequence& s, const Clip& c, FrameTime local) {
    if (const Clip* below = footageBeneath(p, s, c, local)) return below;
    return hasPicture(p, c) ? &c : nullptr;
}

TrackQuad cornerPinQuad(const Effect& e, FrameTime t) {
    TrackQuad q;
    for (int k = 0; k < 4; ++k) q.p[k] = {e.p(kCornerParams[k][0], t, q.p[k].x), e.p(kCornerParams[k][1], t, q.p[k].y)};
    return q;
}

bool trackClipCorners(const Project& p, const Sequence& s, const Clip& c, const Effect& e, FrameTime fromLocal,
                      bool forward, std::vector<std::pair<FrameTime, TrackQuad>>& keys, const TrackProgress& progress,
                      const std::atomic<bool>* cancel, std::string* error) {
    keys.clear();
    fromLocal = std::clamp<FrameTime>(fromLocal, 0, c.duration - 1);
    const Clip* src = cornerTrackSource(p, s, c, fromLocal);
    if (!src) {
        if (error) *error = "Nothing to track: put the clip over video of the surface, or use a video clip";
        return false;
    }
    const MediaItem* m = p.findMedia(src->mediaId);
    // Timeline frames: from the playhead to the end (start) of whichever clip stops first.
    const FrameTime from = c.start + fromLocal;
    const FrameTime to = forward ? std::min(c.end(), src->end()) - 1 : std::max(c.start, src->start);
    if (from == to) {
        if (error) *error = forward ? "Already at the last frame to track" : "Already at the first frame to track";
        return false;
    }
    TrackQuad start = cornerPinQuad(e, fromLocal);
    if (!mapQuad(p, s, c, *src, from, start)) {
        if (error) *error = "The clips have no picture to track";
        return false;
    }
    const double fromSec = sourceSeconds(s, *src, from - src->start), toSec = sourceSeconds(s, *src, to - src->start);
    const auto quads = trackQuad(m->path, fromSec, toSec, start, progress, cancel, error);
    if (quads.size() < 2) {
        if (error && error->empty()) *error = "The surface was lost straight away";
        return false;
    }
    const double fps = m->fps.valid() ? m->fps.toDouble() : s.fpsValue();
    const double dir = toSec >= fromSec ? 1 : -1;
    for (size_t k = 0; k < quads.size(); ++k) {
        const FrameTime t = src->start + FrameTime(std::llround(localFrame(s, *src, fromSec + dir * double(k) / fps)));
        if (t < c.start || t >= c.end() || t < src->start || t >= src->end()) continue;
        TrackQuad q = quads[k];
        if (!mapQuad(p, s, *src, c, t, q)) continue;
        const FrameTime lt = t - c.start;
        if (!keys.empty() && keys.back().first == lt) keys.back().second = q;  // several media frames per sequence frame
        else keys.push_back({lt, q});
    }
    if (keys.size() < 2 && error) *error = "The surface was lost straight away";
    return keys.size() >= 2;
}

void applyCornerTrack(Effect& e, const std::vector<std::pair<FrameTime, TrackQuad>>& keys) {
    if (keys.empty()) return;
    FrameTime lo = keys.front().first, hi = lo;
    for (const auto& kv : keys) {
        lo = std::min(lo, kv.first);
        hi = std::max(hi, kv.first);
    }
    for (int k = 0; k < 4; ++k)
        for (int axis = 0; axis < 2; ++axis) {
            Param& prm = e.params[kCornerParams[k][axis]];
            prm.keys.erase(std::remove_if(prm.keys.begin(), prm.keys.end(), [&](const Keyframe& kf) { return kf.t >= lo && kf.t <= hi; }),
                           prm.keys.end());
            for (const auto& [t, q] : keys) prm.addKey(t, axis == 0 ? q.p[k].x : q.p[k].y, Interp::Linear);
        }
}

// ---- Following -----------------------------------------------------------------------

bool trackClipFollow(const Project& p, const Sequence& s, const Clip& c, FrameTime fromLocal, bool forward,
                     MotionModel model, double size, std::vector<FollowKey>& keys, const TrackProgress& progress,
                     const std::atomic<bool>* cancel, std::string* error) {
    keys.clear();
    fromLocal = std::clamp<FrameTime>(fromLocal, 0, c.duration - 1);
    const Clip* src = footageBeneath(p, s, c, fromLocal);
    if (!src) {
        if (error) *error = "Nothing to follow: put this clip on a track above video";
        return false;
    }
    const MediaItem* m = p.findMedia(src->mediaId);
    const FrameTime from = c.start + fromLocal;
    const FrameTime to = forward ? std::min(c.end(), src->end()) - 1 : std::max(c.start, src->start);
    if (from == to) {
        if (error) *error = forward ? "Already at the last frame to track" : "Already at the first frame to track";
        return false;
    }
    // Where the clip's anchor point lands on screen, and a square around it, in the footage's frame.
    const double x0 = c.motion.p("pos_x", fromLocal), y0 = c.motion.p("pos_y", fromLocal);
    const double scale0 = c.motion.p("scale", fromLocal, 100), rot0 = c.motion.p("rotation", fromLocal);
    const double cx = s.width / 2.0 + x0, cy = s.height / 2.0 + y0, half = std::clamp(size, 0.02, 1.0) * s.height / 2;
    double u, v, ux, vx, uy, vy;
    if (!sequenceToClipFrame(p, s, *src, from, cx, cy, u, v) || !sequenceToClipFrame(p, s, *src, from, cx + half, cy, ux, vx) ||
        !sequenceToClipFrame(p, s, *src, from, cx, cy + half, uy, vy)) {
        if (error) *error = "The footage beneath has no picture there";
        return false;
    }
    const TrackRegion start{u, v, 2 * std::hypot(ux - u, vx - v), 2 * std::hypot(uy - u, vy - v), 0};
    const double fromSec = sourceSeconds(s, *src, from - src->start), toSec = sourceSeconds(s, *src, to - src->start);
    const auto regions = trackRegion(m->path, fromSec, toSec, start, model, progress, cancel, error);
    if (regions.size() < 2) {
        if (error && error->empty()) *error = "Lost straight away: put the clip's position on something with detail";
        return false;
    }
    const double fps = m->fps.valid() ? m->fps.toDouble() : s.fpsValue();
    const double dir = toSec >= fromSec ? 1 : -1;
    for (size_t k = 0; k < regions.size(); ++k) {
        const FrameTime t = src->start + FrameTime(std::llround(localFrame(s, *src, fromSec + dir * double(k) / fps)));
        if (t < c.start || t >= c.end() || t < src->start || t >= src->end()) continue;
        double x, y;
        if (!clipFrameToSequence(p, s, *src, t, regions[k].x, regions[k].y, x, y)) continue;
        FollowKey key{t - c.start, x - s.width / 2.0, y - s.height / 2.0, scale0 * regions[k].w / std::max(1e-9, start.w),
                      rot0 + regions[k].rotation - start.rotation};
        if (!keys.empty() && keys.back().t == key.t) keys.back() = key;  // several media frames per sequence frame
        else keys.push_back(key);
    }
    if (keys.size() < 2 && error) *error = "Lost straight away: put the clip's position on something with detail";
    return keys.size() >= 2;
}

void applyFollow(Clip& c, const std::vector<FollowKey>& keys, MotionModel model) {
    if (keys.empty()) return;
    FrameTime lo = keys.front().t, hi = lo;
    for (const FollowKey& k : keys) {
        lo = std::min(lo, k.t);
        hi = std::max(hi, k.t);
    }
    auto write = [&](const char* name, double FollowKey::*field) {
        Param& prm = c.motion.params[name];
        prm.keys.erase(std::remove_if(prm.keys.begin(), prm.keys.end(), [&](const Keyframe& k) { return k.t >= lo && k.t <= hi; }),
                       prm.keys.end());
        for (const FollowKey& k : keys) prm.addKey(k.t, k.*field, Interp::Linear);
    };
    write("pos_x", &FollowKey::x);
    write("pos_y", &FollowKey::y);
    if (model != MotionModel::Translation) write("scale", &FollowKey::scale);
    if (model == MotionModel::Similarity) write("rotation", &FollowKey::rotation);
}

// ---- Panning that follows the picture ------------------------------------------------

const Clip* panFollowSource(const Project& p, const Sequence& s, const Clip& a, FrameTime t) {
    for (Id id : edit::linkedClips(s, a.id)) {
        const auto loc = edit::locate(s, id);
        const Clip* c = edit::clipById(s, id);
        if (loc && loc->track.kind == TrackKind::Video && !trackAt(s, loc->track)->muted && c && t >= c->start &&
            t < c->end() && hasPicture(p, *c))
            return c;
    }
    for (int i = int(s.videoTracks.size()) - 1; i >= 0; --i) {
        if (s.videoTracks[size_t(i)].muted) continue;
        const Clip* c = edit::clipAt(s, TrackRef{TrackKind::Video, i}, t);
        if (c && hasPicture(p, *c)) return c;
    }
    return nullptr;
}

bool panFollowSubject(const Project& p, const Sequence& s, const Clip& a, FrameTime t, double& x, double& y,
                      std::string* error) {
    const Clip* src = panFollowSource(p, s, a, t);
    if (!src) {
        if (error) *error = "No picture to follow here: the sound needs video on screen with it";
        return false;
    }
    const MediaItem* m = p.findMedia(src->mediaId);
    const double fps = m->fps.valid() ? m->fps.toDouble() : s.fpsValue();
    const double sec = sourceSeconds(s, *src, t - src->start);
    // The frame before too, for what moves.
    const auto pts = findSubject(m->path, std::max(0.0, sec - 1 / fps), sec, 1 / fps, {}, nullptr, error);
    if (pts.empty()) return false;
    double X, Y;
    if (!clipFrameToSequence(p, s, *src, t, pts.back().x, pts.back().y, X, Y)) {
        if (error) *error = "The picture has nothing there to follow";
        return false;
    }
    x = std::clamp(X / s.width, 0.0, 1.0);
    y = std::clamp(Y / s.height, 0.0, 1.0);
    return true;
}

bool trackPanFollow(const Project& p, const Sequence& s, const Clip& a, FrameTime from, double x, double y, double size,
                    std::vector<PanFollowKey>& keys, const TrackProgress& progress, const std::atomic<bool>* cancel,
                    std::string* error) {
    keys.clear();
    if (a.duration <= 0) return false;
    from = std::clamp<FrameTime>(from, a.start, a.end() - 1);
    const Clip* src = panFollowSource(p, s, a, from);
    if (!src) {
        if (error) *error = "No picture to follow here: the sound needs video on screen with it";
        return false;
    }
    const MediaItem* m = p.findMedia(src->mediaId);
    // The square around the point, in the footage's own frame.
    const double cx = std::clamp(x, 0.0, 1.0) * s.width, cy = std::clamp(y, 0.0, 1.0) * s.height;
    const double half = std::clamp(size, 0.02, 1.0) * s.height / 2;
    double u, v, ux, vx, uy, vy;
    if (!sequenceToClipFrame(p, s, *src, from, cx, cy, u, v) || !sequenceToClipFrame(p, s, *src, from, cx + half, cy, ux, vx) ||
        !sequenceToClipFrame(p, s, *src, from, cx, cy + half, uy, vy)) {
        if (error) *error = "The picture has nothing there to follow";
        return false;
    }
    const TrackRegion start{u, v, 2 * std::hypot(ux - u, vx - v), 2 * std::hypot(uy - u, vy - v), 0};
    const FrameTime lo = std::max(a.start, src->start), hi = std::min(a.end(), src->end()) - 1;
    const double fps = m->fps.valid() ? m->fps.toDouble() : s.fpsValue();
    const double span = double(std::max<FrameTime>(1, hi - lo));
    // One direction from `from` to timeline frame `to`, as keys in tracking order.
    auto run = [&](FrameTime to, double offset, double share, std::vector<PanFollowKey>& out) -> bool {
        if (to == from) return true;
        const double fromSec = sourceSeconds(s, *src, from - src->start), toSec = sourceSeconds(s, *src, to - src->start);
        TrackProgress part;
        if (progress) part = [&](double f) { progress(offset + f * share); };
        std::string why;
        const auto regions = trackRegion(m->path, fromSec, toSec, start, MotionModel::Translation, part, cancel, &why);
        if (cancel && cancel->load()) {
            if (error) *error = why.empty() ? "Stopped" : why;
            return false;
        }
        const double dir = toSec >= fromSec ? 1 : -1;
        for (size_t k = 0; k < regions.size(); ++k) {
            const FrameTime t = src->start + FrameTime(std::llround(localFrame(s, *src, fromSec + dir * double(k) / fps)));
            if (t < lo || t > hi) continue;
            double X, Y;
            if (!clipFrameToSequence(p, s, *src, t, regions[k].x, regions[k].y, X, Y)) continue;
            const PanFollowKey key{t, X / s.width, Y / s.height};
            if (!out.empty() && out.back().t == t) out.back() = key;  // several media frames per sequence frame
            else out.push_back(key);
        }
        return true;
    };
    std::vector<PanFollowKey> ahead, behind;
    if (!run(hi, 0, double(hi - from) / span, ahead) || !run(lo, double(hi - from) / span, double(from - lo) / span, behind))
        return false;
    // Backwards from `from`, then forwards from it (the first key of each being `from` itself).
    for (auto it = behind.rbegin(); it != behind.rend(); ++it) keys.push_back(*it);
    for (const PanFollowKey& k : ahead)
        if (keys.empty() || k.t > keys.back().t) keys.push_back(k);
    if (keys.empty()) keys.push_back({from, std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0)});
    if (keys.size() < 2 && error) *error = "Lost straight away: pick something with detail to follow";
    return keys.size() >= 2;
}

void panFollowPosition(const Sequence& s, double x, double width, bool stereo, double& panX, double& panY) {
    x = std::clamp(x, 0.0, 1.0);
    width = std::clamp(width, 0.0, 1.0);
    if (s.spherical) {
        // Longitude across the picture, straight ahead in the middle.
        const double azimuth = (x - 0.5) * (s.vr180 ? 180.0 : 360.0) * M_PI / 180;
        panX = std::sin(azimuth);
        panY = std::cos(azimuth);
        return;
    }
    panX = (2 * x - 1) * width * (stereo ? 1.0 : std::tan(30 * M_PI / 180));
    panY = 1;
}

bool applyPanFollow(const Sequence& s, Track& t, const std::vector<PanFollowKey>& keys, double width) {
    if (keys.size() < 2 || t.kind != TrackKind::Audio) return false;
    const bool stereo = layoutChannels(s.audioLayout) <= 2;
    // A pixel or two of tracking jitter would be heard as the sound wobbling: a five-frame moving average.
    std::vector<double> xs(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        double sum = 0;
        int n = 0;
        for (size_t j = i >= 2 ? i - 2 : 0; j <= std::min(keys.size() - 1, i + 2); ++j, ++n) sum += keys[j].x;
        xs[i] = sum / n;
    }
    const FrameTime lo = keys.front().t, hi = keys.back().t;
    auto write = [&](Param& lane, double still, bool wantY) {
        const double before = lane.animated() ? lane.at(lo - 1) : still, after = lane.animated() ? lane.at(hi + 1) : still;
        const bool keyBefore = lane.keyAt(lo - 1) != nullptr, keyAfter = lane.keyAt(hi + 1) != nullptr;
        std::erase_if(lane.keys, [&](const Keyframe& k) { return k.t >= lo && k.t <= hi; });
        std::vector<Keyframe> path;
        for (size_t i = 0; i < keys.size(); ++i) {
            double px, py;
            panFollowPosition(s, xs[i], width, stereo, px, py);
            path.push_back({keys[i].t, wantY ? py : px});
        }
        thinKeys(path, 0.005);
        for (const Keyframe& k : path) lane.addKey(k.t, k.v, Interp::Linear);
        // Outside the span the lane stays what it was (a lane with no keys before plays its static value there).
        if (lo > 0 && !keyBefore) lane.addKey(lo - 1, before, Interp::Linear);
        if (!keyAfter) lane.addKey(hi + 1, after, Interp::Linear);
    };
    if (stereo) {
        write(t.panAuto, t.pan, false);
    } else {
        write(t.surroundXAuto, t.surround.x, false);
        if (s.spherical) write(t.surroundYAuto, t.surround.y, true);
    }
    if (t.automation == int(AutomationMode::Off)) t.automation = int(AutomationMode::Read);
    return true;
}

// ---- Auto Reframe -----------------------------------------------------------------

bool analyzeClipReframe(const Project& p, const Sequence& s, const Clip& c, int speed, std::vector<ReframeKey>& path,
                        const TrackProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    path.clear();
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image) || m->path.empty()) {
        if (error) *error = "Auto Reframe needs a video or still clip";
        return false;
    }
    if (m->kind == MediaKind::Image) {
        const auto pts = findSubject(m->path, 0, 0, 1, progress, cancel, error);
        if (pts.empty()) return false;
        path.push_back({0, pts[0].x, pts[0].y});
        return true;
    }
    const double a = sourceSeconds(s, c, 0), b = sourceSeconds(s, c, c.duration - 1);
    // Five samples a second of the clip as it plays (ten when following fast action).
    const double every = speed >= 2 ? 0.1 : 0.2;
    const double step = std::max(0.02, every * std::fabs(b - a) / std::max(1e-6, double(c.duration - 1) / s.fpsValue()));
    const auto pts = findSubject(m->path, a, b, c.duration > 1 ? step : 1, progress, cancel, error);
    if (pts.empty()) return false;
    static const double smooth[3] = {1.6, 0.8, 0.35};  // seconds
    const auto sm = smoothSubjectPath(pts, smooth[std::clamp(speed, 0, 2)] * std::max(0.05, std::fabs(b - a)) /
                                               std::max(1e-6, double(std::max<FrameTime>(1, c.duration - 1)) / s.fpsValue()));
    for (const SubjectPoint& q : sm) {
        const FrameTime t = FrameTime(std::llround(localFrame(s, c, q.t)));
        if (t < 0 || t >= c.duration) continue;
        if (!path.empty() && path.back().t == t) continue;
        path.push_back({t, q.x, q.y});
    }
    std::sort(path.begin(), path.end(), [](const ReframeKey& l, const ReframeKey& r) { return l.t < r.t; });
    return !path.empty();
}

void applyReframe(const Project& p, const Sequence& s, Clip& c, const std::vector<ReframeKey>& path) {
    if (path.empty()) return;
    double mw = 0, mh = 0;
    if (!clipFrameSize(p, s, c, mw, mh) || mw <= 0 || mh <= 0) return;
    c.motion.params["fit"] = Param(1.0);  // fill the frame
    const double scale = c.motion.p("scale", 0, 100) / 100.0;
    const double fill = std::max(s.width / mw, s.height / mh) * scale;
    const double dw = mw * fill * std::fabs(c.motion.p("scale_x", 0, 100) / 100.0);
    const double dh = mh * fill * std::fabs(c.motion.p("scale_y", 0, 100) / 100.0);
    const double slackX = std::max(0.0, (dw - s.width) / 2), slackY = std::max(0.0, (dh - s.height) / 2);
    Param px, py;
    bool moves = false;
    for (const ReframeKey& k : path) {
        const double x = std::clamp(-(k.x - 0.5) * dw, -slackX, slackX), y = std::clamp(-(k.y - 0.5) * dh, -slackY, slackY);
        px.addKey(k.t, x, Interp::Linear);
        py.addKey(k.t, y, Interp::Linear);
        moves = moves || std::fabs(x - px.keys.front().v) > 0.5 || std::fabs(y - py.keys.front().v) > 0.5;
    }
    // A subject that stays put gives a still frame, not a row of identical keys.
    c.motion.params["pos_x"] = moves ? px : Param(px.keys.front().v);
    c.motion.params["pos_y"] = moves ? py : Param(py.keys.front().v);
}

bool analyzeSequenceReframe(const Project& p, const Sequence& s, int speed, std::map<Id, std::vector<ReframeKey>>& paths,
                            const TrackProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    paths.clear();
    std::vector<const Clip*> clips;
    for (const Track& t : s.videoTracks)
        for (const Clip& c : t.clips) {
            const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
            if (m && (m->kind == MediaKind::Video || m->kind == MediaKind::Image) && !m->path.empty()) clips.push_back(&c);
        }
    for (size_t i = 0; i < clips.size(); ++i) {
        if (cancel && cancel->load()) return false;
        std::vector<ReframeKey> path;
        std::string err;
        const auto part = [&](double f) {
            if (progress) progress((double(i) + f) / double(clips.size()));
        };
        if (analyzeClipReframe(p, s, *clips[i], speed, path, part, cancel, &err)) paths[clips[i]->id] = std::move(path);
        else if (error && error->empty()) *error = err;  // kept for the caller; other clips still go ahead
    }
    return !(cancel && cancel->load());
}

void reframeSize(const Sequence& s, int aspectW, int aspectH, int& width, int& height) {
    const int shortSide = std::min(s.width, s.height);
    auto even = [](double v) { return std::max(2, int(std::lround(v / 2)) * 2); };
    if (aspectW >= aspectH) {
        height = even(shortSide);
        width = even(double(shortSide) * aspectW / std::max(1, aspectH));
    } else {
        width = even(shortSide);
        height = even(double(shortSide) * aspectH / std::max(1, aspectW));
    }
}

Id makeReframedSequence(Project& p, Id seq, int width, int height, const std::map<Id, std::vector<ReframeKey>>& paths,
                        const std::string& name) {
    const Sequence* src = p.findSequence(seq);
    if (!src) return 0;
    const int g = std::gcd(width, height);
    const std::string label = name.empty() ? src->name + " " + std::to_string(width / std::max(1, g)) + ":" +
                                                 std::to_string(height / std::max(1, g))
                                           : name;
    std::map<Id, Id> ids;
    const Id out = edit::duplicateSequence(p, seq, label, &ids);
    Sequence* s = p.findSequence(out);
    s->width = width;
    s->height = height;
    for (MediaItem& m : p.media)
        if (m.kind == MediaKind::Sequence && m.sequenceId == out) {
            m.width = width;
            m.height = height;
        }
    for (const auto& [oldId, path] : paths) {
        auto it = ids.find(oldId);
        if (it == ids.end()) continue;
        if (Clip* c = edit::clipById(*s, it->second)) applyReframe(p, *s, *c, path);
    }
    return out;
}

// ---- Object masks ---------------------------------------------------------------

namespace {

double mediaFps(const Sequence& s, const MediaItem& m) { return m.fps.valid() ? m.fps.toDouble() : s.fpsValue(); }

ObjectMask objectFor(const Sequence& s, const MediaItem& m, const ObjectMask* existing) {
    ObjectMask o = existing ? *existing : ObjectMask{};
    if (o.fps <= 0) o.fps = mediaFps(s, m);
    return o;
}

// The media frame range a clip shows (inclusive, ascending).
void clipFrameRange(const Sequence& s, const Clip& c, const ObjectMask& o, const MediaItem& m, int64_t& lo, int64_t& hi) {
    const int64_t a = o.frameAt(sourceSeconds(s, c, 0)), b = o.frameAt(sourceSeconds(s, c, c.duration - 1));
    lo = std::max<int64_t>(0, std::min(a, b));
    hi = std::max(a, b);
    if (m.duration > 0) hi = std::min<int64_t>(hi, std::max<int64_t>(0, int64_t(std::ceil(m.duration * o.fps)) - 1));
}

// Decodes media frame n at the model's input size.
Frame16Ptr modelFrame(VideoDecoder& dec, const ObjectMask& o, int64_t n) {
    return dec.frameAt((double(n) + 0.5) / o.fps, kSegmenterInput, kSegmenterInput, true);
}

}  // namespace

int64_t clipObjectFrame(const Project& p, const Sequence& s, const Clip& c, FrameTime local) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m) return 0;
    ObjectMask o;
    o.fps = mediaFps(s, *m);
    return o.frameAt(sourceSeconds(s, c, local));
}

std::shared_ptr<ObjectMask> withObjectPoint(const Project& p, const Sequence& s, const Clip& c, const Effect& e,
                                            FrameTime local, const ObjectPoint& point) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m) return nullptr;
    auto o = std::make_shared<ObjectMask>(objectFor(s, *m, e.object.get()));
    o->prompts[o->frameAt(sourceSeconds(s, c, local))].push_back(point);
    return o;
}

std::shared_ptr<ObjectMask> withObjectPrompts(const Project& p, const Sequence& s, const Clip& c, const Effect& e,
                                              FrameTime local, const std::vector<ObjectPoint>& points) {
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m) return nullptr;
    auto o = std::make_shared<ObjectMask>(objectFor(s, *m, e.object.get()));
    const int64_t n = o->frameAt(sourceSeconds(s, c, local));
    if (points.empty()) {
        o->prompts.erase(n);
        o->frames.erase(n);
    } else {
        o->prompts[n] = points;
    }
    return o;
}

std::shared_ptr<const ObjectMask> segmentClipObjectFrame(const Project& p, const Sequence& s, const Clip& c,
                                                         const ObjectMask& object, FrameTime local, std::string* error) {
    const MediaItem* m = videoMedia(p, c, error);
    if (!m) return nullptr;
    auto o = std::make_shared<ObjectMask>(objectFor(s, *m, &object));
    const int64_t n = o->frameAt(sourceSeconds(s, c, local));
    auto it = o->prompts.find(n);
    if (it == o->prompts.end() || it->second.empty()) {
        // No clicks left on this frame: it is no longer segmented.
        o->frames.erase(n);
        return o;
    }
    VideoDecoder dec;
    if (!dec.open(m->path, error)) return nullptr;
    Frame16Ptr f = modelFrame(dec, *o, n);
    if (!f) {
        if (error) *error = "Cannot decode the frame";
        return nullptr;
    }
    ObjectTracker tracker;
    SegmentResult r;
    if (!tracker.step(*f, it->second, r, error)) return nullptr;
    o->frames[n] = packObjectLogits(r.logits.data());
    return o;
}

std::shared_ptr<const ObjectMask> trackClipObject(const Project& p, const Sequence& s, const Clip& c, const ObjectMask& object,
                                                  FrameTime fromLocal, bool forward, const TrackProgress& progress,
                                                  const std::atomic<bool>* cancel, std::string* error) {
    const MediaItem* m = videoMedia(p, c, error);
    if (!m) return nullptr;
    auto o = std::make_shared<ObjectMask>(objectFor(s, *m, &object));
    int64_t lo = 0, hi = 0;
    clipFrameRange(s, c, *o, *m, lo, hi);
    fromLocal = std::clamp<FrameTime>(fromLocal, 0, std::max<FrameTime>(0, c.duration - 1));
    const int64_t from = std::clamp(o->frameAt(sourceSeconds(s, c, fromLocal)), lo, hi);
    // Media direction: the clip's direction, flipped for reversed clips.
    const bool mediaForward = (sourceSeconds(s, c, std::max<FrameTime>(0, c.duration - 1)) >= sourceSeconds(s, c, 0)) == forward;
    const int dir = mediaForward ? 1 : -1;
    // Start at the nearest clicked frame behind the playhead.
    int64_t start = -1;
    for (const auto& [n, pts] : o->prompts) {
        if (pts.empty() || n < lo || n > hi) continue;
        if (mediaForward ? n <= from && (start < 0 || n > start) : n >= from && (start < 0 || n < start)) start = n;
    }
    if (start < 0) {
        if (error) *error = forward ? "Click the object on this frame, or an earlier one, first"
                                    : "Click the object on this frame, or a later one, first";
        return nullptr;
    }
    const int64_t end = mediaForward ? hi : lo;
    VideoDecoder dec;
    if (!dec.open(m->path, error)) return nullptr;
    ObjectTracker tracker;
    if (!tracker.load(error)) return nullptr;
    const double total = double(std::llabs(end - start) + 1);
    for (int64_t n = start;; n += dir) {
        if (cancel && cancel->load()) {
            if (error) error->clear();
            return nullptr;
        }
        Frame16Ptr f = modelFrame(dec, *o, n);
        if (!f) break;  // past the media's last frame
        static const std::vector<ObjectPoint> none;
        auto it = o->prompts.find(n);
        SegmentResult r;
        if (!tracker.step(*f, it != o->prompts.end() ? it->second : none, r, error)) return nullptr;
        o->frames[n] = packObjectLogits(r.logits.data());
        if (progress) progress(double(std::llabs(n - start) + 1) / total);
        if (n == end) break;
    }
    return o;
}

}  // namespace montage
