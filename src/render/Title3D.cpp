#include "Title3D.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPainterPath>
#include <QPolygonF>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <new>
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

// The supersampled frame over the part of it the title can reach: colour and depth (as 1 / distance, larger nearer;
// 0 empty) per sample. Colour is kept in 16 bits, so a full 4K frame is 330 MB rather than 530.
struct Raster {
    int X0 = 0, Y0 = 0, W = 0, H = 0;  // where it sits in the frame, and its size
    std::vector<uint16_t> rgb;
    std::vector<float> inv;
    Raster(int x0, int y0, int w, int h)
        : X0(x0), Y0(y0), W(w), H(h), rgb(size_t(w) * size_t(h) * 3, 0), inv(size_t(w) * size_t(h), 0.0f) {}
    void plot(int x, int y, float invd, const float* c) {  // frame coordinates, inside the raster
        const size_t i = size_t(y - Y0) * size_t(W) + size_t(x - X0);
        if (invd <= inv[i]) return;
        inv[i] = invd;
        for (int k = 0; k < 3; ++k) rgb[i * 3 + size_t(k)] = uint16_t(std::lround(std::clamp(c[k], 0.0f, 1.0f) * 65535.0f));
    }
};

struct Screen {
    double x = 0, y = 0, inv = 0;  // pixel position, 1 / distance
};

struct Camera {
    double f = 1, cx = 0, cy = 0, nearest = 1;
    // Only for points at least `nearest` in front (see clipNear).
    Screen project(V3 p) const {
        const double d = std::max(nearest, -p.z);
        return {cx + f * p.x / d, cy - f * p.y / d, 1 / d};
    }
};

// A corner of a polygon in camera space with its colour.
struct Corner {
    V3 p;
    float c[3] = {0, 0, 0};
};
// The part of a closed polygon at least `near` in front of the camera (Sutherland-Hodgman against that plane). A
// closed outline stays closed, so nonzero winding still fills what is left of it correctly.
std::vector<Corner> clipNear(const std::vector<Corner>& in, double near) {
    std::vector<Corner> out;
    const size_t n = in.size();
    for (size_t i = 0; i < n; ++i) {
        const Corner& a = in[i];
        const Corner& b = in[(i + 1) % n];
        const double da = -a.p.z - near, db = -b.p.z - near;
        if (da >= 0) out.push_back(a);
        if ((da >= 0) != (db >= 0)) {
            const double t = da / (da - db);
            Corner m;
            m.p = a.p + (b.p - a.p) * t;
            for (int k = 0; k < 3; ++k) m.c[k] = float(a.c[k] + (b.c[k] - a.c[k]) * t);
            out.push_back(m);
        }
    }
    return out;
}

struct Tri {
    Screen v[3];
    float c[3][3];
};

// A triangle with a colour at each corner (linear across the screen), depth tested, within the raster.
void fillTriangle(Raster& r, const Tri& t) {
    const Screen &a = t.v[0], &b = t.v[1], &c = t.v[2];
    const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-9) return;
    const int x0 = std::max(r.X0, int(std::floor(std::min({a.x, b.x, c.x})))),
              x1 = std::min(r.X0 + r.W - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    const int y0 = std::max(r.Y0, int(std::floor(std::min({a.y, b.y, c.y})))),
              y1 = std::min(r.Y0 + r.H - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            const double w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
            const double w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
            const double w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float col[3];
            for (int k = 0; k < 3; ++k) col[k] = float(w0 * t.c[0][k] + w1 * t.c[1][k] + w2 * t.c[2][k]);
            r.plot(x, y, float(w0 * a.inv + w1 * b.inv + w2 * c.inv), col);
        }
}

