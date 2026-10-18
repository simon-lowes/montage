#include "FollowFocus.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/ObjectMask.h"
#include "media/FaceTracks.h"
#include "media/Tracking.h"
#include "media/VideoDepth.h"

namespace montage {

std::vector<double> pullFocus(const std::vector<double>& target, double fps, double ease) {
    std::vector<double> held(target.size(), -1);
    // Lost frames hold the last distance (the first ones take the first found).
    double last = -1;
    for (size_t i = 0; i < target.size(); ++i) {
        if (target[i] >= 0) last = target[i];
        held[i] = last;
    }
    for (size_t i = held.size(); i-- > 0;)
        if (held[i] < 0) held[i] = i + 1 < held.size() ? held[i + 1] : 0.5;
    if (ease <= 0 || held.empty()) return held;
    // A Gaussian over the frames around each, `ease` seconds across (two standard deviations): the move eased into and
    // out of, centred on it as a puller who knows the shot times it, not trailing it.
    const double sigma = std::max(1e-3, ease * std::max(1.0, fps) / 2);
    const int reach = int(std::ceil(3 * sigma));
    std::vector<double> out(held.size());
    for (size_t i = 0; i < held.size(); ++i) {
        double sum = 0, weight = 0;
        for (int k = -reach; k <= reach; ++k) {
            const long j = long(i) + k;
            if (j < 0 || j >= long(held.size())) continue;
            const double w = std::exp(-k * k / (2 * sigma * sigma));
            sum += held[size_t(j)] * w, weight += w;
        }
        out[i] = sum / weight;
    }
    return out;
}

namespace {

double median(std::vector<float>& v) {
    if (v.empty()) return -1;
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(v.size() / 2), v.end());
    return v[v.size() / 2];
}

// The depth inside a box (fractions of the frame, turned by `rotation` degrees about its centre), sampled on a grid.
double boxDepth(const DepthMap& d, double cx, double cy, double w, double h, double rotation) {
    std::vector<float> v;
    const double cr = std::cos(rotation * M_PI / 180), sn = std::sin(rotation * M_PI / 180);
    const int n = 15;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const double rx = ((i + 0.5) / n - 0.5) * w, ry = ((j + 0.5) / n - 0.5) * h;
            const double u = cx + rx * cr - ry * sn, vv = cy + rx * sn + ry * cr;
            if (u >= 0 && vv >= 0 && u <= 1 && vv <= 1) v.push_back(d.at(u, vv));
        }
    return median(v);
}

// Simplified to the keys that keep it within `tolerance` (Ramer–Douglas–Peucker on frame and value).
void keepKeys(const std::vector<double>& v, size_t a, size_t b, double tolerance, std::vector<bool>& keep) {
    if (b <= a + 1) return;
    double worst = 0;
    size_t at = a;
    for (size_t i = a + 1; i < b; ++i) {
        const double line = v[a] + (v[b] - v[a]) * double(i - a) / double(b - a);
        if (const double d = std::fabs(v[i] - line); d > worst) worst = d, at = i;
    }
    if (worst <= tolerance) return;
    keep[at] = true;
    keepKeys(v, a, at, tolerance, keep);
    keepKeys(v, at, b, tolerance, keep);
}

}  // namespace

bool clipMediaRange(const Project& p, const Sequence& seq, const Clip& c, std::string& path, double& from, double& to) {
    const MediaItem* m = c.isGenerator() ? nullptr : p.findMedia(c.mediaId);
    if (!m || m->kind != MediaKind::Video || !m->hasVideo || c.duration <= 0) return false;
    const double frameSeconds = m->fps.valid() ? 1.0 / m->fps.toDouble() : 1.0 / 30;
    from = 1e300, to = -1e300;
    for (FrameTime i = 0; i < c.duration; ++i) {
        const double t = std::clamp(c.sourceFrameAt(c.start + i) / seq.fpsValue(), 0.0, std::max(0.0, m->duration - frameSeconds * 0.5));
        from = std::min(from, t), to = std::max(to, t);
    }
    path = m->path;
    return true;
}

bool analyzeClipDepth(const Project& p, const Sequence& seq, const Clip& c, int size, const std::function<bool(double)>& progress,
                      std::string* error) {
    std::string path;
    double from, to;
    if (!clipMediaRange(p, seq, c, path, from, to)) {
        if (error) *error = "Steady depth is for video clips";
        return false;
    }
    return analyzeVideoDepth(path, from, to, size, progress, error);
}

bool clipDepthAnalysed(const Project& p, const Sequence& seq, const Clip& c) {
    std::string path;
    double from, to, a, b;
    if (!clipMediaRange(p, seq, c, path, from, to) || !analysedDepthRange(path, a, b)) return false;
    const MediaItem* m = p.findMedia(c.mediaId);
    const double frameSeconds = m && m->fps.valid() ? 1.0 / m->fps.toDouble() : 1.0 / 30;
    return a <= from + frameSeconds && b >= to - frameSeconds;
}

