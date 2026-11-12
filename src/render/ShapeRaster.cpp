#include "ShapeRaster.h"

#include <QBrush>
#include <QGradient>
#include <QPainterPathStroker>
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>


namespace montage {

namespace {

struct Seg {
    double x0, y0, x1, y1;
};

// One path to fill (a stroke's outline, or a fill's path), in drawing order, with the groups it is drawn inside.
struct Fill {
    int paint = 0;
    bool evenOdd = false;
    std::vector<Seg> segs;
    std::vector<int> chain;  // the groups round it, outermost first
};

struct Builder {
    std::vector<std::array<float, 4>> paints;  // 5 texels a paint
    std::vector<std::array<float, 4>> stops;   // 2 texels a stop
    std::vector<Fill> fills;
    std::vector<float> groupOpacity;
    int depth = 0;
};

void addLine(std::vector<Seg>& out, const QPointF& a, const QPointF& b) {
    if (!std::isfinite(a.x()) || !std::isfinite(a.y()) || !std::isfinite(b.x()) || !std::isfinite(b.y())) return;
    if (a.y() != b.y()) out.push_back({a.x(), a.y(), b.x(), b.y()});
}

// The path's outline as lines in the frame's pixels: curves cut where they bend by more than a fiftieth of a pixel
// (Wang's bound), each subpath closed as filling closes it.
void flatten(const QPainterPath& path, const QTransform& m, std::vector<Seg>& out) {
    QPointF start, cur;
    bool open = false;
    auto close = [&] {
        if (open) addLine(out, cur, start);
        open = false;
    };
    const int n = path.elementCount();
    for (int i = 0; i < n; ++i) {
        const QPainterPath::Element e = path.elementAt(i);
        const QPointF p = m.map(QPointF(e.x, e.y));
        if (e.type == QPainterPath::MoveToElement) {
            close();
            start = cur = p;
            open = true;
        } else if (e.type == QPainterPath::LineToElement) {
            if (!open) start = cur, open = true;
            addLine(out, cur, p);
            cur = p;
        } else if (e.type == QPainterPath::CurveToElement && i + 2 < n) {
            if (!open) start = cur, open = true;
            const QPointF c1 = p, c2 = m.map(QPointF(path.elementAt(i + 1).x, path.elementAt(i + 1).y));
            const QPointF to = m.map(QPointF(path.elementAt(i + 2).x, path.elementAt(i + 2).y));
            i += 2;
            const QPointF d1 = cur - 2 * c1 + c2, d2 = c1 - 2 * c2 + to;
            const double bend = std::max(std::hypot(d1.x(), d1.y()), std::hypot(d2.x(), d2.y()));
            const int steps = std::isfinite(bend) ? std::clamp(int(std::ceil(std::sqrt(0.75 * bend / 0.02))), 1, 1024) : 1;
            QPointF last = cur;
            for (int k = 1; k <= steps; ++k) {
                const double t = double(k) / steps, u = 1 - t;
                const QPointF q = k == steps ? to : u * u * u * cur + 3 * u * u * t * c1 + 3 * u * t * t * c2 + t * t * t * to;
                addLine(out, last, q);
                last = q;
            }
            cur = to;
        }
    }
    close();
}

// The brush's colour at `opacity`, or its gradient seen from the frame's pixels; -1 for nothing drawn.
int addPaint(Builder& b, const QBrush& brush, const QTransform& m, double opacity) {
    if (opacity <= 0) return -1;
    std::array<std::array<float, 4>, 5> t{};
    const Qt::BrushStyle style = brush.style();
    if (style == Qt::SolidPattern) {
        float r, g, bl, a;
        brush.color().getRgbF(&r, &g, &bl, &a);
        const double k = double(a) * opacity;
        t[1] = {float(r * k), float(g * k), float(bl * k), float(k)};
    } else if ((style == Qt::LinearGradientPattern || style == Qt::RadialGradientPattern) && brush.gradient()) {
        const QGradient* gr = brush.gradient();
        bool invertible = false;
        const QTransform inv = (brush.transform() * m).inverted(&invertible);
        if (!invertible) return -1;
        const QGradientStops st = gr->stops();
        if (st.isEmpty()) return -1;
        t[0] = {style == Qt::LinearGradientPattern ? 1.0f : 2.0f,
                gr->spread() == QGradient::ReflectSpread ? 1.0f : gr->spread() == QGradient::RepeatSpread ? 2.0f : 0.0f, float(st.size()),
                float(b.stops.size())};
        t[1] = {float(inv.m11()), float(inv.m21()), float(inv.dx()), 0};
        t[2] = {float(inv.m12()), float(inv.m22()), float(inv.dy()), 0};
        if (style == Qt::LinearGradientPattern) {
            const auto* lg = static_cast<const QLinearGradient*>(gr);
            const QPointF a = lg->start(), d = lg->finalStop() - a;
            const double len2 = d.x() * d.x() + d.y() * d.y();
            t[3] = {float(a.x()), float(a.y()), float(d.x()), float(d.y())};
            t[4] = {float(len2 > 0 ? 1 / len2 : 0), 0, 0, 0};
        } else {
            const auto* rg = static_cast<const QRadialGradient*>(gr);
            const QPointF f = rg->focalPoint(), d = rg->center() - f;
            const double r = rg->radius();
            t[3] = {float(f.x()), float(f.y()), float(d.x()), float(d.y())};
            t[4] = {float(r), float(d.x() * d.x() + d.y() * d.y() - r * r), 0, 0};
        }
        for (const auto& [at, c] : st) {
            float r, g, bl, a;
            c.getRgbF(&r, &g, &bl, &a);
            const double k = double(a) * opacity;
            b.stops.push_back({float(at), 0, 0, 0});
            b.stops.push_back({float(r * k), float(g * k), float(bl * k), float(k)});
        }
    } else {
        return -1;
    }
    const int index = int(b.paints.size() / 5);
    for (const auto& texel : t) b.paints.push_back(texel);
    return index;
}

void walk(Builder& b, const std::vector<ShapePaint>& ops, const QTransform& to, double opacity, std::vector<int>& chain) {
    for (const ShapePaint& op : ops) {
        if (op.opacity <= 0) continue;
        const QTransform m = op.transform * to;
        if (op.isGroup) {
            b.groupOpacity.push_back(float(std::clamp(opacity * op.opacity, 0.0, 1.0)));
            chain.push_back(int(b.groupOpacity.size()) - 1);
            b.depth = std::max(b.depth, int(chain.size()));
            walk(b, op.group, m, 1.0, chain);
            chain.pop_back();
            continue;
        }
        if (op.path.isEmpty()) continue;
        Fill f;
        QPainterPath outline;
        QBrush brush;
        if (op.stroke) {
            if (op.pen.style() == Qt::NoPen || !(op.pen.widthF() > 0)) continue;
            QPainterPathStroker stroker(op.pen);
            // (its round joins and caps as fine as the curves are cut, at the size they are drawn)
            const double size = std::sqrt(std::fabs(m.determinant()));
            stroker.setCurveThreshold(std::clamp(0.02 / std::max(size, 1e-9), 1e-6, 0.25));
            outline = stroker.createStroke(op.path);
            brush = op.pen.brush();
        } else {
            outline = op.path;
            brush = op.brush;
            f.evenOdd = op.path.fillRule() == Qt::OddEvenFill;
        }
        f.paint = addPaint(b, brush, m, opacity * op.opacity);
        if (f.paint < 0) continue;
        flatten(outline, m, f.segs);
        if (f.segs.empty()) continue;
        f.chain = chain;
        b.fills.push_back(std::move(f));
    }
}

// A fill's part in one tile: the winding of what lies to the tile's left, a row at a time, and its lines in the tile.
struct Entry {
    int fill = 0;
    double backdrop[kShapeTile] = {};
    std::vector<std::array<float, 4>> lines;
};

// ---- Each pixel, as gpufilter.frag's 56 works it out (in floats, in the same order) ----

float clamp01(float v) { return v > 0.0f ? std::min(v, 1.0f) : 0.0f; }

// The signed area of the pixel (lx, ly)'s square (in the tile's pixels) that lies right of the line, down its height:
// clamp(lx + 1 - x, 0, 1) along it, in up to three pieces each straight, each worked out at its middle.
float lineCover(const float* e, float lx, float ly) {
    const float ya = std::min(e[1], e[3]), yb = std::max(e[1], e[3]);
    const float t0 = std::max(ya, ly), t1 = std::min(yb, ly + 1.0f);
    if (!(t1 > t0)) return 0.0f;
    const float k = (e[2] - e[0]) / (e[3] - e[1]);
    const float xa = e[0] + k * (t0 - e[1]), xb = e[0] + k * (t1 - e[1]);
    const float ga = lx + 1.0f - xa, d = (lx + 1.0f - xb) - ga;
    float s1 = 0.0f, s2 = 0.0f;
    if (d != 0.0f) {
        s1 = clamp01(-ga / d);
        s2 = clamp01((1.0f - ga) / d);
        if (s1 > s2) std::swap(s1, s2);
    }
    const float sum = s1 * clamp01(ga + d * (s1 * 0.5f)) + (s2 - s1) * clamp01(ga + d * ((s1 + s2) * 0.5f)) +
                      (1.0f - s2) * clamp01(ga + d * ((s2 + 1.0f) * 0.5f));
    return (e[3] > e[1] ? 1.0f : -1.0f) * (t1 - t0) * sum;
}

struct Reader {
    const float* t;
    const float* at(int i) const { return t + size_t(i) * 4; }
};

void paintAt(const Reader& r, int at, float x, float y, float out[4]) {
    const float* h = r.at(at);
    const int kind = int(h[0]);
    if (kind == 0) {
        std::copy(r.at(at + 1), r.at(at + 1) + 4, out);
        return;
    }
    const float *m1 = r.at(at + 1), *m2 = r.at(at + 2), *pts = r.at(at + 3), *k = r.at(at + 4);
    const float gx = m1[0] * x + m1[1] * y + m1[2], gy = m2[0] * x + m2[1] * y + m2[2];
    float t;
    if (kind == 1) {
        t = ((gx - pts[0]) * pts[2] + (gy - pts[1]) * pts[3]) * k[0];
    } else {
        // on the circle round focal + t (centre - focal) of radius t r
        const float qx = gx - pts[0], qy = gy - pts[1], A = k[1], B = qx * pts[2] + qy * pts[3], C = qx * qx + qy * qy;
        bool none = false;
        if (A == 0.0f) {
            none = !(B > 0.0f);
            t = none ? 0.0f : C / (2.0f * B);
        } else {
            const float disc = B * B - A * C;
            none = disc < 0.0f;
            const float s = std::sqrt(std::max(disc, 0.0f));
            t = A < 0.0f ? (B - s) / A : (B + s) / A;
            none = none || t < 0.0f;
        }
        if (none) {
            std::fill(out, out + 4, 0.0f);
            return;
        }
    }
    const int spread = int(h[1]);
    if (spread == 0) {
        t = clamp01(t);
    } else if (spread == 2) {
        t = t - std::floor(t);
    } else {
        t = t - 2.0f * std::floor(t * 0.5f);
        t = t > 1.0f ? 2.0f - t : t;
    }
    const int n = int(h[2]), s0 = int(h[3]);
    const float* c = r.at(s0 + 1);
    if (!(t > r.at(s0)[0])) {
        std::copy(c, c + 4, out);
        return;
    }
    for (int i = 1; i < n; ++i) {
        const float p0 = r.at(s0 + 2 * (i - 1))[0], p1 = r.at(s0 + 2 * i)[0];
        if (t < p1) {
            const float f = (t - p0) / (p1 - p0);
            const float *a = r.at(s0 + 2 * (i - 1) + 1), *b = r.at(s0 + 2 * i + 1);
            for (int ch = 0; ch < 4; ++ch) out[ch] = a[ch] + (b[ch] - a[ch]) * f;
            return;
        }
    }
    c = r.at(s0 + 2 * (n - 1) + 1);
    std::copy(c, c + 4, out);
}

}  // namespace

ShapeRaster shapeRaster(const std::vector<ShapePaint>& paints, const QTransform& to, double opacity, int w, int h) {
    ShapeRaster r;
    r.width = std::max(0, w), r.height = std::max(0, h);
    r.tilesX = (r.width + kShapeTile - 1) / kShapeTile, r.tilesY = (r.height + kShapeTile - 1) / kShapeTile;
    const int nTiles = r.tilesX * r.tilesY;
    Builder b;
    std::vector<int> chain;
    if (nTiles > 0 && opacity > 0) walk(b, paints, to, std::clamp(opacity, 0.0, 1.0), chain);
    r.depth = b.depth;
    const int T = kShapeTile;
    // Each fill's lines into the tiles they cross, and what lies left of a tile into its backdrop.
    std::vector<std::vector<Entry>> perTile(size_t(std::max(0, nTiles)));
    const double bottom = double(r.tilesY) * T;
    for (size_t fi = 0; fi < b.fills.size(); ++fi) {
        const Fill& f = b.fills[fi];
        std::unordered_map<int, Entry> here;
        std::unordered_map<int, std::vector<std::pair<int, std::array<double, kShapeTile>>>> deltas;  // by band: (column, rows)
        auto entry = [&](int tile) -> Entry& {
            auto [it, made] = here.try_emplace(tile);
            if (made) it->second.fill = int(fi);
            return it->second;
        };
        for (const Seg& s : f.segs) {
            const double ylo = std::min(s.y0, s.y1), yhi = std::max(s.y0, s.y1);
            if (yhi <= 0 || ylo >= bottom) continue;
            const double sign = s.y1 > s.y0 ? 1 : -1, k = (s.x1 - s.x0) / (s.y1 - s.y0);
            const int b0 = int(std::clamp(std::floor(ylo / T), 0.0, double(r.tilesY))), b1 = int(std::clamp(std::floor(yhi / T), -1.0, double(r.tilesY - 1)));
            for (int band = b0; band <= b1; ++band) {
                const double t0 = std::max(ylo, double(band) * T), t1 = std::min(yhi, double(band + 1) * T);
                if (!(t1 > t0)) continue;
                const double xa = s.x0 + k * (t0 - s.y0), xb = s.x0 + k * (t1 - s.y0);
                const double lo = std::min(xa, xb), hi = std::max(xa, xb);
                const int first = int(std::clamp(std::floor(lo / T), -1.0, double(r.tilesX)));
                const int last = int(std::clamp(std::floor(hi / T), -1.0, double(r.tilesX)));
                const double oy = double(band) * T;
                for (int col = std::max(0, first); col <= std::min(r.tilesX - 1, last); ++col) {
                    const double ox = double(col) * T;
                    // (its direction kept: down the band when it goes down)
                    entry(band * r.tilesX + col)
                        .lines.push_back(sign > 0 ? std::array<float, 4>{float(xa - ox), float(t0 - oy), float(xb - ox), float(t1 - oy)}
                                                  : std::array<float, 4>{float(xb - ox), float(t1 - oy), float(xa - ox), float(t0 - oy)});
                }
                if (last + 1 <= r.tilesX - 1) {
                    std::array<double, kShapeTile> rows{};
                    for (int y = 0; y < T; ++y) {
                        const double overlap = std::min(t1, oy + y + 1) - std::max(t0, oy + y);
                        if (overlap > 0) rows[size_t(y)] = sign * overlap;
                    }
                    deltas[band].push_back({std::max(0, last + 1), rows});
                }
            }
        }
        // The backdrops, summed along each band from the left.
        std::vector<int> bands;
        for (const auto& kv : here) bands.push_back(kv.first / r.tilesX);
        for (const auto& kv : deltas) bands.push_back(kv.first);
        std::sort(bands.begin(), bands.end());
        bands.erase(std::unique(bands.begin(), bands.end()), bands.end());
        for (int band : bands) {
            auto& ds = deltas[band];
            std::sort(ds.begin(), ds.end(), [](const auto& a, const auto& c) { return a.first < c.first; });
            double run[kShapeTile] = {};
            size_t di = 0;
            for (int col = 0; col < r.tilesX; ++col) {
                for (; di < ds.size() && ds[di].first == col; ++di)
                    for (int y = 0; y < T; ++y) run[y] += ds[di].second[size_t(y)];
                const int tile = band * r.tilesX + col;
                const bool any = std::any_of(run, run + T, [](double v) { return std::fabs(v) > 1e-9; });
                auto it = here.find(tile);
                if (it == here.end() && !any) continue;
                Entry& e = it == here.end() ? entry(tile) : it->second;
                std::copy(run, run + T, e.backdrop);
            }
        }
        std::vector<int> tiles;
        tiles.reserve(here.size());
        for (const auto& kv : here) tiles.push_back(kv.first);
        std::sort(tiles.begin(), tiles.end());
        for (int tile : tiles) perTile[size_t(tile)].push_back(std::move(here[tile]));
    }
    // The table: the tiles' places, the paints and their stops, then each tile's commands.
    const int paintBase = nTiles, stopBase = paintBase + int(b.paints.size());
    std::vector<float>& t = r.table;
    t.assign(size_t(stopBase + int(b.stops.size())) * 4, 0.0f);
    for (size_t i = 0; i < b.paints.size(); ++i) {
        std::array<float, 4> texel = b.paints[i];
        if (i % 5 == 0 && texel[0] != 0) texel[3] += float(stopBase);
        std::copy(texel.begin(), texel.end(), t.begin() + std::ptrdiff_t((paintBase + int(i)) * 4));
    }
    for (size_t i = 0; i < b.stops.size(); ++i) std::copy(b.stops[i].begin(), b.stops[i].end(), t.begin() + std::ptrdiff_t((stopBase + int(i)) * 4));
    auto push = [&](float a, float c, float d, float e) { t.insert(t.end(), {a, c, d, e}); };
    for (int tile = 0; tile < nTiles; ++tile) {
        const size_t start = t.size() / 4;
        std::vector<int> open;
        for (const Entry& e : perTile[size_t(tile)]) {
            const Fill& f = b.fills[size_t(e.fill)];
            size_t common = 0;
            while (common < open.size() && common < f.chain.size() && open[common] == f.chain[common]) ++common;
            while (open.size() > common) push(2, b.groupOpacity[size_t(open.back())], 0, 0), open.pop_back();
            for (size_t i = common; i < f.chain.size(); ++i) push(1, 0, 0, 0), open.push_back(f.chain[i]);
            push(0, float(paintBase + f.paint * 5), float(e.lines.size()), f.evenOdd ? 1 : 0);
            for (int y = 0; y < T; y += 4) push(float(e.backdrop[y]), float(e.backdrop[y + 1]), float(e.backdrop[y + 2]), float(e.backdrop[y + 3]));
            for (const auto& l : e.lines) push(l[0], l[1], l[2], l[3]);
        }
        while (!open.empty()) push(2, b.groupOpacity[size_t(open.back())], 0, 0), open.pop_back();
        // (as two halves of 4096 each, so places past 2^24 stay exact in floats)
        const size_t end = t.size() / 4;
        float* hd = t.data() + size_t(tile) * 4;
        hd[0] = float(start / 4096), hd[1] = float(start % 4096), hd[2] = float(end / 4096), hd[3] = float(end % 4096);
    }
    // (rows of the table's width)
    const size_t row = size_t(kShapeTableWidth) * 4;
    t.resize((t.size() + row - 1) / row * row, 0.0f);
    return r;
}

Image drawShapeRaster(const ShapeRaster& r) {
    Image img(r.width, r.height);
    if (r.width <= 0 || r.height <= 0 || r.table.empty()) return img;
    const Reader rd{r.table.data()};
    parallelRows(r.height, [&](int y0, int y1) {
        std::vector<std::array<float, 4>> stack;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < r.width; ++x) {
                const int tx = x / kShapeTile, ty = y / kShapeTile;
                const float* hd = rd.at(ty * r.tilesX + tx);
                int i = int(hd[0]) * 4096 + int(hd[1]);
                const int end = int(hd[2]) * 4096 + int(hd[3]);
                const float lx = float(x - tx * kShapeTile), ly = float(y - ty * kShapeTile);
                const float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
                const int row = y - ty * kShapeTile;
                std::array<float, 4> acc{0, 0, 0, 0};
                stack.clear();
                while (i < end) {
                    const float* c = rd.at(i);
                    const int type = int(c[0]);
                    if (type == 1) {
                        stack.push_back(acc);
                        acc = {0, 0, 0, 0};
                        ++i;
                        continue;
                    }
                    if (type == 2) {
                        const float o = c[1];
                        std::array<float, 4> g;
                        for (int k = 0; k < 4; ++k) g[size_t(k)] = acc[size_t(k)] * o;
                        acc = stack.empty() ? std::array<float, 4>{0, 0, 0, 0} : stack.back();
                        if (!stack.empty()) stack.pop_back();
                        for (int k = 0; k < 4; ++k) acc[size_t(k)] = g[size_t(k)] + acc[size_t(k)] * (1.0f - g[3]);
                        ++i;
                        continue;
                    }
                    const int lines = int(c[2]);
                    float wind = rd.at(i + 1 + row / 4)[row % 4];
                    for (int l = 0; l < lines; ++l) wind += lineCover(rd.at(i + 5 + l), lx, ly);
                    float cov;
                    if (c[3] > 0.5f) {
                        float a = std::fabs(wind);
                        a = a - 2.0f * std::floor(a * 0.5f);
                        cov = a > 1.0f ? 2.0f - a : a;
                    } else {
                        cov = std::min(1.0f, std::fabs(wind));
                    }
                    if (cov > 0.0f) {
                        float col[4];
                        paintAt(rd, int(c[1]), cx, cy, col);
                        const float sa = col[3] * cov;
                        for (int k = 0; k < 4; ++k) acc[size_t(k)] = col[k] * cov + acc[size_t(k)] * (1.0f - sa);
                    }
                    i += 5 + lines;
                }
                std::copy(acc.begin(), acc.end(), img.at(x, y));
            }
    });
    return img;
}

}  // namespace montage
