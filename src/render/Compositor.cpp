#include "Compositor.h"
#include "Ofx.h"

#include "core/ColorGroups.h"
#include "core/Automation.h"

#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>

#include "AudioFx.h"
#include "ColorSpace.h"
#include "Exporter.h"
#include "Processing.h"
#include "Retime.h"
#include "Shapes.h"
#include "TemporalFx.h"
#include "VideoDenoise.h"
#include "audio/PluginEffect.h"
#include "audio/SpeechCleanup.h"
#include "audio/TimeStretch.h"
#include "core/ProjectIO.h"
#include "core/Bleep.h"
#include "core/ClipAnimation.h"
#include "render/Spherical.h"
#include "render/AudioReactive.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/Surround.h"
#include "core/History.h"
#include "media/MediaPool.h"
#include "media/Relink.h"
#include "media/DepthMap.h"
#include "media/Matting.h"
#include "media/Faces.h"
#include "media/Rife.h"
#include "media/SuperScale.h"

namespace montage {

namespace {

constexpr int kMaxDepth = 8;

// ---------------------------------------------------------------------------
// Generators

// How far an animation has got (0 hidden, 1 settled), eased out.
double easeOut(double p) { return 1 - std::pow(1 - std::clamp(p, 0.0, 1.0), 3); }

Image renderTitle(const Effect& g, FrameTime t, int w, int h, double scale, FrameTime duration, double fps) {
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
        // Lines after the first can have their own size and colour.
        const bool sub = g.p("sub_style", t) > 0.5;
        QFont fs = f;
        if (sub) fs.setPixelSize(std::max(1, int(std::lround(g.p("size", t, 96) * scale * g.p("sub_scale", t, 60) / 100.0))));
        const QStringList lines = QString::fromStdString(g.s("text", "Title")).split('\n');
        auto fontOf = [&](int i) -> const QFont& { return i > 0 && sub ? fs : f; };

        // Animation: how far in (from the start) and out (towards the end) at this frame.
        fps = fps > 0 ? fps : 30;
        const double sec = double(t) / fps;
        const int inKind = int(std::lround(g.p("anim_in", t))), outKind = int(std::lround(g.p("anim_out", t)));
        const double inDur = std::max(0.01, g.p("anim_in_dur", t, 0.5)), outDur = std::max(0.01, g.p("anim_out_dur", t, 0.5));
        const double pin = inKind ? std::clamp(sec / inDur, 0.0, 1.0) : 1.0;
        const double pout = outKind && duration > 0 ? std::clamp((double(duration - 1 - t)) / fps / outDur, 0.0, 1.0) : 1.0;
        double dx = 0, dy = 0, sc = 1, op = 1, wipe = 1, typed = 1;
        auto animate = [&](int kind, double p, bool entering) {
            const double e = easeOut(p), sign = entering ? 1 : -1;
            switch (kind) {
                case 1: op *= e; break;                                                // fade
                case 2: dy += sign * (1 - e) * 0.12 * h; op *= std::min(1.0, 2 * p); break;   // slide up
                case 3: dy -= sign * (1 - e) * 0.12 * h; op *= std::min(1.0, 2 * p); break;   // slide down
                case 4: dx += sign * (1 - e) * 0.15 * w; op *= std::min(1.0, 2 * p); break;   // slide left
                case 5: dx -= sign * (1 - e) * 0.15 * w; op *= std::min(1.0, 2 * p); break;   // slide right
                case 6: {                                                                       // pop, overshooting a little
                    const double c1 = 1.70158, c3 = c1 + 1, q = std::clamp(p, 0.0, 1.0);
                    const double back = 1 + c3 * std::pow(q - 1, 3) + c1 * std::pow(q - 1, 2);
                    sc *= entering ? 0.6 + 0.4 * back : 0.6 + 0.4 * q;
                    op *= std::min(1.0, 2 * q);
                    break;
                }
                case 7: typed = std::min(typed, p); break;  // typewriter
                case 8: wipe = std::min(wipe, e); break;    // wipe from the left
                default: break;
            }
        };
        animate(inKind, pin, true);
        animate(outKind, pout, false);
        if (op <= 0 || wipe <= 0) return Image(w, h);

        // Layout on the whole text, so nothing moves while it types on.
        std::vector<double> widths, heights, ascents;
        double blockW = 0, totalH = 0;
        for (int i = 0; i < lines.size(); ++i) {
            QFontMetricsF fm(fontOf(i));
            widths.push_back(fm.horizontalAdvance(lines[i]));
            heights.push_back(fm.height());
            ascents.push_back(fm.ascent());
            blockW = std::max(blockW, widths.back());
        }
        const double spacing = g.p("line_spacing", t, 1.15);
        std::vector<double> tops;
        for (int i = 0; i < lines.size(); ++i) {
            tops.push_back(totalH);
            totalH += i + 1 < lines.size() ? heights[size_t(i)] * spacing : heights[size_t(i)];
        }
        const double boxOp = g.p("box_opacity", t) / 100.0;
        const double pad = boxOp > 0 ? g.p("box_padding", t, 24) * scale : 0.0;
        const int barKind = int(std::lround(g.p("bar", t)));
        const double barW = barKind ? g.p("bar_width", t, 8) * scale : 0.0, barGap = barKind == 1 ? barW * 1.5 : 0.0;
        // Placement: Free is Position from the centre; the anchors sit inside the title-safe area (8 %).
        const int anchor = int(std::lround(g.p("anchor", t)));
        const double mx = 0.08 * w, my = 0.08 * h, px = g.p("pos_x", t) * scale, py = g.p("pos_y", t) * scale;
        const double halfW = blockW / 2 + pad, halfH = totalH / 2 + pad + (barKind == 2 ? barW * 2 : 0);
        double cx = w * 0.5 + px, cy = h * 0.5 + py;
        if (anchor == 1 || anchor == 4) cx = mx + barW + barGap + halfW + px;
        if (anchor == 3 || anchor == 5) cx = w - mx - halfW + px;
        if (anchor == 2) cx = w * 0.5 + px;
        if (anchor >= 1 && anchor <= 3) cy = h - my - halfH + py;
        if (anchor == 4 || anchor == 5) cy = my + halfH + py;
        const int align = int(g.p("align", t, 1));
        const double left = cx - blockW / 2, top = cy - totalH / 2;
        // Roll and crawl: from where it starts to where it ends over the clip, at a steady speed between the eases.
        if (const int motion = int(std::lround(g.p("motion", t))); motion > 0 && duration > 1) {
            const double u = std::clamp(double(t) / double(duration - 1), 0.0, 1.0);
            const double a = std::clamp(g.p("motion_ease", t) * fps / double(duration - 1), 0.0, 0.5);
            const double k = a <= 0 ? u
                             : u < a ? u * u / (2 * a * (1 - a))
                             : u > 1 - a ? 1 - (1 - u) * (1 - u) / (2 * a * (1 - a))
                                         : (u - a / 2) / (1 - a);
            const bool startOff = g.p("start_off", t, 1) > 0.5, endOff = g.p("end_off", t, 1) > 0.5;
            if (motion == 1) {
                // Up the frame. Not off screen: where it is laid out if it fits, else its first (last) line inside the safe area.
                const bool fits = 2 * halfH <= h - 2 * my;
                const double from = startOff ? h + halfH : (fits ? cy : my + halfH);
                const double to = endOff ? -halfH : (fits ? cy : h - my - halfH);
                dy += from + (to - from) * k - cy;
            } else {
                const double hw = halfW + (barKind == 1 ? (barW + barGap) / 2 : 0.0);
                const bool fits = 2 * hw <= w - 2 * mx, leftward = motion == 2;
                const double enter = leftward ? w + hw : -hw, exit = leftward ? -hw : w + hw;
                const double from = startOff ? enter : (fits ? cx : (leftward ? mx + hw : w - mx - hw));
                const double to = endOff ? exit : (fits ? cx : (leftward ? w - mx - hw : mx + hw));
                dx += from + (to - from) * k - cx;
            }
        }

        // The text (typed so far), first line and the rest apart; with a text animation (CapCut's and Descript's), each
        // letter, word or line on its own, moved, scaled and faded by how far through its turn it is.
        struct Piece {
            QPainterPath path;
            double opacity = 1;
            bool sub = false;
        };
        std::vector<Piece> pieces;
        int totalChars = 0;
        std::vector<int> lineStart;
        for (const QString& l : lines) lineStart.push_back(totalChars), totalChars += int(l.size());
        int budget = int(std::floor(typed * totalChars + 1e-9));
        const int textAnim = int(std::lround(g.p("text_anim", t)));
        auto lineX = [&](int i) {
            const double lw = widths[size_t(i)];
            return align == 0 ? left : (align == 2 ? left + blockW - lw : cx - lw / 2);
        };
        auto plain = [&] {
            QPainterPath mainPath, subPath;
            for (int i = 0; i < lines.size(); ++i) {
                const QString shown = lines[i].left(std::max(0, budget - lineStart[size_t(i)]));
                if (shown.isEmpty()) continue;
                const double y = top + tops[size_t(i)] + ascents[size_t(i)];
                (i > 0 && sub ? subPath : mainPath).addText(QPointF(lineX(i), y), fontOf(i), shown);
            }
            pieces.clear();
            pieces.push_back({mainPath, 1, false});
            pieces.push_back({subPath, 1, true});
        };
        if (textAnim == 0) {
            plain();
        } else {
            bool settled = true;  // every unit in place: drawn as the plain title is, kerned across the units
            const int by = int(std::lround(g.p("text_anim_by", t)));  // 0 letters, 1 words, 2 lines
            const double across = std::max(0.1, g.p("text_anim_dur", t, 1.0));
            const bool animateOut = g.p("text_anim_out", t) > 0.5;
            struct Unit {
                int line, from, len;
            };
            std::vector<Unit> units;
            for (int i = 0; i < lines.size(); ++i) {
                const QString& ln = lines[i];
                if (by == 2) {
                    if (!ln.trimmed().isEmpty()) units.push_back({i, 0, int(ln.size())});
                    continue;
                }
                for (int k = 0; k < ln.size();) {
                    if (ln[k].isSpace()) {
                        ++k;
                        continue;
                    }
                    int e = k + 1;
                    if (by == 1)
                        while (e < ln.size() && !ln[e].isSpace()) ++e;
                    units.push_back({i, k, e - k});
                    k = e;
                }
            }
            const int n = int(units.size());
            // Each unit takes a share of the time, starting one after another so the last ends `across` in.
            const double unitDur = std::clamp(across * (n > 1 ? 0.45 : 1.0), 0.08, across);
            const double stagger = n > 1 ? (across - unitDur) / (n - 1) : 0;
            static const QString noise = QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#$%&*@");
            for (int u = 0; u < n; ++u) {
                const Unit& un = units[size_t(u)];
                const int typedHere = budget - lineStart[size_t(un.line)] - un.from;
                if (typedHere <= 0) continue;  // not typed on yet
                QString shown = lines[un.line].mid(un.from, std::min(un.len, typedHere));
                const QFont& font = fontOf(un.line);
                const QFontMetricsF fm(font);
                const double x0 = lineX(un.line) + fm.horizontalAdvance(lines[un.line].left(un.from));
                const double y = top + tops[size_t(un.line)] + ascents[size_t(un.line)];
                const double uw = fm.horizontalAdvance(shown), uh = heights[size_t(un.line)];
                double pr = std::clamp((sec - u * stagger) / unitDur, 0.0, 1.0);
                if (animateOut && duration > 0) {
                    const double remaining = double(duration - 1 - t) / fps;  // the last unit leaves last
                    pr = std::min(pr, std::clamp((remaining - (n - 1 - u) * stagger) / unitDur, 0.0, 1.0));
                }
                const double e = easeOut(pr);
                double dyU = 0, scU = 1, opU = 1;
                switch (textAnim) {
                    case 1: dyU = (1 - e) * 0.6 * uh, opU = e; break;  // rise into place
                    case 2: opU = e; break;                             // fade in
                    case 3: {                                           // pop, overshooting a little
                        const double c1 = 1.70158, c3 = c1 + 1;
                        scU = pr <= 0 ? 0 : 1 + c3 * std::pow(pr - 1, 3) + c1 * std::pow(pr - 1, 2);
                        opU = std::min(1.0, 2 * pr);
                        break;
                    }
                    case 4: {  // drop in from above and bounce
                        double b = pr;
                        const double n1 = 7.5625, d1 = 2.75;
                        if (b < 1 / d1) b = n1 * b * b;
                        else if (b < 2 / d1) b -= 1.5 / d1, b = n1 * b * b + 0.75;
                        else if (b < 2.5 / d1) b -= 2.25 / d1, b = n1 * b * b + 0.9375;
                        else b -= 2.625 / d1, b = n1 * b * b + 0.984375;
                        dyU = -(1 - b) * 1.2 * uh;
                        opU = std::min(1.0, 3 * pr);
                        break;
                    }
                    case 5: dyU = 0.12 * uh * std::sin(2 * M_PI * (sec * 0.8 - u * 0.09)); break;  // a wave running through
                    case 6:  // letters shuffling until they settle
                        if (pr < 1)
                            for (int k = 0; k < shown.size(); ++k)
                                if (!shown[k].isSpace()) {
                                    const uint32_t hsh = uint32_t(u + 1) * 73856093u ^ uint32_t(k + 1) * 19349663u ^ uint32_t(t / 2 + 1) * 83492791u;
                                    shown[k] = noise[int(hsh % uint32_t(noise.size()))];
                                }
                        break;
                    default: break;
                }
                if (dyU != 0 || scU != 1 || opU != 1 || (textAnim == 6 && pr < 1)) settled = false;
                if (opU <= 0 || scU <= 0) continue;
                QPainterPath path;
                path.addText(QPointF(x0, y), font, shown);
                const double pcx = x0 + uw / 2, pcy = y - fm.ascent() * 0.35;
                QTransform tr;
                tr.translate(pcx, pcy + dyU);
                tr.scale(scU, scU);
                tr.translate(-pcx, -pcy);
                pieces.push_back({tr.map(path), opU, un.line > 0 && sub});
            }
            if (settled) plain();
        }
        auto col = [&](const char* base, double a) {
            std::string b(base);
            return QColor::fromRgbF(float(std::clamp(g.p(b + ".r", t), 0.0, 1.0)), float(std::clamp(g.p(b + ".g", t), 0.0, 1.0)),
                                    float(std::clamp(g.p(b + ".b", t), 0.0, 1.0)), float(std::clamp(a, 0.0, 1.0)));
        };
        pa.setOpacity(std::clamp(g.p("opacity", t, 100) / 100.0 * op, 0.0, 1.0));
        pa.translate(cx + dx, cy + dy);
        pa.scale(sc, sc);
        pa.translate(-cx, -cy);
        const QRectF block(left - pad, top - pad, blockW + 2 * pad, totalH + 2 * pad);
        if (wipe < 1) {
            const double from = block.left() - barW - barGap;
            pa.setClipRect(QRectF(from, -h, (block.right() - from) * wipe, 3.0 * h));
        }
        if (boxOp > 0 && !lines.isEmpty()) pa.fillRect(block, col("box_color", boxOp));
        if (barKind == 1) pa.fillRect(QRectF(block.left() - barGap - barW, block.top(), barW, block.height()), col("bar_color", 1.0));
        if (barKind == 2) pa.fillRect(QRectF(left, block.bottom() + barW, blockW, barW), col("bar_color", 1.0));
        const double sh = g.p("shadow", t, 4) * scale;
        const double shOp = g.p("shadow_opacity", t, 50) / 100.0;
        if (sh > 0 && shOp > 0)
            for (const Piece& pc : pieces) pa.fillPath(pc.path.translated(sh, sh), QColor::fromRgbF(0, 0, 0, float(shOp * pc.opacity)));
        const double ow = g.p("outline", t) * scale;
        if (ow > 0)
            for (const Piece& pc : pieces) pa.strokePath(pc.path, QPen(col("outline_color", pc.opacity), ow * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const Piece& pc : pieces) pa.fillPath(pc.path, col(pc.sub ? "sub_color" : "color", pc.opacity));
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

Image renderGenerator(const Effect& g, FrameTime t, int w, int h, double scale, FrameTime duration, double fps) {
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
        return renderTitle(g, t, w, h, scale, duration, fps);
    } else if (g.type == "shape") {
        return renderShape(g, t, w, h, scale);
    }
    return img;
}

namespace {

// The picture of a clip whose file is gone: "Media Offline" on red, as the other editors show it.
Frame16Ptr offlineSlate(int w, int h) {
    static std::mutex m;
    static Frame16Ptr last;
    std::lock_guard lock(m);
    if (last && last->width == w && last->height == h) return last;
    QImage qi(std::max(1, w), std::max(1, h), QImage::Format_RGBA8888);
    qi.fill(QColor(0x8c, 0x12, 0x12));
    {
        QPainter pa(&qi);
        pa.setRenderHint(QPainter::TextAntialiasing);
        QFont f;
        f.setPixelSize(std::max(6, qi.height() / 12));
        f.setBold(true);
        pa.setFont(f);
        pa.setPen(Qt::white);
        pa.drawText(qi.rect(), Qt::AlignCenter, QStringLiteral("Media Offline"));
    }
    auto fr = std::make_shared<Frame16>();
    fr->width = qi.width();
    fr->height = qi.height();
    fr->px.resize(size_t(fr->width) * size_t(fr->height) * 4);
    for (int y = 0; y < fr->height; ++y) {
        const uchar* s = qi.constScanLine(y);
        uint16_t* d = fr->px.data() + size_t(y) * size_t(fr->width) * 4;
        for (int x = 0; x < fr->width * 4; ++x) d[x] = uint16_t(s[x] * 257);
    }
    last = fr;
    return last;
}

struct Geometry {
    double mw = 1, mh = 1;  // media logical size (pixels)
    double sx = 1, sy = 1;  // total scale (media px -> sequence px), may be negative for flips
    double rot = 0;         // radians
    double px = 0, py = 0;  // position offset (sequence px)
    double ax = 0, ay = 0;  // anchor offset (media px)
    double cl = 0, cr = 0, ct = 0, cb = 0;  // crop fractions
    double opacity = 1;
};

// Sequence pixels to this image's pixels, for pixel-sized effect parameters.
double effectPixelScale(const Clip& c, FrameTime lt, double sx, double mw, int srcWidth) {
    const double fitX = std::fabs(sx) / std::max(1e-9, std::fabs(c.motion.p("scale", lt, 100) / 100.0 *
                                                                 c.motion.p("scale_x", lt, 100) / 100.0));
    return srcWidth / std::max(1.0, mw * fitX);
}

const Effect* superScaleEffect(const Clip& c) {
    for (const Effect& e : c.effects)
        if (e.type == "super_scale" && e.enabled) return &e;
    return nullptr;
}

// The last few Super Scale results, by what went in (sampled) and the size asked for: a paused or
// repainted frame is not run through the model again.
bool cachedSuperScale(const Image& in, int w, int h, double strength, Image& out) {
    uint64_t key = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ULL; };
    mix(uint64_t(in.width)), mix(uint64_t(in.height)), mix(uint64_t(w)), mix(uint64_t(h)), mix(uint64_t(std::lround(strength * 1000)));
    const size_t step = std::max<size_t>(1, in.px.size() / 8192);
    for (size_t i = 0; i < in.px.size(); i += step) {
        uint32_t bits;
        std::memcpy(&bits, &in.px[i], 4);
        mix(bits);
    }
    static std::mutex m;
    static std::deque<std::pair<uint64_t, std::shared_ptr<const Image>>> cache;
    {
        std::lock_guard lock(m);
        for (const auto& [k, img] : cache)
            if (k == key) {
                out = *img;
                return true;
            }
    }
    Image result;
    if (!superScale(in, w, h, result, strength)) return false;
    auto shared = std::make_shared<const Image>(result);
    std::lock_guard lock(m);
    cache.emplace_front(key, shared);
    if (cache.size() > 2) cache.pop_back();
    out = std::move(result);
    return true;
}

const Effect* denoiseEffect(const Clip& c) {
    for (const Effect& e : c.effects)
        if (e.type == "video_denoise" && e.enabled) return &e;
    return nullptr;
}

const Effect* enabledEffect(const Clip& c, const char* type) {
    for (const Effect& e : c.effects)
        if (e.enabled && e.type == type) return &e;
    return nullptr;
}

// Deflicker's per-frame brightness, kept by media, size and frame: playing on needs one new frame's worth.
bool cachedTileStats(const std::string& key, TileStats& out, const std::function<bool(TileStats&)>& make) {
    static std::mutex m;
    static std::deque<std::pair<std::string, TileStats>> cache;
    {
        std::lock_guard lock(m);
        for (const auto& [k, v] : cache)
            if (k == key) {
                out = v;
                return true;
            }
    }
    if (!make(out)) return false;
    std::lock_guard lock(m);
    cache.emplace_front(key, out);
    if (cache.size() > 256) cache.pop_back();
    return true;
}

// Video Noise Reduction on the source frame, under the effect's mask if it has one.
void applyDenoise(const Effect& e, FrameTime lt, Image& src, const std::vector<Image>& around, const std::vector<MotionFn>& motion,
                  double pixelScale, double sourceSeconds) {
    DenoiseSettings ds;
    ds.motion = e.p("motion", lt, 1) > 0.5;
    ds.temporal = std::max(0.0, e.p("temporal", lt, 1));
    ds.spatialLuma = e.p("luma", lt, 0.25);
    ds.spatialChroma = e.p("chroma", lt, 0.6);
    ds.noise = std::max(0.0, e.p("noise", lt, 0)) / 100;
    ds.blend = e.p("blend", lt, 0);
    std::vector<const Image*> nb;
    for (const Image& im : around) nb.push_back(&im);
    if (!hasMask(e, lt)) {
        src = denoiseFrame(src, nb, ds, motion);
        return;
    }
    const std::vector<float> matte = effectMatte(e, lt, src, pixelScale, sourceSeconds);
    if (e.p("mask.show", lt) > 0.5) {
        for (size_t i = 0; i < matte.size(); ++i) {
            float* q = &src.px[i * 4];
            q[0] = q[1] = q[2] = matte[i] * q[3];
        }
        return;
    }
    const Image clean = denoiseFrame(src, nb, ds, motion);
    for (size_t i = 0; i < matte.size() && i * 4 < src.px.size(); ++i)
        for (int k = 0; k < 4; ++k) src.px[i * 4 + size_t(k)] += (clean.px[i * 4 + size_t(k)] - src.px[i * 4 + size_t(k)]) * matte[i];
}

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

// The clip's transform at clip-local frame lt with its animation presets on top (core/ClipAnimation.h).
Geometry clipGeometry(const Clip& c, FrameTime lt, double mw, double mh, int SW, int SH, double fps) {
    Geometry g = geometryFor(c.motion, lt, mw, mh, SW, SH);
    if (hasClipAnimation(c)) {
        const AnimationPose a = clipAnimationPose(c, double(lt), fps);
        g.px += a.dx * SW;
        g.py += a.dy * SH;
        g.sx *= a.scale;
        g.sy *= a.scale;
        g.rot += a.rotation * M_PI / 180.0;
        g.opacity = std::clamp(g.opacity * a.opacity, 0.0, 1.0);
    }
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

// `below` is what the tracks under the clip have made so far, for adjustment layers.
Image clipLayer(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, const RenderOptions& o,
                const Image* below = nullptr) {
    // Stop Motion: the clip steps, each frame held for a few.
    for (const Effect& e : c.effects)
        if (e.enabled && e.type == "stop_motion") {
            const FrameTime hold = std::max<FrameTime>(1, FrameTime(std::lround(e.p("hold", t - c.start, 3))));
            t -= (t - c.start) % hold;
        }
    const FrameTime lt = t - c.start;
    const int SW = seq.width, SH = seq.height;
    Image src;
    double mw = SW, mh = SH;
    Geometry g;
    double sourceSeconds = -1;  // media time of the frame, for effects that follow the footage
    uint64_t sourceMedia = 0;   // and its media, and whether it was reframed from 360°, for analyses made from it
    bool reframed = false;
    ofx::FrameFetch ofxFetch;   // the clip's source at other clip frames, for OpenFX plugins that ask (render/Ofx.h)
    if (c.isGenerator() && c.generator.type == "adjustment") {
        // An adjustment layer's picture is the composite beneath it (already in the working space).
        if (!below || below->empty()) return {};
        g = clipGeometry(c, lt, SW, SH, SW, SH, seq.fpsValue());
        src = *below;
    } else if (c.isGenerator()) {
        g = clipGeometry(c, lt, SW, SH, SW, SH, seq.fpsValue());
        int w, h;
        sourceSize(g, o.scale, int(SW * 4), int(SH * 4), w, h);
        src = c.generator.type == "audio_viz" ? renderAudioVisualiser(p, seq, c, t, w, h)
                                              : renderGenerator(c.generator, lt, w, h, double(w) / SW, c.duration, seq.fpsValue());
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
            g = clipGeometry(c, lt, mw, mh, SW, SH, seq.fpsValue());
            int w, h;
            sourceSize(g, o.scale, nested->width, nested->height, w, h);
            RenderOptions no = o;
            no.depth = o.depth + 1;
            no.scale = double(w) / nested->width;
            if (nested->multicam) no.soloVideoTrack = std::clamp(c.angle, 0, std::max(0, int(nested->videoTracks.size()) - 1));
            FrameTime nf = FrameTime(std::floor(c.sourceFrameAt(t) * nested->fpsValue() / seq.fpsValue() + 1e-6));
            src = renderSequenceFrame(p, *nested, nf, no);
            if (const Effect* nr = denoiseEffect(c); nr && !src.empty())
                applyDenoise(*nr, lt, src, {}, {}, effectPixelScale(c, lt, g.sx, mw, src.width), -1);
            convertColor(src, sequenceColorSpace(*nested), sequenceColorSpace(seq), seq.hdrPeakNits);
        } else if (m->kind == MediaKind::Video || m->kind == MediaKind::Image) {
            if (!m->hasVideo && m->kind != MediaKind::Image) return {};
            std::string path = (o.useProxies && !m->proxyPath.empty()) ? m->proxyPath : m->path;
            mw = m->width > 0 ? m->width : SW;
            mh = m->height > 0 ? m->height : SH;
            // Reframe 360: the clip is a view, at the sequence's shape, out of the whole sphere.
            const Effect* vr = enabledEffect(c, "reframe_360");
            if (vr) mw = std::max(1.0, std::round(mh * SW / std::max(1, SH)));
            g = clipGeometry(c, lt, mw, mh, SW, SH, seq.fpsValue());
            int w, h;
            sourceSize(g, o.scale, int(mw), int(mh), w, h);
            const int viewW = w, viewH = h;
            const double vrFov = vr ? vr->p("fov", lt, 100) : 0;
            const SphereView vrView = vr ? SphereView(std::clamp(int(vr->p("projection", lt, 0)), 0, 2)) : SphereView::Flat;
            if (vr) {
                // As much of the sphere as the view's pixels need: 360 / fov times its width, up to the footage's own.
                const double across = vrView == SphereView::Flat ? std::clamp(vrFov, 20.0, 170.0) : 180.0;
                const int full = m->width > 0 ? m->width : viewW * 2;
                w = std::clamp(int(std::ceil(viewW * 360.0 / across)), 2, full);
                h = std::max(1, int(std::lround(double(w) * (m->height > 0 ? m->height : full / 2) / full)));
            }
            // Super Scale: shown larger than it was shot, the frame is decoded at its own size and enlarged by
            // the model at the end (not on proxies, which are for speed).
            const Effect* ss = superScaleEffect(c);
            // The size it is shown at (decoding stops at the media's own size), up to four times that.
            const int outW = int(std::ceil(std::clamp(g.mw * std::fabs(g.sx) * o.scale, 1.0, 4.0 * mw)));
            const int outH = int(std::ceil(std::clamp(g.mh * std::fabs(g.sy) * o.scale, 1.0, 4.0 * mh)));
            const bool upscale = ss && !vr && path == m->path && (outW > int(mw) + 1 || outH > int(mh) + 1) &&
                                 ss->p("strength", lt, 100) > 0 && upscalerAvailable() && upscaleModel().installed();
            if (upscale) {
                w = int(mw);
                h = int(mh);
            }
            double sec = 0;
            if (m->kind == MediaKind::Video) {
                sec = c.sourceFrameAt(t) / seq.fpsValue();
                double fd = m->fps.valid() ? 1.0 / m->fps.toDouble() : 1.0 / 30;
                sec = std::clamp(sec, 0.0, std::max(0.0, m->duration - fd * 0.5));
            }
            Frame16Ptr f = MediaPool::instance().videoFrame(path, sec, w, h, o.highQuality);
            if (!f && path == m->path && isOffline(*m)) f = offlineSlate(w, h);
            if (!f) return {};
            if (m->kind == MediaKind::Video) sourceSeconds = sec;
            sourceMedia = m->id;
            if (m->kind == MediaKind::Video && std::any_of(c.effects.begin(), c.effects.end(), [](const Effect& e) { return e.type == "ofx" && e.enabled; })) {
                const double duration = m->duration, frameSeconds = m->fps.valid() ? 1.0 / m->fps.toDouble() : 1.0 / 30, seqFps = seq.fpsValue();
                const bool hq = o.highQuality;
                const Clip clipCopy = c;
                // Neighbouring frames in the same working space as the frame the plugin is given.
                const ColorSpace* from = &mediaColorSpace(*m);
                const ColorSpace* to = &sequenceColorSpace(seq);
                const double peak = seq.hdrPeakNits;
                ofxFetch = [path, w, h, hq, clipCopy, duration, frameSeconds, seqFps, from, to, peak](double at, Image& out) {
                    const FrameTime local = FrameTime(std::floor(at + 1e-6));
                    if (local < 0 || local >= clipCopy.duration) return false;
                    const double s = std::clamp(clipCopy.sourceFrameAt(clipCopy.start + local) / seqFps, 0.0, std::max(0.0, duration - frameSeconds * 0.5));
                    Frame16Ptr frame = MediaPool::instance().videoFrame(path, s, w, h, hq);
                    if (!frame) return false;
                    out = toImage(*frame);
                    convertColor(out, *from, *to, peak);
                    return true;
                };
            }
            src = toImage(*f);
            Image decoded;  // the source frame itself, when src is an in-between
            // Slow motion between two source frames: blend them or follow the motion.
            const int sampling = c.timing.empty() ? 0 : int(c.timing.p("sampling", lt, 0));
            if (sampling > 0 && m->kind == MediaKind::Video && m->fps.valid()) {
                const double mf = m->fps.toDouble(), pos = sec * mf, base = std::floor(pos + 1e-6), frac = pos - base;
                const double next = (base + 1.25) / mf;
                if (frac > 0.02 && frac < 0.98 && next < m->duration) {
                    Frame16Ptr fa = MediaPool::instance().videoFrame(path, (base + 0.25) / mf, w, h, o.highQuality);
                    Frame16Ptr fb = MediaPool::instance().videoFrame(path, next, w, h, o.highQuality);
                    if (fa && fb) {
                        if (denoiseEffect(c)) decoded = src;
                        const Image a = toImage(*fa), b = toImage(*fb);
                        const std::string pair = path + '#' + std::to_string(int64_t(base)) + '@' + std::to_string(w) + 'x' + std::to_string(h);
                        // AI frames when RIFE is here, else optical flow.
                        Image ai;
                        if (sampling == 1) src = blendFrames(a, b, frac);
                        else if (sampling == 3 && rifeAvailable() && rifeModel().installed() && cachedRife(a, b, frac, pair, ai)) src = std::move(ai);
                        else src = interpolateFrames(a, b, frac, pair);
                    }
                }
            }
            // Noise reduction, on the source frames with the frames either side (Resolve's temporal NR).
            if (const Effect* nr = denoiseEffect(c)) {
                std::vector<Image> around;
                std::vector<int> offsets;
                std::vector<MotionFn> motion;
                const int r = std::clamp(int(std::lround(nr->p("frames", lt, 2))), 0, 3);
                if (m->kind == MediaKind::Video && m->fps.valid() && r > 0 && nr->p("temporal", lt, 1) > 0) {
                    const double mf = m->fps.toDouble();
                    const int64_t base = int64_t(std::floor(sec * mf + 1e-6));
                    for (int k = -r; k <= r; ++k) {
                        const double at = (double(base + k) + 0.25) / mf;
                        if (k == 0 || at < 0 || at >= m->duration) continue;
                        if (Frame16Ptr nf = MediaPool::instance().videoFrame(path, at, w, h, o.highQuality)) {
                            around.push_back(toImage(*nf));
                            offsets.push_back(k);
                        }
                    }
                    if (nr->p("motion", lt, 1) > 0.5) {
                        const Image* self = decoded.empty() ? &src : &decoded;
                        motion = chainedMotion(path + '@' + std::to_string(w) + 'x' + std::to_string(h), base, offsets,
                                               [&](int64_t i) -> const Image* {
                                                   if (i == base) return self;
                                                   for (size_t j = 0; j < offsets.size(); ++j)
                                                       if (base + offsets[j] == i) return &around[j];
                                                   return nullptr;
                                               });
                    }
                }
                applyDenoise(*nr, lt, src, around, motion, effectPixelScale(c, lt, g.sx, mw, src.width),
                             m->kind == MediaKind::Video ? sec : -1);
            }
            // Motion Blur and Deflicker, from the source frames either side.
            const Effect* blur = enabledEffect(c, "motion_blur");
            const Effect* flicker = enabledEffect(c, "deflicker");
            if ((blur || flicker) && m->kind == MediaKind::Video && m->fps.valid()) {
                const double mf = m->fps.toDouble();
                const int64_t base = int64_t(std::floor(sec * mf + 1e-6));
                const std::string key = path + '@' + std::to_string(w) + 'x' + std::to_string(h);
                auto fetch = [&](int64_t i, Image& into) {
                    const double at = (double(i) + 0.25) / mf;
                    if (at < 0 || at >= m->duration) return false;
                    Frame16Ptr nf = MediaPool::instance().videoFrame(path, at, w, h, o.highQuality);
                    if (nf) into = toImage(*nf);
                    return bool(nf);
                };
                if (blur) {
                    const Image self = decoded.empty() ? src : decoded;
                    Image prev, next;
                    int havePrev = -1, haveNext = -1;  // not tried yet
                    const std::vector<MotionFn> motion = chainedMotion(key, base, {-1, 1}, [&](int64_t i) -> const Image* {
                        if (i == base) return &self;
                        if (i == base - 1) {
                            if (havePrev < 0) havePrev = fetch(i, prev);
                            return havePrev ? &prev : nullptr;
                        }
                        if (i == base + 1) {
                            if (haveNext < 0) haveNext = fetch(i, next);
                            return haveNext ? &next : nullptr;
                        }
                        return nullptr;
                    });
                    src = motionBlur(src, motion[0], motion[1], blur->p("shutter", lt, 180) / 360);
                }
                if (flicker) {
                    const int r = std::clamp(int(std::lround(flicker->p("frames", lt, 3))), 1, 12);
                    const bool local = flicker->p("area", lt, 1) > 0.5;
                    const int gw = local ? 16 : 1, gh = local ? 9 : 1;
                    std::vector<TileStats> around;
                    std::vector<double> weights;
                    for (int k = -r; k <= r; ++k) {
                        if (k == 0) continue;
                        TileStats st;
                        const std::string id = key + '#' + std::to_string(base + k) + '#' + std::to_string(gw);
                        if (!cachedTileStats(id, st, [&](TileStats& made) {
                                Image f;
                                if (!fetch(base + k, f)) return false;
                                made = tileStats(f, gw, gh);
                                return true;
                            }))
                            continue;
                        around.push_back(std::move(st));
                        // Half weight at the ends, so flicker that alternates frame to frame cancels exactly.
                        weights.push_back(std::abs(k) == r ? 0.5 : 1.0);
                    }
                    deflicker(src, tileStats(src, gw, gh), 1.0, around, weights, flicker->p("strength", lt, 100) / 100);
                }
            }
            if (upscale) {
                Image big;
                src = cachedSuperScale(src, outW, outH, ss->p("strength", lt, 100) / 100, big) ? std::move(big)
                                                                                                : resizeImage(src, outW, outH);
            }
            if (vr) reframed = true;
            if (vr)
                src = reframeEquirect(src, vr->p("yaw", lt, 0), vr->p("pitch", lt, 0), vr->p("roll", lt, 0), vrFov, vrView, viewW, viewH);
            // Input transform: the media's space into the sequence's working space.
            convertColor(src, mediaColorSpace(*m), sequenceColorSpace(seq), seq.hdrPeakNits);
        } else {
            return {};
        }
    }
    if (src.empty()) return {};
    // Filters run in source space (before the fixed transform), like most NLEs.
    const double pixelScale = effectPixelScale(c, lt, g.sx, mw, src.width);
    // Its colour group's pre-clip grade, its own effects, then the group's post-clip grade (core/ColorGroups.h).
    const std::vector<const Effect*> chain = gradeChain(seq, c);
    // The picture's depth, once, for the effects that work by distance.
    std::shared_ptr<const DepthMap> depth;
    if (std::any_of(chain.begin(), chain.end(), [&](const Effect* e) { return needsDepth(*e, lt); }) && depthAvailable() &&
        depthModel().installed())
        depth = cachedDepth(src);
    DepthScope depthScope(depth);
    // And the people in it, for Remove Background and People masks.
    std::shared_ptr<const ValueMap> people;
    if (std::any_of(chain.begin(), chain.end(), [&](const Effect* e) { return needsPersonMatte(*e, lt); }) && mattingAvailable() &&
        mattingModel().installed())
        people = cachedPersonMatte(src);
    PersonScope personScope(people);
    // And the faces, for Face Refinement, Blemish Remover and Redact Faces.
    TrackedSourceScope trackedSource(sourceMedia, reframed);
    std::shared_ptr<const std::vector<FaceBox>> faces;
    // (Redact Faces only where its analysis does not reach.)
    if (std::any_of(chain.begin(), chain.end(),
                    [&](const Effect* e) { return e->type == "redact_faces" ? redactNeedsLiveFaces(*e, sourceSeconds) : needsFaces(*e); }) &&
        faceSearchAvailable() &&
        faceModel().installed())
        faces = cachedFaces(src);
    FaceScope faceScope(faces);
    ofx::FetchScope fetchScope(ofxFetch);
    // Faces are covered first, where they are in the source, whatever moves the picture after (Stabilize, transforms).
    for (const Effect* e : chain)
        if (e->type == "redact_faces") applyVideoEffect(*e, lt, src, pixelScale, sourceSeconds);
    for (const Effect* e : chain)
        if (e->type != "video_denoise" && e->type != "super_scale" && e->type != "reframe_360" && e->type != "redact_faces")  // those ran already
            applyVideoEffect(*e, lt, src, pixelScale, sourceSeconds);
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

namespace {

const Clip* clipAt(const Track& track, FrameTime t) {
    for (const auto& c : track.clips) {
        if (c.start > t) break;
        if (c.contains(t) && c.enabled) return &c;
    }
    return nullptr;
}

const Effect* trackMatteOf(const Clip& c) {
    for (const Effect& e : c.effects)
        if (e.enabled && e.type == "track_matte") return &e;
    return nullptr;
}

// The track a clip on track `ti` takes its matte from, or -1.
int matteTrack(const Sequence& seq, size_t ti, const Clip& c, const Effect& e, FrameTime t) {
    const int v = int(std::lround(e.p("track", t - c.start, 0)));
    const int m = v <= 0 ? int(ti) + 1 : v - 1;
    return m >= 0 && m < int(seq.videoTracks.size()) && m != int(ti) ? m : -1;
}

// `layer` (premultiplied) shown through the matte's alpha or luma, or the reverse.
void applyTrackMatte(Image& layer, const Image& matte, bool luma, bool reverse) {
    parallelRows(layer.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < layer.width; ++x) {
                float m = 0;
                if (x < matte.width && y < matte.height) {
                    const float* q = matte.at(x, y);
                    m = luma ? std::clamp(0.2126f * q[0] + 0.7152f * q[1] + 0.0722f * q[2], 0.0f, 1.0f) : q[3];
                }
                if (reverse) m = 1 - m;
                float* p = layer.at(x, y);
                for (int k = 0; k < 4; ++k) p[k] *= m;
            }
    });
}

}  // namespace

Image renderSequenceFrame(const Project& p, const Sequence& seq, FrameTime t, const RenderOptions& opts) {
    const WorkingSpaceScope working(&sequenceColorSpace(seq));  // effects that work in light know whose
    const int solo = opts.soloVideoTrack;
    RenderOptions o = opts;
    o.soloVideoTrack = -1;
    int W = std::max(1, int(std::lround(seq.width * o.scale))), H = std::max(1, int(std::lround(seq.height * o.scale)));
    Image canvas(W, H);
    // Tracks a Track Matte Key is using now and hiding (the matte is not seen itself).
    std::vector<bool> hiddenMatte(seq.videoTracks.size(), false);
    for (size_t ti = 0; ti < seq.videoTracks.size(); ++ti)
        if (const Clip* c = clipAt(seq.videoTracks[ti], t))
            if (const Effect* e = trackMatteOf(*c); e && e->p("hide", t - c->start, 1) > 0.5)
                if (const int m = matteTrack(seq, ti, *c, *e, t); m >= 0) hiddenMatte[size_t(m)] = true;
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
        if (solo < 0 && hiddenMatte[ti]) continue;
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
            const Image* below = canvasEmpty ? nullptr : &canvas;
            Image mixed;
            if (active->type == "smooth_cut" && A && A->enabled && B && B->enabled) {
                // The first frame of the transition morphs into its last along their optical flow
                // (the pair is the same throughout, so its flow is measured once).
                Image la = clipLayer(p, seq, *A, from, o, below), lb = clipLayer(p, seq, *B, to - 1, o, below);
                if (la.width == W && la.height == H && lb.width == W && lb.height == H)
                    mixed = interpolateFrames(la, lb, u,
                                              "smooth#" + std::to_string(active->id) + '@' + std::to_string(from) + '-' +
                                                  std::to_string(to) + '#' + std::to_string(W) + 'x' + std::to_string(H));
                else
                    mixed = transitionMix("cross_dissolve", active->params, la, lb, u, W, H);
            } else {
                Image la = (A && A->enabled) ? clipLayer(p, seq, *A, t, o, below) : Image();
                Image lb = (B && B->enabled) ? clipLayer(p, seq, *B, t, o, below) : Image();
                mixed = transitionMix(active->type, active->params, la, lb, u, W, H);
            }
            const Clip* top = B ? B : A;
            composite(std::move(mixed), top ? top->blendMode : "normal");
            continue;
        }
        if (const Clip* c = clipAt(track, t)) {
            Image layer = clipLayer(p, seq, *c, t, o, canvasEmpty ? nullptr : &canvas);
            if (const Effect* e = trackMatteOf(*c); e && !layer.empty()) {
                // With no track to use the key does nothing; with nothing on that track now, the matte is empty.
                if (const int m = matteTrack(seq, ti, *c, *e, t); m >= 0) {
                    const FrameTime lt = t - c->start;
                    Image matte;
                    if (const Clip* mc = clipAt(seq.videoTracks[size_t(m)], t)) matte = clipLayer(p, seq, *mc, t, o, nullptr);
                    applyTrackMatte(layer, matte, e->p("composite", lt, 0) > 0.5, e->p("reverse", lt, 0) > 0.5);
                }
            }
            // Behind People: the layer goes behind whoever is in the picture beneath it.
            if (const Effect* bp = enabledEffect(*c, "behind_people"); bp && !layer.empty() && !canvasEmpty && mattingAvailable() &&
                                                                        mattingModel().installed())
                if (std::shared_ptr<const ValueMap> people = cachedPersonMatte(canvas)) {
                    const FrameTime lt = t - c->start;
                    std::vector<float> m = people->resized(layer.width, layer.height);
                    const double soften = bp->p("soften", lt, 1) * o.scale;
                    refineMatte(m, layer.width, layer.height, bp->p("shift", lt, 1) * o.scale, soften, soften);
                    const float amount = float(std::clamp(bp->p("amount", lt, 100) / 100, 0.0, 1.0));
                    for (size_t i = 0; i < m.size() && i * 4 < layer.px.size(); ++i)
                        for (int k = 0; k < 4; ++k) layer.px[i * 4 + size_t(k)] *= 1 - m[i] * amount;
                }
            composite(std::move(layer), c->blendMode);
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
            // Reframe 360: a view at the sequence's shape (as clipLayer draws it).
            if (enabledEffect(c, "reframe_360")) mw = std::max(1.0, std::round(mh * seq.width / std::max(1, seq.height)));
        }
    }
    g = clipGeometry(c, t - c.start, mw, mh, seq.width, seq.height, seq.fpsValue());
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
    const int anim = std::clamp(st.animation, 0, 4);
    const int spoken = anim ? captionWordAt(*cap, t) : -1;  // the word being said
    QFont f(QString::fromStdString(st.font));
    // One word at a time is shown large.
    const double px = std::max(4.0, st.size * img.height * (anim == 4 ? 1.8 : 1.0));
    f.setPixelSize(int(std::lround(px)));
    f.setBold(st.bold || anim == 4);
    f.setHintingPreference(QFont::PreferNoHinting);
    const QFontMetricsF fm(f);
    // The words of each line, numbered through the caption.
    struct Word {
        QString text;
        int line = 0, index = 0;
        double x = 0, w = 0;  // within the line
    };
    std::vector<Word> words;
    QStringList lines = (st.allCaps ? QString::fromStdString(cap->text).toUpper() : QString::fromStdString(cap->text)).split('\n');
    {
        int index = 0;
        for (int li = 0; li < lines.size(); ++li) {
            int pos = 0;
            const QString& line = lines[li];
            while (pos < line.size()) {
                while (pos < line.size() && line[pos].isSpace()) ++pos;
                if (pos >= line.size()) break;
                int end = pos;
                while (end < line.size() && !line[end].isSpace()) ++end;
                const QString word = line.mid(pos, end - pos);
                words.push_back({word, li, index++, fm.horizontalAdvance(line.left(pos)), fm.horizontalAdvance(word)});
                pos = end;
            }
        }
    }
    if (anim == 4) {
        // Only the word being said, on a line of its own.
        const Word* w = nullptr;
        for (const Word& x : words)
            if (x.index == spoken) w = &x;
        if (!w) return;
        lines = QStringList{w->text};
        words = {{w->text, 0, w->index, 0, w->w}};
    }
    const double lineH = fm.height() * 1.1, padX = px * 0.3, padY = px * 0.08;
    double blockW = 0;
    for (const QString& l : lines) blockW = std::max(blockW, fm.horizontalAdvance(l));
    const double blockH = lineH * double(lines.size());
    // Where the style puts captions, or at the top (as far from it as the style is from the bottom),
    // or in the middle; centred, or lined up inside the graphics-safe margins (EBU R95: 5 %).
    const double fromBottom = std::clamp(st.position, 0.05, 1.0) * img.height;
    const double bottom = cap->vertical == kCaptionTop      ? std::min(img.height - fromBottom, img.height / 2.0 - blockH / 2) + blockH
                          : cap->vertical == kCaptionMiddle ? (img.height + blockH) / 2
                                                            : fromBottom;
    const double margin = img.width * 0.05;
    auto leftOf = [&](double w) {
        return cap->align == kCaptionLeft ? margin : cap->align == kCaptionRight ? img.width - margin - w : img.width / 2.0 - w / 2;
    };
    const double grow = (anim == 3 ? 0.25 : 0.0) + std::max(0.0, st.shadow) + std::max(0.0, st.outline);  // room for a popped word, shadow, outline
    // Render only the caption's area, then blend it over the frame.
    const int x0 = std::max(0, int(std::floor(leftOf(blockW) - padX - 2 - grow * px)));
    const int x1 = std::min(img.width, int(std::ceil(leftOf(blockW) + blockW + padX + 2 + grow * px)));
    const int y0 = std::max(0, int(std::floor(bottom - blockH - padY - 2 - grow * px)));
    const int y1 = std::min(img.height, int(std::ceil(bottom + padY + 2 + grow * px)));
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
        QPainterPath text, said;
        for (int li = 0; li < lines.size(); ++li) {
            const double lw = fm.horizontalAdvance(lines[li]);
            const double left = leftOf(lw);
            const double top = bottom - blockH + li * lineH;
            const double baseline = top + (lineH - fm.height()) / 2 + fm.ascent();
            // Word by word: only what has been said so far, its box growing with it.
            double shownW = lw;
            if (anim == 1) {
                shownW = 0;
                for (const Word& w : words)
                    if (w.line == li && w.index <= spoken) shownW = w.x + w.w;
            }
            if (st.boxOpacity > 0 && !lines[li].isEmpty() && shownW > 0)
                pa.fillRect(QRectF(left - padX, top - padY, shownW + 2 * padX, lineH + 2 * padY), col(st.boxR, st.boxG, st.boxB, st.boxOpacity));
            if (anim == 0) {
                text.addText(QPointF(left, baseline), f, lines[li]);
                continue;
            }
            for (const Word& w : words) {
                if (w.line != li || (anim == 1 && w.index > spoken)) continue;
                if (w.index == spoken && anim >= 2) {
                    QPainterPath p;
                    p.addText(QPointF(left + w.x, baseline), f, w.text);
                    if (anim == 3) {
                        // Pops up over its first few frames.
                        const auto starts = captionWordStarts(*cap);
                        const double since = (double(t - cap->start) - starts[size_t(w.index)] * double(cap->end - cap->start));
                        const double sc = 1 + grow * std::clamp(since / 3.0 + 0.34, 0.0, 1.0);
                        const QPointF c(left + w.x + w.w / 2, top + lineH / 2);
                        QTransform tr;
                        tr.translate(c.x(), c.y());
                        tr.scale(sc, sc);
                        tr.translate(-c.x(), -c.y());
                        p = tr.map(p);
                    }
                    said.addPath(p);
                } else {
                    text.addText(QPointF(left + w.x, baseline), f, w.text);
                }
            }
        }
        const double outlineWidth = std::max(st.outline, anim == 4 ? 0.06 : 0.0) * px * 2;
        if (st.shadow > 0) {
            // A soft-edged drop shadow down and to the right, under the outline too.
            const QPointF off(st.shadow * px, st.shadow * px);
            QPainterPath both = text;
            both.addPath(said);
            both.translate(off);
            const QColor sc = col(0, 0, 0, st.shadowOpacity);
            if (outlineWidth > 0) pa.strokePath(both, QPen(sc, outlineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            pa.fillPath(both, sc);
        }
        if (outlineWidth > 0) {
            const QPen pen(col(st.outlineR, st.outlineG, st.outlineB, 1), outlineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            pa.strokePath(text, pen);
            pa.strokePath(said, pen);
        }
        pa.fillPath(text, col(st.textR, st.textG, st.textB, 1));
        pa.fillPath(said, col(st.hiR, st.hiG, st.hiB, 1));
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

Image colourReferenceFrame(const Project& p, const Sequence& s, FrameTime t) {
    RenderOptions o;
    o.scale = std::min(1.0, 480.0 / std::max(1, s.width));
    o.displaySpace = "rec709";  // as renderMediaFrame gives the clips' own frames
    return renderProgramFrame(p, s, std::clamp<FrameTime>(t, 0, std::max<FrameTime>(0, s.duration() - 1)), o);
}

bool keyScreen(Project& p, Sequence& s, Id clip, FrameTime at, std::string* error) {
    Clip* c = edit::clipById(s, clip);
    const MediaItem* m = c && c->mediaId ? p.findMedia(c->mediaId) : nullptr;
    if (!m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image)) {
        if (error) *error = "Keying needs a video or picture clip";
        return false;
    }
    const FrameTime t = c->contains(at) ? at : c->start + c->duration / 2;
    const double sec = m->kind == MediaKind::Video ? std::max(0.0, c->sourceFrameAt(t) / s.fpsValue()) : 0.0;
    double rgb[3];
    if (!estimateScreenColor(renderMediaFrame(p, *m, sec, 480, 270), rgb)) {
        if (error) *error = "No green or blue screen stands out in the clip";
        return false;
    }
    auto it = std::find_if(c->effects.begin(), c->effects.end(), [](const Effect& e) { return e.type == "screen_key"; });
    if (it == c->effects.end()) it = c->effects.insert(c->effects.begin(), makeEffect(p, "screen_key"));
    it->params["key.r"] = Param(rgb[0]);
    it->params["key.g"] = Param(rgb[1]);
    it->params["key.b"] = Param(rgb[2]);
    return true;
}

int matchClipColour(Project& p, Sequence& s, const std::vector<Id>& clips, const Image& reference, FrameTime at) {
    if (reference.empty()) return 0;
    int n = 0;
    for (Id id : clips) {
        Clip* c = edit::clipById(s, id);
        const MediaItem* m = c && c->mediaId ? p.findMedia(c->mediaId) : nullptr;
        if (!m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image)) continue;
        const FrameTime t = c->contains(at) ? at : c->start + c->duration / 2;
        const double sec = m->kind == MediaKind::Video ? std::max(0.0, c->sourceFrameAt(t) / s.fpsValue()) : 0.0;
        Effect e = colorMatchCorrection(renderMediaFrame(p, *m, sec, 480, 270), reference, p.newId());
        e.strings["match"] = "1";
        auto it = std::find_if(c->effects.begin(), c->effects.end(),
                               [](const Effect& x) { return x.type == "color_correct" && x.strings.count("match"); });
        if (it != c->effects.end()) {
            e.id = it->id;
            *it = e;
        } else {
            c->effects.insert(c->effects.begin(), e);
        }
        ++n;
    }
    return n;
}

namespace {

// Blends a premultiplied 8-bit patch over the float frame at (x0, y0), its
// SDR colours first moved into the frame's space.
void blendPatch(Image& img, const QImage& qi, int x0, int y0, const ColorSpace* space, float opacity = 1.0f) {
    Image patch(qi.width(), qi.height());
    for (int y = 0; y < patch.height; ++y) {
        const uchar* s = qi.constScanLine(y);
        float* d = patch.row(y);
        for (int x = 0; x < patch.width * 4; ++x) d[x] = s[x] / 255.0f * opacity;
    }
    if (space && space->id != rec709Space().id) convertColor(patch, rec709Space(), *space);
    for (int y = 0; y < patch.height; ++y) {
        const int ty = y0 + y;
        if (ty < 0 || ty >= img.height) continue;
        const float* s = patch.row(y);
        for (int x = 0; x < patch.width; ++x, s += 4) {
            const int tx = x0 + x;
            if (tx < 0 || tx >= img.width) continue;
            float* d = img.at(tx, ty);
            for (int c = 0; c < 4; ++c) d[c] = s[c] + d[c] * (1 - s[3]);
        }
    }
}

// Where a w x h block sits for corner c (0-2 top left/centre/right, 3-5 bottom), inside a 3 % margin.
QPoint cornerPos(int c, int w, int h, int W, int H) {
    const int mx = int(std::lround(0.03 * W)), my = int(std::lround(0.03 * H));
    const int col = c % 3, row = c / 3;
    const int x = col == 0 ? mx : col == 1 ? (W - w) / 2 : W - mx - w;
    const int y = row == 0 ? my : H - my - h;
    return {x, y};
}

}  // namespace

void drawBurnIns(Image& img, const Project& p, const Sequence& seq, FrameTime t, const BurnIn& b, const QImage* watermark,
                 const ColorSpace* space) {
    if (img.width < 8 || img.height < 8) return;
    // The logo first, so the text stays readable over it.
    if (watermark && !watermark->isNull() && b.watermarkOpacity > 0) {
        const int ww = std::max(1, int(std::lround(std::clamp(b.watermarkWidth, 0.01, 1.0) * img.width)));
        const QImage logo = watermark->scaledToWidth(ww, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        const QPoint at = cornerPos(std::clamp(b.watermarkCorner, 0, 5), logo.width(), logo.height(), img.width, img.height);
        blendPatch(img, logo, at.x(), at.y(), space, float(std::clamp(b.watermarkOpacity, 0.0, 1.0)));
    }
    QStringList lines;
    if (b.timecode) lines << QString::fromStdString(formatTimecode(t, seq.fps));
    if (b.clipName) {
        for (int i = int(seq.videoTracks.size()) - 1; i >= 0; --i) {
            if (seq.videoTracks[size_t(i)].muted) continue;
            if (const Clip* c = edit::clipAt(seq, TrackRef{TrackKind::Video, i}, t); c && c->enabled) {
                std::string name = c->name;
                if (name.empty())
                    if (const MediaItem* m = c->mediaId ? p.findMedia(c->mediaId) : nullptr) name = m->name;
                lines << QString::fromStdString(name);
                break;
            }
        }
    }
    if (!b.text.empty()) lines << QString::fromStdString(b.text);
    if (lines.isEmpty()) return;
    QFont f(QStringLiteral("Monospace"));
    f.setStyleHint(QFont::TypeWriter);
    f.setPixelSize(std::max(6, int(std::lround(std::clamp(b.size, 0.01, 0.2) * img.height))));
    f.setBold(true);
    const QFontMetrics fm(f);
    const int pad = std::max(2, fm.height() / 4);
    int w = 0;
    for (const QString& l : lines) w = std::max(w, fm.horizontalAdvance(l));
    const int lineH = fm.height();
    const int bw = w + 2 * pad, bh = lineH * int(lines.size()) + 2 * pad;
    QImage qi(bw, bh, QImage::Format_RGBA8888_Premultiplied);
    qi.fill(QColor(0, 0, 0, 160));  // a dark box behind white text, readable on any picture
    {
        QPainter pa(&qi);
        pa.setRenderHint(QPainter::TextAntialiasing);
        pa.setFont(f);
        pa.setPen(Qt::white);
        const int corner = std::clamp(b.corner, 0, 5), col = corner % 3;
        for (int i = 0; i < lines.size(); ++i) {
            const int lw = fm.horizontalAdvance(lines[i]);
            const int x = col == 0 ? pad : col == 1 ? (bw - lw) / 2 : bw - pad - lw;
            pa.drawText(x, pad + i * lineH + fm.ascent(), lines[i]);
        }
    }
    const QPoint at = cornerPos(std::clamp(b.corner, 0, 5), bw, bh, img.width, img.height);
    blendPatch(img, qi, at.x(), at.y(), space);
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
    fx::ParametricEq eq;
    fx::DeEsser deesser;
    fx::NoiseGate gate;
    fx::Reverb reverb;
    fx::DeHum dehum;
    fx::MultibandCompressor multiband;
    fx::ModDelay modDelay;
    fx::Phaser phaser;
    fx::Tremolo tremolo;
    fx::Saturator saturator;
    fx::StereoWidth width;
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
        } else if (e.type == "parametric_eq") {
            if (changed) {
                auto band = [&](const char* n, double hz, bool q) {
                    const std::string b(n);
                    return fx::EqBand{e.p(b + "_hz", lt, hz), e.p(b + "_db", lt, 0), q ? e.p(b + "_q", lt, 1) : 1.0};
                };
                st->eq.set(sr, band("low", 100, false), band("b1", 250, true), band("b2", 1000, true), band("b3", 4000, true),
                           band("high", 10000, false), e.p("output_db", lt, 0));
            }
            st->eq.process(buf, frames);
        } else if (e.type == "deesser") {
            st->deesser.process(buf, frames, sr, e.p("hz", lt, 6000), e.p("threshold_db", lt, -30), e.p("reduction_db", lt, 10));
        } else if (e.type == "gate") {
            st->gate.process(buf, frames, sr, e.p("threshold_db", lt, -45), e.p("range_db", lt, -40), e.p("attack_ms", lt, 1),
                             e.p("hold_ms", lt, 50), e.p("release_ms", lt, 150));
        } else if (e.type == "reverb") {
            st->reverb.process(buf, frames, sr, e.p("size", lt, 50) / 100, e.p("damping", lt, 50) / 100, e.p("width", lt, 100) / 100,
                               e.p("mix", lt, 25) / 100);
        } else if (e.type == "multiband") {
            fx::BandSettings b[3];
            const char* names[3] = {"low", "mid", "high"};
            const double ratios[3] = {3, 2.5, 3};
            for (int k = 0; k < 3; ++k) {
                const std::string n(names[k]);
                b[k] = {e.p(n + "_threshold_db", lt, -24), e.p(n + "_ratio", lt, ratios[k]), e.p(n + "_gain_db", lt, 0)};
            }
            st->multiband.process(buf, frames, sr, e.p("low_hz", lt, 200), e.p("high_hz", lt, 2500), b, e.p("attack_ms", lt, 10),
                                  e.p("release_ms", lt, 150), e.p("output_db", lt, 0));
        } else if (e.type == "dehum") {
            st->dehum.process(buf, frames, sr, e.p("mains", lt, 0) > 0.5 ? 60 : 50, int(std::lround(e.p("harmonics", lt, 6))),
                              e.p("reduction_db", lt, 30), e.p("width_hz", lt, 2));
        } else if (e.type == "chorus" || e.type == "flanger") {
            const bool chorus = e.type == "chorus";
            st->modDelay.process(buf, frames, sr, e.p("delay_ms", lt, chorus ? 15 : 1), e.p("depth_ms", lt, chorus ? 3 : 2),
                                 e.p("rate", lt, chorus ? 0.8 : 0.25), chorus ? 0.0 : e.p("feedback", lt, 50) / 100,
                                 e.p("spread", lt, chorus ? 100 : 50) / 100, e.p("mix", lt, 50) / 100);
        } else if (e.type == "phaser") {
            static const int kStages[4] = {4, 6, 8, 12};
            st->phaser.process(buf, frames, sr, kStages[std::clamp(int(std::lround(e.p("stages", lt, 0))), 0, 3)], e.p("low_hz", lt, 300),
                               e.p("high_hz", lt, 3000), e.p("rate", lt, 0.4), e.p("feedback", lt, 30) / 100, e.p("spread", lt, 50) / 100,
                               e.p("mix", lt, 50) / 100);
        } else if (e.type == "tremolo") {
            st->tremolo.process(buf, frames, sr, e.p("rate", lt, 5), e.p("depth", lt, 50) / 100, int(std::lround(e.p("shape", lt, 0))),
                                e.p("mode", lt, 0) > 0.5);
        } else if (e.type == "saturation") {
            st->saturator.process(buf, frames, sr, int(std::lround(e.p("type", lt, 0))), e.p("drive_db", lt, 6), e.p("tone", lt, 0) / 100,
                                  e.p("mix", lt, 100) / 100, e.p("output_db", lt, 0));
        } else if (e.type == "stereo_width") {
            st->width.process(buf, frames, sr, e.p("width", lt, 100) / 100, e.p("bass_mono_hz", lt, 0));
        } else if (e.type == "channels") {
            fx::channelTools(buf, frames, int(e.p("mode", lt, 0)), e.p("invert_l", lt) > 0.5, e.p("invert_r", lt) > 0.5);
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
    const int n = layoutChannels(seq.audioLayout);
    if (n == 2) {
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
        return;
    }
    // A surround mix, folded down to stereo for listening.
    std::vector<float> full(size_t(frames) * size_t(n));
    {
        std::lock_guard lock(m_);
        if (start != nextStart_) {
            resetLocked();
            if (const int pre = maxLatency(seq, seq.sampleRate); pre > 0) {
                std::vector<float> scratch(size_t(pre) * size_t(n));
                mixInto(p, seq, start - pre, pre, scratch.data(), nullptr, 0, 0, -1, n);
            }
        }
        nextStart_ = start + frames;
        mixInto(p, seq, start, frames, full.data(), trackLevels, 0, 0, -1, n);
    }
    downmixToStereo(seq.audioLayout, full.data(), frames, out);
}

void AudioMixer::mixLayout(const Project& p, const Sequence& seq, int64_t start, int frames, float* out) {
    const int n = layoutChannels(seq.audioLayout);
    std::lock_guard lock(m_);
    if (start != nextStart_) {
        resetLocked();
        if (const int pre = maxLatency(seq, seq.sampleRate); pre > 0) {
            std::vector<float> scratch(size_t(pre) * size_t(n));
            mixInto(p, seq, start - pre, pre, scratch.data(), nullptr, 0, 0, -1, n);
        }
    }
    nextStart_ = start + frames;
    mixInto(p, seq, start, frames, out, nullptr, 0, 0, -1, n);
}

bool AudioMixer::mixTrackClips(const Project& p, const Sequence& seq, const Track& track, int64_t start, int frames,
                               double sr, int depth, float* trackBuf) {
    const double fps = seq.fpsValue();
    const int64_t end = start + frames;
    std::vector<float> clipBuf(size_t(frames) * 2);
    bool any = false;
    for (const Clip& c : track.clips) {
        if (!c.enabled || edit::roleMuted(seq, c.role)) continue;
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
            const std::string source = audioKey(m->path, c.channels);  // the file, or the channels the clip plays
            AudioBufferPtr buf = nonBlocking_ ? MediaPool::instance().audioIfReady(source, int(sr))
                                              : MediaPool::instance().audio(source, int(sr));
            if (!buf || buf->samples.empty()) continue;
            // Noise reduction and voice isolation work on the whole source (the
            // original plays until the cleaned copy is ready in real time).
            std::vector<const Effect*> sourceFx;
            for (const Effect& e : c.effects)
                if (e.enabled && isSourceAudioEffect(e.type)) sourceFx.push_back(&e);
            if (!sourceFx.empty())
                if (AudioBufferPtr clean = cleanedAudio(source, buf, sourceFx, !nonBlocking_)) buf = clean;
            const int64_t n = buf->frames();
            const float* src = buf->samples.data();
            // Maintain Audio Pitch: the clip's sound stretched along its time map (WSOLA), read sample for sample;
            // while that is being made for playback, the resampled sound plays.
            AudioBufferPtr stretched;
            if (!c.reverse && c.timing.p("maintain_pitch", 0) > 0.5 && (ramped || std::fabs(c.speed - 1) > 1e-9)) {
                const int hop = stretchHop(int(sr));
                // Named by the sound (cleaned or not) and everything the time map depends on.
                char head[160];
                std::snprintf(head, sizeof head, "|%g|%p|%lld|%.9g|%.9g|", double(sr), static_cast<const void*>(buf.get()), (long long)n,
                              double(c.sourceIn), c.speed);
                const std::string key = m->path + head + (ramped ? effectToJsonString(c.timing) : std::string());
                stretched = stretchedAudio(key, buf, [&] {
                    std::vector<double> positions;
                    for (int64_t j = 0; j <= (ce - cs) / hop + 2; ++j) {
                        const double local = double(j * hop);
                        positions.push_back(ramped ? (c.sourceIn + c.sourceOffset(local * fps / sr)) * sr / fps : srcBase + local * c.speed);
                    }
                    return positions;
                }, hop, ce - cs, !nonBlocking_);
            }
            // Bleeps: stretches of source (as samples) covered by a tone or silence, with 5 ms ramps.
            std::vector<std::pair<double, double>> bleeps;
            bool bleepTone = true;
            double bleepFreq = 1000;
            float bleepLevel = 0.25f;
            for (const Effect& e : c.effects)
                if (e.enabled && e.type == "bleep") {
                    for (const auto& [a, b] : bleepRanges(e)) bleeps.push_back({a * sr, b * sr});
                    bleepTone = e.p("mode", 0, 0) < 0.5;
                    bleepFreq = e.p("frequency", 0, 1000);
                    bleepLevel = dbToLin(e.p("level", 0, -12));
                }
            const double ramp = 0.005 * sr;
            for (int64_t s = s0; s < s1; ++s) {
                double pos = c.reverse ? srcBase + double(ce - 1 - s) * c.speed
                             : ramped  ? (c.sourceIn + c.sourceOffset(double(s - cs) * fps / sr)) * sr / fps
                                       : srcBase + double(s - cs) * c.speed;
                if (pos < 0 || pos >= double(n - 1)) continue;
                float* d = &clipBuf[size_t(s - rs) * 2];
                if (stretched) {
                    const int64_t j = s - cs;
                    if (j < 0 || j >= stretched->frames()) continue;
                    d[0] = stretched->samples[size_t(j) * 2];
                    d[1] = stretched->samples[size_t(j) * 2 + 1];
                } else {
                    int64_t i = int64_t(pos);
                    float f = float(pos - double(i));
                    d[0] = src[i * 2] + (src[i * 2 + 2] - src[i * 2]) * f;
                    d[1] = src[i * 2 + 1] + (src[i * 2 + 3] - src[i * 2 + 1]) * f;
                }
                if (!bleeps.empty()) {
                    double g = 0;
                    for (const auto& [a, b] : bleeps)
                        if (pos > a - ramp && pos < b + ramp)
                            g = std::max(g, std::min({1.0, (pos - (a - ramp)) / ramp, ((b + ramp) - pos) / ramp}));
                    if (g > 0) {
                        const float tone = bleepTone ? bleepLevel * float(std::sin(2 * M_PI * bleepFreq * pos / sr)) : 0.0f;
                        const float k = float(g);
                        d[0] = d[0] * (1 - k) + tone * k;
                        d[1] = d[1] * (1 - k) + tone * k;
                    }
                }
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
                         std::vector<MeterLevels>* trackLevels, int depth, int rate, int onlyTrack, int channels) {
    const double sr = rate > 0 ? rate : seq.sampleRate;
    // Surround: tracks and buses stay stereo inside and are panned onto the layout's speakers at their faders.
    const int nch = depth == 0 && channels > 2 ? channels : 2;
    const bool surround = nch > 2;
    const int lfeCh = [&] {
        const auto& sp = layoutSpeakers(seq.audioLayout);
        for (size_t i = 0; i < sp.size(); ++i)
            if (sp[i].lfe) return int(i);
        return -1;
    }();
    auto panInto = [&](const float* stereo, const SurroundPan& pan, float gain, float* dest) {
        const SurroundGains g = surroundGains(seq.audioLayout, pan);
        for (int i = 0; i < frames; ++i) {
            const float l = stereo[size_t(i) * 2] * gain, r = stereo[size_t(i) * 2 + 1] * gain;
            float* d = dest + size_t(i) * size_t(nch);
            for (int c = 0; c < nch; ++c) d[c] += l * g.left[size_t(c)] + r * g.right[size_t(c)];
            if (lfeCh >= 0 && g.lfe > 0) d[lfeCh] += 0.70710678f * (l + r) * g.lfe;
        }
    };
    const double fps = seq.fpsValue();
    bool anySolo = std::any_of(seq.audioTracks.begin(), seq.audioTracks.end(), [](const Track& t) { return t.solo; });
    if (trackLevels && depth == 0) trackLevels->assign(seq.audioTracks.size(), MeterLevels{});
    auto frameAt = [&](int64_t sample) { return FrameTime(double(sample) * fps / sr); };
    // Delay compensation: each stage is fed ahead by the latency of the stages
    // after it, so everything lines up at the output. The master chain gets the
    // mix of [start + Lm, ...), a bus that of [start + Lm + Lb, ...), and a track
    // its clips at [start + Lm + Lb + Lt, ...).
    const int64_t masterLat = chainLatency(seq.masterEffects, seq.id, sr);
    std::vector<float> master(size_t(frames) * size_t(nch), 0.0f);
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
        if (depth == 0 && !mask_.empty() && (ti >= mask_.size() || !mask_[ti])) continue;
        const bool lfeOnly = depth == 0 && ti < lfeOnly_.size() && lfeOnly_[ti];
        if (lfeOnly && (!surround || lfeCh < 0 || track.output || track.surround.lfeDb <= -99)) continue;
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
        // Fader automation (core/Automation.h): the gain ramps smoothly between frames; pan steps every 64 samples.
        const bool readsLanes = trackAutomation(track) != AutomationMode::Off && trackAutomation(track) != AutomationMode::Write;
        const bool volumeAuto = readsLanes && track.volumeAuto.animated(), panAuto = readsLanes && track.panAuto.animated();
        const int64_t heard = start + downstream;  // the timeline sample the fader acts on now
        if (volumeAuto) {
            for (int i = 0; i < frames; i += 64) {
                const int e = std::min(frames, i + 64);
                const float g0 = dbToLin(trackVolumeAt(track, double(heard + i) * fps / sr));
                const float g1 = dbToLin(trackVolumeAt(track, double(heard + e) * fps / sr));
                for (int k = i; k < e; ++k) {
                    const float g = g0 + (g1 - g0) * float(k - i) / float(e - i);
                    trackBuf[size_t(k) * 2] *= g;
                    trackBuf[size_t(k) * 2 + 1] *= g;
                }
            }
            tg = 1;
        }
        tg *= dbToLin(edit::folderGain(seq, TrackKind::Audio, track.folder));  // its folder's fader (a VCA)
        MeterLevels lv;
        if (surround && bus == busBufs.end()) {
            // Straight to the speakers through the track's surround panner.
            for (int i = 0; i < frames; ++i) {
                lv.peakL = std::max(lv.peakL, std::fabs(trackBuf[size_t(i) * 2] * tg));
                lv.peakR = std::max(lv.peakR, std::fabs(trackBuf[size_t(i) * 2 + 1] * tg));
            }
            if (lfeOnly) {
                const float lfe = float(std::pow(10.0, track.surround.lfeDb / 20));
                for (int i = 0; i < frames; ++i)
                    master[size_t(i) * size_t(nch) + size_t(lfeCh)] += 0.70710678f * (trackBuf[size_t(i) * 2] + trackBuf[size_t(i) * 2 + 1]) * tg * lfe;
            } else if (readsLanes && surroundAnimated(track)) {
                // A moving position (its x, y, z lanes): the speakers' gains glide from one 64-sample step to the next.
                SurroundGains g0 = surroundGains(seq.audioLayout, trackSurroundAt(track, double(heard) * fps / sr));
                for (int i = 0; i < frames; i += 64) {
                    const int e = std::min(frames, i + 64);
                    const SurroundGains g1 = surroundGains(seq.audioLayout, trackSurroundAt(track, double(heard + e) * fps / sr));
                    for (int k = i; k < e; ++k) {
                        const float w = float(k - i) / float(e - i);
                        const float l = trackBuf[size_t(k) * 2] * tg, r = trackBuf[size_t(k) * 2 + 1] * tg;
                        float* d = master.data() + size_t(k) * size_t(nch);
                        for (int c = 0; c < nch; ++c)
                            d[c] += l * (g0.left[size_t(c)] + (g1.left[size_t(c)] - g0.left[size_t(c)]) * w) +
                                    r * (g0.right[size_t(c)] + (g1.right[size_t(c)] - g0.right[size_t(c)]) * w);
                        if (lfeCh >= 0 && g0.lfe > 0) d[lfeCh] += 0.70710678f * (l + r) * g0.lfe;
                    }
                    g0 = g1;
                }
            } else {
                panInto(trackBuf.data(), track.surround, tg, master.data());
            }
        } else {
            panGains(track.pan, tl, tr);
            float* dest = bus != busBufs.end() ? bus->second.data() : master.data();
            for (int i = 0; i < frames; ++i) {
                if (panAuto && i % 64 == 0) panGains(trackPanAt(track, double(heard + i) * fps / sr), tl, tr);
                float l = trackBuf[size_t(i) * 2] * tg * tl, r = trackBuf[size_t(i) * 2 + 1] * tg * tr;
                dest[i * 2] += l;
                dest[i * 2 + 1] += r;
                lv.peakL = std::max(lv.peakL, std::fabs(l));
                lv.peakR = std::max(lv.peakR, std::fabs(r));
            }
        }
        if (trackLevels && depth == 0) (*trackLevels)[ti] = lv;
    }
    for (const Bus& b : seq.buses) {
        std::vector<float>& bb = busBufs[b.id];
        if (!b.effects.empty()) processChain(b.effects, b.id, frameAt(start + masterLat + busLat[b.id]), sr, bb.data(), frames);
        if (b.muted) continue;
        float g = dbToLin(b.volumeDb), bl, br;
        if (surround) {
            panInto(bb.data(), b.surround, g, master.data());
            continue;
        }
        panGains(b.pan, bl, br);
        for (int i = 0; i < frames; ++i) {
            master[size_t(i) * 2] += bb[size_t(i) * 2] * g * bl;
            master[size_t(i) * 2 + 1] += bb[size_t(i) * 2 + 1] * g * br;
        }
    }
    // Master inserts are stereo: in a surround mix they are left out.
    if (!seq.masterEffects.empty() && !surround) processChain(seq.masterEffects, seq.id, frameAt(start + masterLat), sr, master.data(), frames);
    const float g = dbToLin(seq.masterVolumeDb);
    for (int i = 0; i < frames * nch; ++i) out[i] = master[size_t(i)] * g;
}

}  // namespace montage
