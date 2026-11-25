#include "Title3D.h"
#include "TemporalFx.h"
#include "Titles.h"

#include "media/Model3D.h"

#include <QFont>
#include <QFontMetricsF>
#include <QPainterPath>
#include <QPolygonF>
#include <QStringList>
#include <algorithm>
#include <array>
#include <atomic>
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
M3 transposed(const M3& a) {
    M3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    return r;
}
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

// What a face of the title is filled with: its colour, a gradient from the title's top to its bottom, or a picture
// fitted to the title's box; at (u, v) across that box (0 to 1, v down), then mixed as the animations mix its colour.
struct Paint {
    int kind = 0;  // 0 colour, 1 gradient, 2 picture
    float c1[3] = {0, 0, 0}, c2[3] = {0, 0, 0};
    const Image* picture = nullptr;
    float keep = 1, add[3] = {0, 0, 0};  // the animations' mix: base * keep + add
    void at(double u, double v, float* out) const {
        float b[3] = {c1[0], c1[1], c1[2]};
        if (kind == 2 && picture && !picture->empty()) {
            // (bilinear, the picture's colour where it is opaque at all)
            const double X = std::clamp(u, 0.0, 1.0) * picture->width - 0.5, Y = std::clamp(v, 0.0, 1.0) * picture->height - 0.5;
            const int x0 = std::clamp(int(std::floor(X)), 0, picture->width - 1), y0 = std::clamp(int(std::floor(Y)), 0, picture->height - 1);
            const int x1 = std::min(x0 + 1, picture->width - 1), y1 = std::min(y0 + 1, picture->height - 1);
            const float fx = float(std::clamp(X - x0, 0.0, 1.0)), fy = float(std::clamp(Y - y0, 0.0, 1.0));
            float q[4];
            for (int k = 0; k < 4; ++k)
                q[k] = (picture->at(x0, y0)[k] * (1 - fx) + picture->at(x1, y0)[k] * fx) * (1 - fy) +
                       (picture->at(x0, y1)[k] * (1 - fx) + picture->at(x1, y1)[k] * fx) * fy;
            for (int k = 0; k < 3; ++k) b[k] = q[3] > 1e-6f ? q[k] / q[3] : 0.0f;
        } else if (kind == 1) {
            const float f = float(std::clamp(v, 0.0, 1.0));
            for (int k = 0; k < 3; ++k) b[k] = c1[k] + (c2[k] - c1[k]) * f;
        }
        for (int k = 0; k < 3; ++k) out[k] = b[k] * keep + add[k];
    }
};

// A corner of a polygon in camera space with its colour, or (a painted face) where it is across the title's box and
// how it is lit: colour = min(1, paint * lm + la).
struct Corner {
    V3 p;
    float c[3] = {0, 0, 0};
    double u = 0, v = 0;
    float lm = 0, la = 0;
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
            m.u = a.u + (b.u - a.u) * t, m.v = a.v + (b.v - a.v) * t;
            m.lm = float(a.lm + (b.lm - a.lm) * t), m.la = float(a.la + (b.la - a.la) * t);
            out.push_back(m);
        }
    }
    return out;
}

struct Tri {
    Screen v[3];
    float c[3][3];
    // Painted: its paint, where each corner is across the title's box and how it is lit (else its corners' colours).
    const Paint* paint = nullptr;
    double u[3] = {0, 0, 0}, uv[3] = {0, 0, 0};
    float lm[3] = {0, 0, 0}, la[3] = {0, 0, 0};
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
            if (t.paint) {
                // (the paint where the sample is, true to perspective; the light across the screen, as colours are)
                const double q0 = w0 * a.inv, q1 = w1 * b.inv, q2 = w2 * c.inv, qs = q0 + q1 + q2;
                if (!(qs > 0)) continue;
                t.paint->at((q0 * t.u[0] + q1 * t.u[1] + q2 * t.u[2]) / qs, (q0 * t.uv[0] + q1 * t.uv[1] + q2 * t.uv[2]) / qs, col);
                const double lm = w0 * t.lm[0] + w1 * t.lm[1] + w2 * t.lm[2], la = w0 * t.la[0] + w1 * t.la[1] + w2 * t.la[2];
                for (int k = 0; k < 3; ++k) col[k] = float(std::min(1.0, col[k] * lm + la));
            } else {
                for (int k = 0; k < 3; ++k) col[k] = float(w0 * t.c[0][k] + w1 * t.c[1][k] + w2 * t.c[2][k]);
            }
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
    // The same light as a factor and a highlight: colour = min(1, base * lm + la).
    void parts(V3 n, V3 p, float& lm, float& la) const {
        const V3 view = normalized(p * -1.0);
        if (dot(n, view) < 0) n = n * -1.0;
        const double diffuse = std::max(0.0, dot(n, light));
        lm = float(ambient + (1 - ambient) * diffuse);
        la = float(specular * std::pow(std::max(0.0, dot(n, normalized(light + view))), 32));
    }
};

double easeOut(double p) { return 1 - std::pow(1 - std::clamp(p, 0.0, 1.0), 3); }

struct Contour {
    std::vector<V3> pts;  // y up, centred
    double x0, y0, x1, y1;
    double area = 0;
};
// What is extruded as one: its contours and how deep (raster pixels, before the title's scale).
struct Solid {
    std::vector<Contour> contours;
    double depth = 0;
    // Its own turn, scale and move about its middle (object space, before the title's): a point p of it is at
    // rot * ((p - pivot) * s) + pivot + move; and its colours.
    M3 rot;
    V3 pivot, move;
    double s = 1;
    float face[3] = {0, 0, 0}, side[3] = {0, 0, 0};
    Paint front, sides, back;  // (a colour is drawn from face and side, as it always was)
    V3 place(V3 p) const { return rot * ((p - pivot) * s) + pivot + move; }
    V3 unplace(V3 q) const { return transposed(rot) * (q - pivot - move) * (1 / s) + pivot; }
};
struct Edge {
    double x0, y0, x1, y1;
    int dir;
};
// A front or back face: the projected outline filled by nonzero winding, each sample's depth from its plane.
struct Face {
    std::vector<Edge> edges;
    V3 normal;
    double nd = 0;  // the plane: dot(normal, p) == nd
    float col[3];
    // Painted (a gradient or a picture): the paint, its light (col = min(1, paint * lm + la)) and its solid, to
    // find where each sample is on the text.
    const Paint* paint = nullptr;
    float lm = 0, la = 0;
    const Solid* solid = nullptr;
    int owner = 0;  // its solid's index
};

