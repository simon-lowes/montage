#include "Compositor.h"

#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

#include "ColorSpace.h"
#include "Processing.h"
#include "Retime.h"
#include "audio/PluginEffect.h"
#include "audio/SpeechCleanup.h"
#include "core/EditOps.h"
#include "media/MediaPool.h"

namespace montage {

namespace {

constexpr int kMaxDepth = 8;

// ---------------------------------------------------------------------------
// Generators

Image renderTitle(const Effect& g, FrameTime t, int w, int h, double scale) {
    QImage qi(w, h, QImage::Format_RGBA8888_Premultiplied);
    qi.fill(Qt::transparent);
    {
        QPainter pa(&qi);
        pa.setRenderHint(QPainter::Antialiasing);
        pa.setRenderHint(QPainter::TextAntialiasing);
        QFont f(QString::fromStdString(g.s("font", "Sans Serif")));
        f.setPixelSize(std::max(1, int(std::lround(g.p("size", t, 96) * scale))));
        f.setBold(g.p("bold", t, 1) > 0.5);
        f.setItalic(g.p("italic", t) > 0.5);
        f.setLetterSpacing(QFont::AbsoluteSpacing, g.p("tracking", t) * scale);
        f.setHintingPreference(QFont::PreferNoHinting);
        QStringList lines = QString::fromStdString(g.s("text", "Title")).split('\n');
        QFontMetricsF fm(f);
        double lineH = fm.height() * g.p("line_spacing", t, 1.15);
        double blockW = 0;
        for (const auto& l : lines) blockW = std::max(blockW, fm.horizontalAdvance(l));
        double totalH = lineH * double(lines.size() - 1) + fm.height();
        double cx = w * 0.5 + g.p("pos_x", t) * scale, cy = h * 0.5 + g.p("pos_y", t) * scale;
        int align = int(g.p("align", t, 1));
        QPainterPath path;
        for (int i = 0; i < lines.size(); ++i) {
            double lw = fm.horizontalAdvance(lines[i]);
            double x = align == 0 ? cx - blockW / 2 : (align == 2 ? cx + blockW / 2 - lw : cx - lw / 2);
            double y = cy - totalH / 2 + i * lineH + fm.ascent();
            path.addText(QPointF(x, y), f, lines[i]);
        }
        auto col = [&](const char* base, double a) {
            std::string b(base);
            return QColor::fromRgbF(float(std::clamp(g.p(b + ".r", t), 0.0, 1.0)), float(std::clamp(g.p(b + ".g", t), 0.0, 1.0)),
                                    float(std::clamp(g.p(b + ".b", t), 0.0, 1.0)), float(std::clamp(a, 0.0, 1.0)));
        };
        pa.setOpacity(std::clamp(g.p("opacity", t, 100) / 100.0, 0.0, 1.0));
        double boxOp = g.p("box_opacity", t) / 100.0;
        if (boxOp > 0 && !lines.isEmpty()) {
            double pad = g.p("box_padding", t, 24) * scale;
            QRectF box(cx - blockW / 2 - pad, cy - totalH / 2 - pad, blockW + 2 * pad, totalH + 2 * pad);
            pa.fillRect(box, col("box_color", boxOp));
        }
        double sh = g.p("shadow", t, 4) * scale;
        double shOp = g.p("shadow_opacity", t, 50) / 100.0;
        if (sh > 0 && shOp > 0) pa.fillPath(path.translated(sh, sh), QColor::fromRgbF(0, 0, 0, float(shOp)));
        double ow = g.p("outline", t) * scale;
        if (ow > 0) {
            QPen pen(col("outline_color", 1.0), ow * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            pa.strokePath(path, pen);
        }
        pa.fillPath(path, col("color", 1.0));
    }
    Image img(w, h);
    const float k = 1.0f / 255.0f;
    for (int y = 0; y < h; ++y) {
        const uchar* s = qi.constScanLine(y);
        float* d = img.row(y);
        for (int x = 0; x < w * 4; ++x) d[x] = s[x] * k;
    }
    return img;
}

}  // namespace

Image renderGenerator(const Effect& g, FrameTime t, int w, int h, double scale) {
    Image img(w, h);
    if (g.type == "color") {
        img.fill(float(g.p("color.r", t)), float(g.p("color.g", t)), float(g.p("color.b", t)),
                 float(std::clamp(g.p("alpha", t, 100) / 100.0, 0.0, 1.0)));
    } else if (g.type == "gradient") {
        float a[3] = {float(g.p("color_a.r", t)), float(g.p("color_a.g", t)), float(g.p("color_a.b", t))};
        float b[3] = {float(g.p("color_b.r", t)), float(g.p("color_b.g", t)), float(g.p("color_b.b", t))};
        double ang = g.p("angle", t, 90) * M_PI / 180.0;
        bool radial = g.p("shape", t) > 0.5;
        float dx = float(std::cos(ang)), dy = float(std::sin(ang));
        float ext = 0.5f * (std::fabs(dx) + std::fabs(dy));
        parallelRows(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < w; ++x) {
                    float nx = (x + 0.5f) / w - 0.5f, ny = (y + 0.5f) / h - 0.5f;
                    float u = radial ? std::min(1.0f, std::sqrt(nx * nx + ny * ny) / 0.7071f)
                                     : std::clamp((nx * dx + ny * dy + ext) / (2 * ext), 0.0f, 1.0f);
                    float* p = img.at(x, y);
                    for (int c = 0; c < 3; ++c) p[c] = a[c] + (b[c] - a[c]) * u;
                    p[3] = 1;
                }
        });
    } else if (g.type == "bars") {
        static const float top[7][3] = {{.75f, .75f, .75f}, {.75f, .75f, 0}, {0, .75f, .75f}, {0, .75f, 0},
                                        {.75f, 0, .75f},    {.75f, 0, 0},    {0, 0, .75f}};
        static const float mid[7][3] = {{0, 0, .75f}, {0, 0, 0}, {.75f, 0, .75f}, {0, 0, 0},
                                        {0, .75f, .75f}, {0, 0, 0}, {.75f, .75f, .75f}};
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                int bar = std::min(6, x * 7 / std::max(1, w));
                const float* c;
                static const float bottom[4][3] = {{0, .13f, .3f}, {1, 1, 1}, {.2f, 0, .42f}, {.03f, .03f, .03f}};
                if (y < h * 2 / 3) c = top[bar];
                else if (y < h * 3 / 4) c = mid[bar];
                else c = bottom[std::min(3, x * 4 / std::max(1, w))];
                float* p = img.at(x, y);
                p[0] = c[0];
                p[1] = c[1];
                p[2] = c[2];
                p[3] = 1;
            }
    } else if (g.type == "title") {
        return renderTitle(g, t, w, h, scale);
    }
    return img;
}

