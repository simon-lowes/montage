#include "ConformCheck.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "Compositor.h"
#include "core/EditOps.h"
#include "media/Tracking.h"

namespace montage {

const char* conformVerdictName(ConformVerdict v) {
    switch (v) {
        case ConformVerdict::Matches: return "Matches";
        case ConformVerdict::Reframed: return "Reframed";
        case ConformVerdict::Slipped: return "Slipped";
        case ConformVerdict::OtherTake: return "Other take";
        case ConformVerdict::Missing: return "Missing";
        case ConformVerdict::NoMatch: break;
    }
    return "No match";
}

namespace {

// x' = a x + b y + c, y' = d x + e y + f.
struct Affine {
    double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;
    Point2 apply(Point2 p) const { return {a * p.x + b * p.y + c, d * p.x + e * p.y + f}; }
    Affine then(const Affine& o) const {  // o after this
        return {o.a * a + o.b * d, o.a * b + o.b * e, o.a * c + o.b * f + o.c, o.d * a + o.e * d, o.d * b + o.e * e, o.d * c + o.e * f + o.f};
    }
    Affine inverse() const {
        const double det = a * e - b * d;
        if (std::fabs(det) < 1e-12) return {};
        const double ia = e / det, ib = -b / det, id = -d / det, ie = a / det;
        return {ia, ib, -(ia * c + ib * f), id, ie, -(id * c + ie * f)};
    }
};

Affine fromSimilarity(const Similarity& s) {
    const double c = s.scale * std::cos(s.angle), n = s.scale * std::sin(s.angle);
    return {c, -n, s.tx, n, c, s.ty};
}

// A picture as the check sees it: grey, each picture's levels set to the same mean and contrast where it counts, so a
// grade or a different gamma does not tell two pictures apart.
struct Picture {
    GrayImage g;
    std::vector<uint8_t> valid;  // 1 where it shows the shot (inside a warped picture, outside the mask)
};

void normalise(Picture& p, const std::vector<uint8_t>* mask) {
    double sum = 0, sq = 0;
    size_t n = 0;
    for (size_t i = 0; i < p.g.px.size(); ++i)
        if (p.valid[i] && !(mask && (*mask)[i])) sum += p.g.px[i], sq += double(p.g.px[i]) * p.g.px[i], ++n;
    if (n < 16) return;
    const double mean = sum / double(n), sd = std::sqrt(std::max(1e-12, sq / double(n) - mean * mean));
    for (float& v : p.g.px) v = float(0.5 + (v - mean) / sd * 0.15);
}

GrayImage grey(const Image& img) {
    GrayImage g;
    g.width = img.width;
    g.height = img.height;
    g.px.resize(size_t(img.width) * size_t(img.height));
    for (size_t i = 0; i < g.px.size(); ++i) {
        const float* q = &img.px[i * 4];
        g.px[i] = 0.2126f * q[0] + 0.7152f * q[1] + 0.0722f * q[2];
    }
    return g;
}

// `src` drawn into a w x h frame through `toFrame` (src pixel -> frame pixel).
Picture warp(const GrayImage& src, const Affine& toFrame, int w, int h) {
    Picture out;
    out.g.width = w;
    out.g.height = h;
    out.g.px.assign(size_t(w) * size_t(h), 0.0f);
    out.valid.assign(out.g.px.size(), 0);
    const Affine back = toFrame.inverse();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const Point2 q = back.apply({x + 0.5, y + 0.5});
            if (q.x < 0 || q.y < 0 || q.x > src.width || q.y > src.height) continue;
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            out.g.px[i] = src.sample(q.x - 0.5, q.y - 0.5);
            out.valid[i] = 1;
        }
    return out;
}

// Normalised cross-correlation of two frames where both show picture and the mask leaves it; 0 when they overlap on
// less than a third of the frame.
double likeness(const Picture& a, const Picture& b, const std::vector<uint8_t>& mask) {
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    size_t n = 0;
    for (size_t i = 0; i < a.g.px.size(); ++i) {
        if (!a.valid[i] || !b.valid[i] || mask[i]) continue;
        const double x = a.g.px[i], y = b.g.px[i];
        sa += x, sb += y, saa += x * x, sbb += y * y, sab += x * y, ++n;
    }
    if (n < a.g.px.size() / 3 || n < 16) return 0;
    const double cov = sab - sa * sb / double(n), va = saa - sa * sa / double(n), vb = sbb - sb * sb / double(n);
    if (va <= 1e-12 || vb <= 1e-12) return 0;
    return std::max(0.0, cov / std::sqrt(va * vb));
}

struct Checker {
    const Project& p;
    const Sequence& seq;
    const MediaItem& ref;
    ConformOptions o;
    int aw = 0, ah = 0;
    double k = 1;  // analysis pixels per sequence pixel
    double fps = 25;
    std::vector<uint8_t> mask;  // 1: a burn-in or other overlay of the reference's own
    std::map<FrameTime, Picture> refCache;
    std::map<std::pair<Id, int64_t>, GrayImage> mediaCache;