// A 3D title as it stands at one time, ready to draw: its solids, the camera, and what each solid's sides and faces
// project to (clipped at the near plane), with where on the raster they land.
struct Built {
    int super = kSuper, W = 0, H = 0;
    double opacity = 0;
    Camera cam;
    M3 R;
    V3 T;
    double k = 1;
    double boxW = 1, boxH = 1;
    double bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    double zf = 0;  // where every solid's front face is (object space)
    std::vector<Solid> solids;
    std::vector<Tri> tris;
    std::vector<int> triOwner;
    std::vector<Face> faces;
    Built() = default;
    Built(const Built&) = delete;
    Built& operator=(const Built&) = delete;
    Built(Built&&) = default;
    Built& operator=(Built&&) = default;
    // Where a point of the text (centred, y up) is across the title's box, for a face's gradient or picture.
    void across(V3 q, double& u, double& v) const { u = q.x / boxW + 0.5, v = 0.5 - q.y / boxH; }
};

std::atomic<uint64_t> gSolidsDrawn{0};

// The "title3d" generator at time t (clip frames, between frames for motion blur) on a w x h output: false when
// nothing of it shows.
bool buildTitle3D(const Effect& g, double t, int w, int h, double scale, FrameTime duration, double fps, const Title3DPictures* pictures,
                  Built& b) {
    if (w <= 0 || h <= 0) return false;
    // Animation: how far in (from the start) and out (towards the end).
    fps = fps > 0 ? fps : 30;
    const double sec = t / fps;
    const int inKind = int(std::lround(g.pAt("anim_in", t))), outKind = int(std::lround(g.pAt("anim_out", t)));
    const double inDur = std::max(0.01, g.pAt("anim_in_dur", t, 0.8)), outDur = std::max(0.01, g.pAt("anim_out_dur", t, 0.8));
    const double pin = inKind ? std::clamp(sec / inDur, 0.0, 1.0) : 1.0;
    const double pout = outKind && duration > 0 ? std::clamp((double(duration - 1) - t) / fps / outDur, 0.0, 1.0) : 1.0;
    double rx = g.pAt("rot_x", t, 10), ry = g.pAt("rot_y", t, -20), rz = g.pAt("rot_z", t), opacity = std::clamp(g.pAt("opacity", t, 100) / 100, 0.0, 1.0);
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
    if (opacity <= 0) return false;
    b.opacity = opacity;

    // Supersampled, but not past a 4K frame's worth of samples (finer frames have the pixels to spare).
    const int super = double(w) * h > 3840.0 * 2160 ? 1 : kSuper;
    const int W = w * super, H = h * super;
    b.super = super, b.W = W, b.H = H;
    const double px = scale * super;  // raster pixels per sequence pixel
    // The text's outline, centred, in raster pixels (y down, as Qt draws).
    QFont font(QString::fromStdString(g.s("font", "Sans Serif")));
    font.setPixelSize(std::max(1, int(std::lround(g.pAt("size", t, 120) * px))));
    font.setBold(g.pAt("bold", t, 1) > 0.5);
    font.setItalic(g.pAt("italic", t) > 0.5);
    font.setLetterSpacing(QFont::AbsoluteSpacing, g.pAt("tracking", t) * px);
    font.setHintingPreference(QFont::PreferNoHinting);
    const QStringList lines = QString::fromStdString(g.s("text", "3D Title")).split('\n');
    const QFontMetricsF fm(font);
    const double lineH = fm.height() * std::clamp(g.pAt("line_spacing", t, 1.1), 0.5, 3.0);
    double blockW = 0;
    for (const QString& l : lines) blockW = std::max(blockW, fm.horizontalAdvance(l));
    const int align = int(std::lround(g.pAt("align", t, 1)));
    // Letter by letter when the title has text animations or font axes of its own: each glyph cluster its own depth
    // (those as deep as each other, and not moved, one outline), and a letter the animations move, turn, scale or
    // colour a solid of its own, sorted with the rest by depth; else the text as one outline, its styled runs
    // (render/Titles.h) each in its own font, size, weight and tracking.
    const std::vector<int> anims = textAnimators(g);
    const bool perLetter = !anims.empty() || !titleAxes(g).empty();
    const double lineSpacing = std::clamp(g.pAt("line_spacing", t, 1.1), 0.5, 3.0);
    const float face[3] = {float(g.pAt("color.r", t, 0.95)), float(g.pAt("color.g", t, 0.95)), float(g.pAt("color.b", t, 0.95))};
    const float side[3] = {float(g.pAt("side_color.r", t, 0.45)), float(g.pAt("side_color.g", t, 0.5)), float(g.pAt("side_color.b", t, 0.6))};
    // Each face's fill: its colour, a gradient from that colour at the title's top to a second at its bottom, or a
    // track's picture fitted to the title's box. The back is filled as the front unless it has a fill of its own.
    struct FacePaint {
        int kind = 0;
        float c1[3] = {0, 0, 0}, c2[3] = {0, 0, 0};
        const Image* picture = nullptr;
    };
    auto facePaint = [&](const std::string& pre, int fill, const float* def, const Image* pic) {
        FacePaint fp;
        fp.kind = fill == 1 ? 1 : fill == 2 && pic && !pic->empty() ? 2 : 0;
        fp.picture = fp.kind == 2 ? pic : nullptr;
        const char* ch[3] = {".r", ".g", ".b"};
        for (int q = 0; q < 3; ++q) {
            fp.c1[q] = float(g.pAt(pre + "color" + ch[q], t, def[q]));
            fp.c2[q] = float(g.pAt(pre + "color2" + ch[q], t, fp.c1[q]));
        }
        return fp;
    };
    const float backDef[3] = {face[0], face[1], face[2]};
    const int backFill = int(std::lround(g.pAt("back_fill", t, 0)));  // 0 as the front, 1 colour, 2 gradient, 3 picture
    const FacePaint frontPaint = facePaint("", int(std::lround(g.pAt("face_fill", t, 0))), face, pictures ? &pictures->front : nullptr);
    const FacePaint sidePaint = facePaint("side_", int(std::lround(g.pAt("side_fill", t, 0))), side, pictures ? &pictures->sides : nullptr);
    const FacePaint backPaint = facePaint("back_", backFill - 1, backDef, pictures ? &pictures->back : nullptr);
    const bool backOwn = backFill >= 1 && backFill <= 3;
    // Each outline and how it stands: its depth, and (a letter the animations move, turn, scale or colour) where it
    // turns about, how, how far it moves and its own colours.
    struct Outline {
        QPainterPath path;
        double depth = 0;
        bool own = false;  // moved, turned, scaled or coloured: a solid of its own
        double ox = 0, oy = 0, oz = 0, rx = 0, ry = 0, rz = 0, s = 1;  // raster pixels (y up, z towards the camera), degrees
        std::array<double, 3> face{0, 0, 0}, side{0, 0, 0};
        bool coloured = false;
        // The same mixes as base * keep + add, for a face painted with a gradient or a picture.
        double faceKeep = 1, sideKeep = 1;
        std::array<double, 3> faceAdd{0, 0, 0}, sideAdd{0, 0, 0};
    };
    std::vector<Outline> outlines;
    const double baseDepth = std::max(0.0, g.pAt("depth", t, 40)) * px;
    if (perLetter) {
        int letters = 0, solids = 0, words = 0, nLines = 0;
        const std::vector<TitleCharOutline> chars = titleCharOutlines(g, t, px, 120, lineSpacing, QStringLiteral("3D Title"), letters, solids, words, nLines);
        for (const TitleCharOutline& c : chars) {
            if (c.path.isEmpty()) continue;
            double d = g.pAt("depth", t, 40);
            Outline o;
            o.path = c.path;
            for (int q = 0; q < 3; ++q) o.face[size_t(q)] = face[q], o.side[size_t(q)] = side[q];
            for (int k : anims) {
                const std::string pre = "ta" + std::to_string(k) + ".";
                const int by = int(std::lround(g.pAt(pre + "by", t)));
                const int i = by == 0 ? c.letter : by == 1 ? c.solid : by == 2 ? c.word : c.line;
                const int n = by == 0 ? letters : by == 1 ? solids : by == 2 ? words : nLines;
                if (i < 0) continue;
                const double v = textSelection(g, k, t, fps, duration, i, n);
                if (v == 0) continue;
                d += g.pAt(pre + "depth", t) * v;
                o.ox += g.pAt(pre + "x", t) * px * v;
                o.oy -= g.pAt(pre + "y", t) * px * v;
                o.oz += g.pAt(pre + "z", t) * px * v;
                o.rx += g.pAt(pre + "rot_x", t) * v;
                o.ry += g.pAt(pre + "rot_y", t) * v;
                o.rz += g.pAt(pre + "rotation", t) * v;
                o.s *= std::max(0.0, 1 + (g.pAt(pre + "scale", t, 100) / 100 - 1) * v);
                // Colours mixed in as the 2D title's are, each animation over what the ones before it left.
                const double fmix = std::clamp(g.pAt(pre + "fill_mix", t) / 100 * v, 0.0, 1.0), smix = std::clamp(g.pAt(pre + "side_mix", t) / 100 * v, 0.0, 1.0);
                const char* ch[3] = {".r", ".g", ".b"};
                for (int q = 0; q < 3; ++q) {
                    const double fc = std::clamp(g.pAt(pre + "fill" + ch[q], t, 1), 0.0, 1.0), sc = std::clamp(g.pAt(pre + "side" + ch[q], t, 1), 0.0, 1.0);
                    o.face[size_t(q)] += (fc - o.face[size_t(q)]) * fmix;
                    o.side[size_t(q)] += (sc - o.side[size_t(q)]) * smix;
                    o.faceAdd[size_t(q)] = o.faceAdd[size_t(q)] * (1 - fmix) + fc * fmix;
                    o.sideAdd[size_t(q)] = o.sideAdd[size_t(q)] * (1 - smix) + sc * smix;
                }
                o.faceKeep *= 1 - fmix;
                o.sideKeep *= 1 - smix;
                o.coloured = o.coloured || fmix > 0 || smix > 0;
            }
            // (letters as deep as each other and left in place are one outline, so where they overlap they merge, as a
            // whole title does)
            o.depth = std::max(0.0, d) * px;
            o.own = o.ox != 0 || o.oy != 0 || o.oz != 0 || o.rx != 0 || o.ry != 0 || o.rz != 0 || o.s != 1 || o.coloured;
            if (o.s <= 0) continue;
            auto same = o.own ? outlines.end()
                              : std::find_if(outlines.begin(), outlines.end(), [&](const Outline& u) { return !u.own && std::abs(u.depth - o.depth) < 1e-9; });
            if (same == outlines.end()) outlines.push_back(std::move(o));
            else same->path.addPath(c.path);
        }
        for (Outline& o : outlines) o.path.setFillRule(Qt::WindingFill);
    } else {
        QPainterPath path = styledTextOutline(g, t, px, 120, lineSpacing);
        path.setFillRule(Qt::WindingFill);
        if (path.isEmpty())
            for (int i = 0; i < lines.size(); ++i) {
                const double lw = fm.horizontalAdvance(lines[i]);
                const double x = align == 0 ? 0 : align == 2 ? blockW - lw : (blockW - lw) / 2;
                path.addText(QPointF(x, fm.ascent() + i * lineH), font, lines[i]);
            }
        if (!path.isEmpty()) {
            Outline o;
            o.path = path;
            o.depth = baseDepth;
            for (int q = 0; q < 3; ++q) o.face[size_t(q)] = face[q], o.side[size_t(q)] = side[q];
            outlines.push_back(std::move(o));
        }
    }
    if (outlines.empty()) return false;
    // Overlaps merged (letters run together by tight tracking or line spacing, fonts built from overlapping
    // strokes), so every contour bounds solid on one side: outlines then run anticlockwise and holes clockwise (y up),
    // and each edge's outward normal is (dy, -dx). All turn about the middle of the whole text.
    std::vector<QPainterPath> simple;
    QRectF box;
    for (const Outline& o : outlines) {
        simple.push_back(o.path.simplified());
        box = box.isNull() ? simple.back().boundingRect() : box.united(simple.back().boundingRect());
    }
    const QPointF centre = box.center();
    b.boxW = std::max(1e-9, box.width()), b.boxH = std::max(1e-9, box.height());
    std::vector<Solid>& solids = b.solids;
    for (size_t o = 0; o < outlines.size(); ++o) {
        Solid so;
        so.depth = outlines[o].depth;
        std::vector<Contour>& contours = so.contours;
        for (const QPolygonF& poly : simple[o].toSubpathPolygons()) {
            Contour c;
            for (const QPointF& q : poly) {
                const V3 v{q.x() - centre.x(), centre.y() - q.y(), 0};
                if (c.pts.empty() || std::hypot(v.x - c.pts.back().x, v.y - c.pts.back().y) > 1e-6) c.pts.push_back(v);
            }
            while (c.pts.size() > 1 && std::hypot(c.pts.front().x - c.pts.back().x, c.pts.front().y - c.pts.back().y) < 1e-6) c.pts.pop_back();
            if (c.pts.size() < 3) continue;
            c.x0 = c.x1 = c.pts[0].x, c.y0 = c.y1 = c.pts[0].y;
            for (size_t i = 0; i < c.pts.size(); ++i) {
                const V3 &a = c.pts[i], &bb = c.pts[(i + 1) % c.pts.size()];
                c.area += a.x * bb.y - bb.x * a.y;
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
                const Contour& oc = contours[j];
                if (j != i && q.x >= oc.x0 && q.x <= oc.x1 && q.y >= oc.y0 && q.y <= oc.y1 && insidePolygon(oc.pts, q.x, q.y)) ++around;
            }
            if ((contours[i].area > 0) == (around % 2 == 1)) std::reverse(contours[i].pts.begin(), contours[i].pts.end());
        }
        if (contours.empty()) continue;
        const Outline& ol = outlines[o];
        if (ol.own) {
            // About the middle of the letter's box, halfway back through its depth.
            const QRectF bb = simple[o].boundingRect();
            so.pivot = {bb.center().x() - centre.x(), centre.y() - bb.center().y(), 0};  // (z set below, with the face)
            so.rot = rotation(ol.rx, ol.ry, ol.rz);
            so.s = ol.s;
            so.move = {ol.ox, ol.oy, ol.oz};
        }
        for (int q = 0; q < 3; ++q) so.face[q] = ol.own ? float(ol.face[size_t(q)]) : face[q], so.side[q] = ol.own ? float(ol.side[size_t(q)]) : side[q];
        // Its paints: the title's, mixed as the animations mix its colours (the back as the front's, unless it has
        // its own).
        auto paintOf = [&](const FacePaint& fp, double keep, const std::array<double, 3>& add, const float* colour) {
            Paint pt;
            pt.kind = fp.kind;
            pt.picture = fp.picture;
            for (int q = 0; q < 3; ++q) pt.c1[q] = fp.c1[q], pt.c2[q] = fp.c2[q], pt.add[q] = ol.own ? float(add[size_t(q)]) : 0.0f;
            pt.keep = ol.own ? float(keep) : 1.0f;
            if (pt.kind == 0)  // (a colour: as it always was)
                for (int q = 0; q < 3; ++q) pt.c1[q] = colour[q], pt.add[q] = 0, pt.keep = 1;
            return pt;
        };
        so.front = paintOf(frontPaint, ol.faceKeep, ol.faceAdd, so.face);
        so.sides = paintOf(sidePaint, ol.sideKeep, ol.sideAdd, so.side);
        if (backOwn) {
            so.back = paintOf(backPaint, ol.faceKeep, ol.faceAdd, so.face);
            if (backPaint.kind == 0)  // (its own colour, mixed as the front's is)
                for (int q = 0; q < 3; ++q)
                    so.back.c1[q] = ol.own ? float(backPaint.c1[q] * ol.faceKeep + ol.faceAdd[size_t(q)]) : backPaint.c1[q];
        } else {
            so.back = so.front;
        }
        solids.push_back(std::move(so));
    }
    if (solids.empty()) return false;

    // Placement and camera: the face at rest sits in the frame's plane at the size it was set (the extrusion going back
    // from it, turning about its middle), through a lens of `fov` degrees across the frame's height.
    const double fov = std::clamp(g.pAt("fov", t, 35), 5.0, 120.0);
    Camera& cam = b.cam;
    cam.f = (H / 2.0) / std::tan(fov * kDeg / 2);
    cam.cx = W / 2.0;
    cam.cy = H / 2.0;
    cam.nearest = std::max(1.0, cam.f * 0.05);  // nothing nearer than this is drawn
    const double k = std::clamp(g.pAt("scale", t, 100), 1.0, 2000.0) / 100;
    b.k = k;
    const double depth = baseDepth * k;  // (the title's own: its face sits in the frame's plane, turning about its middle)
    M3 R = rotation(rx, ry, rz);
    V3 T{g.pAt("pos_x", t) * px, -g.pAt("pos_y", t) * px, -cam.f - depth / 2 - away * H};
    // Or through a camera in a USD or glTF file: the title stands in the file's scene, its face in the plane z = 0
    // about the origin, `units` sequence pixels a scene unit; the camera moves as the file's animation moves it.
    if (const std::string file = g.s("camera_file"); !file.empty())
        if (const std::shared_ptr<const Model> model = cachedModel(file)) {
            const int index = int(std::lround(g.pAt("camera_index", t, 1))) - 1;
            const ModelPose pose = poseModel(*model, model->animations.empty() ? -1 : 0, sec);
            std::array<double, 3> at;
            std::array<std::array<double, 3>, 3> axes;
            if (index >= 0 && size_t(index) < model->cameras.size() && modelCameraPlace(pose, model->cameras[size_t(index)], at, axes)) {
                const ModelCameraInfo& fc = model->cameras[size_t(index)];
                modelCameraLens(fc, sec, W, H, cam.f, cam.cx, cam.cy);
                const double u = std::clamp(g.pAt("units", t, 100), 1e-6, 1e9) * px;  // raster pixels a scene unit
                cam.nearest = std::max(1e-3, fc.nearClip * u);
                M3 view;  // world to camera: rows the camera's right, up and back
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) view.m[i][j] = axes[size_t(i)][size_t(j)];
                R = view * R;
                T = view * V3{g.pAt("pos_x", t) * px - at[0] * u, -g.pAt("pos_y", t) * px - at[1] * u, -depth / 2 - away * H - at[2] * u};
            }
        }
    b.R = R, b.T = T;
    auto toCamera = [&](V3 p) { return R * (p * k) + T; };
    Lighting light;
    const double az = g.pAt("light_angle", t, -40) * kDeg, el = std::clamp(g.pAt("light_height", t, 40), -90.0, 90.0) * kDeg;
    light.light = normalized({std::sin(az) * std::cos(el), std::cos(az) * std::cos(el), std::sin(el)});
    light.ambient = std::clamp(g.pAt("ambient", t, 35) / 100, 0.0, 1.0);
    light.specular = std::clamp(g.pAt("specular", t, 40) / 100, 0.0, 1.0);

    // Everything is projected first (clipped where it comes nearer than the near plane), so the raster only needs
    // to cover where the title lands.
    double bx0 = W, by0 = H, bx1 = 0, by1 = 0;
    auto grow = [&](const Screen& s) {
        bx0 = std::min(bx0, s.x), by0 = std::min(by0, s.y), bx1 = std::max(bx1, s.x), by1 = std::max(by1, s.y);
    };
    // The sides: a quad along every edge, lit at its corners with normals smoothed where the outline curves.
    std::vector<Tri>& tris = b.tris;
    const double zf = depth / 2 / k;  // object space (scaled by k with everything else): every face at the front
    b.zf = zf;
    for (Solid& so : solids) so.pivot.z = zf - so.depth / 2;
    auto toCam = [&](const Solid& so, V3 p) { return toCamera(so.place(p)); };
    const double smooth = std::cos(35 * kDeg);
    for (size_t si = 0; si < solids.size(); ++si) {
        const Solid& so = solids[si];
        const M3 Rs = R * so.rot;  // its normals into the camera's space
        const double zb = zf - so.depth;
        if (so.depth <= 0) continue;
        for (const Contour& c : so.contours) {
            const size_t n = c.pts.size();
            std::vector<V3> edgeN(n);
            for (size_t i = 0; i < n; ++i) {
                const V3 a = c.pts[i], bb = c.pts[(i + 1) % n];
                edgeN[i] = normalized({bb.y - a.y, -(bb.x - a.x), 0});
            }
            for (size_t i = 0; i < n; ++i) {
                const size_t j = (i + 1) % n;
                const V3 na = dot(edgeN[(i + n - 1) % n], edgeN[i]) > smooth ? normalized(edgeN[(i + n - 1) % n] + edgeN[i]) : edgeN[i];
                const V3 nb = dot(edgeN[i], edgeN[j]) > smooth ? normalized(edgeN[i] + edgeN[j]) : edgeN[i];
                std::vector<Corner> quad(4);
                quad[0].p = toCam(so, {c.pts[i].x, c.pts[i].y, zf});
                quad[1].p = toCam(so, {c.pts[j].x, c.pts[j].y, zf});
                quad[2].p = toCam(so, {c.pts[j].x, c.pts[j].y, zb});
                quad[3].p = toCam(so, {c.pts[i].x, c.pts[i].y, zb});
                // Facing away from the camera all along: hidden behind the rest.
                const V3 nc = Rs * edgeN[i];
                if (dot(nc, quad[0].p) > 0 && dot(nc, quad[1].p) > 0 && dot(nc, quad[2].p) > 0 && dot(nc, quad[3].p) > 0) continue;
                light.shade(Rs * na, quad[0].p, so.side, quad[0].c);
                light.shade(Rs * nb, quad[1].p, so.side, quad[1].c);
                light.shade(Rs * nb, quad[2].p, so.side, quad[2].c);
                light.shade(Rs * na, quad[3].p, so.side, quad[3].c);
                const bool painted = so.sides.kind != 0;
                if (painted) {
                    const V3* at[4] = {&c.pts[i], &c.pts[j], &c.pts[j], &c.pts[i]};
                    const V3* nn[4] = {&na, &nb, &nb, &na};
                    for (int q = 0; q < 4; ++q) {
                        b.across(*at[q], quad[size_t(q)].u, quad[size_t(q)].v);
                        light.parts(Rs * *nn[q], quad[size_t(q)].p, quad[size_t(q)].lm, quad[size_t(q)].la);
                    }
                }
                const std::vector<Corner> poly = clipNear(quad, cam.nearest);
                for (size_t m = 1; m + 1 < poly.size(); ++m) {
                    Tri tr;
                    const Corner* cs[3] = {&poly[0], &poly[m], &poly[m + 1]};
                    if (painted) tr.paint = &so.sides;
                    for (int v = 0; v < 3; ++v) {
                        tr.v[v] = cam.project(cs[v]->p);
                        std::copy(cs[v]->c, cs[v]->c + 3, tr.c[v]);
                        tr.u[v] = cs[v]->u, tr.uv[v] = cs[v]->v, tr.lm[v] = cs[v]->lm, tr.la[v] = cs[v]->la;
                        grow(tr.v[v]);
                    }
                    tris.push_back(tr);
                    b.triOwner.push_back(int(si));
                }
            }
        }
    }
    // The front and back faces: the projected outline (what is in front of the near plane) filled by nonzero winding,
    // each sample's depth from the plane.
    std::vector<Face>& faces = b.faces;
    for (size_t si = 0; si < solids.size(); ++si) {
        const Solid& so = solids[si];
        for (int face_ = 0; face_ < 2; ++face_) {
            const double z = face_ == 0 ? zf : zf - so.depth;
            if (face_ == 1 && so.depth <= 0) break;  // flat: one face, seen from either side
            Face fc;
            fc.owner = int(si);
            fc.normal = R * so.rot * V3{0, 0, face_ == 0 ? 1.0 : -1.0};
            const V3 onPlane = toCam(so, {0, 0, z});
            fc.nd = dot(fc.normal, onPlane);
            if (so.depth > 0 && fc.nd >= 0) continue;  // facing away
            const Paint& paint = face_ == 0 ? so.front : so.back;
            if (paint.kind != 0) {
                fc.paint = &paint, fc.solid = &so;
                light.parts(fc.normal, onPlane, fc.lm, fc.la);
            } else if (face_ == 1) {
                light.shade(fc.normal, onPlane, paint.c1, fc.col);  // (the back's colour: the front's unless its own)
            } else {
                light.shade(fc.normal, onPlane, so.face, fc.col);
            }
            for (const Contour& c : so.contours) {
                std::vector<Corner> ring(c.pts.size());
                for (size_t i = 0; i < c.pts.size(); ++i) ring[i].p = toCam(so, {c.pts[i].x, c.pts[i].y, z});
                const std::vector<Corner> kept = clipNear(ring, cam.nearest);
                for (size_t i = 0; i < kept.size(); ++i) {
                    const Screen a = cam.project(kept[i].p), bb = cam.project(kept[(i + 1) % kept.size()].p);
                    grow(a);
                    if (a.y != bb.y) fc.edges.push_back(a.y < bb.y ? Edge{a.x, a.y, bb.x, bb.y, 1} : Edge{bb.x, bb.y, a.x, a.y, -1});
                }
            }
            if (!fc.edges.empty()) faces.push_back(std::move(fc));
        }
    }
    b.bx0 = bx0, b.by0 = by0, b.bx1 = bx1, b.by1 = by1;
    return true;
}