namespace {

struct Geometry {
    double mw = 1, mh = 1;  // media logical size (pixels)
    double sx = 1, sy = 1;  // total scale (media px -> sequence px), may be negative for flips
    double rot = 0;         // radians
    double px = 0, py = 0;  // position offset (sequence px)
    double ax = 0, ay = 0;  // anchor offset (media px)
    double cl = 0, cr = 0, ct = 0, cb = 0;  // crop fractions
    double opacity = 1;
};

Geometry geometryFor(const Effect& motion, FrameTime lt, double mw, double mh, int SW, int SH) {
    Geometry g;
    g.mw = std::max(1.0, mw);
    g.mh = std::max(1.0, mh);
    int fit = int(motion.p("fit", lt, 0));
    double fx = 1, fy = 1;
    switch (fit) {
        case 0: fx = fy = std::min(SW / g.mw, SH / g.mh); break;
        case 1: fx = fy = std::max(SW / g.mw, SH / g.mh); break;
        case 2: fx = SW / g.mw; fy = SH / g.mh; break;
        default: break;
    }
    double s = motion.p("scale", lt, 100) / 100.0;
    g.sx = fx * s * motion.p("scale_x", lt, 100) / 100.0;
    g.sy = fy * s * motion.p("scale_y", lt, 100) / 100.0;
    if (motion.p("flip_h", lt) > 0.5) g.sx = -g.sx;
    if (motion.p("flip_v", lt) > 0.5) g.sy = -g.sy;
    g.rot = motion.p("rotation", lt) * M_PI / 180.0;
    g.px = motion.p("pos_x", lt);
    g.py = motion.p("pos_y", lt);
    g.ax = motion.p("anchor_x", lt);
    g.ay = motion.p("anchor_y", lt);
    g.cl = std::clamp(motion.p("crop_left", lt) / 100.0, 0.0, 1.0);
    g.cr = std::clamp(motion.p("crop_right", lt) / 100.0, 0.0, 1.0);
    g.ct = std::clamp(motion.p("crop_top", lt) / 100.0, 0.0, 1.0);
    g.cb = std::clamp(motion.p("crop_bottom", lt) / 100.0, 0.0, 1.0);
    g.opacity = std::clamp(motion.p("opacity", lt, 100) / 100.0, 0.0, 1.0);
    return g;
}

// Draws `src` (which depicts the media at any resolution) onto a canvas of
// W x H (sequence size * scale) according to the geometry.
// True when the layer maps one source pixel to one output pixel, unmoved,
// filling the output exactly at full opacity: it can be used as it is.
bool identityLayer(const Image& src, const Geometry& g, int SW, int SH, double scale) {
    const int W = std::max(1, int(std::lround(SW * scale))), H = std::max(1, int(std::lround(SH * scale)));
    if (src.width != W || src.height != H || g.opacity < 1 || g.rot != 0 || g.sx <= 0 || g.sy <= 0) return false;
    if (g.cl != 0 || g.cr != 0 || g.ct != 0 || g.cb != 0) return false;
    const double kx = src.width / g.mw, ky = src.height / g.mh;
    const double pxX = g.sx * scale / kx, pxY = g.sy * scale / ky;
    const double ox = (SW / 2.0 + g.px - (g.mw / 2 + g.ax) * g.sx) * scale;
    const double oy = (SH / 2.0 + g.py - (g.mh / 2 + g.ay) * g.sy) * scale;
    return std::fabs(pxX - 1) < 1e-6 && std::fabs(pxY - 1) < 1e-6 && std::fabs(ox) < 1e-3 && std::fabs(oy) < 1e-3;
}

Image transformLayer(const Image& src, const Geometry& g, int SW, int SH, double scale) {
    int W = std::max(1, int(std::lround(SW * scale))), H = std::max(1, int(std::lround(SH * scale)));
    Image out(W, H);
    if (src.empty() || g.sx == 0 || g.sy == 0 || g.opacity <= 0) return out;
    const double kx = src.width / g.mw, ky = src.height / g.mh;  // media px -> image px
    const double cosr = std::cos(g.rot), sinr = std::sin(g.rot);
    const double cxs = SW / 2.0 + g.px, cys = SH / 2.0 + g.py;
    const double u0 = g.mw * g.cl, u1 = g.mw * (1 - g.cr), v0 = g.mh * g.ct, v1 = g.mh * (1 - g.cb);
    if (u1 <= u0 || v1 <= v0) return out;
    const float op = float(g.opacity);
    bool axis = std::fabs(sinr) < 1e-9 && cosr > 0 && g.sx > 0 && g.sy > 0;
    // Fast path: axis aligned, uncropped, one source pixel per output pixel at
    // an integer offset (the common "clip fills the frame" case): copy rows.
    if (axis && g.cl == 0 && g.cr == 0 && g.ct == 0 && g.cb == 0) {
        double pxX = g.sx * scale / kx, pxY = g.sy * scale / ky;  // output pixels per image pixel
        double ox = (cxs - (g.mw / 2 + g.ax) * g.sx) * scale, oy = (cys - (g.mh / 2 + g.ay) * g.sy) * scale;
        if (std::fabs(pxX - 1) < 1e-6 && std::fabs(pxY - 1) < 1e-6 && std::fabs(ox - std::round(ox)) < 1e-3 &&
            std::fabs(oy - std::round(oy)) < 1e-3) {
            int dx = int(std::lround(ox)), dy = int(std::lround(oy));
            int x0 = std::max(0, dx), x1 = std::min(W, dx + src.width);
            if (x1 > x0)
                parallelRows(H, [&](int y0, int y1) {
                    for (int Y = y0; Y < y1; ++Y) {
                        int sy = Y - dy;
                        if (sy < 0 || sy >= src.height) continue;
                        const float* in = src.at(x0 - dx, sy);
                        float* o = out.at(x0, Y);
                        if (op >= 1.0f) std::copy(in, in + size_t(x1 - x0) * 4, o);
                        else
                            for (int i = 0; i < (x1 - x0) * 4; ++i) o[i] = in[i] * op;
                    }
                });
            return out;
        }
    }
    parallelRows(H, [&](int y0, int y1) {
        for (int Y = y0; Y < y1; ++Y) {
            float* o = out.row(Y);
            for (int X = 0; X < W; ++X, o += 4) {
                double pxs = (X + 0.5) / scale - cxs, pys = (Y + 0.5) / scale - cys;
                double rx = axis ? pxs : pxs * cosr + pys * sinr;
                double ry = axis ? pys : -pxs * sinr + pys * cosr;
                double u = rx / g.sx + g.mw / 2 + g.ax;
                double v = ry / g.sy + g.mh / 2 + g.ay;
                if (u < u0 - 1 || u > u1 + 1 || v < v0 - 1 || v > v1 + 1) continue;
                // Coverage for anti-aliased crop / frame edges (in output pixels).
                double pxPerMediaX = std::fabs(g.sx) * scale, pxPerMediaY = std::fabs(g.sy) * scale;
                double cov = std::clamp((u - u0) * pxPerMediaX + 0.5, 0.0, 1.0) * std::clamp((u1 - u) * pxPerMediaX + 0.5, 0.0, 1.0) *
                             std::clamp((v - v0) * pxPerMediaY + 0.5, 0.0, 1.0) * std::clamp((v1 - v) * pxPerMediaY + 0.5, 0.0, 1.0);
                if (cov <= 0) continue;
                // Bilinear sample with edge clamping (coverage handles the borders).
                double ix = u * kx - 0.5, iy = v * ky - 0.5;
                int x0 = int(std::floor(ix)), y0i = int(std::floor(iy));
                float fx = float(ix - x0), fy = float(iy - y0i);
                int xa = std::clamp(x0, 0, src.width - 1), xb = std::clamp(x0 + 1, 0, src.width - 1);
                int ya = std::clamp(y0i, 0, src.height - 1), yb = std::clamp(y0i + 1, 0, src.height - 1);
                const float* p00 = src.at(xa, ya);
                const float* p10 = src.at(xb, ya);
                const float* p01 = src.at(xa, yb);
                const float* p11 = src.at(xb, yb);
                float k = op * float(cov);
                for (int c = 0; c < 4; ++c) {
                    float top = p00[c] + (p10[c] - p00[c]) * fx;
                    float bot = p01[c] + (p11[c] - p01[c]) * fx;
                    o[c] = (top + (bot - top) * fy) * k;
                }
            }
        }
    });
    return out;
}

// Size in pixels to decode/generate the clip's source at, given its geometry.
void sourceSize(const Geometry& g, double scale, int nativeW, int nativeH, int& w, int& h) {
    double wantW = g.mw * std::fabs(g.sx) * scale, wantH = g.mh * std::fabs(g.sy) * scale;
    // Rotated content needs a little more resolution than its axis-aligned footprint.
    w = int(std::ceil(std::min<double>(nativeW, std::max(1.0, wantW))));
    h = int(std::ceil(std::min<double>(nativeH, std::max(1.0, wantH))));
}

Image clipLayer(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, const RenderOptions& o) {
    const FrameTime lt = t - c.start;
    const int SW = seq.width, SH = seq.height;
    Image src;
    double mw = SW, mh = SH;
    Geometry g;
    double sourceSeconds = -1;  // media time of the frame, for effects that follow the footage
    if (c.isGenerator()) {
        g = geometryFor(c.motion, lt, SW, SH, SW, SH);
        int w, h;
        sourceSize(g, o.scale, int(SW * 4), int(SH * 4), w, h);
        src = renderGenerator(c.generator, lt, w, h, double(w) / SW);
        // Titles and mattes are authored in SDR: graphics white sits at HDR reference white.
        convertColor(src, rec709Space(), sequenceColorSpace(seq), seq.hdrPeakNits);
    } else {
        const MediaItem* m = p.findMedia(c.mediaId);
        if (!m) return {};
        if (m->kind == MediaKind::Sequence) {
            const Sequence* nested = p.findSequence(m->sequenceId);
            if (!nested || o.depth >= kMaxDepth || nested->id == seq.id) return {};
            mw = nested->width;
            mh = nested->height;
            g = geometryFor(c.motion, lt, mw, mh, SW, SH);
            int w, h;
            sourceSize(g, o.scale, nested->width, nested->height, w, h);
            RenderOptions no = o;
            no.depth = o.depth + 1;
            no.scale = double(w) / nested->width;
            if (nested->multicam) no.soloVideoTrack = std::clamp(c.angle, 0, std::max(0, int(nested->videoTracks.size()) - 1));
            FrameTime nf = FrameTime(std::floor(c.sourceFrameAt(t) * nested->fpsValue() / seq.fpsValue() + 1e-6));
            src = renderSequenceFrame(p, *nested, nf, no);
            convertColor(src, sequenceColorSpace(*nested), sequenceColorSpace(seq), seq.hdrPeakNits);
        } else if (m->kind == MediaKind::Video || m->kind == MediaKind::Image) {
            if (!m->hasVideo && m->kind != MediaKind::Image) return {};
            std::string path = (o.useProxies && !m->proxyPath.empty()) ? m->proxyPath : m->path;
            mw = m->width > 0 ? m->width : SW;
            mh = m->height > 0 ? m->height : SH;
            g = geometryFor(c.motion, lt, mw, mh, SW, SH);
            int w, h;
            sourceSize(g, o.scale, int(mw), int(mh), w, h);
            double sec = 0;
            if (m->kind == MediaKind::Video) {
                sec = c.sourceFrameAt(t) / seq.fpsValue();
                double fd = m->fps.valid() ? 1.0 / m->fps.toDouble() : 1.0 / 30;
                sec = std::clamp(sec, 0.0, std::max(0.0, m->duration - fd * 0.5));
            }
            Frame16Ptr f = MediaPool::instance().videoFrame(path, sec, w, h, o.highQuality);
            if (!f) return {};
            if (m->kind == MediaKind::Video) sourceSeconds = sec;
            src = toImage(*f);
            // Slow motion between two source frames: blend them or follow the motion.
            const int sampling = c.timing.empty() ? 0 : int(c.timing.p("sampling", lt, 0));
            if (sampling > 0 && m->kind == MediaKind::Video && m->fps.valid()) {
                const double mf = m->fps.toDouble(), pos = sec * mf, base = std::floor(pos + 1e-6), frac = pos - base;
                const double next = (base + 1.25) / mf;
                if (frac > 0.02 && frac < 0.98 && next < m->duration) {
                    Frame16Ptr fa = MediaPool::instance().videoFrame(path, (base + 0.25) / mf, w, h, o.highQuality);
                    Frame16Ptr fb = MediaPool::instance().videoFrame(path, next, w, h, o.highQuality);
                    if (fa && fb) {
                        const Image a = toImage(*fa), b = toImage(*fb);
                        src = sampling == 1 ? blendFrames(a, b, frac)
                                            : interpolateFrames(a, b, frac, path + '#' + std::to_string(int64_t(base)) + '@' +
                                                                                std::to_string(w) + 'x' + std::to_string(h));
                    }
                }
            }
            // Input transform: the media's space into the sequence's working space.
            convertColor(src, mediaColorSpace(*m), sequenceColorSpace(seq), seq.hdrPeakNits);
        } else {
            return {};
        }
    }
    if (src.empty()) return {};
    // Filters run in source space (before the fixed transform), like most NLEs.
    double fitX = std::fabs(g.sx) / std::max(1e-9, std::fabs(c.motion.p("scale", lt, 100) / 100.0 *
                                                                c.motion.p("scale_x", lt, 100) / 100.0));
    double pixelScale = src.width / std::max(1.0, mw * fitX);
    for (const auto& e : c.effects) applyVideoEffect(e, lt, src, pixelScale, sourceSeconds);
    if (identityLayer(src, g, SW, SH, o.scale)) return src;  // a full-frame clip: no copy
    return transformLayer(src, g, SW, SH, o.scale);
}

const Clip* findClip(const Track& t, Id id) {
    if (!id) return nullptr;
    for (const auto& c : t.clips)
        if (c.id == id) return &c;
    return nullptr;
}

}  // namespace