    bool refCovers(FrameTime t) const {
        const double sec = double(t - o.referenceStart) / fps;
        return sec >= 0 && sec < ref.duration - 0.5 / fps;
    }
    Picture& reference(FrameTime t) {
        auto it = refCache.find(t);
        if (it != refCache.end()) return it->second;
        Picture pic;
        pic.g = grey(renderMediaFrame(p, ref, double(t - o.referenceStart) / fps, aw, ah));
        pic.valid.assign(pic.g.px.size(), 1);
        normalise(pic, &mask);
        if (refCache.size() > 64) refCache.clear();
        return refCache.emplace(t, std::move(pic)).first->second;
    }

    // How the clip's media sits in the frame before its own Transform (its Fit), as a factor per axis.
    void fit(const Clip& c, const MediaItem& m, FrameTime lt, double& fx, double& fy) const {
        const double mw = std::max(1, m.width), mh = std::max(1, m.height);
        fx = fy = 1;
        switch (int(c.motion.p("fit", lt, 0))) {
            case 0: fx = fy = std::min(seq.width / mw, seq.height / mh); break;
            case 1: fx = fy = std::max(seq.width / mw, seq.height / mh); break;
            case 2: fx = seq.width / mw, fy = seq.height / mh; break;
            default: break;
        }
    }
    // The media at sequence frame `src` (source frames, as Clip::sourceIn), at the size its Fit gives it in the
    // analysis frame.
    const GrayImage& media(const MediaItem& m, double fx, double fy, double src) {
        const int64_t frame = int64_t(std::floor(src + 1e-6));
        const auto key = std::make_pair(m.id, frame);
        auto it = mediaCache.find(key);
        if (it != mediaCache.end()) return it->second;
        const int tw = std::max(8, int(std::lround(std::max(1, m.width) * fx * k)));
        const int th = std::max(8, int(std::lround(std::max(1, m.height) * fy * k)));
        GrayImage g = grey(renderMediaFrame(p, m, m.kind == MediaKind::Image ? 0.0 : std::max(0.0, double(frame) / fps), tw, th));
        if (mediaCache.size() > 160) mediaCache.clear();
        return mediaCache.emplace(key, std::move(g)).first->second;
    }
    // Media thumbnail pixels -> analysis frame, under a Transform's values (anchor in media pixels).
    Affine placement(const GrayImage& t, const MediaItem& m, double fx, double fy, double posX, double posY, double scale,
                     double scaleX, double scaleY, double rotation, double anchorX, double anchorY) const {
        const double mw = std::max(1, m.width), mh = std::max(1, m.height);
        // q (thumbnail) -> u (media) -> frame: C + pos + R * diag(sx, sy) * (u - mc - anchor), all times k.
        const double ux = mw / t.width, uy = mh / t.height;
        const double sx = fx * scale / 100 * scaleX / 100 * k, sy = fy * scale / 100 * scaleY / 100 * k;
        const double r = rotation * M_PI / 180, cr = std::cos(r), sr = std::sin(r);
        Affine a;
        a.a = cr * sx * ux, a.b = -sr * sy * uy;
        a.d = sr * sx * ux, a.e = cr * sy * uy;
        const double ox = -(mw / 2 + anchorX), oy = -(mh / 2 + anchorY);
        a.c = aw / 2.0 + posX * k + cr * sx * ox - sr * sy * oy;
        a.f = ah / 2.0 + posY * k + sr * sx * ox + cr * sy * oy;
        return a;
    }
    Affine current(const Clip& c, const GrayImage& t, const MediaItem& m, FrameTime lt) const {
        double fx = 1, fy = 1;
        fit(c, m, lt, fx, fy);
        const Effect& mo = c.motion;
        return placement(t, m, fx, fy, mo.p("pos_x", lt), mo.p("pos_y", lt), mo.p("scale", lt, 100), mo.p("scale_x", lt, 100),
                         mo.p("scale_y", lt, 100), mo.p("rotation", lt), mo.p("anchor_x", lt), mo.p("anchor_y", lt));
    }
    // The placement that best lays `t` over the reference, from `guess`: features of the reference followed into the
    // picture as `guess` places it, a similarity fitted to them (twice, the second from the first's placement). `wide`:
    // when that does not match, a coarse search first (sizes and positions far from `guess`), then the same.
    bool solve(const Picture& reference, const GrayImage& t, Affine guess, Affine& out, bool wide = false) {
        std::vector<Point2> corners;
        for (const Point2& q : goodFeatures(reference.g, 500, std::max(4.0, aw / 64.0))) {
            const size_t i = size_t(std::clamp(int(q.y), 0, ah - 1)) * size_t(aw) + size_t(std::clamp(int(q.x), 0, aw - 1));
            if (!mask[i]) corners.push_back(q);
        }
        if (corners.size() < 8) return false;
        auto follow = [&](Affine from, Affine& to) {
            bool any = false;
            for (int round = 0; round < 2; ++round) {
                Picture placed = warp(t, from, aw, ah);
                normalise(placed, &mask);
                std::vector<Point2> tracked;
                std::vector<bool> ok;
                trackPoints(reference.g, placed.g, corners, tracked, ok);
                std::vector<Point2> src, onto;
                for (size_t i = 0; i < corners.size(); ++i)
                    if (ok[i]) {
                        const size_t at = size_t(std::clamp(int(tracked[i].y), 0, ah - 1)) * size_t(aw) +
                                          size_t(std::clamp(int(tracked[i].x), 0, aw - 1));
                        if (placed.valid[at]) src.push_back(tracked[i]), onto.push_back(corners[i]);
                    }
                Similarity sim;
                int inliers = 0;
                if (src.size() < 8 || !fitMotion(src, onto, MotionModel::Similarity, sim, &inliers) || inliers < 6) return any;
                from = from.then(fromSimilarity(sim));
                to = from;
                any = true;
            }
            return any;
        };
        Affine closeBy;
        const bool found = follow(guess, closeBy);
        if (!wide) {
            if (found) out = closeBy;
            return found;
        }
        const double nearScore = found ? score(reference, t, closeBy) : 0;
        if (nearScore >= o.matchScore) {
            out = closeBy;
            return true;
        }
        Affine distant;
        if (!follow(coarse(reference, t, guess), distant) || score(reference, t, distant) <= nearScore) {
            if (found) out = closeBy;
            return found;
        }
        out = distant;
        return true;
    }
    // The size (about the frame's centre) and position that best lay `t`, as `guess` places it, over the reference, at
    // about 64 pixels across; `guess` with them added.
    Affine coarse(const Picture& reference, const GrayImage& t, const Affine& guess) {
        const int f = std::max(1, aw / 64), w = aw / f, h = ah / f;
        if (w < 8 || h < 8) return guess;
        const Picture placed = warp(t, guess, aw, ah);
        // Both pictures boxed down; a small pixel counts when all of it shows the shot and none of it is masked.
        std::vector<float> ref(size_t(w) * size_t(h)), pic(ref.size());
        std::vector<uint8_t> refOk(ref.size()), picOk(ref.size());
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                double a = 0, b = 0;
                bool masked = false, shows = true;
                for (int yy = y * f; yy < (y + 1) * f; ++yy)
                    for (int xx = x * f; xx < (x + 1) * f; ++xx) {
                        const size_t i = size_t(yy) * size_t(aw) + size_t(xx);
                        a += reference.g.px[i], b += placed.g.px[i];
                        masked = masked || mask[i];
                        shows = shows && placed.valid[i];
                    }
                const size_t j = size_t(y) * size_t(w) + size_t(x);
                ref[j] = float(a / (f * f)), pic[j] = float(b / (f * f));
                refOk[j] = !masked, picOk[j] = shows;
            }
        GrayImage small;
        small.width = w, small.height = h, small.px = pic;
        double best = -2, bestScale = 1, bestX = 0, bestY = 0;
        std::vector<float> scaled(ref.size());
        std::vector<uint8_t> scaledOk(ref.size());
        const double cx = w / 2.0, cy = h / 2.0;
        for (double s : {1.0, 1.06, 1.12, 1.2, 1.3, 1.42, 1.56, 1.75, 2.0, 0.94, 0.88, 0.8}) {
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    const double sx = cx + (x + 0.5 - cx) / s - 0.5, sy = cy + (y + 0.5 - cy) / s - 0.5;
                    const size_t j = size_t(y) * size_t(w) + size_t(x);
                    const int ix = int(std::lround(sx)), iy = int(std::lround(sy));
                    scaledOk[j] = ix >= 0 && iy >= 0 && ix < w && iy < h && picOk[size_t(iy) * size_t(w) + size_t(ix)];
                    scaled[j] = scaledOk[j] ? small.sample(sx, sy) : 0.0f;
                }
            for (int dy = -h / 2; dy <= h / 2; ++dy)
                for (int dx = -w / 2; dx <= w / 2; ++dx) {
                    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
                    int n = 0;
                    for (int y = std::max(0, dy); y < std::min(h, h + dy); ++y)
                        for (int x = std::max(0, dx); x < std::min(w, w + dx); ++x) {
                            const size_t i = size_t(y) * size_t(w) + size_t(x), j = size_t(y - dy) * size_t(w) + size_t(x - dx);
                            if (!refOk[i] || !scaledOk[j]) continue;
                            const double a = ref[i], b = scaled[j];
                            sa += a, sb += b, saa += a * a, sbb += b * b, sab += a * b, ++n;
                        }
                    if (n < w * h / 3) continue;
                    const double cov = sab - sa * sb / n, va = saa - sa * sa / n, vb = sbb - sb * sb / n;
                    if (va <= 1e-12 || vb <= 1e-12) continue;
                    if (const double r = cov / std::sqrt(va * vb); r > best) best = r, bestScale = s, bestX = dx, bestY = dy;
                }
        }
        // Scaled about the frame's centre, then moved.
        const double fcx = aw / 2.0, fcy = ah / 2.0;
        const Affine around{bestScale, 0, fcx - bestScale * fcx + bestX * f, 0, bestScale, fcy - bestScale * fcy + bestY * f};
        return guess.then(around);
    }
    double score(const Picture& reference, const GrayImage& t, const Affine& a) {
        Picture placed = warp(t, a, aw, ah);
        normalise(placed, &mask);
        return likeness(reference, placed, mask);
    }
    // A placement as the Transform's values (Fit kept; anchor, width and height scales back to their defaults).
    ConformKey key(const Affine& a, const GrayImage& t, const MediaItem& m, double fx, FrameTime lt) const {
        const double mw = std::max(1, m.width);
        const double unit = fx * k * mw / t.width;  // analysis pixels per thumbnail pixel at 100 %
        ConformKey kf;
        kf.at = lt;
        kf.scale = std::sqrt(std::max(0.0, a.a * a.e - a.b * a.d)) / unit * 100;
        kf.rotation = std::atan2(a.d, a.a) * 180 / M_PI;
        const Point2 centre = a.apply({t.width / 2.0, t.height / 2.0});
        kf.posX = (centre.x - aw / 2.0) / k;
        kf.posY = (centre.y - ah / 2.0) / k;
        return kf;
    }
};

