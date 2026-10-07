#include "ClipAnalysis.h"

#include <algorithm>
#include <cmath>

namespace montage {

namespace {

// Media seconds shown at clip-local frame lt.
double sourceSeconds(const Sequence& s, const Clip& c, FrameTime lt) { return c.sourceFrameAt(c.start + lt) / s.fpsValue(); }

// The clip-local frame showing media time `sec` (nearest).
double localFrame(const Sequence& s, const Clip& c, double sec) {
    double local = (sec * s.fpsValue() - c.sourceIn) / c.speed;
    if (c.reverse) local = double(c.duration - 1) - local;
    return local;
}

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

}  // namespace montage