// The raster a built title (or several, times through the shutter) needs: where they land, a sample of margin round.
bool rasterBounds(const std::vector<const Built*>& builds, int& x0, int& y0, int& x1, int& y1) {
    double bx0 = 1e300, by0 = 1e300, bx1 = -1e300, by1 = -1e300;
    int W = 0, H = 0;
    for (const Built* b : builds) {
        bx0 = std::min(bx0, b->bx0), by0 = std::min(by0, b->by0), bx1 = std::max(bx1, b->bx1), by1 = std::max(by1, b->by1);
        W = b->W, H = b->H;
    }
    x0 = std::max(0, int(std::floor(bx0)) - 1), y0 = std::max(0, int(std::floor(by0)) - 1);
    x1 = std::min(W, int(std::ceil(bx1)) + 2), y1 = std::min(H, int(std::ceil(by1)) + 2);
    return x1 > x0 && y1 > y0;
}

// Draws the solids of `b` that `which` picks (all when null) into the raster, depth tested.
void drawSolids(const Built& b, Raster& r, const std::vector<char>* which) {
    auto picked = [&](int s) { return !which || (*which)[size_t(s)]; };
    for (size_t s = 0; s < b.solids.size(); ++s)
        if (picked(int(s))) gSolidsDrawn.fetch_add(1);
    const Camera& cam = b.cam;
    const M3 Rt = transposed(b.R);
    for (size_t i = 0; i < b.tris.size(); ++i)
        if (picked(b.triOwner[i])) fillTriangle(r, b.tris[i]);
    for (const Face& fc : b.faces) {
        if (!picked(fc.owner)) continue;
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
                              bb = std::min(r.X0 + r.W - 1, int(std::ceil(xs[i + 1].first - 0.5)) - 1);
                    for (int x = a; x <= bb; ++x) {
                        // Where the sample's ray meets the face's plane.
                        const V3 ray{(x + 0.5 - cam.cx) / cam.f, -(sy - cam.cy) / cam.f, -1};
                        const double den = dot(fc.normal, ray);
                        if (std::fabs(den) < 1e-12) continue;
                        const double dist = fc.nd / den;
                        if (dist < cam.nearest) continue;
                        if (fc.paint) {
                            // (the sample back on the text: through the camera, the title's turn and scale, and the
                            // solid's own)
                            const V3 q = Rt * (ray * dist - b.T) * (1 / b.k);
                            double u, v;
                            b.across(fc.solid->unplace(q), u, v);
                            float col[3];
                            fc.paint->at(u, v, col);
                            for (int ch = 0; ch < 3; ++ch) col[ch] = std::min(1.0f, col[ch] * fc.lm + fc.la);
                            r.plot(x, y, float(1 / dist), col);
                            continue;
                        }
                        r.plot(x, y, float(1 / dist), fc.col);
                    }
                }
            }
        });
    }
}