Image renderSequenceFrame(const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& opts) {
    const int solo = opts.soloVideoTrack;
    RenderOptions o = opts;
    o.soloVideoTrack = -1;
    int W = std::max(1, int(std::lround(seq.width * o.scale))), H = std::max(1, int(std::lround(seq.height * o.scale)));
    Image canvas(W, H);
    bool canvasEmpty = true;  // nothing drawn yet: the first normal layer can be moved in
    auto composite = [&](Image&& layer, const std::string& mode) {
        if (layer.empty()) return;
        if (canvasEmpty && mode == "normal" && layer.width == W && layer.height == H) canvas = std::move(layer);
        else blendOnto(canvas, layer, mode, 1.0f);
        canvasEmpty = false;
    };
    for (size_t ti = 0; ti < seq.videoTracks.size(); ++ti) {
        const Track& track = seq.videoTracks[ti];
        if (solo >= 0 ? int(ti) != solo : track.muted) continue;  // an angle shows even if its track is hidden
        const Transition* active = nullptr;
        FrameTime from = 0, to = 0;
        for (const auto& tr : track.transitions) {
            FrameTime a, b;
            if (edit::transitionRange(track, tr, a, b) && t >= a && t < b) {
                active = &tr;
                from = a;
                to = b;
                break;
            }
        }
        if (active) {
            const Clip* A = findClip(track, active->clipA);
            const Clip* B = findClip(track, active->clipB);
            double u = (double(t - from) + 0.5) / double(std::max<FrameTime>(1, to - from));
            Image la = (A && A->enabled) ? clipLayer(p, seq, *A, t, o) : Image();
            Image lb = (B && B->enabled) ? clipLayer(p, seq, *B, t, o) : Image();
            Image mixed = transitionMix(active->type, active->params, la, lb, u, W, H);
            const Clip* top = B ? B : A;
            composite(std::move(mixed), top ? top->blendMode : "normal");
            continue;
        }
        for (const auto& c : track.clips) {
            if (c.start > t) break;
            if (!c.contains(t) || !c.enabled) continue;
            composite(clipLayer(p, seq, c, t, o), c.blendMode);
            break;
        }
    }
    return canvas;
}