// The stretches where each clip from media is the picture on top (the longest one per clip).
std::vector<ConformShot> shotsOf(const Project& p, const Sequence& seq) {
    std::vector<ConformShot> shots;
    for (int ti = int(seq.videoTracks.size()) - 1; ti >= 0; --ti) {
        const Track& tr = seq.videoTracks[size_t(ti)];
        if (tr.muted) continue;
        for (const Clip& c : tr.clips) {
            if (!c.enabled || c.isGenerator() || !c.mediaId) continue;
            const MediaItem* m = p.findMedia(c.mediaId);
            if (!m || !(m->hasVideo || m->kind == MediaKind::Image || m->kind == MediaKind::Sequence)) continue;
            // What the tracks above cover (their clips from media, fully opaque).
            std::vector<std::pair<FrameTime, FrameTime>> above;
            for (size_t u = size_t(ti) + 1; u < seq.videoTracks.size(); ++u) {
                if (seq.videoTracks[u].muted) continue;
                for (const Clip& o : seq.videoTracks[u].clips)
                    if (o.enabled && !o.isGenerator() && o.mediaId && o.end() > c.start && o.start < c.end() &&
                        o.motion.p("opacity", 0, 100) >= 100)
                        above.emplace_back(std::max(o.start, c.start), std::min(o.end(), c.end()));
            }
            std::sort(above.begin(), above.end());
            FrameTime at = c.start, bestFrom = 0, bestTo = 0;
            auto consider = [&](FrameTime a, FrameTime b) {
                if (b - a > bestTo - bestFrom) bestFrom = a, bestTo = b;
            };
            for (const auto& [a, b] : above) {
                if (a > at) consider(at, a);
                at = std::max(at, b);
            }
            if (at < c.end()) consider(at, c.end());
            if (bestTo <= bestFrom) continue;
            ConformShot s;
            s.clip = c.id;
            s.start = bestFrom;
            s.length = bestTo - bestFrom;
            shots.push_back(s);
        }
    }
    std::sort(shots.begin(), shots.end(), [](const ConformShot& a, const ConformShot& b) { return a.start < b.start; });
    return shots;
}