// The raster down to the output: the samples averaged, premultiplied, at the title's opacity (added into `out`,
// times `weight`, when `add`).
void resolveRaster(const Raster& r, int super, double opacity, Image& out, bool add = false, float weight = 1) {
    const int w = out.width, h = out.height;
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
                if (add) {
                    for (int c = 0; c < 3; ++c) p[c] += sum[c] / n * a * weight;
                    p[3] += cover / n * a * weight;
                    continue;
                }
                for (int c = 0; c < 3; ++c) p[c] = sum[c] / n * a;
                p[3] = cover / n * a;
            }
    });
}

// Everything a solid of a built title draws, as numbers, to tell whether it is drawn the same at two times.
std::vector<double> solidSignature(const Built& b, int s) {
    std::vector<double> v;
    auto paint = [&](const Paint* p) {
        if (!p) {
            v.push_back(-1);
            return;
        }
        v.push_back(p->kind);
        v.insert(v.end(), {p->c1[0], p->c1[1], p->c1[2], p->c2[0], p->c2[1], p->c2[2], p->keep, p->add[0], p->add[1], p->add[2]});
        v.push_back(double(reinterpret_cast<uintptr_t>(p->picture)));
    };
    for (size_t i = 0; i < b.tris.size(); ++i) {
        if (b.triOwner[i] != s) continue;
        const Tri& t = b.tris[i];
        for (int k = 0; k < 3; ++k) {
            v.insert(v.end(), {t.v[k].x, t.v[k].y, t.v[k].inv, t.c[k][0], t.c[k][1], t.c[k][2], t.u[k], t.uv[k], t.lm[k], t.la[k]});
        }
        paint(t.paint);
    }
    for (const Face& f : b.faces) {
        if (f.owner != s) continue;
        for (const Edge& e : f.edges) v.insert(v.end(), {e.x0, e.y0, e.x1, e.y1, double(e.dir)});
        v.insert(v.end(), {f.normal.x, f.normal.y, f.normal.z, f.nd, f.col[0], f.col[1], f.col[2], f.lm, f.la});
        paint(f.paint);
        if (f.paint) {
            // (where its samples are on the text: the solid's own place and the title's)
            const Solid& so = *f.solid;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) v.push_back(so.rot.m[i][j]);
            v.insert(v.end(), {so.pivot.x, so.pivot.y, so.pivot.z, so.move.x, so.move.y, so.move.z, so.s});
        }
    }
    return v;
}