Image renderProgramFrame(const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& o) {
    Image img = renderSequenceFrame(p, seq, t, o);
    flattenOver(img, 0, 0, 0);
    if (o.captions && o.depth == 0)
        if (const CaptionTrack* track = captionTrackFor(seq)) drawCaption(img, *track, t, &sequenceColorSpace(seq));
    if (!o.displaySpace.empty() && o.depth == 0)
        if (const ColorSpace* d = findColorSpace(o.displaySpace)) convertColor(img, sequenceColorSpace(seq), *d, seq.hdrPeakNits);
    return img;
}

namespace {
bool clipGeometry(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, Geometry& g) {
    double mw = seq.width, mh = seq.height;
    if (!c.isGenerator()) {
        const MediaItem* m = p.findMedia(c.mediaId);
        if (!m) return false;
        if (m->kind == MediaKind::Sequence) {
            const Sequence* nested = p.findSequence(m->sequenceId);
            if (!nested) return false;
            mw = nested->width;
            mh = nested->height;
        } else {
            if (!m->hasVideo && m->kind != MediaKind::Image) return false;
            if (m->width > 0) mw = m->width;
            if (m->height > 0) mh = m->height;
        }
    }
    g = geometryFor(c.motion, t - c.start, mw, mh, seq.width, seq.height);
    return g.sx != 0 && g.sy != 0;
}
}  // namespace

bool clipFrameSize(const Project& p, const Sequence& seq, const Clip& c, double& w, double& h) {
    Geometry g;
    if (!clipGeometry(p, seq, c, c.start, g)) return false;
    w = g.mw;
    h = g.mh;
    return true;
}

bool clipFrameToSequence(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, double u, double v,
                         double& x, double& y) {
    Geometry g;
    if (!clipGeometry(p, seq, c, t, g)) return false;
    const double rx = (u * g.mw - g.mw / 2 - g.ax) * g.sx, ry = (v * g.mh - g.mh / 2 - g.ay) * g.sy;
    const double cr = std::cos(g.rot), sr = std::sin(g.rot);
    x = seq.width / 2.0 + g.px + rx * cr - ry * sr;
    y = seq.height / 2.0 + g.py + rx * sr + ry * cr;
    return true;
}

bool sequenceToClipFrame(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, double x, double y,
                         double& u, double& v) {
    Geometry g;
    if (!clipGeometry(p, seq, c, t, g)) return false;
    const double pxs = x - seq.width / 2.0 - g.px, pys = y - seq.height / 2.0 - g.py;
    const double cr = std::cos(g.rot), sr = std::sin(g.rot);
    const double rx = pxs * cr + pys * sr, ry = -pxs * sr + pys * cr;
    u = (rx / g.sx + g.mw / 2 + g.ax) / g.mw;
    v = (ry / g.sy + g.mh / 2 + g.ay) / g.mh;
    return true;
}