std::string formatKey(const ConformKey& k) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "scale %.1f %%, rotation %.2f°, position (%.1f, %.1f)", k.scale, k.rotation, k.posX, k.posY);
    return buf;
}

}  // namespace

std::vector<ConformShot> conformCheck(const Project& p, const Sequence& seq, Id reference, const ConformOptions& options,
                                      const ConformProgress& progress, const std::atomic<bool>* cancel, std::string* error) {
    const MediaItem* ref = p.findMedia(reference);
    if (!ref || !(ref->hasVideo || ref->kind == MediaKind::Sequence) || ref->duration <= 0) {
        if (error) *error = "The reference has to be a movie in the project";
        return {};
    }
    Checker ck{p, seq, *ref, options};
    ck.fps = std::max(1.0, seq.fpsValue());
    ck.aw = std::max(16, std::min(seq.width, std::max(64, options.analysisWidth)));
    ck.k = double(ck.aw) / std::max(1, seq.width);
    ck.ah = std::max(16, int(std::lround(seq.height * ck.k)));
    ck.mask.assign(size_t(ck.aw) * size_t(ck.ah), 0);
    const int range = options.slipRange > 0 ? options.slipRange : int(std::lround(2 * ck.fps));
    std::vector<ConformShot> shots = shotsOf(p, seq);

    // The reference's own overlays: what stays the same across its shots (a burnt-in timecode's box, a watermark,
    // letterbox bars), closed so the changing digits inside a box are covered too. Not when that is most of the frame.
    {
        std::vector<GrayImage> seen;
        for (const ConformShot& s : shots)
            if (const FrameTime t = s.start + s.length / 2; ck.refCovers(t))
                seen.push_back(grey(renderMediaFrame(p, *ref, double(t - options.referenceStart) / ck.fps, ck.aw, ck.ah)));
        if (seen.size() >= 3) {
            std::vector<uint8_t> still(ck.mask.size(), 0);
            for (size_t i = 0; i < still.size(); ++i) {
                double lo = 1e9, hi = -1e9;
                for (const GrayImage& g : seen) lo = std::min(lo, double(g.px[i])), hi = std::max(hi, double(g.px[i]));
                still[i] = hi - lo < 0.02;
            }
            const int r = std::max(2, ck.aw / 160);
            auto morph = [&](const std::vector<uint8_t>& in, bool grow) {
                std::vector<uint8_t> out(in.size());
                for (int y = 0; y < ck.ah; ++y)
                    for (int x = 0; x < ck.aw; ++x) {
                        bool v = !grow;
                        for (int dy = -r; dy <= r && v != grow; ++dy)
                            for (int dx = -r; dx <= r; ++dx) {
                                const int xx = std::clamp(x + dx, 0, ck.aw - 1), yy = std::clamp(y + dy, 0, ck.ah - 1);
                                if (bool(in[size_t(yy) * size_t(ck.aw) + size_t(xx)]) == grow) {
                                    v = grow;
                                    break;
                                }
                            }
                        out[size_t(y) * size_t(ck.aw) + size_t(x)] = v;
                    }
                return out;
            };
            std::vector<uint8_t> closed = morph(morph(still, true), false);
            const size_t covered = size_t(std::count(closed.begin(), closed.end(), uint8_t(1)));
            if (covered < closed.size() / 2) ck.mask = std::move(closed);
        }
    }

    for (size_t si = 0; si < shots.size(); ++si) {
        if (cancel && cancel->load()) return {};
        if (progress) progress(double(si) / double(shots.size()));
        ConformShot& shot = shots[si];
        const Clip* c = edit::clipById(seq, shot.clip);
        const MediaItem* m = c ? p.findMedia(c->mediaId) : nullptr;
        if (!c || !m) continue;
        // Three frames of the shot: a quarter, half and three quarters of the way in.
        std::vector<FrameTime> at;
        for (double f : {0.25, 0.5, 0.75}) {
            const FrameTime t = shot.start + std::clamp(FrameTime(std::floor(double(shot.length) * f)), FrameTime(0), shot.length - 1);
            if (ck.refCovers(t) && (at.empty() || at.back() != t)) at.push_back(t);
        }
        if (at.empty()) {
            shot.details = "Outside the reference";
            continue;
        }
        double fx = 1, fy = 1;
        ck.fit(*c, *m, 0, fx, fy);
        auto sourceAt = [&](const Clip& clip, FrameTime t) { return clip.sourceAt(double(t - clip.start)); };
        // As it is.
        double worst = 1;
        for (FrameTime t : at) {
            const GrayImage& g = ck.media(*m, fx, fy, sourceAt(*c, t));
            worst = std::min(worst, ck.score(ck.reference(t), g, ck.current(*c, g, *m, t - c->start)));
        }
        shot.score = worst;
        if (worst >= options.matchScore) {
            shot.verdict = ConformVerdict::Matches;
            continue;
        }
        // The same frames framed otherwise: a placement solved at each.
        auto solvedAt = [&](const MediaItem& med, double src, FrameTime t, const Affine* from, Affine& out, double& s) {
            const GrayImage& g = ck.media(med, fx, fy, src);
            const Affine guess = from ? *from : ck.current(*c, g, med, t - c->start);
            if (!ck.solve(ck.reference(t), g, guess, out, true)) out = guess;
            s = ck.score(ck.reference(t), g, out);
            return s;
        };
        auto holds = [&](const MediaItem& med, double offset, double& least, std::vector<Affine>& placements) {
            least = 1;
            placements.clear();
            for (FrameTime t : at) {
                Affine a;
                double s = 0;
                solvedAt(med, sourceAt(*c, t) + offset, t, nullptr, a, s);
                placements.push_back(a);
                least = std::min(least, s);
            }
            return least >= options.matchScore;
        };
        // Its keys: the placement every keyEvery frames, one key when it does not move.
        auto keysFor = [&](const MediaItem& med, double offset, std::vector<ConformKey>& keys) {
            std::vector<FrameTime> times;
            const int step = std::max(1, options.keyEvery);
            for (FrameTime t = shot.start; t < shot.start + shot.length; t += step)
                if (ck.refCovers(t)) times.push_back(t);
            if (times.size() > 24) {
                std::vector<FrameTime> fewer;
                for (size_t i = 0; i < 24; ++i) fewer.push_back(times[i * times.size() / 24]);
                times = fewer;
            }
            const FrameTime last = shot.start + shot.length - 1;
            if (ck.refCovers(last) && (times.empty() || times.back() != last)) times.push_back(last);
            std::vector<ConformKey> all;
            for (FrameTime t : times) {
                Affine a;
                double s = 0;
                if (solvedAt(med, sourceAt(*c, t) + offset, t, nullptr, a, s) < options.matchScore) continue;
                const GrayImage& g = ck.media(med, fx, fy, sourceAt(*c, t) + offset);
                all.push_back(ck.key(a, g, med, fx, t - c->start));
            }
            if (all.empty()) return;
            // Still within what the measure itself wanders by (about half a pixel, a third of a percent and a tenth of a
            // degree, from frame to frame of a still reframe): one key, the median.
            auto median = [&](double ConformKey::*f) {
                std::vector<double> v;
                for (const ConformKey& kf : all) v.push_back(kf.*f);
                std::nth_element(v.begin(), v.begin() + ptrdiff_t(v.size() / 2), v.end());
                return v[v.size() / 2];
            };
            ConformKey mid;
            mid.at = 0;
            mid.posX = median(&ConformKey::posX), mid.posY = median(&ConformKey::posY);
            mid.scale = median(&ConformKey::scale), mid.rotation = median(&ConformKey::rotation);
            const double tol = 0.5 / std::max(0.25, ck.k);
            const bool still = std::all_of(all.begin(), all.end(), [&](const ConformKey& kf) {
                return std::fabs(kf.posX - mid.posX) < tol && std::fabs(kf.posY - mid.posY) < tol && std::fabs(kf.scale - mid.scale) < 0.35 &&
                       std::fabs(kf.rotation - mid.rotation) < 0.12;
            });
            keys = still ? std::vector<ConformKey>{mid} : all;
        };
        // Whether a placement differs from the clip's own Transform by more than the check can see.
        auto moved = [&](const std::vector<ConformKey>& keys) {
            const double tol = 0.5 / std::max(0.25, ck.k);
            for (const ConformKey& kf : keys) {
                const Effect& mo = c->motion;
                if (std::fabs(kf.posX - mo.p("pos_x", kf.at)) > tol || std::fabs(kf.posY - mo.p("pos_y", kf.at)) > tol ||
                    std::fabs(kf.scale - mo.p("scale", kf.at, 100)) > 0.25 || std::fabs(kf.rotation - mo.p("rotation", kf.at)) > 0.1 ||
                    mo.p("anchor_x", kf.at) != 0 || mo.p("anchor_y", kf.at) != 0)
                    return true;
            }
            return false;
        };
        std::vector<Affine> placements;
        double least = 0;
        const bool reframed = holds(*m, 0, least, placements);
        const double reframedScore = least;
        std::vector<ConformKey> reframeKeys;
        if (reframed) {
            keysFor(*m, 0, reframeKeys);
            // Moved but not resized or turned, it may be the same picture a few frames on in a shot that pans: the
            // slips are tried too, and the closer of the two taken (a slip, when they are as close).
            const bool turned = std::any_of(reframeKeys.begin(), reframeKeys.end(), [&](const ConformKey& kf) {
                return std::fabs(kf.scale - c->motion.p("scale", kf.at, 100)) > 0.5 || std::fabs(kf.rotation - c->motion.p("rotation", kf.at)) > 0.2;
            });
            if (turned || reframeKeys.empty()) {
                shot.keys = reframeKeys;
                shot.score = least;
                shot.verdict = ConformVerdict::Reframed;
                shot.details = shot.keys.empty()          ? "framed differently"
                               : shot.keys.size() == 1 ? formatKey(shot.keys[0])
                                                       : std::to_string(shot.keys.size()) + " keys, from " + formatKey(shot.keys[0]);
                continue;
            }
        }
        auto asReframed = [&] {
            shot.keys = reframeKeys;
            shot.score = reframedScore;
            shot.verdict = ConformVerdict::Reframed;
            shot.details = shot.keys.size() == 1 ? formatKey(shot.keys[0]) : std::to_string(shot.keys.size()) + " keys, from " + formatKey(shot.keys[0]);
        };
        // Another frame of its source, placed as it is: the middle frame searched either side, then the others checked.
        const FrameTime mid = at[at.size() / 2];
        const double speed = c->speedAt(double(mid - c->start)) > 1e-9 ? c->speedAt(double(mid - c->start)) : 1.0;
        auto inSource = [&](double src) { return src >= 0 && (m->kind == MediaKind::Image || src / ck.fps <= m->duration); };
        auto asPlaced = [&](double offset, FrameTime t) {
            const GrayImage& g = ck.media(*m, fx, fy, sourceAt(*c, t) + offset);
            return ck.score(ck.reference(t), g, ck.current(*c, g, *m, t - c->start));
        };
        auto slipped = [&](int d, double score) {
            shot.verdict = ConformVerdict::Slipped;
            shot.slip = d;
            shot.score = score;
            char buf[96];
            std::snprintf(buf, sizeof buf, "%d frame%s %s", std::abs(d), std::abs(d) == 1 ? "" : "s",
                          d > 0 ? "late (the reference is further into the take)" : "early (the reference is earlier in the take)");
            shot.details = buf;
        };
        double best = 0;
        int bestSlip = 0;
        for (int d = -range; d <= range; ++d)
            if (d && inSource(sourceAt(*c, mid) + d * speed))
                if (const double sc = asPlaced(d * speed, mid); sc > best) best = sc, bestSlip = d;
        if (best >= options.matchScore) {
            double held = 1;
            for (FrameTime t : at) held = std::min(held, asPlaced(bestSlip * speed, t));
            // (Where the reframe found above only moved the picture, the closer of the two; a slip when they are as close.)
            if (held >= options.matchScore && (!reframed || held >= reframedScore - 0.005)) {
                slipped(bestSlip, held);
                continue;
            }
        }
        if (reframed) {
            asReframed();
            continue;
        }
        // Slipped and reframed: the slips again, a placement solved for each (also from the one found at the middle
        // frame above, when the clip's own is too far from it).
        const Affine* placed = placements.size() == at.size() ? &placements[at.size() / 2] : nullptr;
        double both = 0;
        int bothSlip = 0;
        for (int d = -range; d <= range; ++d) {
            const double src = sourceAt(*c, mid) + d * speed;
            if (!d || !inSource(src)) continue;
            const GrayImage& g = ck.media(*m, fx, fy, src);
            Affine a;
            const Affine guess = ck.current(*c, g, *m, mid - c->start);
            if (!ck.solve(ck.reference(mid), g, guess, a)) a = guess;
            double sc = ck.score(ck.reference(mid), g, a);
            if (placed && sc < options.matchScore) {
                Affine b;
                if (ck.solve(ck.reference(mid), g, *placed, b)) sc = std::max(sc, ck.score(ck.reference(mid), g, b));
            }
            if (sc > both) both = sc, bothSlip = d;
        }
        if (both >= options.matchScore && holds(*m, bothSlip * speed, least, placements)) {
            slipped(bothSlip, least);
            std::vector<ConformKey> keys;
            keysFor(*m, bothSlip * speed, keys);
            if (moved(keys)) {
                shot.keys = keys;
                shot.details += "; reframed: " + formatKey(keys[0]);
            }
            continue;
        }
        best = std::max(best, both);
        // Another take: the scene's other media (all the picture media when the clip's has no scene) scanned small for
        // the reference's middle frame, then the best of them searched frame by frame and checked at the others.
        const auto sceneOf = [](const MediaItem& x) {
            const auto it = x.metadata.find("scene");
            return it == x.metadata.end() ? std::string() : it->second;
        };
        const std::string scene = sceneOf(*m);
        struct Candidate {
            const MediaItem* media;
            double src;  // sequence frames
            double coarse;
        };
        std::vector<Candidate> candidates;
        Checker tiny{p, seq, *ref, options};
        tiny.fps = ck.fps;
        tiny.aw = std::max(16, std::min(ck.aw, 128));
        tiny.k = double(tiny.aw) / std::max(1, seq.width);
        tiny.ah = std::max(9, int(std::lround(seq.height * tiny.k)));
        tiny.mask.assign(size_t(tiny.aw) * size_t(tiny.ah), 0);
        for (int y = 0; y < tiny.ah; ++y)
            for (int x = 0; x < tiny.aw; ++x)
                tiny.mask[size_t(y) * size_t(tiny.aw) + size_t(x)] =
                    ck.mask[size_t(std::min(ck.ah - 1, y * ck.ah / tiny.ah)) * size_t(ck.aw) + size_t(std::min(ck.aw - 1, x * ck.aw / tiny.aw))];
        const Picture& small = tiny.reference(mid);
        for (const MediaItem& o : p.media) {
            if (o.id == m->id || o.id == ref->id || !o.hasVideo || o.kind == MediaKind::Image || o.duration <= 0) continue;
            if (!scene.empty() && sceneOf(o) != scene) continue;
            const double step = std::max(0.2, o.duration / 300);
            Candidate cand{&o, 0, 0};
            for (double sec = 0; sec < o.duration; sec += step) {
                if (cancel && cancel->load()) return {};
                double ofx = 1, ofy = 1;
                tiny.fit(*c, o, 0, ofx, ofy);
                const GrayImage& g = tiny.media(o, ofx, ofy, sec * ck.fps);
                const double s = tiny.score(small, g, tiny.current(*c, g, o, mid - c->start));
                if (s > cand.coarse) cand.coarse = s, cand.src = sec * ck.fps;
            }
            candidates.push_back(cand);
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.coarse > b.coarse; });
        if (candidates.size() > 2) candidates.resize(2);
        bool found = false;
        for (const Candidate& cand : candidates) {
            const MediaItem& o = *cand.media;
            const int around = int(std::ceil(std::max(0.2, o.duration / 300) * ck.fps)) + 2;
            double bestHere = 0, bestSrc = 0;
            for (int d = -around; d <= around; ++d) {
                const double src = std::floor(cand.src) + d;
                if (src < 0 || src / ck.fps > o.duration) continue;
                const GrayImage& g = ck.media(o, fx, fy, src);
                Affine a;
                const Affine guess = ck.current(*c, g, o, mid - c->start);
                if (!ck.solve(ck.reference(mid), g, guess, a)) a = guess;
                if (const double s = ck.score(ck.reference(mid), g, a); s > bestHere) bestHere = s, bestSrc = src;
            }
            if (bestHere < options.matchScore) continue;
            const double offset = bestSrc - sourceAt(*c, mid);
            if (!holds(o, offset, least, placements)) continue;
            shot.verdict = ConformVerdict::OtherTake;
            shot.media = o.id;
            shot.sourceIn = c->sourceIn + offset;
            shot.score = least;
            shot.details = "the reference shows " + o.name;
            std::vector<ConformKey> keys;
            keysFor(o, offset, keys);
            if (moved(keys)) {
                shot.keys = keys;
                shot.details += "; reframed: " + formatKey(keys[0]);
            }
            found = true;
            break;
        }
        if (!found) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "nothing near matches (best %.0f %%)", std::max(best, worst) * 100);
            shot.details = buf;
        }
    }
    // Where this sequence shows nothing at all while the reference shows a picture (not black, not only its overlays):
    // a shot missing.
    {
        const FrameTime to = options.referenceStart + FrameTime(std::floor(ref->duration * ck.fps + 1e-6));
        std::vector<std::pair<FrameTime, FrameTime>> covered;
        for (const Track& tr : seq.videoTracks)
            if (!tr.muted)
                for (const Clip& c : tr.clips)
                    if (c.enabled) covered.emplace_back(c.start, c.end());
        std::sort(covered.begin(), covered.end());
        std::vector<std::pair<FrameTime, FrameTime>> gaps;
        FrameTime at = std::max<FrameTime>(0, options.referenceStart);
        for (const auto& [a, b] : covered) {
            if (a > at && at < to) gaps.emplace_back(at, std::min(a, to));
            at = std::max(at, b);
        }
        if (at < to) gaps.emplace_back(at, to);
        for (const auto& [a, b] : gaps) {
            const FrameTime t = a + (b - a) / 2;
            if (!ck.refCovers(t)) continue;
            const GrayImage g = grey(renderMediaFrame(p, *ref, double(t - options.referenceStart) / ck.fps, ck.aw, ck.ah));
            std::vector<float> v;
            for (size_t i = 0; i < g.px.size(); ++i)
                if (!ck.mask[i]) v.push_back(g.px[i]);
            if (v.empty()) continue;
            std::nth_element(v.begin(), v.begin() + ptrdiff_t(v.size() / 2), v.end());
            const float median = v[v.size() / 2];
            const size_t apart = size_t(std::count_if(v.begin(), v.end(), [&](float x) { return std::fabs(x - median) > 0.05f; }));
            if (apart * 10 < v.size()) continue;  // (flat: black, or a colour, maybe with a slate's few words)
            ConformShot missing;
            missing.start = a;
            missing.length = b - a;
            missing.verdict = ConformVerdict::Missing;
            missing.details = "the reference shows a picture here; this sequence has none";
            shots.push_back(missing);
        }
        std::stable_sort(shots.begin(), shots.end(), [](const ConformShot& x, const ConformShot& y) { return x.start < y.start; });
    }
    if (progress) progress(1);
    return shots;
}

