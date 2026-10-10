#include "Title3D.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPainterPath>
#include <QPolygonF>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <vector>

namespace montage {

namespace {

constexpr int kSuper = 2;  // samples per pixel each way
constexpr double kDeg = M_PI / 180;

struct V3 {
    double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 normalized(V3 a) {
    const double l = std::sqrt(dot(a, a));
    return l > 1e-12 ? a * (1 / l) : V3{0, 0, 1};
}

struct M3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    V3 operator*(V3 v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    M3 operator*(const M3& o) const {
        M3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
        return r;
    }
};
// Turned about X (tipping the top away), Y (the right side away) and Z (clockwise on screen), in that order.
M3 rotation(double rx, double ry, double rz) {
    const double cx = std::cos(rx * kDeg), sx = std::sin(rx * kDeg), cy = std::cos(ry * kDeg), sy = std::sin(ry * kDeg),
                 cz = std::cos(rz * kDeg), sz = std::sin(rz * kDeg);
    M3 X, Y, Z;
    X.m[1][1] = cx, X.m[1][2] = sx, X.m[2][1] = -sx, X.m[2][2] = cx;  // the top goes back (to -z) as rx grows
    Y.m[0][0] = cy, Y.m[0][2] = sy, Y.m[2][0] = -sy, Y.m[2][2] = cy;  // the right side goes back as ry grows
    Z.m[0][0] = cz, Z.m[0][1] = sz, Z.m[1][0] = -sz, Z.m[1][1] = cz;
    return Z * Y * X;
}

// The supersampled frame: colour, coverage and depth (as 1 / distance, larger nearer; 0 empty) per sample.
struct Raster {
    int W = 0, H = 0;
    std::vector<float> rgb, inv;
    Raster(int w, int h) : W(w), H(h), rgb(size_t(w) * size_t(h) * 3, 0.0f), inv(size_t(w) * size_t(h), 0.0f) {}
    void plot(int x, int y, float invd, const float* c) {
        const size_t i = size_t(y) * size_t(W) + size_t(x);
        if (invd <= inv[i]) return;
        inv[i] = invd;
        rgb[i * 3] = c[0], rgb[i * 3 + 1] = c[1], rgb[i * 3 + 2] = c[2];
    }
};

struct Screen {
    double x = 0, y = 0, inv = 0;  // pixel position, 1 / distance
    bool ok = false;
};

struct Camera {
    double f = 1, cx = 0, cy = 0, nearest = 1;
    Screen project(V3 p) const {
        const double d = -p.z;
        if (d < nearest) return {};
        return {cx + f * p.x / d, cy - f * p.y / d, 1 / d, true};
    }
};

// A triangle with a colour at each corner (linear across the screen), depth tested.
void fillTriangle(Raster& r, const Screen& a, const Screen& b, const Screen& c, const float* ca, const float* cb, const float* cc) {
    if (!a.ok || !b.ok || !c.ok) return;
    const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-9) return;
    const int x0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x})))), x1 = std::min(r.W - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y})))), y1 = std::min(r.H - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            const double w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
            const double w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
            const double w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float col[3];
            for (int k = 0; k < 3; ++k) col[k] = float(w0 * ca[k] + w1 * cb[k] + w2 * cc[k]);
            r.plot(x, y, float(w0 * a.inv + w1 * b.inv + w2 * c.inv), col);
        }
}

struct Lighting {
    V3 light;  // towards the light, camera space
    double ambient = 0.35, specular = 0.4;
    // A surface of `base` colour facing `n` at camera-space point `p`.
    void shade(V3 n, V3 p, const float* base, float* out) const {
        const V3 view = normalized(p * -1.0);
        if (dot(n, view) < 0) n = n * -1.0;  // the side facing the camera
        const double diffuse = std::max(0.0, dot(n, light));
        const double spec = specular * std::pow(std::max(0.0, dot(n, normalized(light + view))), 32);
        for (int k = 0; k < 3; ++k) out[k] = float(std::min(1.0, base[k] * (ambient + (1 - ambient) * diffuse) + spec));
    }
};

double easeOut(double p) { return 1 - std::pow(1 - std::clamp(p, 0.0, 1.0), 3); }

}  // namespace