// Whether two builds stand the same for every solid but their own (the camera, the title's turn and place, its box and
// opacity), so their solids can be compared one by one.
bool sameFrame(const Built& a, const Built& b) {
    if (a.solids.size() != b.solids.size() || a.opacity != b.opacity || a.k != b.k || a.boxW != b.boxW || a.boxH != b.boxH) return false;
    if (a.cam.f != b.cam.f || a.cam.cx != b.cam.cx || a.cam.cy != b.cam.cy || a.cam.nearest != b.cam.nearest) return false;
    if (a.T.x != b.T.x || a.T.y != b.T.y || a.T.z != b.T.z) return false;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (a.R.m[i][j] != b.R.m[i][j]) return false;
    return true;
}

// A face's outline (the solid's contours, which do not cross: outlines anticlockwise, holes clockwise) cut into
// triangles: between each two heights at which a corner lies, the strips where the winding is not zero, each a
// trapezoid of two triangles (corners where the outline's edges cross those heights), anticlockwise.
std::vector<std::array<V3, 3>> faceTriangles(const std::vector<Contour>& contours) {
    struct E {
        double x0, y0, x1, y1;
        int dir;
        double xAt(double y) const { return y1 == y0 ? x0 : x0 + (x1 - x0) * (y - y0) / (y1 - y0); }
    };
    std::vector<E> edges;
    std::vector<double> ys;
    for (const Contour& c : contours)
        for (size_t i = 0; i < c.pts.size(); ++i) {
            const V3 a = c.pts[i], b = c.pts[(i + 1) % c.pts.size()];
            ys.push_back(a.y);
            if (a.y == b.y) continue;
            edges.push_back(a.y < b.y ? E{a.x, a.y, b.x, b.y, 1} : E{b.x, b.y, a.x, a.y, -1});
        }
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());
    std::vector<std::array<V3, 3>> out;
    std::vector<std::pair<double, const E*>> across;
    for (size_t s = 0; s + 1 < ys.size(); ++s) {
        const double y0 = ys[s], y1 = ys[s + 1], ym = (y0 + y1) / 2;
        across.clear();
        for (const E& e : edges)
            if (e.y0 <= ym && e.y1 > ym) across.push_back({e.xAt(ym), &e});
        std::sort(across.begin(), across.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        int wind = 0;
        for (size_t i = 0; i + 1 < across.size(); ++i) {
            // (an edge going up has the solid on its left: anticlockwise outlines wind +1 inside)
            wind -= across[i].second->dir;
            if (wind == 0) continue;
            const E &l = *across[i].second, &r = *across[i + 1].second;
            const V3 a{l.xAt(y0), y0, 0}, b{r.xAt(y0), y0, 0}, c{r.xAt(y1), y1, 0}, d{l.xAt(y1), y1, 0};
            if (b.x - a.x > 1e-12) out.push_back({a, b, c});
            if (c.x - d.x > 1e-12) out.push_back({a, c, d});
        }
    }
    return out;
}

}  // namespace