bool applyConformFix(Project& p, Sequence& seq, const ConformShot& shot) {
    if (!shot.fixable()) return false;
    Clip* c = edit::clipById(seq, shot.clip);
    if (!c) return false;
    if (shot.verdict == ConformVerdict::Slipped && !edit::slip(p, seq, shot.clip, shot.slip).ok) return false;
    if (shot.verdict == ConformVerdict::OtherTake) {
        const MediaItem* to = p.findMedia(shot.media);
        if (!to) return false;
        // The picture and the sound linked to it that plays the same media.
        const Id from = c->mediaId;
        const double shift = shot.sourceIn - c->sourceIn;
        for (Id id : edit::linkedClips(seq, shot.clip))
            if (Clip* l = edit::clipById(seq, id); l && l->mediaId == from) {
                l->mediaId = to->id;
                l->sourceIn = std::max(0.0, l->sourceIn + shift);
            }
        c = edit::clipById(seq, shot.clip);
    }
    if (!shot.keys.empty()) {
        Effect& mo = c->motion;
        auto set = [&](const char* name, double ConformKey::*f) {
            Param prm;
            if (shot.keys.size() == 1) prm = Param(shot.keys[0].*f);
            else
                for (const ConformKey& kf : shot.keys) prm.addKey(kf.at, kf.*f);
            mo.params[name] = prm;
        };
        set("pos_x", &ConformKey::posX);
        set("pos_y", &ConformKey::posY);
        set("scale", &ConformKey::scale);
        set("rotation", &ConformKey::rotation);
        for (const char* name : {"scale_x", "scale_y"}) mo.params[name] = Param(100.0);
        for (const char* name : {"anchor_x", "anchor_y"}) mo.params[name] = Param(0.0);
    }
    return true;
}

}  // namespace montage