Image renderTitle3D(const Effect& g, FrameTime t, int w, int h, double scale, FrameTime duration, double fps) {
    Image out(w, h);
    if (w <= 0 || h <= 0) return out;
    // Animation: how far in (from the start) and out (towards the end).
    fps = fps > 0 ? fps : 30;
    const double sec = double(t) / fps;
    const int inKind = int(std::lround(g.p("anim_in", t))), outKind = int(std::lround(g.p("anim_out", t)));
    const double inDur = std::max(0.01, g.p("anim_in_dur", t, 0.8)), outDur = std::max(0.01, g.p("anim_out_dur", t, 0.8));
    const double pin = inKind ? std::clamp(sec / inDur, 0.0, 1.0) : 1.0;
    const double pout = outKind && duration > 0 ? std::clamp(double(duration - 1 - t) / fps / outDur, 0.0, 1.0) : 1.0;
    double rx = g.p("rot_x", t, 10), ry = g.p("rot_y", t, -20), rz = g.p("rot_z", t), opacity = std::clamp(g.p("opacity", t, 100) / 100, 0.0, 1.0);
    double away = 0;  // pushed back, in frame heights
    auto animate = [&](int kind, double p, bool entering) {
        const double e = easeOut(p), sign = entering ? 1 : -1;
        switch (kind) {
            case 1: opacity *= e; break;                                                      // fade
            case 2: ry += sign * (1 - e) * -180; opacity *= std::min(1.0, 3 * p); break;      // spin round
            case 3: rx += sign * (1 - e) * 90; break;                                          // flip up
            case 4: ry += sign * (1 - e) * 90; break;                                          // swing in
            case 5: away += (1 - e) * 4; opacity *= std::min(1.0, 2 * p); break;              // zoom from far
            default: break;
        }
    };
    animate(inKind, pin, true);
    animate(outKind, pout, false);
    if (opacity <= 0) return out;

    const int W = w * kSuper, H = h * kSuper;
    const double px = scale * kSuper;  // raster pixels per sequence pixel
    // The text's outline, centred, in raster pixels (y down, as Qt draws).
    QFont font(QString::fromStdString(g.s("font", "Sans Serif")));
    font.setPixelSize(std::max(1, int(std::lround(g.p("size", t, 120) * px))));
    font.setBold(g.p("bold", t, 1) > 0.5);
    font.setItalic(g.p("italic", t) > 0.5);
    font.setLetterSpacing(QFont::AbsoluteSpacing, g.p("tracking", t) * px);
    font.setHintingPreference(QFont::PreferNoHinting);
    const QStringList lines = QString::fromStdString(g.s("text", "3D Title")).split('\n');
    const QFontMetricsF fm(font);
    const double lineH = fm.height() * std::clamp(g.p("line_spacing", t, 1.1), 0.5, 3.0);
    double blockW = 0;
    for (const QString& l : lines) blockW = std::max(blockW, fm.horizontalAdvance(l));
    const int align = int(std::lround(g.p("align", t, 1)));
    QPainterPath path;
    path.setFillRule(Qt::WindingFill);
    for (int i = 0; i < lines.size(); ++i) {
        const double lw = fm.horizontalAdvance(lines[i]);
        const double x = align == 0 ? 0 : align == 2 ? blockW - lw : (blockW - lw) / 2;
        path.addText(QPointF(x, fm.ascent() + i * lineH), font, lines[i]);
    }
    if (path.isEmpty()) return out;
    const QRectF box = path.boundingRect();
    const QPointF centre = box.center();
    // Contours with the side the solid lies on: an edge's outward normal (dy, -dx) in y-up coordinates points out
    // of the letter when the solid is on its left; checked once per contour against the path's own winding.
    struct Contour {
        std::vector<V3> pts;  // y up, centred
        bool flip = false;
    };
    std::vector<Contour> contours;
    for (const QPolygonF& poly : path.toSubpathPolygons()) {
        Contour c;
        for (const QPointF& q : poly) {
            const V3 v{q.x() - centre.x(), centre.y() - q.y(), 0};
            if (c.pts.empty() || std::hypot(v.x - c.pts.back().x, v.y - c.pts.back().y) > 1e-6) c.pts.push_back(v);
        }
        while (c.pts.size() > 1 && std::hypot(c.pts.front().x - c.pts.back().x, c.pts.front().y - c.pts.back().y) < 1e-6) c.pts.pop_back();
        if (c.pts.size() < 3) continue;
        size_t longest = 0;
        double best = -1;
        for (size_t i = 0; i < c.pts.size(); ++i) {
            const V3 d = c.pts[(i + 1) % c.pts.size()] - c.pts[i];
            if (dot(d, d) > best) best = dot(d, d), longest = i;
        }
        const V3 a = c.pts[longest], b = c.pts[(longest + 1) % c.pts.size()];
        const V3 n = normalized({b.y - a.y, -(b.x - a.x), 0});
        const V3 probe = (a + b) * 0.5 + n * 0.75;
        c.flip = path.contains(QPointF(probe.x + centre.x(), centre.y() - probe.y));
        contours.push_back(std::move(c));
    }

    // Placement and camera: the face at rest sits in the frame's plane at the size it was set, through a lens of
    // `fov` degrees across the frame's height.
    const double fov = std::clamp(g.p("fov", t, 35), 5.0, 120.0);
    Camera cam;
    cam.f = (H / 2.0) / std::tan(fov * kDeg / 2);
    cam.cx = W / 2.0;
    cam.cy = H / 2.0;
    cam.nearest = 1;
    const double k = std::clamp(g.p("scale", t, 100), 1.0, 2000.0) / 100;
    const double depth = std::max(0.0, g.p("depth", t, 40)) * px * k;
    const M3 R = rotation(rx, ry, rz);
    const V3 T{g.p("pos_x", t) * px, -g.p("pos_y", t) * px, -cam.f - away * H};
    auto toCamera = [&](V3 p) { return R * (p * k) + T; };
    Lighting light;
    const double az = g.p("light_angle", t, -40) * kDeg, el = std::clamp(g.p("light_height", t, 40), -90.0, 90.0) * kDeg;
    light.light = normalized({std::sin(az) * std::cos(el), std::cos(az) * std::cos(el), std::sin(el)});
    light.ambient = std::clamp(g.p("ambient", t, 35) / 100, 0.0, 1.0);
    light.specular = std::clamp(g.p("specular", t, 40) / 100, 0.0, 1.0);
    const float face[3] = {float(g.p("color.r", t, 0.95)), float(g.p("color.g", t, 0.95)), float(g.p("color.b", t, 0.95))};
    const float side[3] = {float(g.p("side_color.r", t, 0.45)), float(g.p("side_color.g", t, 0.5)), float(g.p("side_color.b", t, 0.6))};

    Raster r(W, H);
    // The sides: a quad along every edge, lit at its corners with normals smoothed where the outline curves.
    const double zf = depth / 2 / k, zb = -depth / 2 / k;  // object space (scaled by k with everything else)
    const double smooth = std::cos(35 * kDeg);
    if (depth > 0)
        for (const Contour& c : contours) {
            const size_t n = c.pts.size();
            std::vector<V3> edgeN(n);
            for (size_t i = 0; i < n; ++i) {
                const V3 a = c.pts[i], b = c.pts[(i + 1) % n];
                V3 nn = normalized({b.y - a.y, -(b.x - a.x), 0});
                edgeN[i] = c.flip ? nn * -1.0 : nn;
            }
            for (size_t i = 0; i < n; ++i) {
                const size_t j = (i + 1) % n;
                const V3 na = dot(edgeN[(i + n - 1) % n], edgeN[i]) > smooth ? normalized(edgeN[(i + n - 1) % n] + edgeN[i]) : edgeN[i];
                const V3 nb = dot(edgeN[i], edgeN[j]) > smooth ? normalized(edgeN[i] + edgeN[j]) : edgeN[i];
                const V3 aF = toCamera({c.pts[i].x, c.pts[i].y, zf}), bF = toCamera({c.pts[j].x, c.pts[j].y, zf});
                const V3 aB = toCamera({c.pts[i].x, c.pts[i].y, zb}), bB = toCamera({c.pts[j].x, c.pts[j].y, zb});
                // Facing away from the camera all along: hidden behind the rest.
                const V3 nc = R * edgeN[i];
                if (dot(nc, aF) > 0 && dot(nc, bB) > 0 && dot(nc, bF) > 0 && dot(nc, aB) > 0) continue;
                float caF[3], cbF[3], caB[3], cbB[3];
                light.shade(R * na, aF, side, caF);
                light.shade(R * nb, bF, side, cbF);
                light.shade(R * na, aB, side, caB);
                light.shade(R * nb, bB, side, cbB);
                const Screen sAF = cam.project(aF), sBF = cam.project(bF), sAB = cam.project(aB), sBB = cam.project(bB);
                fillTriangle(r, sAF, sBF, sBB, caF, cbF, cbB);
                fillTriangle(r, sAF, sBB, sAB, caF, cbB, caB);
            }
        }
    // The front and back faces: the projected outline filled by nonzero winding, each sample's depth from the plane.
    for (int face_ = 0; face_ < 2; ++face_) {
        const double z = face_ == 0 ? zf : zb;
        if (face_ == 1 && depth <= 0) break;  // flat: one face, seen from either side
        const V3 normal = R * V3{0, 0, face_ == 0 ? 1.0 : -1.0};
        const V3 onPlane = toCamera({0, 0, z});
        if (depth > 0 && dot(normal, onPlane) >= 0) continue;  // facing away
        float col[3];
        light.shade(normal, onPlane, face, col);
        struct Edge {
            double x0, y0, x1, y1;
            int dir;
        };
        std::vector<Edge> edges;
        bool visible = true;
        for (const Contour& c : contours) {
            const size_t n = c.pts.size();
            for (size_t i = 0; i < n && visible; ++i) {
                const Screen a = cam.project(toCamera({c.pts[i].x, c.pts[i].y, z}));
                const Screen b = cam.project(toCamera({c.pts[(i + 1) % n].x, c.pts[(i + 1) % n].y, z}));
                if (!a.ok || !b.ok) visible = false;  // reaching behind the camera: left out
                else if (a.y != b.y) edges.push_back(a.y < b.y ? Edge{a.x, a.y, b.x, b.y, 1} : Edge{b.x, b.y, a.x, a.y, -1});
            }
        }
        if (!visible || edges.empty()) continue;
        const double nd = dot(normal, onPlane);
        parallelRows(H, [&](int r0, int r1) {
            std::vector<std::pair<double, int>> xs;
            for (int y = r0; y < r1; ++y) {
                const double sy = y + 0.5;
                xs.clear();
                for (const Edge& e : edges)
                    if (sy >= e.y0 && sy < e.y1) xs.push_back({e.x0 + (sy - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), e.dir});
                if (xs.size() < 2) continue;
                std::sort(xs.begin(), xs.end());
                int wind = 0;
                for (size_t i = 0; i + 1 < xs.size(); ++i) {
                    wind += xs[i].second;
                    if (wind == 0) continue;
                    const int a = std::max(0, int(std::ceil(xs[i].first - 0.5))), b = std::min(W - 1, int(std::ceil(xs[i + 1].first - 0.5)) - 1);
                    for (int x = a; x <= b; ++x) {
                        // Where the sample's ray meets the face's plane.
                        const V3 ray{(x + 0.5 - cam.cx) / cam.f, -(sy - cam.cy) / cam.f, -1};
                        const double den = dot(normal, ray);
                        if (std::fabs(den) < 1e-12) continue;
                        const double dist = nd / den;
                        if (dist < cam.nearest) continue;
                        r.plot(x, y, float(1 / dist), col);
                    }
                }
            }
        });
    }
    // Down to the output: the samples averaged, premultiplied, at the title's opacity.
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                float sum[3] = {0, 0, 0}, cover = 0;
                for (int sy = 0; sy < kSuper; ++sy)
                    for (int sx = 0; sx < kSuper; ++sx) {
                        const size_t i = size_t(y * kSuper + sy) * size_t(W) + size_t(x * kSuper + sx);
                        if (r.inv[i] <= 0) continue;
                        cover += 1;
                        for (int c = 0; c < 3; ++c) sum[c] += r.rgb[i * 3 + size_t(c)];
                    }
                const float n = float(kSuper * kSuper), a = float(opacity);
                float* p = out.at(x, y);
                for (int c = 0; c < 3; ++c) p[c] = sum[c] / n * a;
                p[3] = cover / n * a;
            }
    });
    return out;
}

}  // namespace montage