void drawCaption(Image& img, const CaptionTrack& track, FrameTime t, const ColorSpace* space) {
    const Caption* cap = captionAt(track, t);
    if (!cap || img.width < 8 || img.height < 8) return;
    const CaptionStyle& st = track.style;
    QFont f(QString::fromStdString(st.font));
    const double px = std::max(4.0, st.size * img.height);
    f.setPixelSize(int(std::lround(px)));
    f.setBold(st.bold);
    f.setHintingPreference(QFont::PreferNoHinting);
    const QFontMetricsF fm(f);
    const QStringList lines = QString::fromStdString(cap->text).split('\n');
    const double lineH = fm.height() * 1.1, padX = px * 0.3, padY = px * 0.08;
    double blockW = 0;
    for (const QString& l : lines) blockW = std::max(blockW, fm.horizontalAdvance(l));
    const double blockH = lineH * double(lines.size());
    const double bottom = std::clamp(st.position, 0.05, 1.0) * img.height;
    // Render only the caption's area, then blend it over the frame.
    const int x0 = std::max(0, int(std::floor(img.width / 2.0 - blockW / 2 - padX - 2)));
    const int x1 = std::min(img.width, int(std::ceil(img.width / 2.0 + blockW / 2 + padX + 2)));
    const int y0 = std::max(0, int(std::floor(bottom - blockH - padY - 2)));
    const int y1 = std::min(img.height, int(std::ceil(bottom + padY + 2)));
    if (x1 <= x0 || y1 <= y0) return;
    QImage qi(x1 - x0, y1 - y0, QImage::Format_RGBA8888_Premultiplied);
    qi.fill(Qt::transparent);
    {
        QPainter pa(&qi);
        pa.setRenderHint(QPainter::Antialiasing);
        pa.translate(-x0, -y0);
        auto col = [](double r, double g, double b, double a) {
            return QColor::fromRgbF(float(std::clamp(r, 0.0, 1.0)), float(std::clamp(g, 0.0, 1.0)),
                                    float(std::clamp(b, 0.0, 1.0)), float(std::clamp(a, 0.0, 1.0)));
        };
        QPainterPath text;
        for (int i = 0; i < lines.size(); ++i) {
            const double w = fm.horizontalAdvance(lines[i]);
            const double top = bottom - blockH + i * lineH;
            if (st.boxOpacity > 0 && !lines[i].isEmpty())
                pa.fillRect(QRectF(img.width / 2.0 - w / 2 - padX, top - padY, w + 2 * padX, lineH + 2 * padY),
                            col(st.boxR, st.boxG, st.boxB, st.boxOpacity));
            text.addText(QPointF(img.width / 2.0 - w / 2, top + (lineH - fm.height()) / 2 + fm.ascent()), f, lines[i]);
        }
        if (st.outline > 0)
            pa.strokePath(text, QPen(QColor(0, 0, 0), st.outline * px * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        pa.fillPath(text, col(st.textR, st.textG, st.textB, 1));
    }
    if (space && space->id != rec709Space().id) {
        // Caption colours are SDR: convert the rendered patch into the picture's space.
        Image patch(qi.width(), qi.height());
        for (int y = 0; y < patch.height; ++y) {
            const uchar* s = qi.constScanLine(y);
            float* d = patch.row(y);
            for (int x = 0; x < patch.width * 4; ++x) d[x] = s[x] / 255.0f;
        }
        convertColor(patch, rec709Space(), *space);
        for (int y = 0; y < patch.height; ++y) {
            const float* s = patch.row(y);
            float* d = img.row(y0 + y) + size_t(x0) * 4;
            for (int x = 0; x < patch.width; ++x, s += 4, d += 4)
                for (int c = 0; c < 4; ++c) d[c] = s[c] + d[c] * (1 - s[3]);
        }
        return;
    }
    const float k = 1.0f / 255.0f;
    for (int y = y0; y < y1; ++y) {
        const uchar* s = qi.constScanLine(y - y0);
        float* d = img.row(y) + size_t(x0) * 4;
        for (int x = 0; x < x1 - x0; ++x, s += 4, d += 4) {
            const float a = s[3] * k;
            if (a <= 0) continue;
            for (int c = 0; c < 4; ++c) d[c] = s[c] * k + d[c] * (1 - a);
        }
    }
}

Image renderMediaFrame(const Project& p, const MediaItem& m, double seconds, int w, int h) {
    Image out(std::max(1, w), std::max(1, h));
    out.fill(0, 0, 0, 1);
    if (m.kind == MediaKind::Sequence) {
        const Sequence* s = p.findSequence(m.sequenceId);
        if (!s) return out;
        RenderOptions o;
        o.scale = std::min(double(w) / s->width, double(h) / s->height);
        o.displaySpace = "rec709";
        Image img = renderProgramFrame(p, *s, FrameTime(std::floor(seconds * s->fpsValue() + 1e-6)), o);
        Geometry g;
        g.mw = img.width;
        g.mh = img.height;
        g.sx = g.sy = 1;
        Image placed = transformLayer(img, g, w, h, 1.0);
        blendOnto(out, placed, "normal", 1);
        return out;
    }
    if (!m.hasVideo && m.kind != MediaKind::Image) return out;
    double mw = m.width > 0 ? m.width : w, mh = m.height > 0 ? m.height : h;
    double fit = std::min(w / mw, h / mh);
    int dw = std::max(1, int(std::lround(mw * fit))), dh = std::max(1, int(std::lround(mh * fit)));
    Frame16Ptr f = MediaPool::instance().videoFrame(m.path, std::max(0.0, seconds), dw, dh);
    if (!f) return out;
    Image img = toImage(*f);
    convertColor(img, mediaColorSpace(m), rec709Space());  // the source monitor shows SDR
    Geometry g;
    g.mw = mw;
    g.mh = mh;
    g.sx = g.sy = fit;
    Image placed = transformLayer(img, g, w, h, 1.0);
    blendOnto(out, placed, "normal", 1);
    return out;
}

// ---------------------------------------------------------------------------
// Audio

namespace {

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] = {0, 0}, z2[2] = {0, 0};
    void set(double nb0, double nb1, double nb2, double a0, double na1, double na2) {
        b0 = nb0 / a0;
        b1 = nb1 / a0;
        b2 = nb2 / a0;
        a1 = na1 / a0;
        a2 = na2 / a0;
    }
    void peaking(double fs, double f0, double q, double db) {
        double A = std::pow(10.0, db / 40), w0 = 2 * M_PI * std::min(f0, fs * 0.49) / fs, al = std::sin(w0) / (2 * q);
        set(1 + al * A, -2 * std::cos(w0), 1 - al * A, 1 + al / A, -2 * std::cos(w0), 1 - al / A);
    }
    void lowShelf(double fs, double f0, double db) {
        double A = std::pow(10.0, db / 40), w0 = 2 * M_PI * std::min(f0, fs * 0.49) / fs, cs = std::cos(w0);
        double al = std::sin(w0) / 2 * std::sqrt(2.0), sa = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) - (A - 1) * cs + sa), 2 * A * ((A - 1) - (A + 1) * cs), A * ((A + 1) - (A - 1) * cs - sa),
            (A + 1) + (A - 1) * cs + sa, -2 * ((A - 1) + (A + 1) * cs), (A + 1) + (A - 1) * cs - sa);
    }
    void highShelf(double fs, double f0, double db) {
        double A = std::pow(10.0, db / 40), w0 = 2 * M_PI * std::min(f0, fs * 0.49) / fs, cs = std::cos(w0);
        double al = std::sin(w0) / 2 * std::sqrt(2.0), sa = 2 * std::sqrt(A) * al;
        set(A * ((A + 1) + (A - 1) * cs + sa), -2 * A * ((A - 1) + (A + 1) * cs), A * ((A + 1) + (A - 1) * cs - sa),
            (A + 1) - (A - 1) * cs + sa, 2 * ((A - 1) - (A + 1) * cs), (A + 1) - (A - 1) * cs - sa);
    }
    void lowPass(double fs, double f0) {
        double w0 = 2 * M_PI * std::min(f0, fs * 0.49) / fs, cs = std::cos(w0), al = std::sin(w0) / (2 * 0.70710678);
        set((1 - cs) / 2, 1 - cs, (1 - cs) / 2, 1 + al, -2 * cs, 1 - al);
    }
    void highPass(double fs, double f0) {
        double w0 = 2 * M_PI * std::min(f0, fs * 0.49) / fs, cs = std::cos(w0), al = std::sin(w0) / (2 * 0.70710678);
        set((1 + cs) / 2, -(1 + cs), (1 + cs) / 2, 1 + al, -2 * cs, 1 - al);
    }
    float process(int ch, float x) {
        double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return float(y);
    }
};

inline void panGains(double pan, float& l, float& r) {
    double a = (std::clamp(pan, -1.0, 1.0) + 1) * M_PI / 4;
    l = float(std::cos(a) * M_SQRT2);
    r = float(std::sin(a) * M_SQRT2);
}

inline float dbToLin(double db) { return db <= -96 ? 0.0f : float(std::pow(10.0, db / 20)); }

}  // namespace

struct AudioMixer::State {
    Biquad bq[3];
    std::vector<double> lastParams;
    double env = 0;
    double gain = 1;
    std::vector<float> delay;
    size_t pos = 0;
    // "plugin" effects: the running plugin, kept across seeks.
    std::unique_ptr<plugins::Instance> plugin;
    double pluginRate = 0;
    bool pluginFailed = false;
    std::string pluginState;  // the effect's saved state the plugin last loaded
    std::vector<float> planar;
};