bool followFocus(const Project& p, const Sequence& seq, const Clip& c, const Effect& e, const FollowFocusSettings& s, Param& focus,
                 const std::function<bool(double)>& progress, std::string* error) {
    auto fail = [&](const char* why) {
        if (error && error->empty()) *error = why;
        return false;
    };
    const MediaItem* m = c.isGenerator() ? nullptr : p.findMedia(c.mediaId);
    if (!m || m->kind != MediaKind::Video || !m->hasVideo) return fail("Follow Focus is for video clips");
    if (c.duration <= 0) return fail("The clip is empty");
    if (s.target == FocusTarget::Object && (!e.object || e.object->frames.empty()))
        return fail("Pick the object first: set the effect's mask to Object and click it on the monitor, then track it");
    const double fps = seq.fpsValue();
    const double frameSeconds = m->fps.valid() ? 1.0 / m->fps.toDouble() : 1.0 / 30;
    auto mediaTime = [&](FrameTime local) {
        return std::clamp(c.sourceFrameAt(c.start + std::clamp<FrameTime>(local, 0, c.duration - 1)) / fps, 0.0,
                          std::max(0.0, m->duration - frameSeconds * 0.5));
    };
    std::vector<double> times(size_t(c.duration));
    for (FrameTime i = 0; i < c.duration; ++i) times[size_t(i)] = mediaTime(i);
    const double first = *std::min_element(times.begin(), times.end()), last = *std::max_element(times.begin(), times.end());
    // Depth across the clip: its analysis, made now where it does not reach.
    auto stage = [&](double from, double to) {
        return [&progress, from, to](double f) { return !progress || progress(from + (to - from) * f); };
    };
    if (!clipDepthAnalysed(p, seq, c)) {
        if (!videoDepthAvailable() || !videoDepthModel().installed()) return fail("Follow Focus needs the video depth model");
        if (!analyzeVideoDepth(m->path, first, last, s.size, stage(0, 0.6), error)) return false;
    }
    const double at = mediaTime(s.at);
    std::vector<double> target(times.size(), -1);
    if (s.target == FocusTarget::Point) {
        // A small box round the point, tracked from its frame to either end.
        const double aspect = m->height > 0 ? double(m->width) / m->height : 16.0 / 9;
        TrackRegion start;
        start.x = s.x, start.y = s.y, start.w = 0.06, start.h = 0.06 * aspect;
        const std::vector<TrackRegion> ahead = trackRegion(m->path, at, last, start, MotionModel::TranslationScale, stage(0.6, 0.75), nullptr, error);
        const std::vector<TrackRegion> behind = at > first ? trackRegion(m->path, at, first, start, MotionModel::TranslationScale, stage(0.75, 0.9), nullptr, error)
                                                           : std::vector<TrackRegion>{start};
        if (ahead.empty() && behind.empty()) return fail("The point could not be tracked");
        const double mfps = 1 / frameSeconds;
        for (size_t i = 0; i < times.size(); ++i) {
            const double k = (times[i] - at) * mfps;
            const std::vector<TrackRegion>& run = k >= 0 ? ahead : behind;
            const long n = long(std::lround(std::fabs(k)));
            if (run.empty() || n >= long(run.size())) continue;  // lost by then
            const TrackRegion& r = run[size_t(n)];
            if (auto d = analysedDepth(m->path, times[i])) target[i] = boxDepth(*d, r.x, r.y, r.w, r.h, r.rotation);
        }
    } else if (s.target == FocusTarget::Face) {
        FaceTracks tracks;
        if (!trackFaces(m->path, first, last, tracks, stage(0.6, 0.9), error)) return false;
        // The face nearest the point on its frame (or, with none there, the nearest to it where each face is first seen).
        int chosen = -1;
        double best = 1e9;
        auto consider = [&](double when) {
            for (const TrackedFace& f : trackedFacesAt(tracks, when, 2)) {
                const double dx = f.box.x + f.box.w / 2 - s.x, dy = f.box.y + f.box.h / 2 - s.y;
                if (const double dist = dx * dx + dy * dy; dist < best) best = dist, chosen = f.track;
            }
        };
        consider(at);
        if (chosen < 0)
            for (const FaceTrack& t : tracks.tracks)
                if (!t.boxes.empty()) consider(t.boxes.front().time);
        if (chosen < 0) return fail("No face was found in the clip");
        for (size_t i = 0; i < times.size(); ++i)
            for (const TrackedFace& f : trackedFacesAt(tracks, times[i], 2))
                if (f.track == chosen)
                    if (auto d = analysedDepth(m->path, times[i]))
                        // (the middle of the face: its box reaches past the cheeks and hair)
                        target[i] = boxDepth(*d, f.box.x + f.box.w / 2, f.box.y + f.box.h / 2, f.box.w * 0.5, f.box.h * 0.5, 0);
    } else {
        std::vector<float> logits;
        for (size_t i = 0; i < times.size(); ++i) {
            if (!e.object->maskLogitsAt(times[i], logits) || logits.size() != size_t(kObjectGrid) * kObjectGrid) continue;
            auto d = analysedDepth(m->path, times[i]);
            if (!d) continue;
            std::vector<float> inside;
            for (int y = 0; y < kObjectGrid; y += 2)
                for (int x = 0; x < kObjectGrid; x += 2)
                    if (logits[size_t(y) * kObjectGrid + size_t(x)] > 0) inside.push_back(d->at((x + 0.5) / kObjectGrid, (y + 0.5) / kObjectGrid));
            if (inside.size() >= 4) target[i] = median(inside);
        }
    }
    if (std::none_of(target.begin(), target.end(), [](double v) { return v >= 0; })) return fail("The target was not found on any frame");
    const std::vector<double> pulled = pullFocus(target, fps, s.ease);
    std::vector<double> pct(pulled.size());
    for (size_t i = 0; i < pulled.size(); ++i) pct[i] = std::clamp(pulled[i] * 100, 0.0, 100.0);
    std::vector<bool> keep(pct.size(), false);
    keep.front() = keep.back() = true;
    keepKeys(pct, 0, pct.size() - 1, 0.25, keep);
    focus = Param(pct.front());
    for (size_t i = 0; i < pct.size(); ++i)
        if (keep[i]) focus.addKey(FrameTime(i), pct[i]);
    if (progress) progress(1.0);
    return true;
}

}  // namespace montage
