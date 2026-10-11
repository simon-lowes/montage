#include "Shapes.h"

#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QRadialGradient>
#include <QTransform>
#include <algorithm>
#include <cmath>

namespace montage {

namespace {

QColor colorOf(const Effect& g, const char* name, FrameTime t, double opacity) {
    const std::string n(name);
    return QColor::fromRgbF(float(std::clamp(g.p(n + ".r", t), 0.0, 1.0)), float(std::clamp(g.p(n + ".g", t), 0.0, 1.0)),
                            float(std::clamp(g.p(n + ".b", t), 0.0, 1.0)), float(std::clamp(opacity, 0.0, 1.0)));
}

// A regular polygon or star round the origin, the first point straight up.
QPolygonF ring(int n, double rx, double ry, double inner) {
    QPolygonF p;
    const bool star = inner > 0;
    const int count = star ? n * 2 : n;
    for (int i = 0; i < count; ++i) {
        const double a = -M_PI / 2 + 2 * M_PI * i / count;
        const double k = star && (i % 2) ? inner : 1.0;
        p << QPointF(rx * k * std::cos(a), ry * k * std::sin(a));
    }
    return p;
}

// A closed polygon with its corners rounded by up to `r` (never more than half the shorter neighbouring side).
QPainterPath roundedPolygon(const QPolygonF& p, double r) {
    QPainterPath path;
    const int n = int(p.size());
    if (r <= 0) {
        path.addPolygon(p);
        path.closeSubpath();
        return path;
    }
    for (int i = 0; i < n; ++i) {
        const QPointF prev = p[(i + n - 1) % n], cur = p[i], next = p[(i + 1) % n];
        const QPointF a = prev - cur, b = next - cur;
        const double la = std::hypot(a.x(), a.y()), lb = std::hypot(b.x(), b.y());
        const double k = std::min({r, la / 2, lb / 2});
        const QPointF from = cur + a * (k / la), to = cur + b * (k / lb);
        if (i == 0) path.moveTo(from);
        else path.lineTo(from);
        path.quadTo(cur, to);
    }
    path.closeSubpath();
    return path;
}

}  // namespace

QPainterPath shapeOutline(const Effect& g, FrameTime t, int w, int h, double scale) {
    const int kind = int(std::lround(g.p("shape", t)));
    const double sw = std::max(0.0, g.p("width", t, 400)) * scale, sh = std::max(0.0, g.p("height", t, 300)) * scale;
    const double round = std::max(0.0, g.p("roundness", t)) * scale;
    QPainterPath path;
    switch (kind) {
        case 1: path.addEllipse(QPointF(0, 0), sw / 2, sh / 2); break;
        case 2:
        case 3: {
            const int n = std::clamp(int(std::lround(g.p("points", t, 5))), 3, 64);
            const double inner = kind == 3 ? std::clamp(g.p("inner", t, 45) / 100.0, 0.01, 1.0) : 0.0;
            path = roundedPolygon(ring(n, sw / 2, sh / 2, inner), round);
            break;
        }
        case 4:  // a line across the width
            path.moveTo(-sw / 2, 0);
            path.lineTo(sw / 2, 0);
            break;
        case 5: {  // an arrow pointing right: the head as long as it is wide, at most 40 % of the length
            const double headLen = std::min(sh, sw * 0.4), shaft = sh * 0.35;
            QPolygonF p;
            p << QPointF(-sw / 2, -shaft / 2) << QPointF(sw / 2 - headLen, -shaft / 2) << QPointF(sw / 2 - headLen, -sh / 2)
              << QPointF(sw / 2, 0) << QPointF(sw / 2 - headLen, sh / 2) << QPointF(sw / 2 - headLen, shaft / 2)
              << QPointF(-sw / 2, shaft / 2);
            path = roundedPolygon(p, round);
            break;
        }
        default: {
            const double r = std::min({round, sw / 2, sh / 2});
            if (r > 0) path.addRoundedRect(QRectF(-sw / 2, -sh / 2, sw, sh), r, r);
            else path.addRect(QRectF(-sw / 2, -sh / 2, sw, sh));
        }
    }
    QTransform tr;
    tr.translate(w / 2.0 + g.p("pos_x", t) * scale, h / 2.0 + g.p("pos_y", t) * scale);
    tr.rotate(g.p("rotation", t));
    return tr.map(path);
}

QPainterPath trimPath(const QPainterPath& path, double start, double end, double offset) {
    start = std::clamp(start, 0.0, 1.0);
    end = std::clamp(end, 0.0, 1.0);
    if (start > end) std::swap(start, end);
    if (start <= 0 && end >= 1) return path;
    QPainterPath out;
    if (end - start <= 1e-6) return out;
    // Along the flattened outline (each subpath on its own), by length.
    for (const QPolygonF& poly : path.toSubpathPolygons()) {
        if (poly.size() < 2) continue;
        const bool closed = poly.isClosed();
        std::vector<double> at(size_t(poly.size()), 0.0);
        for (int i = 1; i < poly.size(); ++i) {
            const QPointF d = poly[i] - poly[i - 1];
            at[size_t(i)] = at[size_t(i - 1)] + std::hypot(d.x(), d.y());
        }
        const double total = at.back();
        if (total <= 0) continue;
        auto pointAt = [&](double s) {
            s = std::clamp(s, 0.0, total);
            const size_t i = size_t(std::upper_bound(at.begin(), at.end(), s) - at.begin());
            if (i >= at.size()) return poly.back();
            const double seg = at[i] - at[i - 1], u = seg > 0 ? (s - at[i - 1]) / seg : 0;
            return poly[int(i) - 1] + (poly[int(i)] - poly[int(i) - 1]) * u;
        };
        auto addRun = [&](double a, double b) {  // a < b, both within [0, total]
            out.moveTo(pointAt(a));
            for (size_t i = 0; i < at.size(); ++i)
                if (at[i] > a && at[i] < b) out.lineTo(poly[int(i)]);
            out.lineTo(pointAt(b));
        };
        double a = (start + offset) * total, b = (end + offset) * total;
        if (closed) {
            // Round the loop: the part may cross the start point.
            const double shift = std::floor(a / total) * total;
            a -= shift, b -= shift;
            if (b <= total) addRun(a, b);
            else {
                addRun(a, total);
                addRun(0, b - total);
            }
        } else {
            a = std::clamp(a, 0.0, total);
            b = std::clamp(b, 0.0, total);
            if (b > a) addRun(a, b);
        }
    }
    return out;
}

Image renderShape(const Effect& g, FrameTime t, int w, int h, double scale) {
    QImage qi(w, h, QImage::Format_RGBA8888_Premultiplied);
    qi.fill(Qt::transparent);
    {
        QPainter pa(&qi);
        pa.setRenderHint(QPainter::Antialiasing);
        const int kind = int(std::lround(g.p("shape", t)));
        const double opacity = std::clamp(g.p("opacity", t, 100) / 100.0, 0.0, 1.0);
        pa.setOpacity(opacity);
        const QPainterPath outline = shapeOutline(g, t, w, h, scale);
        const double start = g.p("trim_start", t, 0) / 100.0, end = g.p("trim_end", t, 100) / 100.0;
        const double offset = g.p("trim_offset", t, 0) / 360.0;
        const bool trimmed = start > 0 || end < 1 || std::fabs(offset) > 1e-9;
        const QPainterPath path = trimmed ? trimPath(outline, start, end, offset) : outline;
        // Fill (not for a line), solid or a gradient across the shape.
        if (kind != 4 && g.p("fill", t, 1) > 0.5 && !path.isEmpty()) {
            const double fo = g.p("fill_opacity", t, 100) / 100.0;
            const int grad = int(std::lround(g.p("gradient", t)));
            const QRectF box = outline.boundingRect();
            if (grad == 0) {
                pa.fillPath(path, colorOf(g, "fill_color", t, fo));
            } else {
                QGradientStops stops = {{0.0, colorOf(g, "fill_color", t, fo)}, {1.0, colorOf(g, "fill_color2", t, fo)}};
                if (grad == 2) {
                    QRadialGradient rg(box.center(), std::max(box.width(), box.height()) / 2);
                    rg.setStops(stops);
                    pa.fillPath(path, rg);
                } else {
                    // Along the angle (0 left to right, 90 top to bottom), end to end across the shape.
                    const double a = g.p("gradient_angle", t) * M_PI / 180;
                    const QPointF dir(std::cos(a), std::sin(a));
                    const double half = (std::fabs(dir.x()) * box.width() + std::fabs(dir.y()) * box.height()) / 2;
                    QLinearGradient lg(box.center() - dir * half, box.center() + dir * half);
                    lg.setStops(stops);
                    pa.fillPath(path, lg);
                }
            }
        }
        // Stroke (a line always has one).
        double sw = std::max(0.0, g.p("stroke", t)) * scale;
        if (kind == 4 && sw <= 0) sw = 8 * scale;
        if (sw > 0 && !path.isEmpty()) {
            static const Qt::PenJoinStyle joins[] = {Qt::RoundJoin, Qt::MiterJoin, Qt::BevelJoin};
            const int j = std::clamp(int(std::lround(g.p("join", t))), 0, 2);
            QPen pen(colorOf(g, "stroke_color", t, g.p("stroke_opacity", t, 100) / 100.0), sw, Qt::SolidLine,
                     j == 0 ? Qt::RoundCap : Qt::FlatCap, joins[j]);
            const double dash = g.p("dash", t) * scale, gap = std::max(0.0, g.p("gap", t, 10)) * scale;
            if (dash > 0) pen.setDashPattern({dash / sw, std::max(gap, 0.01) / sw});  // in stroke widths
            pa.strokePath(path, pen);
        }
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

}  // namespace montage