namespace {
constexpr int kPluginBlock = 4096;
}

namespace {
// Runs a third-party plugin over an interleaved stereo block.
}  // namespace

// Loads the effect's plugin (tried once) and keeps it activated at `sr`; true if it was just loaded.
bool AudioMixer::ensurePlugin(State& st, const Effect& e, double sr) {
    if (st.plugin && st.pluginRate != sr) {
        st.pluginRate = sr;
        if (!st.plugin->activate(sr, kPluginBlock)) st.plugin.reset();
    }
    if (st.plugin || st.pluginFailed) return false;
    // A missing or broken plugin leaves the audio unprocessed.
    st.pluginFailed = true;
    if (auto d = plugins::Registry::instance().find(e.s("plugin_id")))
        if (auto inst = plugins::instantiate(*d))
            if (inst->activate(sr, kPluginBlock)) {
                const std::string state = plugins::decodeState(e.s("state"));
                if (!state.empty()) inst->loadState(state);
                st.pluginState = e.s("state");
                st.plugin = std::move(inst);
                st.pluginRate = sr;
                st.pluginFailed = false;
                return true;
            }
    return false;
}

namespace {
void processPlugin(AudioMixer::State& st, const Effect& e, double sr, FrameTime lt, bool paramsChanged, float* buf,
                   int frames) {
    if (st.plugin && st.pluginRate != sr) {
        st.pluginRate = sr;
        if (!st.plugin->activate(sr, kPluginBlock)) st.plugin.reset();
    }
    if (AudioMixer::ensurePlugin(st, e, sr)) paramsChanged = true;
    if (!st.plugin) return;
    // Settings changed in the plugin's editor arrive as a new saved state.
    if (e.s("state") != st.pluginState) {
        st.pluginState = e.s("state");
        const std::string state = plugins::decodeState(st.pluginState);
        if (!state.empty()) st.plugin->loadState(state);
        paramsChanged = true;
    }
    if (paramsChanged)
        for (const auto& [key, param] : e.params)
            if (key.rfind("param.", 0) == 0)
                st.plugin->setParameter(uint32_t(std::strtoul(key.c_str() + 6, nullptr, 10)), param.at(lt));
    st.planar.resize(size_t(frames) * 2);
    float* ch[2] = {st.planar.data(), st.planar.data() + frames};
    for (int i = 0; i < frames; ++i) {
        ch[0][i] = buf[size_t(i) * 2];
        ch[1][i] = buf[size_t(i) * 2 + 1];
    }
    st.plugin->process(ch, 2, frames);
    for (int i = 0; i < frames; ++i) {
        buf[size_t(i) * 2] = ch[0][i];
        buf[size_t(i) * 2 + 1] = ch[1][i];
    }
}
}  // namespace

AudioMixer::AudioMixer() = default;
AudioMixer::~AudioMixer() = default;

void AudioMixer::reset() {
    std::lock_guard lock(m_);
    resetLocked();
}

void AudioMixer::resetLocked() {
    nextStart_ = -1;
    // Plugins are expensive to load: keep them, clearing only their audio state.
    for (auto it = states_.begin(); it != states_.end();) {
        if (auto plugin = std::move(it->second->plugin)) {
            plugin->reset();
            const double rate = it->second->pluginRate;
            std::string state = std::move(it->second->pluginState);
            *it->second = State{};
            it->second->plugin = std::move(plugin);
            it->second->pluginRate = rate;
            it->second->pluginState = std::move(state);
            ++it;
        } else {
            it = states_.erase(it);
        }
    }
}

void AudioMixer::processChain(const std::vector<Effect>& chain, Id owner, FrameTime lt, double sr, float* buf, int frames) {
    for (const Effect& e : chain) {
        if (!e.enabled || isSourceAudioEffect(e.type)) continue;
        auto& st = states_[{owner, e.id}];
        if (!st) st = std::make_unique<State>();
        std::vector<double> params;
        for (const auto& [k, v] : e.params) params.push_back(v.at(lt));
        bool changed = params != st->lastParams;
        st->lastParams = params;
        if (e.type == "eq3") {
            if (changed) {
                st->bq[0].lowShelf(sr, e.p("low_hz", lt, 200), e.p("low_db", lt));
                st->bq[1].peaking(sr, e.p("mid_hz", lt, 1000), std::max(0.1, e.p("mid_q", lt, 0.9)), e.p("mid_db", lt));
                st->bq[2].highShelf(sr, e.p("high_hz", lt, 5000), e.p("high_db", lt));
            }
            for (int i = 0; i < frames; ++i)
                for (int ch = 0; ch < 2; ++ch) {
                    float v = buf[size_t(i) * 2 + ch];
                    for (auto& bq : st->bq) v = bq.process(ch, v);
                    buf[size_t(i) * 2 + ch] = v;
                }
        } else if (e.type == "highpass" || e.type == "lowpass") {
            if (changed) {
                if (e.type == "highpass") st->bq[0].highPass(sr, e.p("hz", lt, 80));
                else st->bq[0].lowPass(sr, e.p("hz", lt, 12000));
            }
            for (int i = 0; i < frames; ++i)
                for (int ch = 0; ch < 2; ++ch)
                    buf[size_t(i) * 2 + ch] = st->bq[0].process(ch, buf[size_t(i) * 2 + ch]);
        } else if (e.type == "compressor") {
            double thr = e.p("threshold_db", lt, -18), ratio = std::max(1.0, e.p("ratio", lt, 4));
            double att = std::exp(-1.0 / (std::max(0.1, e.p("attack_ms", lt, 10)) * sr / 1000));
            double rel = std::exp(-1.0 / (std::max(1.0, e.p("release_ms", lt, 120)) * sr / 1000));
            double makeup = e.p("makeup_db", lt);
            for (int i = 0; i < frames; ++i) {
                float* d = &buf[size_t(i) * 2];
                double lvl = std::max(std::fabs(d[0]), std::fabs(d[1]));
                st->env = lvl > st->env ? att * st->env + (1 - att) * lvl : rel * st->env + (1 - rel) * lvl;
                double envDb = 20 * std::log10(st->env + 1e-9);
                double over = envDb - thr;
                double gr = over > 0 ? over * (1 - 1 / ratio) : 0;
                float g = dbToLin(makeup - gr);
                d[0] *= g;
                d[1] *= g;
            }
        } else if (e.type == "limiter") {
            float ceil = dbToLin(e.p("ceiling_db", lt, -1));
            double rel = std::exp(-1.0 / (std::max(1.0, e.p("release_ms", lt, 60)) * sr / 1000));
            for (int i = 0; i < frames; ++i) {
                float* d = &buf[size_t(i) * 2];
                double pk = std::max(std::fabs(d[0]), std::fabs(d[1]));
                double target = pk > ceil ? ceil / pk : 1.0;
                st->gain = target < st->gain ? target : rel * st->gain + (1 - rel) * target;
                d[0] = std::clamp(float(d[0] * st->gain), -ceil, ceil);
                d[1] = std::clamp(float(d[1] * st->gain), -ceil, ceil);
            }
        } else if (e.type == "plugin") {
            processPlugin(*st, e, sr, lt, changed, buf, frames);
        } else if (e.type == "delay") {
            size_t del = size_t(std::max(1.0, e.p("time_ms", lt, 300) * sr / 1000));
            float fb = float(std::clamp(e.p("feedback", lt, 0.35), 0.0, 0.95));
            float mix = float(e.p("mix", lt, 30) / 100.0);
            size_t N = size_t(sr * 2.1);
            if (st->delay.size() != N * 2) st->delay.assign(N * 2, 0.0f);
            del = std::min(del, N - 1);
            for (int i = 0; i < frames; ++i) {
                size_t r = (st->pos + N - del) % N;
                for (int ch = 0; ch < 2; ++ch) {
                    float x = buf[size_t(i) * 2 + ch];
                    float delayed = st->delay[r * 2 + size_t(ch)];
                    st->delay[st->pos * 2 + size_t(ch)] = x + delayed * fb;
                    buf[size_t(i) * 2 + ch] = x + delayed * mix;
                }
                st->pos = (st->pos + 1) % N;
            }
        }
    }
}