uint64_t title3dSolidsDrawn() { return gSolidsDrawn.load(); }

bool title3dMesh(const Effect& g, double t, double seqWidth, double seqHeight, FrameTime duration, double fps, Title3DMesh& out) {
    out = Title3DMesh{};
    const int w = std::max(1, int(std::lround(seqWidth))), h = std::max(1, int(std::lround(seqHeight)));
    Built b;
    if (!buildTitle3D(g, t, w, h, 1.0, duration, fps, nullptr, b) || !g.s("camera_file").empty()) return false;
    // The title's camera space (raster pixels) to the scene's: a unit a sequence pixel, the frame at z = 0.
    const double unit = 1.0 / b.super;
    auto toWorld = [&](V3 q) { return V3{q.x * unit, q.y * unit, (q.z + b.cam.f) * unit}; };
    out.camera.R = {1, 0, 0, 0, -1, 0, 0, 0, -1};
    out.camera.C = {0, 0, b.cam.f * unit};
    out.camera.screen = {b.cam.f * unit, 0, b.cam.cx * unit, 0, b.cam.f * unit, b.cam.cy * unit};
    out.camera.nearZ = b.cam.nearest * unit;
    PosedTriangles& tr = out.triangles;
    auto vertex = [&](V3 p, V3 n, const float* col) {
        tr.positions.insert(tr.positions.end(), {float(p.x), float(p.y), float(p.z)});
        tr.normals.insert(tr.normals.end(), {float(n.x), float(n.y), float(n.z)});
        tr.uvs.insert(tr.uvs.end(), {0.0f, 0.0f});
        tr.tangents.insert(tr.tangents.end(), {0.0f, 0.0f, 0.0f, 0.0f});
        // (the title's colours are display colours, as its own drawing shows them: made linear for the scene)
        for (int c = 0; c < 3; ++c) {
            const double v = std::clamp(double(col[c]), 0.0, 1.0);
            tr.colors.push_back(float(v <= 0.081 ? v / 4.5 : std::pow((v + 0.099) / 1.099, 1 / 0.45)));
        }
        tr.colors.push_back(1.0f);
        return uint32_t(tr.positions.size() / 3 - 1);
    };
    auto colourAt = [&](const Paint& paint, V3 q, float* col) {
        double u, v;
        b.across(q, u, v);
        paint.at(u, v, col);
    };
    for (size_t si = 0; si < b.solids.size(); ++si) {
        const Solid& so = b.solids[si];
        const M3 Rs = b.R * so.rot;
        auto place = [&](V3 p) { return toWorld(b.R * (so.place(p) * b.k) + b.T); };
        auto triangle = [&](uint32_t a, uint32_t bb, uint32_t c, bool face = false) {
            tr.indices.insert(tr.indices.end(), {a, bb, c});
            tr.materials.push_back(0);
            out.letterOf.push_back(int(si));
            out.onFace.push_back(face ? 1 : 0);
        };
        const std::vector<std::array<V3, 3>> faces = faceTriangles(so.contours);
        for (int side = 0; side < (so.depth > 0 ? 2 : 1); ++side) {
            const double z = side == 0 ? b.zf : b.zf - so.depth;
            const V3 n = Rs * V3{0, 0, side == 0 ? 1.0 : -1.0};
            const Paint& paint = side == 0 ? so.front : so.back;
            for (const auto& f : faces) {
                uint32_t idx[3];
                for (int v = 0; v < 3; ++v) {
                    float col[3];
                    colourAt(paint, f[size_t(v)], col);
                    idx[v] = vertex(place({f[size_t(v)].x, f[size_t(v)].y, z}), n, col);
                }
                if (side == 0) triangle(idx[0], idx[1], idx[2], true);
                else triangle(idx[0], idx[2], idx[1]);
            }
        }
        if (so.depth <= 0) continue;
        // The sides, a quad along every edge, normals smoothed where the outline curves (as it is drawn alone).
        const double smooth = std::cos(35 * kDeg), zb = b.zf - so.depth;
        for (const Contour& c : so.contours) {
            const size_t n = c.pts.size();
            std::vector<V3> edgeN(n);
            for (size_t i = 0; i < n; ++i) {
                const V3 a = c.pts[i], bb = c.pts[(i + 1) % n];
                edgeN[i] = normalized({bb.y - a.y, -(bb.x - a.x), 0});
            }
            for (size_t i = 0; i < n; ++i) {
                const size_t j = (i + 1) % n;
                const V3 na = dot(edgeN[(i + n - 1) % n], edgeN[i]) > smooth ? normalized(edgeN[(i + n - 1) % n] + edgeN[i]) : edgeN[i];
                const V3 nb = dot(edgeN[i], edgeN[j]) > smooth ? normalized(edgeN[i] + edgeN[j]) : edgeN[i];
                float ca[3], cb[3];
                colourAt(so.sides, c.pts[i], ca);
                colourAt(so.sides, c.pts[j], cb);
                const uint32_t q0 = vertex(place({c.pts[i].x, c.pts[i].y, b.zf}), Rs * na, ca), q1 = vertex(place({c.pts[j].x, c.pts[j].y, b.zf}), Rs * nb, cb),
                               q2 = vertex(place({c.pts[j].x, c.pts[j].y, zb}), Rs * nb, cb), q3 = vertex(place({c.pts[i].x, c.pts[i].y, zb}), Rs * na, ca);
                // (outward: the edge's normal is (dy, -dx), so front, along the edge, then back is anticlockwise seen
                // from outside)
                triangle(q0, q3, q2);
                triangle(q0, q2, q1);
            }
        }
    }
    if (tr.indices.empty()) return false;
    auto model = std::make_shared<Model>();
    ModelMaterial m;
    m.name = "title";
    m.metallic = 0;
    m.roughness = float(std::clamp(1 - 0.6 * std::clamp(g.pAt("specular", t, 40) / 100, 0.0, 1.0), 0.05, 1.0));
    m.doubleSided = true;  // (a flat title is seen from either side)
    model->materials.push_back(m);
    out.model = std::move(model);
    out.opacity = b.opacity;
    return true;
}