// Whether point (x, y) is inside polygon `pts` (even-odd).
bool insidePolygon(const std::vector<V3>& pts, double x, double y) {
    bool in = false;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        const V3 &a = pts[i], &b = pts[j];
        if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
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

    // Supersampled, but not past a 4K frame's worth of samples (finer frames have the pixels to spare).
    const int super = double(w) * h > 3840.0 * 2160 ? 1 : kSuper;
    const int W = w * super, H = h * super;
    const double px = scale * super;  // raster pixels per sequence pixel
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
    // Overlaps merged (letters run together by tight tracking or line spacing, fonts built from overlapping
    // strokes), so every contour bounds solid on one side: outlines then run anticlockwise and holes clockwise (y up),
    // and each edge's outward normal is (dy, -dx).
    const QPainterPath simple = path.simplified();
    const QRectF box = simple.boundingRect();
    const QPointF centre = box.center();
    struct Contour {
        std::vector<V3> pts;  // y up, centred
        double x0, y0, x1, y1;
        double area = 0;
    };
    std::vector<Contour> contours;
    for (const QPolygonF& poly : simple.toSubpathPolygons()) {
        Contour c;
        for (const QPointF& q : poly) {
            const V3 v{q.x() - centre.x(), centre.y() - q.y(), 0};
            if (c.pts.empty() || std::hypot(v.x - c.pts.back().x, v.y - c.pts.back().y) > 1e-6) c.pts.push_back(v);
        }
        while (c.pts.size() > 1 && std::hypot(c.pts.front().x - c.pts.back().x, c.pts.front().y - c.pts.back().y) < 1e-6) c.pts.pop_back();
        if (c.pts.size() < 3) continue;
        c.x0 = c.x1 = c.pts[0].x, c.y0 = c.y1 = c.pts[0].y;
        for (size_t i = 0; i < c.pts.size(); ++i) {
            const V3 &a = c.pts[i], &b = c.pts[(i + 1) % c.pts.size()];
            c.area += a.x * b.y - b.x * a.y;
            c.x0 = std::min(c.x0, a.x), c.x1 = std::max(c.x1, a.x), c.y0 = std::min(c.y0, a.y), c.y1 = std::max(c.y1, a.y);
        }
        if (std::fabs(c.area) < 1e-9) continue;
        contours.push_back(std::move(c));
    }
    for (size_t i = 0; i < contours.size(); ++i) {
        // A hole lies inside an odd number of other contours.
        const V3 q = contours[i].pts[0];
        int around = 0;
        for (size_t j = 0; j < contours.size(); ++j) {
            const Contour& o = contours[j];
            if (j != i && q.x >= o.x0 && q.x <= o.x1 && q.y >= o.y0 && q.y <= o.y1 && insidePolygon(o.pts, q.x, q.y)) ++around;
        }
        if ((contours[i].area > 0) == (around % 2 == 1)) std::reverse(contours[i].pts.begin(), contours[i].pts.end());
    }
    if (contours.empty()) return out;

    // Placement and camera: the face at rest sits in the frame's plane at the size it was set (the extrusion going back
    // from it, turning about its middle), through a lens of `fov` degrees across the frame's height.
    const double fov = std::clamp(g.p("fov", t, 35), 5.0, 120.0);
    Camera cam;
    cam.f = (H / 2.0) / std::tan(fov * kDeg / 2);
    cam.cx = W / 2.0;
    cam.cy = H / 2.0;
    cam.nearest = std::max(1.0, cam.f * 0.05);  // nothing nearer than this is drawn
    const double k = std::clamp(g.p("scale", t, 100), 1.0, 2000.0) / 100;
    const double depth = std::max(0.0, g.p("depth", t, 40)) * px * k;
    const M3 R = rotation(rx, ry, rz);
    const V3 T{g.p("pos_x", t) * px, -g.p("pos_y", t) * px, -cam.f - depth / 2 - away * H};
    auto toCamera = [&](V3 p) { return R * (p * k) + T; };
    Lighting light;
    const double az = g.p("light_angle", t, -40) * kDeg, el = std::clamp(g.p("light_height", t, 40), -90.0, 90.0) * kDeg;
    light.light = normalized({std::sin(az) * std::cos(el), std::cos(az) * std::cos(el), std::sin(el)});
    light.ambient = std::clamp(g.p("ambient", t, 35) / 100, 0.0, 1.0);
    light.specular = std::clamp(g.p("specular", t, 40) / 100, 0.0, 1.0);
    const float face[3] = {float(g.p("color.r", t, 0.95)), float(g.p("color.g", t, 0.95)), float(g.p("color.b", t, 0.95))};
    const float side[3] = {float(g.p("side_color.r", t, 0.45)), float(g.p("side_color.g", t, 0.5)), float(g.p("side_color.b", t, 0.6))};

    // Everything is projected first (clipped where it comes nearer than the near plane), so the raster only needs
    // to cover where the title lands.
    double bx0 = W, by0 = H, bx1 = 0, by1 = 0;
    auto grow = [&](const Screen& s) {
        bx0 = std::min(bx0, s.x), by0 = std::min(by0, s.y), bx1 = std::max(bx1, s.x), by1 = std::max(by1, s.y);
    };
    // The sides: a quad along every edge, lit at its corners with normals smoothed where the outline curves.
    std::vector<Tri> tris;
    const double zf = depth / 2 / k, zb = -depth / 2 / k;  // object space (scaled by k with everything else)
    const double smooth = std::cos(35 * kDeg);
    if (depth > 0)
        for (const Contour& c : contours) {
            const size_t n = c.pts.size();
            std::vector<V3> edgeN(n);
            for (size_t i = 0; i < n; ++i) {
                const V3 a = c.pts[i], b = c.pts[(i + 1) % n];
                edgeN[i] = normalized({b.y - a.y, -(b.x - a.x), 0});
            }
            for (size_t i = 0; i < n; ++i) {
                const size_t j = (i + 1) % n;
                const V3 na = dot(edgeN[(i + n - 1) % n], edgeN[i]) > smooth ? normalized(edgeN[(i + n - 1) % n] + edgeN[i]) : edgeN[i];
                const V3 nb = dot(edgeN[i], edgeN[j]) > smooth ? normalized(edgeN[i] + edgeN[j]) : edgeN[i];
                std::vector<Corner> quad(4);
                quad[0].p = toCamera({c.pts[i].x, c.pts[i].y, zf});
                quad[1].p = toCamera({c.pts[j].x, c.pts[j].y, zf});
                quad[2].p = toCamera({c.pts[j].x, c.pts[j].y, zb});
                quad[3].p = toCamera({c.pts[i].x, c.pts[i].y, zb});
                // Facing away from the camera all along: hidden behind the rest.
                const V3 nc = R * edgeN[i];
                if (dot(nc, quad[0].p) > 0 && dot(nc, quad[1].p) > 0 && dot(nc, quad[2].p) > 0 && dot(nc, quad[3].p) > 0) continue;
                light.shade(R * na, quad[0].p, side, quad[0].c);
                light.shade(R * nb, quad[1].p, side, quad[1].c);
                light.shade(R * nb, quad[2].p, side, quad[2].c);
                light.shade(R * na, quad[3].p, side, quad[3].c);
                const std::vector<Corner> poly = clipNear(quad, cam.nearest);
                for (size_t m = 1; m + 1 < poly.size(); ++m) {
                    Tri tr;
                    const Corner* cs[3] = {&poly[0], &poly[m], &poly[m + 1]};
                    for (int v = 0; v < 3; ++v) {
                        tr.v[v] = cam.project(cs[v]->p);
                        std::copy(cs[v]->c, cs[v]->c + 3, tr.c[v]);
                        grow(tr.v[v]);
                    }
                    tris.push_back(tr);
                }
            }
        }
    // The front and back faces: the projected outline (what is in front of the near plane) filled by nonzero winding,
    // each sample's depth from the plane.
    struct Edge {
        double x0, y0, x1, y1;
        int dir;
    };
    struct Face {
        std::vector<Edge> edges;
        V3 normal;
        double nd = 0;  // the plane: dot(normal, p) == nd
        float col[3];
    };
    std::vector<Face> faces;
    for (int face_ = 0; face_ < 2; ++face_) {
        const double z = face_ == 0 ? zf : zb;
        if (face_ == 1 && depth <= 0) break;  // flat: one face, seen from either side
        Face fc;
        fc.normal = R * V3{0, 0, face_ == 0 ? 1.0 : -1.0};
        const V3 onPlane = toCamera({0, 0, z});
        fc.nd = dot(fc.normal, onPlane);
        if (depth > 0 && fc.nd >= 0) continue;  // facing away
        light.shade(fc.normal, onPlane, face, fc.col);
        for (const Contour& c : contours) {
            std::vector<Corner> ring(c.pts.size());
            for (size_t i = 0; i < c.pts.size(); ++i) ring[i].p = toCamera({c.pts[i].x, c.pts[i].y, z});
            const std::vector<Corner> kept = clipNear(ring, cam.nearest);
            for (size_t i = 0; i < kept.size(); ++i) {
                const Screen a = cam.project(kept[i].p), b = cam.project(kept[(i + 1) % kept.size()].p);
                grow(a);
                if (a.y != b.y) fc.edges.push_back(a.y < b.y ? Edge{a.x, a.y, b.x, b.y, 1} : Edge{b.x, b.y, a.x, a.y, -1});
            }
        }
        if (!fc.edges.empty()) faces.push_back(std::move(fc));
    }
    const int rx0 = std::max(0, int(std::floor(bx0)) - 1), ry0 = std::max(0, int(std::floor(by0)) - 1);
    const int rx1 = std::min(W, int(std::ceil(bx1)) + 2), ry1 = std::min(H, int(std::ceil(by1)) + 2);
    if (rx1 <= rx0 || ry1 <= ry0) return out;
    std::unique_ptr<Raster> raster;
    try {
        raster = std::make_unique<Raster>(rx0, ry0, rx1 - rx0, ry1 - ry0);
    } catch (const std::bad_alloc&) {
        return out;  // too big to draw: left out rather than taking the render down
    }
    Raster& r = *raster;
    for (const Tri& tr : tris) fillTriangle(r, tr);
    for (const Face& fc : faces) {
        parallelRows(r.H, [&](int b0, int b1) {
            // The edges reaching this band of rows, then each row's crossings.
            const double top = r.Y0 + b0 + 0.5, bottom = r.Y0 + b1 - 0.5;
            std::vector<const Edge*> band;
            for (const Edge& e : fc.edges)
                if (e.y1 > top && e.y0 <= bottom) band.push_back(&e);
            std::vector<std::pair<double, int>> xs;
            for (int y = r.Y0 + b0; y < r.Y0 + b1; ++y) {
                const double sy = y + 0.5;
                xs.clear();
                for (const Edge* e : band)
                    if (sy >= e->y0 && sy < e->y1) xs.push_back({e->x0 + (sy - e->y0) * (e->x1 - e->x0) / (e->y1 - e->y0), e->dir});
                if (xs.size() < 2) continue;
                std::sort(xs.begin(), xs.end());
                int wind = 0;
                for (size_t i = 0; i + 1 < xs.size(); ++i) {
                    wind += xs[i].second;
                    if (wind == 0) continue;
                    const int a = std::max(r.X0, int(std::ceil(xs[i].first - 0.5))),
                              b = std::min(r.X0 + r.W - 1, int(std::ceil(xs[i + 1].first - 0.5)) - 1);
                    for (int x = a; x <= b; ++x) {
                        // Where the sample's ray meets the face's plane.
                        const V3 ray{(x + 0.5 - cam.cx) / cam.f, -(sy - cam.cy) / cam.f, -1};
                        const double den = dot(fc.normal, ray);
                        if (std::fabs(den) < 1e-12) continue;
                        const double dist = fc.nd / den;
                        if (dist < cam.nearest) continue;
                        r.plot(x, y, float(1 / dist), fc.col);
                    }
                }
            }
        });
    }
    // Down to the output: the samples averaged, premultiplied, at the title's opacity.
    const int ox0 = r.X0 / super, oy0 = r.Y0 / super, ox1 = std::min(w, (r.X0 + r.W + super - 1) / super),
              oy1 = std::min(h, (r.Y0 + r.H + super - 1) / super);
    parallelRows(oy1 - oy0, [&](int b0, int b1) {
        for (int y = oy0 + b0; y < oy0 + b1; ++y)
            for (int x = ox0; x < ox1; ++x) {
                float sum[3] = {0, 0, 0}, cover = 0;
                for (int sy = 0; sy < super; ++sy)
                    for (int sx = 0; sx < super; ++sx) {
                        const int X = x * super + sx - r.X0, Y = y * super + sy - r.Y0;
                        if (X < 0 || Y < 0 || X >= r.W || Y >= r.H) continue;
                        const size_t i = size_t(Y) * size_t(r.W) + size_t(X);
                        if (r.inv[i] <= 0) continue;
                        cover += 1;
                        for (int c = 0; c < 3; ++c) sum[c] += float(r.rgb[i * 3 + size_t(c)]) / 65535.0f;
                    }
                const float n = float(super * super), a = float(opacity);
                float* p = out.at(x, y);
                for (int c = 0; c < 3; ++c) p[c] = sum[c] / n * a;
                p[3] = cover / n * a;
            }
    });
    return out;
}

}  // namespace montage