void AudioMixer::mix(const Project& p, const Sequence& seq, int64_t start, int frames, float* out,
                     std::vector<MeterLevels>* trackLevels) {
    std::lock_guard lock(m_);
    if (start != nextStart_) {
        // Not where the last block ended (a seek, or the first block): start the
        // effects afresh, and fill plugin pipelines with the audio just before
        // `start` so delay-compensated output is right from the first sample.
        resetLocked();
        if (const int pre = maxLatency(seq, seq.sampleRate); pre > 0) {
            std::vector<float> scratch(size_t(pre) * 2);
            mixInto(p, seq, start - pre, pre, scratch.data(), nullptr, 0);
        }
    }
    nextStart_ = start + frames;
    mixInto(p, seq, start, frames, out, trackLevels, 0);
}

bool AudioMixer::mixTrackClips(const Project& p, const Sequence& seq, const Track& track, int64_t start, int frames,
                               double sr, int depth, float* trackBuf) {
    const double fps = seq.fpsValue();
    const int64_t end = start + frames;
    std::vector<float> clipBuf(size_t(frames) * 2);
    bool any = false;
    for (const Clip& c : track.clips) {
        if (!c.enabled) continue;
        int64_t cs = int64_t(std::llround(c.start * sr / fps)), ce = int64_t(std::llround(c.end() * sr / fps));
        // Transitions extend the playable range and add fades.
        int64_t ps = cs, pe = ce;
        int64_t fiS = 0, fiE = 0, foS = 0, foE = 0;
        bool equalPowerIn = true, equalPowerOut = true;
        for (const auto& tr : track.transitions) {
            FrameTime a, b;
            if (!edit::transitionRange(track, tr, a, b)) continue;
            int64_t as = int64_t(std::llround(a * sr / fps)), bs = int64_t(std::llround(b * sr / fps));
            if (tr.clipB == c.id) {
                ps = std::min(ps, as);
                fiS = as;
                fiE = bs;
                equalPowerIn = tr.type != "crossfade_linear";
            }
            if (tr.clipA == c.id) {
                pe = std::max(pe, bs);
                foS = as;
                foE = bs;
                equalPowerOut = tr.type != "crossfade_linear";
            }
        }
        // A chain with latency is fed its source that many samples ahead, so its
        // output lines up with the picture; it starts that much before the clip.
        const int64_t lat = chainLatency(c.effects, c.id, sr);
        if (pe <= start || ps - lat >= end) continue;
        const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr;
        if (!m) continue;
        std::fill(clipBuf.begin(), clipBuf.end(), 0.0f);
        const int64_t rs = start + lat;  // the source window fed to the chain
        int64_t s0 = std::max(rs, ps), s1 = std::min(rs + frames, pe);
        const double srcBase = c.sourceIn * sr / fps;
        const bool ramped = c.ramped();  // Time Remapping: the source position follows the speed curve
        if (m->kind == MediaKind::Sequence) {
            const Sequence* nested = p.findSequence(m->sequenceId);
            if (!nested || depth >= kMaxDepth || nested->id == seq.id) continue;
            // Mix the span of the nested sequence this block covers (at our
            // rate), then resample it for the clip's speed and direction.
            auto srcPos = [&](int64_t smp) {
                if (c.reverse) return srcBase + double(ce - 1 - smp) * c.speed;
                if (ramped) return (c.sourceIn + c.sourceOffset(double(smp - cs) * fps / sr)) * sr / fps;  // speed ramp
                return srcBase + double(smp - cs) * c.speed;
            };
            const double lo = s1 > s0 ? std::min(srcPos(s0), srcPos(s1 - 1)) : 0;
            const double hi = s1 > s0 ? std::max(srcPos(s0), srcPos(s1 - 1)) : 0;
            int64_t nStart = int64_t(std::floor(lo));
            int64_t nLen = s1 > s0 ? int64_t(std::floor(hi)) - nStart + 2 : 0;
            std::vector<float> nb(size_t(std::max<int64_t>(0, nLen)) * 2, 0.0f);
            const int only = nested->multicam ? c.audioAngle : -1;
            if (nLen > 0) mixInto(p, *nested, nStart, int(nLen), nb.data(), nullptr, depth + 1, int(sr), only);
            for (int64_t smp = s0; smp < s1; ++smp) {
                double rel = srcPos(smp) - double(nStart);
                int64_t i = std::clamp<int64_t>(int64_t(rel), 0, nLen - 2);
                float f = float(std::clamp(rel - double(i), 0.0, 1.0));
                float* d = &clipBuf[size_t(smp - rs) * 2];
                d[0] = nb[size_t(i) * 2] + (nb[size_t(i + 1) * 2] - nb[size_t(i) * 2]) * f;
                d[1] = nb[size_t(i) * 2 + 1] + (nb[size_t(i + 1) * 2 + 1] - nb[size_t(i) * 2 + 1]) * f;
            }
        } else {
            if (!m->hasAudio) continue;
            AudioBufferPtr buf = nonBlocking_ ? MediaPool::instance().audioIfReady(m->path, int(sr))
                                              : MediaPool::instance().audio(m->path, int(sr));
            if (!buf || buf->samples.empty()) continue;
            // Noise reduction and voice isolation work on the whole source (the
            // original plays until the cleaned copy is ready in real time).
            std::vector<const Effect*> sourceFx;
            for (const Effect& e : c.effects)
                if (e.enabled && isSourceAudioEffect(e.type)) sourceFx.push_back(&e);
            if (!sourceFx.empty())
                if (AudioBufferPtr clean = cleanedAudio(m->path, buf, sourceFx, !nonBlocking_)) buf = clean;
            const int64_t n = buf->frames();
            const float* src = buf->samples.data();
            for (int64_t s = s0; s < s1; ++s) {
                double pos = c.reverse ? srcBase + double(ce - 1 - s) * c.speed
                             : ramped  ? (c.sourceIn + c.sourceOffset(double(s - cs) * fps / sr)) * sr / fps
                                       : srcBase + double(s - cs) * c.speed;
                if (pos < 0 || pos >= double(n - 1)) continue;
                int64_t i = int64_t(pos);
                float f = float(pos - double(i));
                float* d = &clipBuf[size_t(s - rs) * 2];
                d[0] = src[i * 2] + (src[i * 2 + 2] - src[i * 2]) * f;
                d[1] = src[i * 2 + 1] + (src[i * 2 + 3] - src[i * 2 + 1]) * f;
            }
        }
        // Clip filters (stateful, processed over the whole block for continuity).
        processChain(c.effects, c.id, FrameTime(double(start) * fps / sr) - c.start, sr, clipBuf.data(), frames);
        // Clip volume / pan (keyframed, evaluated every 64 samples) and fades.
        const int64_t o0 = std::max(start, ps), o1 = std::min(end, pe);
        for (int64_t s = o0; s < o1; s += 64) {
            int64_t e2 = std::min(o1, s + 64);
            FrameTime lt = FrameTime(std::floor(double(s) * fps / sr)) - c.start;
            float g = dbToLin(c.audio.p("gain_db", lt, 0));
            float pl, pr;
            panGains(c.audio.p("pan", lt, 0), pl, pr);
            for (int64_t k = s; k < e2; ++k) {
                float fade = 1;
                if (fiE > fiS && k < fiE) {
                    double u = std::clamp(double(k - fiS) / double(fiE - fiS), 0.0, 1.0);
                    fade *= equalPowerIn ? float(std::sin(u * M_PI / 2)) : float(u);
                }
                if (foE > foS && k >= foS) {
                    double u = std::clamp(double(k - foS) / double(foE - foS), 0.0, 1.0);
                    fade *= equalPowerOut ? float(std::cos(u * M_PI / 2)) : float(1 - u);
                }
                float* d = &clipBuf[size_t(k - start) * 2];
                float* tb = &trackBuf[size_t(k - start) * 2];
                tb[0] += d[0] * g * pl * fade;
                tb[1] += d[1] * g * pr * fade;
            }
        }
        any = true;
    }
    return any;
}

