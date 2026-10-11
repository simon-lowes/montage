#include "ExtendClip.h"

#include <algorithm>
#include <cmath>

#include "core/EditOps.h"
#include "core/Effects.h"
#include "media/Tracking.h"
#include "render/Compositor.h"

namespace montage {

bool measureEndMotion(const Project& p, const Sequence& s, const Clip& c, EndMotion& out, std::string* error,
                      const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    out = EndMotion{};
    const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
    if (!m || m->kind != MediaKind::Video || m->path.empty()) {
        if (error) *error = "Extend Clip needs a video clip";
        return false;
    }
    const double fps = s.fpsValue();
    const double end = c.sourceFrameAt(c.end() - 1) / fps;  // media seconds of the last frame shown
    const double from = std::max(0.0, end - 1.0);
    if (end - from < 2 / fps) return true;  // too short to tell: a plain hold
    const CameraMotion cam = analyzeCameraMotion(m->path, from, end, progress, cancel, error);
    if (cam.steps.size() < 3) return true;  // nothing to follow (no texture): a plain hold
    // The centre of the picture, frame to frame, over the last half second (steps[0] is the identity).
    const double aspect = m->width > 0 ? double(m->height) / m->width : 9.0 / 16.0;
    const size_t n = std::min<size_t>(cam.steps.size() - 1, size_t(std::max(2.0, std::round(cam.fps * 0.5))));
    double cx = 0, cy = 0, z = 0, a = 0;
    for (size_t i = cam.steps.size() - n; i < cam.steps.size(); ++i) {
        const Similarity& st = cam.steps[i];
        const Point2 centre{0.5, 0.5 * aspect};
        const Point2 moved = st.apply(centre);
        cx += moved.x - centre.x;
        cy += moved.y - centre.y;
        z += std::log(std::max(1e-6, st.scale));
        a += st.angle;
    }
    cx /= double(n), cy /= double(n), z /= double(n), a /= double(n);
    // Per media frame to per sequence frame, then from fractions of the picture's width to sequence pixels as the
    // clip shows it there.
    const double perFrame = (cam.fps > 0 ? cam.fps : fps) / fps;
    cx *= perFrame, cy *= perFrame, z *= perFrame, a *= perFrame;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    const FrameTime t = c.end() - 1;
    if (!clipFrameToSequence(p, s, c, t, 0.5, 0.5, x0, y0) || !clipFrameToSequence(p, s, c, t, 0.5 + cx, 0.5 + cy / aspect, x1, y1))
        return true;
    out.dx = x1 - x0;
    out.dy = y1 - y0;
    out.zoom = z;
    out.turn = a * 180 / M_PI;
    out.samples = int(n);
    return true;
}

Clip makeExtension(Project& p, const Sequence& s, const Clip& c, FrameTime frames, const EndMotion& motion) {
    Clip h = c;
    h.id = p.newId();
    h.name = c.name + " (extended)";
    h.start = c.end();
    h.duration = std::max<FrameTime>(1, frames);
    h.linkGroup = 0;
    const FrameTime last = c.duration - 1;  // clip frame
    // The last frame shown, held.
    h.sourceIn = c.sourceFrameAt(c.end() - 1);
    h.speed = 1;
    h.reverse = false;
    h.timing = makeEffect("time", p.newId());
    h.timing.params["speed"] = Param(0.0);
    // Effects as they are at that frame.
    auto freeze = [&](Effect& e) {
        for (auto& [name, q] : e.params)
            if (q.animated()) q = Param(q.at(last));
    };
    for (Effect& e : h.effects) freeze(e);
    if (h.motion.empty()) h.motion = makeEffect(p, "transform");
    freeze(h.motion);
    // Did the picture fill the frame? Then it must keep doing so as it drifts.
    bool fills = true;
    for (const auto& [u, v] : {std::pair{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.0, 1.0}}) {
        double x = 0, y = 0;
        if (!clipFrameToSequence(p, s, c, c.end() - 1, u, v, x, y)) fills = false;
        else if ((u == 0 && x > 0.5) || (u == 1 && x < s.width - 0.5) || (v == 0 && y > 0.5) || (v == 1 && y < s.height - 0.5)) fills = false;
    }
    const double px = c.motion.p("pos_x", last), py = c.motion.p("pos_y", last);
    const double sc = c.motion.p("scale", last, 100), rot = c.motion.p("rotation", last);
    Param kx, ky, ks, kr;
    double dx = 0, dy = 0, z = 0, a = 0;
    for (FrameTime k = 0; k < h.duration; ++k) {
        if (k > 0) {
            // The velocity falls as the square of the time left: smooth out of the shot, still at the end.
            const double f = std::pow(1.0 - double(k) / double(h.duration), 2);
            dx += motion.dx * f, dy += motion.dy * f, z += motion.zoom * f, a += motion.turn * f;
        }
        double cover = 1;
        if (fills) {
            cover = 1 + 2 * std::max(std::fabs(dx) / s.width, std::fabs(dy) / s.height) + std::fabs(a) * M_PI / 180 * 0.6;
            cover = std::max(cover, std::exp(-std::min(0.0, z)) * cover);  // a pull-out must not show the edges
        }
        kx.addKey(k, px + dx);
        ky.addKey(k, py + dy);
        ks.addKey(k, sc * std::exp(z) * cover);
        kr.addKey(k, rot + a);
    }
    const bool moves = motion.samples > 0 && (std::fabs(motion.dx) + std::fabs(motion.dy) > 1e-3 || std::fabs(motion.zoom) > 1e-6 ||
                                              std::fabs(motion.turn) > 1e-6);
    if (moves && h.duration > 1) {
        h.motion.params["pos_x"] = kx;
        h.motion.params["pos_y"] = ky;
        h.motion.params["scale"] = ks;
        h.motion.params["rotation"] = kr;
    }
    h.markers.clear();
    return h;
}

bool extendClip(Project& p, Sequence& s, Id clip, FrameTime frames, const EndMotion& motion, bool ripple, Id* made, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const auto loc = edit::locate(s, clip);
    if (!loc || loc->track.kind != TrackKind::Video) return fail("Extend Clip needs a video clip");
    if (frames <= 0) return fail("Nothing to extend by");
    const Clip c = *edit::clipById(s, clip);
    Track* t = trackAt(s, loc->track);
    if (t->locked) return fail("The track is locked");
    const bool clear = std::none_of(t->clips.begin(), t->clips.end(), [&](const Clip& o) { return o.start < c.end() + frames && o.end() > c.end(); });
    if (!clear) {
        if (!ripple) return fail("Something follows the clip: make room or extend with ripple");
        const edit::Result r = edit::insertGap(p, s, loc->track, c.end(), frames);
        if (!r.ok) return fail(r.error);
    }
    Clip h = makeExtension(p, s, c, frames, motion);
    const edit::Result r = edit::overwrite(p, s, loc->track, h);
    if (!r.ok) return fail(r.error);
    if (made) *made = h.id;
    return true;
}

}  // namespace montage
