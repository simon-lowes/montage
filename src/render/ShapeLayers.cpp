#include "ShapeLayers.h"

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPathStroker>
#include <QRadialGradient>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <set>

#include "Shapes.h"

namespace montage {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kKappa = 0.5522847498307936;  // a quarter circle's Bézier handles, as a share of its radius

// A path as its points with their handles (relative to each point, as drawn masks have them), open or closed.
struct BPath {
    std::vector<PathPoint> v;
    bool closed = true;
};
using Paths = std::vector<BPath>;

QPainterPath toPainterPath(const BPath& p) {
    QPainterPath q;
    const size_t n = p.v.size();
    if (n == 0) return q;
    q.moveTo(p.v[0].x, p.v[0].y);
    auto segment = [&](const PathPoint& a, const PathPoint& b) {
        if (a.ox == 0 && a.oy == 0 && b.ix == 0 && b.iy == 0) q.lineTo(b.x, b.y);
        else q.cubicTo(a.x + a.ox, a.y + a.oy, b.x + b.ix, b.y + b.iy, b.x, b.y);
    };
    for (size_t i = 1; i < n; ++i) segment(p.v[i - 1], p.v[i]);
    if (p.closed && n > 1) {
        segment(p.v[n - 1], p.v[0]);
        q.closeSubpath();
    }
    return q;
}

QPainterPath toPainterPath(const Paths& ps, Qt::FillRule rule = Qt::WindingFill) {
    QPainterPath q;
    q.setFillRule(rule);
    for (const BPath& p : ps) q.addPath(toPainterPath(p));
    return q;
}

// A painter path's subpaths as points and handles (a subpath ending where it started is closed).
Paths fromPainterPath(const QPainterPath& q) {
    Paths out;
    for (int i = 0; i < q.elementCount(); ++i) {
        const QPainterPath::Element e = q.elementAt(i);
        if (e.isMoveTo() || out.empty()) {
            out.push_back({{}, false});
            out.back().v.push_back({e.x, e.y});
        } else if (e.isLineTo()) {
            out.back().v.push_back({e.x, e.y});
        } else if (e.isCurveTo() && i + 2 < q.elementCount()) {
            const QPainterPath::Element c2 = q.elementAt(i + 1), end = q.elementAt(i + 2);
            PathPoint& a = out.back().v.back();
            a.ox = e.x - a.x, a.oy = e.y - a.y;
            PathPoint b{end.x, end.y};
            b.ix = c2.x - end.x, b.iy = c2.y - end.y;
            out.back().v.push_back(b);
            i += 2;
        }
    }
    for (BPath& p : out) {
        if (p.v.size() > 2 && std::hypot(p.v.back().x - p.v[0].x, p.v.back().y - p.v[0].y) < 1e-9) {
            p.v[0].ix = p.v.back().ix, p.v[0].iy = p.v.back().iy;
            p.v.pop_back();
            p.closed = true;
        }
    }
    std::erase_if(out, [](const BPath& p) { return p.v.size() < 2; });
    return out;
}

BPath mapped(const BPath& p, const QTransform& m) {
    BPath out = p;
    for (PathPoint& v : out.v) {
        const QPointF a = m.map(QPointF(v.x, v.y)), i = m.map(QPointF(v.x + v.ix, v.y + v.iy)), o = m.map(QPointF(v.x + v.ox, v.y + v.oy));
        v = {a.x(), a.y(), i.x() - a.x(), i.y() - a.y(), o.x() - a.x(), o.y() - a.y()};
    }
    return out;
}

// The point and the direction at s along the segment from a to b.
QPointF bezierAt(const PathPoint& a, const PathPoint& b, double s, QPointF* tangent = nullptr) {
    const QPointF p0(a.x, a.y), p1(a.x + a.ox, a.y + a.oy), p2(b.x + b.ix, b.y + b.iy), p3(b.x, b.y);
    const double u = 1 - s;
    if (tangent) {
        *tangent = 3 * u * u * (p1 - p0) + 6 * u * s * (p2 - p1) + 3 * s * s * (p3 - p2);
        if (std::hypot(tangent->x(), tangent->y()) < 1e-12) *tangent = p3 - p0;
    }
    return u * u * u * p0 + 3 * u * u * s * p1 + 3 * u * s * s * p2 + s * s * s * p3;
}

int segmentCount(const BPath& p) { return p.v.size() < 2 ? 0 : int(p.v.size()) - (p.closed ? 0 : 1); }

// The path as points along it, `per` steps a segment (corners kept).
std::vector<QPointF> flattened(const BPath& p, int per) {
    std::vector<QPointF> out;
    if (p.v.empty()) return out;
    out.push_back({p.v[0].x, p.v[0].y});
    for (int s = 0; s < segmentCount(p); ++s) {
        const PathPoint& a = p.v[size_t(s)];
        const PathPoint& b = p.v[size_t(s + 1) % p.v.size()];
        const bool straight = a.ox == 0 && a.oy == 0 && b.ix == 0 && b.iy == 0;
        const int n = straight ? 1 : per;
        for (int k = 1; k <= n; ++k) out.push_back(bezierAt(a, b, double(k) / n));
    }
    if (p.closed && out.size() > 1) out.pop_back();  // (back at the first)
    return out;
}

// ---- Shapes ----

BPath ellipsePath(double cx, double cy, double rx, double ry) {
    BPath p;
    const double kx = rx * kKappa, ky = ry * kKappa;
    p.v = {{cx, cy - ry, -kx, 0, kx, 0}, {cx + rx, cy, 0, -ky, 0, ky}, {cx, cy + ry, kx, 0, -kx, 0}, {cx - rx, cy, 0, ky, 0, -ky}};
    return p;
}

// Corners (points without handles) rounded by an arc reaching up to r along each side (at most half of either).
BPath roundCorners(const BPath& p, double r) {
    if (r <= 0 || p.v.size() < 3) return p;
    BPath out;
    out.closed = p.closed;
    const size_t n = p.v.size();
    for (size_t i = 0; i < n; ++i) {
        const PathPoint& cur = p.v[i];
        const bool end = !p.closed && (i == 0 || i == n - 1);
        if (end || cur.smooth()) {
            out.v.push_back(cur);
            continue;
        }
        const PathPoint& prev = p.v[(i + n - 1) % n];
        const PathPoint& next = p.v[(i + 1) % n];
        // Towards each neighbour along the side (its handle's end where the side curves).
        const QPointF c(cur.x, cur.y);
        const QPointF a = QPointF(prev.x + prev.ox, prev.y + prev.oy) - c, b = QPointF(next.x + next.ix, next.y + next.iy) - c;
        const double la = std::hypot(a.x(), a.y()), lb = std::hypot(b.x(), b.y());
        const double sideA = std::hypot(prev.x - cur.x, prev.y - cur.y), sideB = std::hypot(next.x - cur.x, next.y - cur.y);
        if (la < 1e-9 || lb < 1e-9) {
            out.v.push_back(cur);
            continue;
        }
        const QPointF ua = a / la, ub = b / lb;
        const double cosPhi = std::clamp(ua.x() * ub.x() + ua.y() * ub.y(), -1.0, 1.0);
        const double phi = std::acos(cosPhi), theta = kPi - phi;
        if (theta < 1e-6 || phi < 1e-6) {
            out.v.push_back(cur);
            continue;
        }
        const double k = std::min({r, sideA / 2, sideB / 2});
        const double radius = k * std::tan(phi / 2), h = 4.0 / 3 * std::tan(theta / 4) * radius;
        const QPointF from = c + ua * k, to = c + ub * k;
        out.v.push_back({from.x(), from.y(), 0, 0, -ua.x() * h, -ua.y() * h});
        out.v.push_back({to.x(), to.y(), -ub.x() * h, -ub.y() * h, 0, 0});
    }
    return out;
}

BPath ringPath(int n, double outer, double inner, double cx, double cy, double rotation) {
    BPath p;
    const bool star = inner >= 0;
    const int count = star ? n * 2 : n;
    for (int i = 0; i < count; ++i) {
        const double a = -kPi / 2 + rotation * kPi / 180 + 2 * kPi * i / count;
        const double r = star && (i % 2) ? inner : outer;
        p.v.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    return p;
}

// ---- Operators ----

BPath reversed(const BPath& p) {
    BPath out = p;
    std::reverse(out.v.begin(), out.v.end());
    for (PathPoint& v : out.v) std::swap(v.ix, v.ox), std::swap(v.iy, v.oy);
    return out;
}

// Each path turned so that filled with the non-zero rule, holes stay holes: outlines one way round, those inside
// one other the other way, and so on (as a combined result's outlines and holes come).
Paths orientedForWinding(Paths ps) {
    std::vector<QPolygonF> polys;
    for (const BPath& p : ps) {
        QPolygonF q;
        for (const QPointF& pt : flattened(p, 8)) q << pt;
        polys.push_back(q);
    }
    for (size_t i = 0; i < ps.size(); ++i) {
        if (!ps[i].closed || polys[i].size() < 3) continue;
        double area = 0;
        for (int k = 0; k < polys[i].size(); ++k) {
            const QPointF a = polys[i][k], b = polys[i][(k + 1) % polys[i].size()];
            area += a.x() * b.y() - b.x() * a.y();
        }
        int depth = 0;
        for (size_t j = 0; j < ps.size(); ++j)
            if (j != i && polys[j].size() >= 3 && polys[j].containsPoint(polys[i][0], Qt::OddEvenFill)) ++depth;
        if ((area > 0) != (depth % 2 == 0)) ps[i] = reversed(ps[i]);
    }
    return ps;
}

std::vector<BPath> merged(const Paths& in, int mode) {
    if (mode == 0 || in.size() < 2) return in;
    QPainterPath acc = toPainterPath(in[0]);
    for (size_t i = 1; i < in.size(); ++i) {
        const QPainterPath q = toPainterPath(in[i]);
        switch (mode) {
            case 1: acc = acc.united(q); break;
            case 2: acc = acc.subtracted(q); break;
            case 3: acc = acc.intersected(q); break;
            default: acc = acc.united(q).subtracted(acc.intersected(q)); break;
        }
    }
    return orientedForWinding(fromPainterPath(acc.simplified()));
}

Qt::PenJoinStyle joinStyle(int j) { return j == 1 ? Qt::RoundJoin : j == 2 ? Qt::BevelJoin : Qt::MiterJoin; }

Paths offsetPaths(const Paths& in, double d, int join, double miter) {
    if (std::fabs(d) < 1e-9) return in;
    Paths out;
    for (const BPath& p : in) {
        QPainterPathStroker st;
        st.setWidth(2 * std::fabs(d));
        st.setJoinStyle(joinStyle(join));
        st.setMiterLimit(miter);
        st.setCapStyle(join == 1 ? Qt::RoundCap : Qt::FlatCap);
        const QPainterPath q = toPainterPath(p);
        const QPainterPath edge = st.createStroke(q);
        const QPainterPath r = !p.closed ? edge : d > 0 ? q.united(edge) : q.subtracted(edge);
        for (BPath& b : orientedForWinding(fromPainterPath(r.simplified()))) out.push_back(std::move(b));
    }
    return out;
}

QPointF centreOf(const BPath& p) {
    QPointF c(0, 0);
    for (const PathPoint& v : p.v) c += QPointF(v.x, v.y);
    return p.v.empty() ? c : c / double(p.v.size());
}

// Points pulled towards the middle and handles pushed away (a > 0), or the other way round.
BPath puckered(const BPath& in, double a) {
    BPath p = in;
    const size_t n = p.v.size();
    // Straight sides first get handles a third of the way along, so they can bend.
    for (int s = 0; s < segmentCount(p); ++s) {
        PathPoint& x = p.v[size_t(s)];
        PathPoint& y = p.v[size_t(s + 1) % n];
        if (x.ox == 0 && x.oy == 0 && y.ix == 0 && y.iy == 0) {
            x.ox = (y.x - x.x) / 3, x.oy = (y.y - x.y) / 3;
            y.ix = (x.x - y.x) / 3, y.iy = (x.y - y.y) / 3;
        }
    }
    const QPointF c = centreOf(in);
    for (PathPoint& v : p.v) {
        const QPointF P(v.x, v.y), I(v.x + v.ix, v.y + v.iy), O(v.x + v.ox, v.y + v.oy);
        const QPointF P2 = P + (c - P) * a, I2 = I - (c - I) * a, O2 = O - (c - O) * a;
        v = {P2.x(), P2.y(), I2.x() - P2.x(), I2.y() - P2.y(), O2.x() - P2.x(), O2.y() - P2.y()};
    }
    return p;
}

BPath twisted(const BPath& in, double degrees, QPointF centre) {
    BPath p;
    p.closed = in.closed;
    const std::vector<QPointF> pts = flattened(in, 24);
    double far = 1e-9;
    for (const QPointF& q : pts) far = std::max(far, std::hypot(q.x() - centre.x(), q.y() - centre.y()));
    for (const QPointF& q : pts) {
        const double a = degrees * kPi / 180 * std::hypot(q.x() - centre.x(), q.y() - centre.y()) / far;
        const QPointF d = q - centre;
        p.v.push_back({centre.x() + d.x() * std::cos(a) - d.y() * std::sin(a), centre.y() + d.x() * std::sin(a) + d.y() * std::cos(a)});
    }
    return p;
}

BPath zigzagged(const BPath& in, double size, int ridges, bool smooth) {
    BPath p;
    p.closed = in.closed;
    int k = 0;
    auto add = [&](QPointF at, QPointF tangent) {
        const double l = std::hypot(tangent.x(), tangent.y());
        const QPointF n = l > 1e-12 ? QPointF(-tangent.y() / l, tangent.x() / l) : QPointF(0, 0);
        const QPointF q = at + n * (k++ % 2 ? -size : size);
        p.v.push_back({q.x(), q.y()});
    };
    const int segs = segmentCount(in);
    for (int s = 0; s < segs; ++s) {
        const PathPoint& a = in.v[size_t(s)];
        const PathPoint& b = in.v[size_t(s + 1) % in.v.size()];
        for (int j = 0; j <= ridges; ++j) {
            QPointF tg;
            const QPointF at = bezierAt(a, b, double(j) / (ridges + 1), &tg);
            add(at, tg);
        }
    }
    if (!in.closed && !in.v.empty()) {
        QPointF tg;
        const QPointF at = segs > 0 ? bezierAt(in.v[in.v.size() - 2], in.v.back(), 1, &tg) : QPointF(in.v.back().x, in.v.back().y);
        add(at, tg);
    }
    if (smooth)
        for (size_t i = 0; i < p.v.size(); ++i) {
            if (!p.closed && (i == 0 || i + 1 == p.v.size())) continue;
            const PathPoint s = smoothed(p.v, int(i), p.closed);
            p.v[i].ix = s.ix, p.v[i].iy = s.iy, p.v[i].ox = s.ox, p.v[i].oy = s.oy;
        }
    return p;
}

// Smooth noise (-1..1) through whole-numbered x and y, the same wherever and however often it is asked for.
double noise2(double x, double y, uint32_t seed) {
    auto hash = [seed](int64_t i, int64_t j) {
        uint64_t h = uint64_t(i) * 0x9E3779B97F4A7C15ull ^ (uint64_t(j) + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full ^ uint64_t(seed) * 0x165667B19E3779F9ull;
        h ^= h >> 33, h *= 0xFF51AFD7ED558CCDull, h ^= h >> 33, h *= 0xC4CEB9FE1A85EC53ull, h ^= h >> 33;
        return double(h >> 11) / double(1ull << 53) * 2 - 1;
    };
    const double fx = std::floor(x), fy = std::floor(y);
    const int64_t ix = int64_t(fx), iy = int64_t(fy);
    const double u = x - fx, v = y - fy;
    const double su = u * u * (3 - 2 * u), sv = v * v * (3 - 2 * v);
    const double a = hash(ix, iy) + (hash(ix + 1, iy) - hash(ix, iy)) * su;
    const double b = hash(ix, iy + 1) + (hash(ix + 1, iy + 1) - hash(ix, iy + 1)) * su;
    return a + (b - a) * sv;
}

BPath wiggled(const BPath& in, double size, double spacing, double time, double correlation, uint32_t seed, int most) {
    // Points every `spacing` along it (its corners among them where they fall), each moved by noise in time; points
    // near each other move alike as much as `correlation` asks.
    const std::vector<QPointF> fine = flattened(in, 32);
    BPath p;
    p.closed = in.closed;
    if (fine.size() < 2) return in;
    std::vector<QPointF> loop = fine;
    if (in.closed) loop.push_back(fine.front());
    std::vector<double> at(loop.size(), 0.0);
    for (size_t i = 1; i < loop.size(); ++i) at[i] = at[i - 1] + std::hypot(loop[i].x() - loop[i - 1].x(), loop[i].y() - loop[i - 1].y());
    const double total = at.back();
    const int n = std::max(in.closed ? 3 : 2, int(std::min(double(most), std::round(total / std::max(1.0, spacing)) + (in.closed ? 0 : 1))));
    const double along = (1 - std::clamp(correlation, 0.0, 1.0)) * 0.7;
    for (int i = 0; i < n; ++i) {
        const double s = in.closed ? total * i / n : total * i / (n - 1);
        const size_t j = std::min(loop.size() - 1, size_t(std::upper_bound(at.begin(), at.end(), s) - at.begin()));
        const double seg = at[j] - at[j > 0 ? j - 1 : 0];
        const double u = j > 0 && seg > 0 ? (s - at[j - 1]) / seg : 0;
        const QPointF q = j > 0 ? loop[j - 1] + (loop[j] - loop[j - 1]) * u : loop[0];
        const double x = i * along;
        p.v.push_back({q.x() + size * noise2(x, time, seed * 2 + 1), q.y() + size * noise2(x + 1000.5, time, seed * 2 + 2)});
    }
    for (size_t i = 0; i < p.v.size(); ++i) {
        if (!p.closed && (i == 0 || i + 1 == p.v.size())) continue;
        const PathPoint s = smoothed(p.v, int(i), p.closed);
        p.v[i].ix = s.ix, p.v[i].iy = s.iy, p.v[i].ox = s.ox, p.v[i].oy = s.oy;
    }
    return p;
}

Paths trimmed(const Paths& in, double start, double end, double offset, bool oneAfterAnother) {
    if (start <= 0 && end >= 1 && std::fabs(offset) < 1e-9) return in;
    Paths out;
    if (!oneAfterAnother) {
        for (const BPath& p : in)
            for (BPath& b : fromPainterPath(trimPath(toPainterPath(p), start, end, offset))) out.push_back(std::move(b));
        return out;
    }
    // Along all the paths end to end.
    std::vector<double> len;
    double total = 0;
    for (const BPath& p : in) len.push_back(toPainterPath(p).length()), total += len.back();
    if (total <= 0) return out;
    double a = std::min(start, end) + offset, b = std::max(start, end) + offset;
    a -= std::floor(a), b = a + (std::max(start, end) - std::min(start, end));
    double from = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        const double s0 = from / total, s1 = (from + len[i]) / total;
        from += len[i];
        for (const double shift : {0.0, 1.0}) {  // (the part past the end comes round from the start)
            const double lo = std::max(a - shift, s0), hi = std::min(b - shift, s1);
            if (hi <= lo || s1 <= s0) continue;
            const QPainterPath part = trimPath(toPainterPath(in[i]), (lo - s0) / (s1 - s0), (hi - s0) / (s1 - s0), 0);
            for (BPath& q : fromPainterPath(part)) out.push_back(std::move(q));
        }
    }
    return out;
}

// ---- The contents ----

QColor colour(const Effect& g, const ShapeItem& it, const char* name, FrameTime t) {
    const std::string n(name);
    auto c = [&](const char* k) { return float(std::clamp(shapeItemParam(g, it, n + "." + k, t), 0.0, 1.0)); };
    return QColor::fromRgbF(c("r"), c("g"), c("b"));
}

QBrush paintBrush(const Effect& g, const ShapeItem& it, FrameTime t) {
    auto P = [&](const char* name) { return shapeItemParam(g, it, name, t); };
    const int kind = int(std::lround(P("gradient")));
    if (kind == 0) return QBrush(colour(g, it, "color", t));
    QGradientStops stops;
    if (it.stops.empty()) {
        stops = {{0.0, colour(g, it, "color", t)}, {1.0, colour(g, it, "color2", t)}};
    } else {
        for (const auto& s : it.stops)
            stops.push_back({std::clamp(s[0], 0.0, 1.0), QColor::fromRgbF(float(s[1]), float(s[2]), float(s[3]), float(std::clamp(s[4], 0.0, 1.0)))});
    }
    const QPointF a(P("start_x"), P("start_y")), b(P("end_x"), P("end_y"));
    QGradient grad;
    if (kind == 2) {
        QRadialGradient rg(a, std::hypot(b.x() - a.x(), b.y() - a.y()), it.hasFocal ? QPointF(it.focalX, it.focalY) : a);
        grad = rg;
    } else {
        grad = QLinearGradient(a, b);
    }
    grad.setStops(stops);
    grad.setSpread(it.spread == 1 ? QGradient::ReflectSpread : it.spread == 2 ? QGradient::RepeatSpread : QGradient::PadSpread);
    QBrush brush(grad);
    if (it.hasGradientMatrix) {
        const auto& m = it.gradientMatrix;
        brush.setTransform(QTransform(m[0], m[1], m[2], m[3], m[4], m[5]));
    }
    return brush;
}

struct Drawn {
    Paths paths;                 // in the group's space
    std::vector<ShapePaint> ops;  // the lowest first
};

// What one drawing may make in all (path points, and the points of what is painted): copies of copies, zig-zags of
// zig-zags and the like stop there rather than run out of memory.
struct Budget {
    double left = 4e6;
    bool out = false;
    bool take(double n) {
        if (out || !(n <= left)) return !(out = true);
        left -= n;
        return true;
    }
};

double pointsIn(const Paths& ps) {
    double n = 0;
    for (const BPath& p : ps) n += double(p.v.size());
    return n;
}

double pointsIn(const std::vector<ShapePaint>& ops) {
    double n = 0;
    for (const ShapePaint& op : ops) n += double(op.path.elementCount());
    return n;
}

QTransform repeaterStep(double k, double px, double py, double ax, double ay, double scale, double rotation) {
    // One copy's step taken k times (k may be fractional or negative): about the anchor, scaled, turned and moved.
    QTransform once;
    once.translate(ax + px, ay + py);
    once.rotate(rotation);
    once.scale(scale, scale);
    once.translate(-ax, -ay);
    if (!std::isfinite(k)) k = 0;
    k = std::clamp(k, -1e6, 1e6);
    const int whole = int(std::floor(k));
    const double part = k - whole;
    QTransform t;
    QTransform step = whole >= 0 ? once : once.inverted();
    for (int n = std::abs(whole); n > 0; n >>= 1, step = step * step)
        if (n & 1) t = t * step;
    if (part > 1e-12) {
        QTransform f;
        f.translate(ax + px * part, ay + py * part);
        f.rotate(rotation * part);
        const double s = std::pow(std::max(1e-9, std::fabs(scale)), part) * (scale < 0 ? -1 : 1);
        f.scale(s, s);
        f.translate(-ax, -ay);
        t = f * t;
    }
    return t;
}

Drawn drawItems(const Effect& g, const std::vector<ShapeItem>& items, FrameTime t, double fps, int depth, Budget& budget) {
    Drawn d;
    if (depth > kShapeDepth) return d;
    auto P = [&](const ShapeItem& it, const char* name) { return shapeItemParam(g, it, name, t); };
    for (const ShapeItem& it : items) {
        if (budget.out) break;
        if (!it.enabled) continue;
        // (what each makes, ahead of making it)
        const double here = pointsIn(d.paths);
        const std::string& kind = it.type;
        if ((kind == "rect" || kind == "ellipse" || kind == "polygon" || kind == "star") && !budget.take(130)) break;
        if ((kind == "fill" || kind == "stroke" || kind == "pucker" || kind == "trim") && !budget.take(here)) break;
        if ((kind == "merge" || kind == "offset" || kind == "round") && !budget.take(here * 3)) break;
        if (kind == "twist" && !budget.take(here * 25)) break;
        if (kind == "zigzag" && !budget.take(here * (2 * std::clamp(P(it, "ridges"), 0.0, 100.0) + 3))) break;
        const std::string& ty = it.type;
        if (ty == "rect") {
            const double w = std::max(0.0, P(it, "width")), h = std::max(0.0, P(it, "height")), x = P(it, "pos_x"), y = P(it, "pos_y");
            BPath p;
            p.v = {{x - w / 2, y - h / 2}, {x + w / 2, y - h / 2}, {x + w / 2, y + h / 2}, {x - w / 2, y + h / 2}};
            d.paths.push_back(roundCorners(p, std::min({P(it, "roundness"), w / 2, h / 2})));
        } else if (ty == "ellipse") {
            d.paths.push_back(ellipsePath(P(it, "pos_x"), P(it, "pos_y"), std::max(0.0, P(it, "width")) / 2, std::max(0.0, P(it, "height")) / 2));
        } else if (ty == "polygon" || ty == "star") {
            const int n = std::clamp(int(std::lround(P(it, "points"))), 3, 64);
            const bool star = ty == "star";
            const BPath p = ringPath(n, std::max(0.0, P(it, star ? "outer" : "radius")), star ? std::max(0.0, P(it, "inner")) : -1, P(it, "pos_x"),
                                     P(it, "pos_y"), P(it, "rotation"));
            d.paths.push_back(roundCorners(p, P(it, "roundness")));
        } else if (ty == "path") {
            BPath p;
            p.v = shapePathPoints(g, it.id, t);
            p.closed = it.closed;
            if (!budget.take(double(p.v.size()))) break;
            if (p.v.size() >= 2) d.paths.push_back(std::move(p));
        } else if (ty == "group") {
            Drawn in = drawItems(g, it.items, t, fps, depth + 1, budget);
            QTransform m;
            if (it.hasMatrix) m = QTransform(it.matrix[0], it.matrix[1], it.matrix[2], it.matrix[3], it.matrix[4], it.matrix[5]);
            QTransform own;
            own.translate(P(it, "pos_x"), P(it, "pos_y"));
            own.rotate(P(it, "rotation"));
            own.scale(P(it, "scale_x") / 100, P(it, "scale_y") / 100);
            own.translate(-P(it, "anchor_x"), -P(it, "anchor_y"));
            const QTransform G = m * own;
            const double opacity = std::clamp(P(it, "opacity") / 100, 0.0, 1.0);
            for (const BPath& p : in.paths) d.paths.push_back(mapped(p, G));
            for (ShapePaint& op : in.ops) op.transform = op.transform * G, op.opacity *= opacity;
            d.ops.insert(d.ops.begin(), in.ops.begin(), in.ops.end());
        } else if (ty == "fill" || ty == "stroke") {
            if (d.paths.empty()) continue;
            ShapePaint op;
            op.opacity = std::clamp(P(it, "opacity") / 100, 0.0, 1.0);
            if (ty == "fill") {
                op.path = toPainterPath(d.paths, std::lround(P(it, "rule")) == 1 ? Qt::OddEvenFill : Qt::WindingFill);
                op.brush = paintBrush(g, it, t);
            } else {
                const double width = std::max(0.0, P(it, "width"));
                if (width <= 0) continue;
                op.stroke = true;
                op.path = toPainterPath(d.paths);
                const int cap = int(std::lround(P(it, "cap")));
                op.pen = QPen(paintBrush(g, it, t), width, Qt::SolidLine, cap == 1 ? Qt::RoundCap : cap == 2 ? Qt::SquareCap : Qt::FlatCap,
                              joinStyle(int(std::lround(P(it, "join")))));
                op.pen.setMiterLimit(P(it, "miter"));
                const double dash = P(it, "dash"), gap = std::max(0.0, P(it, "gap"));
                if (dash > 0) {
                    op.pen.setDashPattern({dash / width, std::max(gap, 0.01) / width});  // (in stroke widths)
                    op.pen.setDashOffset(P(it, "dash_offset") / width);
                }
            }
            d.ops.insert(d.ops.begin(), op);
        } else if (ty == "merge") {
            d.paths = merged(d.paths, int(std::lround(P(it, "mode"))));
        } else if (ty == "offset") {
            d.paths = offsetPaths(d.paths, P(it, "amount"), int(std::lround(P(it, "join"))), P(it, "miter"));
        } else if (ty == "round") {
            for (BPath& p : d.paths) p = roundCorners(p, P(it, "radius"));
        } else if (ty == "pucker") {
            for (BPath& p : d.paths) p = puckered(p, std::clamp(P(it, "amount") / 100, -1.0, 1.0));
        } else if (ty == "twist") {
            for (BPath& p : d.paths) p = twisted(p, P(it, "angle"), QPointF(P(it, "center_x"), P(it, "center_y")));
        } else if (ty == "zigzag") {
            const int ridges = std::clamp(int(std::lround(P(it, "ridges"))), 0, 100);
            for (BPath& p : d.paths) p = zigzagged(p, P(it, "size"), ridges, std::lround(P(it, "points")) == 1);
        } else if (ty == "wiggle") {
            const double time = double(t) / (fps > 0 ? fps : 25) * P(it, "speed");
            const uint32_t seed = uint32_t(std::lround(std::max(0.0, P(it, "seed"))));
            const int most = int(std::min(20000.0, budget.left / double(std::max<size_t>(1, d.paths.size()))));
            for (size_t i = 0; i < d.paths.size(); ++i)
                d.paths[i] = wiggled(d.paths[i], P(it, "size"), P(it, "spacing"), time, P(it, "correlation") / 100, seed * 7919u + uint32_t(i), most);
            if (!budget.take(pointsIn(d.paths) * 33)) break;
        } else if (ty == "trim") {
            d.paths = trimmed(d.paths, P(it, "start") / 100, P(it, "end") / 100, P(it, "offset") / 360, std::lround(P(it, "each")) == 1);
        } else if (ty == "repeater") {
            int copies = std::clamp(int(std::lround(P(it, "copies"))), 0, 1000);
            const double each = std::max(1.0, here + pointsIn(d.ops));
            copies = int(std::min(double(copies), std::floor(budget.left / each)));
            budget.take(copies * each);
            const double off = P(it, "offset");
            const double o0 = std::clamp(P(it, "start_opacity") / 100, 0.0, 1.0), o1 = std::clamp(P(it, "end_opacity") / 100, 0.0, 1.0);
            const bool above = std::lround(P(it, "composite")) == 1;
            Drawn r;
            for (int c = 0; c < copies; ++c) {
                const QTransform T = repeaterStep(c + off, P(it, "pos_x"), P(it, "pos_y"), P(it, "anchor_x"), P(it, "anchor_y"),
                                                  P(it, "scale") / 100, P(it, "rotation"));
                for (const BPath& p : d.paths) r.paths.push_back(mapped(p, T));
            }
            for (int k = 0; k < copies; ++k) {
                const int c = above ? k : copies - 1 - k;  // (the lowest first)
                const QTransform T = repeaterStep(c + off, P(it, "pos_x"), P(it, "pos_y"), P(it, "anchor_x"), P(it, "anchor_y"),
                                                  P(it, "scale") / 100, P(it, "rotation"));
                const double op = copies > 1 ? o0 + (o1 - o0) * c / (copies - 1) : o0;
                for (ShapePaint s : d.ops) {
                    s.transform = s.transform * T;
                    s.opacity *= op;
                    r.ops.push_back(std::move(s));
                }
            }
            d = std::move(r);
        }
    }
    return d;
}

}  // namespace

std::vector<ShapePaint> shapePaints(const Effect& g, FrameTime t, double fps) {
    Budget budget;
    return drawItems(g, shapeContents(g), t, fps, 0, budget).ops;
}

Image renderShapeContents(const Effect& g, FrameTime t, int w, int h, double scale, double fps) {
    QImage qi(w, h, QImage::Format_RGBA8888_Premultiplied);
    qi.fill(Qt::transparent);
    {
        QPainter pa(&qi);
        pa.setRenderHint(QPainter::Antialiasing);
        QTransform layer;
        layer.translate(w / 2.0 + g.p("pos_x", t) * scale, h / 2.0 + g.p("pos_y", t) * scale);
        layer.rotate(g.p("rotation", t));
        layer.scale(scale, scale);
        const double opacity = std::clamp(g.p("opacity", t, 100) / 100.0, 0.0, 1.0);
        for (const ShapePaint& op : shapePaints(g, t, fps)) {
            if (op.opacity <= 0 || op.path.isEmpty()) continue;
            pa.setTransform(op.transform * layer);
            pa.setOpacity(opacity * op.opacity);
            if (op.stroke) pa.strokePath(op.path, op.pen);
            else pa.fillPath(op.path, op.brush);
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

std::vector<ShapeItem> plainShapeAsContents(Effect& g) {
    if (!g.s("contents").empty()) return {};
    std::vector<ShapeItem> items;
    auto copy = [&](const std::string& from, const std::string& to) {
        if (auto it = g.params.find(from); it != g.params.end()) g.params[to] = it->second;
    };
    auto add = [&](const char* type) -> ShapeItem& {
        ShapeItem it;
        it.type = type;
        it.id = newShapeItemId(items, type);
        items.push_back(it);
        return items.back();
    };
    const int kind = int(std::lround(g.p("shape", 0)));
    const double sw = std::max(0.0, g.p("width", 0, 400)), sh = std::max(0.0, g.p("height", 0, 300));
    std::vector<ShapeItem>* into = &items;
    const bool trimmed = g.p("trim_start", 0) > 0 || g.p("trim_end", 0, 100) < 100 || std::fabs(g.p("trim_offset", 0)) > 1e-9 ||
                         (g.params.count("trim_start") && g.params.at("trim_start").varies()) ||
                         (g.params.count("trim_end") && g.params.at("trim_end").varies()) ||
                         (g.params.count("trim_offset") && g.params.at("trim_offset").varies());
    if (kind == 1 && (trimmed || g.p("dash", 0) > 0)) {
        // An ellipse trimmed or dashed: a path going round as the plain one does (from its right, downwards), so the
        // same part of it is drawn; keyed where its size is.
        ShapeItem& path = add("path");
        std::set<FrameTime> times;
        for (const char* k : {"width", "height"})
            if (auto it = g.params.find(k); it != g.params.end())
                for (const Keyframe& key : it->second.keys) times.insert(key.t);
        const bool keyed = !times.empty();
        if (!keyed) times.insert(0);
        for (const FrameTime t : times) {
            const double rx = std::max(0.0, g.p("width", t, 400)) / 2, ry = std::max(0.0, g.p("height", t, 300)) / 2;
            const double kx = rx * kKappa, ky = ry * kKappa;
            setShapePathPoints(g, path.id, t, {{rx, 0, 0, -ky, 0, ky}, {0, ry, kx, 0, -kx, 0}, {-rx, 0, 0, ky, 0, -ky}, {0, -ry, -kx, 0, kx, 0}}, keyed);
        }
    } else if (kind == 0 || kind == 1) {
        const std::string id = add(kind == 0 ? "rect" : "ellipse").id;
        g.params[id + ".width"] = Param(sw), g.params[id + ".height"] = Param(sh);
        copy("width", id + ".width"), copy("height", id + ".height");
        if (kind == 0) copy("roundness", id + ".roundness");
    } else if (kind == 2 || kind == 3) {
        // (a ring as wide and high as the shape: one a unit high in a group stretching it)
        ShapeItem& group = add("group");
        g.params[group.id + ".scale_y"] = Param(sw > 0 ? sh / sw * 100 : 100);
        ShapeItem ring;
        ring.type = kind == 2 ? "polygon" : "star";
        ring.id = newShapeItemId(items, ring.type);
        g.params[ring.id + (kind == 2 ? ".radius" : ".outer")] = Param(sw / 2);
        if (kind == 3) g.params[ring.id + ".inner"] = Param(sw / 2 * std::clamp(g.p("inner", 0, 45) / 100, 0.01, 1.0));
        copy("points", ring.id + ".points");
        copy("roundness", ring.id + ".roundness");
        group.items.push_back(ring);
        into = &group.items;
    } else {
        // A line, or an arrow, as a path.
        ShapeItem& path = add("path");
        path.closed = kind == 5;
        std::vector<PathPoint> pts;
        if (kind == 4) {
            pts = {{-sw / 2, 0}, {sw / 2, 0}};
        } else {
            const double head = std::min(sh, sw * 0.4), shaft = sh * 0.35;
            pts = {{-sw / 2, -shaft / 2}, {sw / 2 - head, -shaft / 2}, {sw / 2 - head, -sh / 2}, {sw / 2, 0}, {sw / 2 - head, sh / 2},
                   {sw / 2 - head, shaft / 2}, {-sw / 2, shaft / 2}};
        }
        setShapePathPoints(g, path.id, 0, pts);
    }
    (void)into;
    if (trimmed) {
        const std::string id = add("trim").id;
        copy("trim_start", id + ".start"), copy("trim_end", id + ".end"), copy("trim_offset", id + ".offset");
    }
    const double stroke = g.p("stroke", 0);
    if (stroke > 0 || kind == 4 || (g.params.count("stroke") && g.params.at("stroke").animated())) {
        const std::string id = add("stroke").id;
        g.params[id + ".width"] = Param(stroke > 0 || kind != 4 ? stroke : 8.0);
        if (stroke > 0 || kind != 4) copy("stroke", id + ".width");
        for (const char* c : {".r", ".g", ".b"}) {
            g.params[id + ".color" + c] = Param(g.p(std::string("stroke_color") + c, 0, 1));
            copy(std::string("stroke_color") + c, id + ".color" + c);
        }
        copy("stroke_opacity", id + ".opacity");
        const int join = int(std::lround(g.p("join", 0)));
        g.params[id + ".join"] = Param(join == 0 ? 1.0 : join == 1 ? 0.0 : 2.0);
        g.params[id + ".cap"] = Param(join == 0 ? 1.0 : 0.0);
        g.params[id + ".miter"] = Param(2.0);  // (Qt's, which the plain shape's stroke has)
        copy("dash", id + ".dash"), copy("gap", id + ".gap");
    }
    if (kind != 4 && g.p("fill", 0, 1) > 0.5) {
        const std::string id = add("fill").id;
        const double defs[3] = {0.95, 0.75, 0.1};
        for (int k = 0; k < 3; ++k) {
            const std::string c = std::string(".") + "rgb"[k];
            g.params[id + ".color" + c] = Param(g.p("fill_color" + c, 0, defs[k]));
            copy("fill_color" + c, id + ".color" + c);
            copy("fill_color2" + c, id + ".color2" + c);
        }
        copy("fill_opacity", id + ".opacity");
        const int grad = int(std::lround(g.p("gradient", 0)));
        if (grad > 0) {
            g.params[id + ".gradient"] = Param(double(grad));
            // Across the outline's box on the frame, as the plain shape's gradient is, then back into the layer.
            const QRectF box = shapeOutline(g, 0, 0, 0, 1).boundingRect();
            const QPointF c = box.center();
            QPointF from = c, to = c + QPointF(std::max(box.width(), box.height()) / 2, 0);
            if (grad != 2) {
                const double a = g.p("gradient_angle", 0) * kPi / 180;
                const QPointF dir(std::cos(a), std::sin(a));
                const double half = (std::fabs(dir.x()) * box.width() + std::fabs(dir.y()) * box.height()) / 2;
                from = c - dir * half, to = c + dir * half;
            }
            QTransform layer;
            layer.translate(g.p("pos_x", 0), g.p("pos_y", 0));
            layer.rotate(g.p("rotation", 0));
            const QTransform back = layer.inverted();
            from = back.map(from), to = back.map(to);
            g.params[id + ".start_x"] = Param(from.x()), g.params[id + ".start_y"] = Param(from.y());
            g.params[id + ".end_x"] = Param(to.x()), g.params[id + ".end_y"] = Param(to.y());
        }
    }
    setShapeContents(g, items);
    return items;
}

// ---- SVG ----

namespace {

struct SvgNode {
    QString name;
    std::map<QString, QString> attrs;
    std::vector<SvgNode> children;
    QString text;
};

bool readSvgTree(QXmlStreamReader& xr, SvgNode& node, int depth = 0) {
    for (const QXmlStreamAttribute& a : xr.attributes()) node.attrs[a.qualifiedName().toString()] = a.value().toString();
    while (!xr.atEnd()) {
        xr.readNext();
        if (xr.isStartElement()) {
            if (depth >= 256) {
                xr.raiseError(QStringLiteral("elements nested more than 256 deep"));
                return false;
            }
            SvgNode c;
            c.name = xr.name().toString();
            if (!readSvgTree(xr, c, depth + 1)) return false;
            node.children.push_back(std::move(c));
        } else if (xr.isCharacters()) {
            node.text += xr.text();
        } else if (xr.isEndElement()) {
            return true;
        }
    }
    return !xr.hasError();
}

// The properties that pass down to children, and those an element sets for itself.
using Style = std::map<QString, QString>;

Style styleOf(const SvgNode& n) {
    Style s;
    static const char* const props[] = {"fill", "stroke", "fill-opacity", "stroke-opacity", "stroke-width", "stroke-linecap", "stroke-linejoin",
                                        "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset", "fill-rule", "opacity", "display",
                                        "visibility", "stop-color", "stop-opacity", "filter", "mask", "clip-path"};
    for (const char* p : props)
        if (auto it = n.attrs.find(p); it != n.attrs.end()) s[p] = it->second.trimmed();
    if (auto it = n.attrs.find("style"); it != n.attrs.end())
        for (const QString& decl : it->second.split(';', Qt::SkipEmptyParts)) {
            const int colon = decl.indexOf(':');
            if (colon > 0) s[decl.left(colon).trimmed()] = decl.mid(colon + 1).trimmed();
        }
    return s;
}

std::vector<double> numbersIn(const QString& s) {
    static const QRegularExpression re(QStringLiteral(R"([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)"));
    std::vector<double> out;
    for (auto m = re.globalMatch(s); m.hasNext();) out.push_back(m.next().captured().toDouble());
    return out;
}

double lengthOf(const QString& s, double whole = 0, double def = 0) {
    const QString v = s.trimmed();
    if (v.isEmpty()) return def;
    const std::vector<double> n = numbersIn(v);
    if (n.empty()) return def;
    if (v.endsWith('%')) return n[0] / 100 * whole;
    if (v.endsWith("mm")) return n[0] * 96 / 25.4;
    if (v.endsWith("cm")) return n[0] * 96 / 2.54;
    if (v.endsWith("in")) return n[0] * 96;
    if (v.endsWith("pt")) return n[0] * 96 / 72;
    if (v.endsWith("pc")) return n[0] * 16;
    return n[0];
}

QTransform transformOf(const QString& s) {
    QTransform out;
    static const QRegularExpression re(QStringLiteral(R"((matrix|translate|scale|rotate|skewX|skewY)\s*\(([^)]*)\))"));
    for (auto m = re.globalMatch(s); m.hasNext();) {
        const auto mm = m.next();
        const QString op = mm.captured(1);
        const std::vector<double> a = numbersIn(mm.captured(2));
        QTransform t;
        if (op == "matrix" && a.size() == 6) t = QTransform(a[0], a[1], a[2], a[3], a[4], a[5]);
        else if (op == "translate" && !a.empty()) t.translate(a[0], a.size() > 1 ? a[1] : 0);
        else if (op == "scale" && !a.empty()) t.scale(a[0], a.size() > 1 ? a[1] : a[0]);
        else if (op == "rotate" && !a.empty()) {
            if (a.size() >= 3) t.translate(a[1], a[2]), t.rotate(a[0]), t.translate(-a[1], -a[2]);
            else t.rotate(a[0]);
        } else if (op == "skewX" && !a.empty()) t = QTransform(1, 0, std::tan(a[0] * kPi / 180), 1, 0, 0);
        else if (op == "skewY" && !a.empty()) t = QTransform(1, std::tan(a[0] * kPi / 180), 0, 1, 0, 0);
        out = t * out;  // (the rightmost applies first)
    }
    return out;
}

// An elliptical arc (SVG's endpoint form) as cubic Béziers added to q.
void arcTo(QPainterPath& q, QPointF from, double rx, double ry, double phiDeg, bool large, bool sweep, QPointF to) {
    if (from == to) return;
    rx = std::fabs(rx), ry = std::fabs(ry);
    if (rx < 1e-12 || ry < 1e-12) {
        q.lineTo(to);
        return;
    }
    const double phi = phiDeg * kPi / 180, cp = std::cos(phi), sp = std::sin(phi);
    const double dx = (from.x() - to.x()) / 2, dy = (from.y() - to.y()) / 2;
    const double x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
    const double lambda = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
    if (lambda > 1) rx *= std::sqrt(lambda), ry *= std::sqrt(lambda);
    const double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1, den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double k = den > 0 ? std::sqrt(std::max(0.0, num / den)) : 0;
    if (large == sweep) k = -k;
    const double cx1 = k * rx * y1 / ry, cy1 = -k * ry * x1 / rx;
    const double cx = cp * cx1 - sp * cy1 + (from.x() + to.x()) / 2, cy = sp * cx1 + cp * cy1 + (from.y() + to.y()) / 2;
    auto ang = [](double ux, double uy, double vx, double vy) {
        const double a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
        return a;
    };
    const double t1 = ang(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
    double dt = ang((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
    if (!sweep && dt > 0) dt -= 2 * kPi;
    if (sweep && dt < 0) dt += 2 * kPi;
    const int n = std::max(1, int(std::ceil(std::fabs(dt) / (kPi / 2) - 1e-9)));
    const double step = dt / n, h = 4.0 / 3 * std::tan(step / 4);
    auto point = [&](double a) { return QPointF(cx + rx * std::cos(a) * cp - ry * std::sin(a) * sp, cy + rx * std::cos(a) * sp + ry * std::sin(a) * cp); };
    auto deriv = [&](double a) { return QPointF(-rx * std::sin(a) * cp - ry * std::cos(a) * sp, -rx * std::sin(a) * sp + ry * std::cos(a) * cp); };
    for (int i = 0; i < n; ++i) {
        const double a0 = t1 + step * i, a1 = a0 + step;
        const QPointF p0 = point(a0), p3 = i == n - 1 ? to : point(a1);
        q.cubicTo(p0 + deriv(a0) * h, p3 - deriv(a1) * h, p3);
    }
}

QPainterPath pathData(const QString& d) {
    QPainterPath q;
    // Tokens: commands and numbers (flags may run together, as "a10 10 0 01 5 5").
    std::vector<std::pair<QChar, std::vector<double>>> cmds;
    static const QRegularExpression number(QStringLiteral(R"([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)"));
    static const QString letters = QStringLiteral("MmZzLlHhVvCcSsQqTtAa");
    for (qsizetype at = 0; at < d.size();) {
        const QChar ch = d[at];
        if (letters.contains(ch)) {
            cmds.push_back({ch, {}});
            ++at;
            continue;
        }
        // (an arc's two flags are a digit each, whatever follows)
        if (!cmds.empty() && cmds.back().first.toUpper() == 'A' && (ch == '0' || ch == '1') &&
            (cmds.back().second.size() % 7 == 3 || cmds.back().second.size() % 7 == 4)) {
            cmds.back().second.push_back(ch == '1' ? 1 : 0);
            ++at;
            continue;
        }
        const QRegularExpressionMatch m = number.match(d, at, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        if (!m.hasMatch()) {
            ++at;
            continue;
        }
        if (!cmds.empty()) cmds.back().second.push_back(m.captured().toDouble());
        at = m.capturedEnd();
    }
    QPointF cur, start, lastCtrl;
    QChar prev;
    for (auto& [c, args] : cmds) {
        const bool rel = c.isLower();
        const QChar C = c.toUpper();
        auto P = [&](double x, double y) { return rel ? cur + QPointF(x, y) : QPointF(x, y); };
        size_t i = 0;
        auto has = [&](size_t n) { return i + n <= args.size(); };
        if (C == 'Z') {
            q.closeSubpath();
            cur = start;
            prev = C;
            continue;
        }
        bool first = true;
        do {
            if (C == 'M' && has(2)) {
                const QPointF p = P(args[i], args[i + 1]);
                i += 2;
                if (first) q.moveTo(p), start = p;
                else q.lineTo(p);
                cur = p;
            } else if (C == 'L' && has(2)) {
                cur = P(args[i], args[i + 1]), i += 2;
                q.lineTo(cur);
            } else if (C == 'H' && has(1)) {
                cur = QPointF(rel ? cur.x() + args[i] : args[i], cur.y()), i += 1;
                q.lineTo(cur);
            } else if (C == 'V' && has(1)) {
                cur = QPointF(cur.x(), rel ? cur.y() + args[i] : args[i]), i += 1;
                q.lineTo(cur);
            } else if (C == 'C' && has(6)) {
                const QPointF c1 = P(args[i], args[i + 1]), c2 = P(args[i + 2], args[i + 3]), e = P(args[i + 4], args[i + 5]);
                i += 6;
                q.cubicTo(c1, c2, e), lastCtrl = c2, cur = e;
            } else if (C == 'S' && has(4)) {
                const QPointF c1 = (prev == 'C' || prev == 'S') ? cur * 2 - lastCtrl : cur;
                const QPointF c2 = P(args[i], args[i + 1]), e = P(args[i + 2], args[i + 3]);
                i += 4;
                q.cubicTo(c1, c2, e), lastCtrl = c2, cur = e;
            } else if (C == 'Q' && has(4)) {
                const QPointF c1 = P(args[i], args[i + 1]), e = P(args[i + 2], args[i + 3]);
                i += 4;
                q.quadTo(c1, e), lastCtrl = c1, cur = e;
            } else if (C == 'T' && has(2)) {
                const QPointF c1 = (prev == 'Q' || prev == 'T') ? cur * 2 - lastCtrl : cur;
                const QPointF e = P(args[i], args[i + 1]);
                i += 2;
                q.quadTo(c1, e), lastCtrl = c1, cur = e;
            } else if (C == 'A' && has(7)) {
                const QPointF e = P(args[i + 5], args[i + 6]);
                arcTo(q, cur, args[i], args[i + 1], args[i + 2], args[i + 3] != 0, args[i + 4] != 0, e);
                i += 7;
                cur = e;
            } else {
                break;
            }
            prev = C == 'M' ? QChar('L') : C;
            first = false;
        } while (i < args.size());
        if (C == 'M') prev = 'M';
    }
    return q;
}

struct SvgGradient {
    bool radial = false, userSpace = false;
    std::map<QString, QString> attrs;
    std::vector<std::array<double, 5>> stops;
    QTransform transform;
    int spread = 0;
};

class SvgImporter {
public:
    SvgImporter(Effect& g, std::vector<std::string>* notes) : g_(g), notes_(notes) {}

    void gather(const SvgNode& n) {
        if (auto it = n.attrs.find("id"); it != n.attrs.end()) ids_[it->second] = &n;
        for (const SvgNode& c : n.children) gather(c);
    }

    void note(const std::string& what) {
        if (notes_ && seen_.insert(what).second) notes_->push_back(what);
    }

    // The gradient an url(#id) names, its stops and settings following any it refers to.
    bool gradient(const QString& id, SvgGradient& out, int depth = 0) {
        auto it = ids_.find(id);
        if (it == ids_.end() || depth > 8) return false;
        const SvgNode& n = *it->second;
        if (n.name != "linearGradient" && n.name != "radialGradient") {
            if (n.name == "pattern") note("Pattern fills (drawn with their first colour, or none)");
            return false;
        }
        QString href = n.attrs.count("href") ? n.attrs.at("href") : n.attrs.count("xlink:href") ? n.attrs.at("xlink:href") : QString();
        if (href.startsWith('#')) gradient(href.mid(1), out, depth + 1);
        out.radial = n.name == "radialGradient";
        for (const auto& [k, v] : n.attrs) out.attrs[k] = v;
        if (n.attrs.count("gradientUnits")) out.userSpace = n.attrs.at("gradientUnits") == "userSpaceOnUse";
        if (n.attrs.count("gradientTransform")) out.transform = transformOf(n.attrs.at("gradientTransform"));
        if (n.attrs.count("spreadMethod")) out.spread = n.attrs.at("spreadMethod") == "reflect" ? 1 : n.attrs.at("spreadMethod") == "repeat" ? 2 : 0;
        std::vector<std::array<double, 5>> stops;
        double last = 0;
        for (const SvgNode& s : n.children) {
            if (s.name != "stop") continue;
            const Style st = styleOf(s);
            const QString off = s.attrs.count("offset") ? s.attrs.at("offset") : QString("0");
            double o = numbersIn(off).empty() ? 0 : numbersIn(off)[0];
            if (off.contains('%')) o /= 100;
            o = std::max(last, std::clamp(o, 0.0, 1.0));
            last = o;
            QColor c = st.count("stop-color") ? parseColour(st.at("stop-color")) : QColor(Qt::black);
            double a = c.isValid() ? c.alphaF() : 1;
            if (st.count("stop-opacity")) a *= std::clamp(numbersIn(st.at("stop-opacity")).empty() ? 1.0 : numbersIn(st.at("stop-opacity"))[0], 0.0, 1.0);
            if (!c.isValid()) c = Qt::black;
            stops.push_back({o, c.redF(), c.greenF(), c.blueF(), a});
        }
        if (!stops.empty()) out.stops = stops;
        return true;
    }

    static QColor parseColour(const QString& s0) {
        const QString s = s0.trimmed();
        if (s.startsWith("rgb")) {
            const std::vector<double> n = numbersIn(s);
            if (n.size() < 3) return {};
            const bool pctv = s.contains('%');
            auto ch = [&](double v) { return std::clamp(pctv ? v / 100 : v / 255, 0.0, 1.0); };
            return QColor::fromRgbF(float(ch(n[0])), float(ch(n[1])), float(ch(n[2])), n.size() > 3 ? float(std::clamp(n[3], 0.0, 1.0)) : 1.0f);
        }
        return QColor::fromString(s);
    }

    std::string id(const std::string& type) {
        for (;;)
            if (const std::string i = type + std::to_string(++counter_); !used_.count(i)) {
                used_.insert(i);
                return i;
            }
    }
    void set(const std::string& item, const char* name, double v) { g_.params[item + "." + name] = Param(v); }
    void setColour(const std::string& item, const char* name, const QColor& c) {
        set(item, (std::string(name) + ".r").c_str(), c.redF());
        set(item, (std::string(name) + ".g").c_str(), c.greenF());
        set(item, (std::string(name) + ".b").c_str(), c.blueF());
    }

    // A paint (fill or stroke) from its value: a colour, a gradient, or nothing.
    bool paint(const QString& value, double opacity, const QRectF& box, ShapeItem& item) {
        const QString v = value.trimmed();
        if (v.isEmpty() || v == "none" || v == "transparent") return false;
        if (v.startsWith("url(")) {
            const int a = v.indexOf('#'), b = v.indexOf(')');
            SvgGradient gr;
            if (a < 0 || b < a || !gradient(v.mid(a + 1, b - a - 1).trimmed(), gr) || gr.stops.empty()) {
                // A fallback colour after the url, else nothing.
                const QString rest = b > 0 ? v.mid(b + 1).trimmed() : QString();
                return !rest.isEmpty() && paint(rest, opacity, box, item);
            }
            item.stops = gr.stops;
            item.spread = gr.spread;
            // (in the user's units, or as shares of the shape's box)
            auto len = [&](const char* k, double def, double whole) {
                return gr.attrs.count(k) ? lengthOf(gr.attrs.at(k), gr.userSpace ? whole : 1, def) : def;
            };
            const double W = gr.userSpace ? viewW_ : 1, H = gr.userSpace ? viewH_ : 1;
            if (gr.radial) {
                const double cx = len("cx", 0.5 * W, W), cy = len("cy", 0.5 * H, H), r = len("r", 0.5 * std::hypot(W, H) / std::sqrt(2.0), std::hypot(W, H) / std::sqrt(2.0));
                set(item.id, "gradient", 2);
                set(item.id, "start_x", cx), set(item.id, "start_y", cy), set(item.id, "end_x", cx + r), set(item.id, "end_y", cy);
                if (gr.attrs.count("fx") || gr.attrs.count("fy")) {
                    item.hasFocal = true;
                    item.focalX = len("fx", cx, W), item.focalY = len("fy", cy, H);
                }
            } else {
                set(item.id, "gradient", 1);
                set(item.id, "start_x", len("x1", 0, W)), set(item.id, "start_y", len("y1", 0, H));
                set(item.id, "end_x", len("x2", W, W)), set(item.id, "end_y", len("y2", 0, H));
            }
            QTransform m = gr.transform;
            if (!gr.userSpace) m = m * QTransform(box.width(), 0, 0, box.height(), box.x(), box.y());
            if (!m.isIdentity()) {
                item.hasGradientMatrix = true;
                item.gradientMatrix = {m.m11(), m.m12(), m.m21(), m.m22(), m.dx(), m.dy()};
            }
            set(item.id, "opacity", std::clamp(opacity, 0.0, 1.0) * 100);
            return true;
        }
        QColor c = v == "currentColor" ? QColor(Qt::black) : parseColour(v);
        if (!c.isValid()) return false;
        setColour(item.id, "color", c);
        if (std::clamp(opacity * c.alphaF(), 0.0, 1.0) < 1) set(item.id, "opacity", std::clamp(opacity * c.alphaF(), 0.0, 1.0) * 100);
        return true;
    }

    double num(const Style& st, const char* k, double def) {
        auto it = st.find(k);
        if (it == st.end()) return def;
        const std::vector<double> n = numbersIn(it->second);
        if (n.empty()) return def;
        return it->second.endsWith('%') ? n[0] / 100 : n[0];
    }

    // An element and what is in it, as items added to `out` (inheriting `inherited`).
    void element(const SvgNode& n, const Style& inherited, std::vector<ShapeItem>& out, int depth) {
        // (each element a group, so no deeper than a layer's groups are drawn; and only so much of it, as <use> can
        // copy copies)
        if (depth >= kShapeDepth) return note("Elements nested more than " + std::to_string(kShapeDepth) + " deep (left out)");
        if (++elements_ > 50000) {
            tooBig_ = true;
            return;
        }
        Style st = inherited;
        const Style own = styleOf(n);
        for (const auto& [k, v] : own)
            if (k != "opacity" && k != "display") st[k] = v;
        if (own.count("display") && own.at("display") == "none") return;
        const QString& name = n.name;
        static const std::set<QString> skipped{"defs", "linearGradient", "radialGradient", "pattern", "clipPath", "mask", "symbol", "marker",
                                               "metadata", "title", "desc", "filter", "style", "script"};
        if (name == "style") note("Stylesheets (styles in <style> are not read; attributes and style= are)");
        if (skipped.count(name)) return;
        if (name == "text") return note("Text (add a title for it)");
        if (name == "image") return note("Pictures inside the SVG (place the picture as a clip)");
        if (name == "foreignObject" || name == "switch") return note("Embedded content (foreignObject, switch)");
        if (own.count("filter") && own.at("filter") != "none") note("Filters (the shapes are drawn without them)");
        if (own.count("mask") && own.at("mask") != "none") note("Masks (the shapes are drawn unmasked)");
        if (own.count("clip-path") && own.at("clip-path") != "none") note("Clipping paths (the shapes are drawn unclipped)");

        ShapeItem group;
        group.type = "group";
        group.id = id("group");
        if (n.attrs.count("id")) group.name = n.attrs.at("id").toStdString();
        else group.name = name.toStdString();
        QTransform m = n.attrs.count("transform") ? transformOf(n.attrs.at("transform")) : QTransform();
        if (name == "svg" && depth > 0) m = QTransform::fromTranslate(lengthOf(n.attrs.count("x") ? n.attrs.at("x") : QString()),
                                                                      lengthOf(n.attrs.count("y") ? n.attrs.at("y") : QString())) * m;
        if (name == "use") {
            const QString href = n.attrs.count("href") ? n.attrs.at("href") : n.attrs.count("xlink:href") ? n.attrs.at("xlink:href") : QString();
            m = QTransform::fromTranslate(lengthOf(n.attrs.count("x") ? n.attrs.at("x") : QString()), lengthOf(n.attrs.count("y") ? n.attrs.at("y") : QString())) * m;
            if (href.startsWith('#') && ids_.count(href.mid(1))) {
                const SvgNode& target = *ids_.at(href.mid(1));
                // (a <use> inside what it copies would copy itself for ever)
                if (!using_.insert(&target).second) return note("A <use> that copies itself (left out)");
                if (target.name == "symbol") {
                    for (const SvgNode& c : target.children) element(c, st, group.items, depth + 1);
                } else {
                    element(target, st, group.items, depth + 1);
                }
                using_.erase(&target);
            }
        }
        if (!m.isIdentity()) {
            group.hasMatrix = true;
            group.matrix = {m.m11(), m.m12(), m.m21(), m.m22(), m.dx(), m.dy()};
        }
        const double opacity = own.count("opacity") ? std::clamp(num(own, "opacity", 1), 0.0, 1.0) : 1.0;
        if (opacity < 1) set(group.id, "opacity", opacity * 100);

        if (name == "g" || name == "svg" || name == "a") {
            for (const SvgNode& c : n.children) element(c, st, group.items, depth + 1);
        }
        // (SVG paints later elements over earlier ones; here the first item is on top)
        if (name == "g" || name == "svg" || name == "a" || name == "use") {
            std::reverse(group.items.begin(), group.items.end());
        } else {
            QPainterPath q;
            auto A = [&](const char* k, double whole = 0) { return lengthOf(n.attrs.count(k) ? n.attrs.at(k) : QString(), whole); };
            if (name == "path") {
                q = pathData(n.attrs.count("d") ? n.attrs.at("d") : QString());
            } else if (name == "rect") {
                const double x = A("x", viewW_), y = A("y", viewH_), w = A("width", viewW_), h = A("height", viewH_);
                double rx = n.attrs.count("rx") ? A("rx", viewW_) : -1, ry = n.attrs.count("ry") ? A("ry", viewH_) : -1;
                if (rx < 0) rx = std::max(0.0, ry);
                if (ry < 0) ry = rx;
                rx = std::min(rx, w / 2), ry = std::min(ry, h / 2);
                if (w > 0 && h > 0) {
                    if (rx > 0 && ry > 0) q.addRoundedRect(QRectF(x, y, w, h), rx, ry);
                    else q.addRect(QRectF(x, y, w, h)), q.closeSubpath();
                }
            } else if (name == "circle" || name == "ellipse") {
                const double cx = A("cx", viewW_), cy = A("cy", viewH_);
                const double rx = name == "circle" ? A("r", std::hypot(viewW_, viewH_) / std::sqrt(2.0)) : A("rx", viewW_);
                const double ry = name == "circle" ? rx : A("ry", viewH_);
                if (rx > 0 && ry > 0) {
                    const BPath e = ellipsePath(cx, cy, rx, ry);
                    q = toPainterPath(e);
                }
            } else if (name == "line") {
                q.moveTo(A("x1", viewW_), A("y1", viewH_));
                q.lineTo(A("x2", viewW_), A("y2", viewH_));
            } else if (name == "polyline" || name == "polygon") {
                const std::vector<double> p = numbersIn(n.attrs.count("points") ? n.attrs.at("points") : QString());
                for (size_t i = 0; i + 1 < p.size(); i += 2) {
                    if (i == 0) q.moveTo(p[0], p[1]);
                    else q.lineTo(p[i], p[i + 1]);
                }
                if (name == "polygon" && p.size() >= 4) q.closeSubpath();
            } else {
                note(("Elements not read: <" + name + ">").toStdString());
                return;
            }
            if (q.isEmpty()) return;
            // Its subpaths as paths, then its stroke above its fill (as SVG paints them).
            for (const BPath& b : fromPainterPathKeepingOpen(q)) {
                if ((points_ += double(b.v.size())) > 1e5) {  // (each point is six parameters, copied with each undo step)
                    tooBig_ = true;
                    return;
                }
                ShapeItem path;
                path.type = "path";
                path.id = id("path");
                path.closed = b.closed;
                setShapePathPoints(g_, path.id, 0, b.v);
                group.items.push_back(std::move(path));
            }
            const QRectF box = q.boundingRect();
            const bool hidden = st.count("visibility") && (st.at("visibility") == "hidden" || st.at("visibility") == "collapse");
            if (!hidden) {
                ShapeItem stroke;
                stroke.type = "stroke";
                stroke.id = id("stroke");
                const double so = num(st, "stroke-opacity", 1);
                if (st.count("stroke") && paint(st.at("stroke"), so, box, stroke)) {
                    set(stroke.id, "width", std::max(0.0, lengthOf(st.count("stroke-width") ? st.at("stroke-width") : QString("1"), std::hypot(viewW_, viewH_) / std::sqrt(2.0), 1)));
                    const QString cap = st.count("stroke-linecap") ? st.at("stroke-linecap") : QString("butt");
                    set(stroke.id, "cap", cap == "round" ? 1 : cap == "square" ? 2 : 0);
                    const QString join = st.count("stroke-linejoin") ? st.at("stroke-linejoin") : QString("miter");
                    set(stroke.id, "join", join == "round" ? 1 : join == "bevel" ? 2 : 0);
                    set(stroke.id, "miter", std::clamp(num(st, "stroke-miterlimit", 4), 1.0, 100.0));
                    if (st.count("stroke-dasharray") && st.at("stroke-dasharray") != "none") {
                        std::vector<double> dashes = numbersIn(st.at("stroke-dasharray"));
                        if (dashes.size() >= 2) {
                            if (dashes.size() > 2) note("Dash patterns of more than one dash and gap (the first pair is used)");
                            set(stroke.id, "dash", dashes[0]), set(stroke.id, "gap", dashes[1]);
                        } else if (dashes.size() == 1 && dashes[0] > 0) {
                            set(stroke.id, "dash", dashes[0]), set(stroke.id, "gap", dashes[0]);
                        }
                        set(stroke.id, "dash_offset", num(st, "stroke-dashoffset", 0));
                    }
                    group.items.push_back(std::move(stroke));
                }
                ShapeItem fill;
                fill.type = "fill";
                fill.id = id("fill");
                const QString fv = st.count("fill") ? st.at("fill") : QString("black");
                if (paint(fv, num(st, "fill-opacity", 1), box, fill)) {
                    if (st.count("fill-rule") && st.at("fill-rule") == "evenodd") set(fill.id, "rule", 1);
                    group.items.push_back(std::move(fill));
                }
            }
        }
        if (group.items.empty()) return;
        out.push_back(std::move(group));
    }

    // Subpaths as paths, open ones kept open (a closing segment SVG draws is a closed path).
    static Paths fromPainterPathKeepingOpen(const QPainterPath& q) { return fromPainterPath(q); }

    double viewW_ = 0, viewH_ = 0;
    bool tooBig_ = false;

private:
    std::set<const SvgNode*> using_;
    long elements_ = 0;
    double points_ = 0;
    Effect& g_;
    std::vector<std::string>* notes_;
    std::map<QString, const SvgNode*> ids_;
    std::set<std::string> seen_, used_;
    int counter_ = 0;
};

}  // namespace

bool importSvgShapes(const std::string& file, Effect& g, double& width, double& height, std::vector<std::string>* notes, std::string* error) {
    QFile f(QString::fromStdString(file));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "Cannot open " + file;
        return false;
    }
    QXmlStreamReader xr(&f);
    SvgNode root;
    while (!xr.atEnd() && !xr.isStartElement()) xr.readNext();
    if (!xr.isStartElement() || xr.name() != QLatin1String("svg")) {
        if (error) *error = file + " is not an SVG file";
        return false;
    }
    root.name = "svg";
    if (!readSvgTree(xr, root)) {
        if (error) *error = file + ": " + xr.errorString().toStdString();
        return false;
    }
    // Its size and what of its drawing that shows (the viewBox, fitted in the middle, as preserveAspectRatio's
    // default xMidYMid meet does).
    const std::vector<double> vb = numbersIn(root.attrs.count("viewBox") ? root.attrs.at("viewBox") : QString());
    double vx = 0, vy = 0, vw = 0, vh = 0;
    if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0) vx = vb[0], vy = vb[1], vw = vb[2], vh = vb[3];
    width = root.attrs.count("width") && !root.attrs.at("width").contains('%') ? lengthOf(root.attrs.at("width")) : vw;
    height = root.attrs.count("height") && !root.attrs.at("height").contains('%') ? lengthOf(root.attrs.at("height")) : vh;
    if (width <= 0 && height > 0 && vw > 0) width = height * vw / vh;
    if (height <= 0 && width > 0 && vh > 0) height = width * vh / vw;
    if (width <= 0 || height <= 0) width = 300, height = 150;
    if (vw <= 0 || vh <= 0) vw = width, vh = height;

    // (made on a copy, so `g` is as it was when the file cannot be made into shapes)
    Effect work = g;
    for (const ShapeItem& it : shapeContents(work)) eraseShapeItemParams(work, it);
    SvgImporter im(work, notes);
    im.viewW_ = vw, im.viewH_ = vh;
    im.gather(root);
    std::vector<ShapeItem> items;
    Style none;
    im.element(root, none, items, 0);
    if (im.tooBig_) {
        if (error) *error = file + " is too large to make into shapes (over 50,000 elements or 100,000 points)";
        return false;
    }
    if (items.empty()) {
        if (error) *error = file + " has nothing that can be drawn as shapes";
        return false;
    }
    // The drawing's own group: its viewBox into its size, the middle at the layer's middle.
    const double k = std::min(width / vw, height / vh);
    const QTransform fit = QTransform::fromTranslate(-vx - vw / 2, -vy - vh / 2) * QTransform::fromScale(k, k);
    ShapeItem& top = items.front();
    const QTransform own = top.hasMatrix ? QTransform(top.matrix[0], top.matrix[1], top.matrix[2], top.matrix[3], top.matrix[4], top.matrix[5]) : QTransform();
    const QTransform m = own * fit;
    top.hasMatrix = true;
    top.matrix = {m.m11(), m.m12(), m.m21(), m.m22(), m.dx(), m.dy()};
    top.name = QFileInfo(QString::fromStdString(file)).completeBaseName().toStdString();
    setShapeContents(work, items);
    g = std::move(work);
    return true;
}

}  // namespace montage