int AudioMixer::chainLatency(const std::vector<Effect>& chain, Id owner, double sr) {
    int total = 0;
    for (const Effect& e : chain) {
        if (!e.enabled || e.type != "plugin") continue;
        auto& st = states_[{owner, e.id}];
        if (!st) st = std::make_unique<State>();
        ensurePlugin(*st, e, sr);
        if (st->plugin) total += std::max(0, st->plugin->latencySamples());
    }
    return total;
}

int AudioMixer::maxLatency(const Sequence& seq, double sr) {
    int master = chainLatency(seq.masterEffects, seq.id, sr), bus = 0, track = 0;
    for (const Bus& b : seq.buses) bus = std::max(bus, chainLatency(b.effects, b.id, sr));
    for (const Track& t : seq.audioTracks) {
        int clip = 0;
        for (const Clip& c : t.clips) clip = std::max(clip, chainLatency(c.effects, c.id, sr));
        track = std::max(track, clip + chainLatency(t.effects, t.id, sr));
    }
    return master + bus + track;
}

void AudioMixer::mixInto(const Project& p, const Sequence& seq, int64_t start, int frames, float* out,
                         std::vector<MeterLevels>* trackLevels, int depth, int rate, int onlyTrack) {
    const double sr = rate > 0 ? rate : seq.sampleRate;
    const double fps = seq.fpsValue();
    bool anySolo = std::any_of(seq.audioTracks.begin(), seq.audioTracks.end(), [](const Track& t) { return t.solo; });
    if (trackLevels && depth == 0) trackLevels->assign(seq.audioTracks.size(), MeterLevels{});
    auto frameAt = [&](int64_t sample) { return FrameTime(double(sample) * fps / sr); };
    // Delay compensation: each stage is fed ahead by the latency of the stages
    // after it, so everything lines up at the output. The master chain gets the
    // mix of [start + Lm, ...), a bus that of [start + Lm + Lb, ...), and a track
    // its clips at [start + Lm + Lb + Lt, ...).
    const int64_t masterLat = chainLatency(seq.masterEffects, seq.id, sr);
    std::vector<float> master(size_t(frames) * 2, 0.0f);
    std::map<Id, std::vector<float>> busBufs;
    std::map<Id, int64_t> busLat;
    for (const Bus& b : seq.buses) {
        busBufs[b.id].assign(size_t(frames) * 2, 0.0f);
        busLat[b.id] = chainLatency(b.effects, b.id, sr);
    }
    std::vector<float> trackBuf(size_t(frames) * 2);
    for (size_t ti = 0; ti < seq.audioTracks.size(); ++ti) {
        const Track& track = seq.audioTracks[ti];
        if (onlyTrack >= 0 ? int(ti) != onlyTrack : (track.muted || (anySolo && !track.solo))) continue;
        auto bus = track.output ? busBufs.find(track.output) : busBufs.end();
        const int64_t downstream = masterLat + (bus != busBufs.end() ? busLat[bus->first] : 0);
        const int64_t trackLat = chainLatency(track.effects, track.id, sr);
        std::fill(trackBuf.begin(), trackBuf.end(), 0.0f);
        const bool any = mixTrackClips(p, seq, track, start + downstream + trackLat, frames, sr, depth, trackBuf.data());
        // Track inserts keep running without clips, so reverb and delay tails ring out.
        if (!any && track.effects.empty()) continue;
        if (!track.effects.empty())
            processChain(track.effects, track.id, frameAt(start + downstream + trackLat), sr, trackBuf.data(), frames);
        float tg = dbToLin(track.volumeDb), tl, tr;
        panGains(track.pan, tl, tr);
        float* dest = bus != busBufs.end() ? bus->second.data() : master.data();
        MeterLevels lv;
        for (int i = 0; i < frames; ++i) {
            float l = trackBuf[size_t(i) * 2] * tg * tl, r = trackBuf[size_t(i) * 2 + 1] * tg * tr;
            dest[i * 2] += l;
            dest[i * 2 + 1] += r;
            lv.peakL = std::max(lv.peakL, std::fabs(l));
            lv.peakR = std::max(lv.peakR, std::fabs(r));
        }
        if (trackLevels && depth == 0) (*trackLevels)[ti] = lv;
    }
    for (const Bus& b : seq.buses) {
        std::vector<float>& bb = busBufs[b.id];
        if (!b.effects.empty()) processChain(b.effects, b.id, frameAt(start + masterLat + busLat[b.id]), sr, bb.data(), frames);
        if (b.muted) continue;
        float g = dbToLin(b.volumeDb), bl, br;
        panGains(b.pan, bl, br);
        for (int i = 0; i < frames; ++i) {
            master[size_t(i) * 2] += bb[size_t(i) * 2] * g * bl;
            master[size_t(i) * 2 + 1] += bb[size_t(i) * 2 + 1] * g * br;
        }
    }
    if (!seq.masterEffects.empty()) processChain(seq.masterEffects, seq.id, frameAt(start + masterLat), sr, master.data(), frames);
    const float g = dbToLin(seq.masterVolumeDb);
    for (int i = 0; i < frames * 2; ++i) out[i] = master[size_t(i)] * g;
}

}  // namespace montage