Image renderTitle3D(const Effect& g, FrameTime t, int w, int h, double scale, FrameTime duration, double fps, const Title3DPictures* pictures) {
    Image out(w, h);
    if (w <= 0 || h <= 0) return out;
    const double shutter = std::clamp(g.p("letter_shutter", t, 180), 0.0, 720.0) / 360.0;
    const int n = std::clamp(int(std::lround(g.p("letter_samples", t, 16))), 2, 64);
    const bool blur = g.p("letter_blur", t, 0) > 0.5 && shutter > 0 && !motionBlurOff();
    Built at;
    if (!buildTitle3D(g, double(t), w, h, scale, duration, fps, pictures, at) && !blur) return out;
    if (!blur) {
        int x0, y0, x1, y1;
        if (!rasterBounds({&at}, x0, y0, x1, y1)) return out;
        std::unique_ptr<Raster> raster;
        try {
            raster = std::make_unique<Raster>(x0, y0, x1 - x0, y1 - y0);
        } catch (const std::bad_alloc&) {
            return out;  // too big to draw: left out rather than taking the render down
        }
        drawSolids(at, *raster, nullptr);
        resolveRaster(*raster, at.super, at.opacity, out);
        return out;
    }
    // Letter motion blur: the title built at evenly spread times through the shutter, centred on the frame, and the
    // frame the mean of each drawn by itself. A letter drawn the same at every time (and at the frame) is drawn once,
    // into a raster the moving letters are then drawn into at each time; when the title as a whole moves, turns, fades
    // or changes size, every letter is drawn at every time.
    std::vector<Built> times(static_cast<size_t>(n));
    std::vector<char> shown(size_t(n), 0);
    for (int i = 0; i < n; ++i)
        shown[size_t(i)] = buildTitle3D(g, double(t) + shutter * ((i + 0.5) / n - 0.5), w, h, scale, duration, fps, pictures, times[size_t(i)]);
    const bool still0 = !at.solids.empty();
    bool together = still0;  // the solids line up one for one at every time
    for (int i = 0; i < n && together; ++i) together = shown[size_t(i)] && sameFrame(at, times[size_t(i)]);
    std::vector<char> moving(at.solids.size(), 1);
    bool anyStill = false, anyMoving = !together;
    if (together) {
        for (size_t s = 0; s < at.solids.size(); ++s) {
            const std::vector<double> here = solidSignature(at, int(s));
            bool same = true;
            for (int i = 0; i < n && same; ++i) same = solidSignature(times[size_t(i)], int(s)) == here;
            moving[s] = !same;
            anyStill = anyStill || same;
            anyMoving = anyMoving || !same;
        }
    }
    std::vector<const Built*> all;
    if (still0) all.push_back(&at);
    for (int i = 0; i < n; ++i)
        if (shown[size_t(i)]) all.push_back(&times[size_t(i)]);
    int x0, y0, x1, y1;
    if (all.empty() || !rasterBounds(all, x0, y0, x1, y1)) return out;
    std::unique_ptr<Raster> base, one;
    try {
        base = std::make_unique<Raster>(x0, y0, x1 - x0, y1 - y0);
        if (anyMoving) one = std::make_unique<Raster>(x0, y0, x1 - x0, y1 - y0);
    } catch (const std::bad_alloc&) {
        return out;
    }
    if (!anyMoving) {  // (nothing moves: drawn once, as without the blur)
        drawSolids(at, *base, nullptr);
        resolveRaster(*base, at.super, at.opacity, out);
        return out;
    }
    if (anyStill) {
        std::vector<char> still(moving.size());
        for (size_t s = 0; s < moving.size(); ++s) still[s] = !moving[s];
        drawSolids(at, *base, &still);
    }
    const int super = all.front()->super;
    for (int i = 0; i < n; ++i) {
        one->rgb = base->rgb, one->inv = base->inv;
        if (shown[size_t(i)]) drawSolids(times[size_t(i)], *one, together ? &moving : nullptr);
        resolveRaster(*one, super, shown[size_t(i)] ? times[size_t(i)].opacity : at.opacity, out, true, 1.0f / float(n));
    }
    return out;
}

}  // namespace montage
