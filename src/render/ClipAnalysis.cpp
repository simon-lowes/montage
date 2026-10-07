#include "ClipAnalysis.h"

#include <algorithm>
#include <cmath>

#include "media/Decoder.h"
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
