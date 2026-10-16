#include "FaceShape.h"

#include <algorithm>
#include <cmath>

namespace montage {

namespace {

// MediaPipe's canonical mesh, by its numbers.
constexpr int kOval[] = {10,  338, 297, 332, 284, 251, 389, 356, 454, 323, 361, 288, 397, 365, 379, 378, 400, 377,
                         152, 148, 176, 149, 150, 136, 172, 58,  132, 93,  234, 127, 162, 21,  54,  103, 67,  109};
constexpr int kRightEye[] = {33, 7, 163, 144, 145, 153, 154, 155, 133, 173, 157, 158, 159, 160, 161, 246};
constexpr int kLeftEye[] = {263, 249, 390, 373, 374, 380, 381, 382, 362, 398, 384, 385, 386, 387, 388, 466};
constexpr int kLips[] = {61,  146, 91,  181, 84,  17,  314, 405, 321, 375, 291, 409, 270, 269, 267, 0,   37,  39,  40,  185,
                         78,  95,  88,  178, 87,  14,  317, 402, 318, 324, 308, 415, 310, 311, 312, 13,  82,  81,  80,  191};
constexpr int kNoseWings[] = {48, 49, 64, 98, 102, 129, 219, 278, 279, 294, 327, 331, 358, 439};
constexpr int kNoseTip[] = {1, 2, 4, 5, 19, 94};

using Pt = std::array<float, 2>;

double bump(double t, double centre, double half) {
    const double d = std::fabs(t - centre) / half;
    return d >= 1 ? 0 : 0.5 * (1 + std::cos(M_PI * d));
}

// A face's own directions: across (from its right eye to its left, as the picture shows them) and down.
struct Frame {
    double ex[2], ey[2];
    double cx, cy;      // between the eyes
    double width, height;  // cheek to cheek, forehead to chin
    double across(double x, double y) const { return (x - cx) * ex[0] + (y - cy) * ex[1]; }
    double down(double x, double y) const { return (x - cx) * ey[0] + (y - cy) * ey[1]; }
};

Frame faceFrame(const FaceMesh& m) {
    Frame f;
    double ax = m.x(263) - m.x(33), ay = m.y(263) - m.y(33);
    const double l = std::max(1e-6, std::hypot(ax, ay));
    f.ex[0] = ax / l, f.ex[1] = ay / l;
    f.ey[0] = -f.ex[1], f.ey[1] = f.ex[0];
    f.cx = m.x(168), f.cy = m.y(168);
    f.width = std::max(1.0, std::fabs(f.across(m.x(454), m.y(454)) - f.across(m.x(234), m.y(234))));
    f.height = std::max(1.0, f.down(m.x(152), m.y(152)) - f.down(m.x(10), m.y(10)));
    return f;
}

bool insidePolygon(const std::vector<Pt>& poly, double x, double y) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i][1] > y) != (poly[j][1] > y) &&
            x < (poly[j][0] - poly[i][0]) * (y - poly[i][1]) / (poly[j][1] - poly[i][1]) + poly[i][0])
            in = !in;
    return in;
}

double distanceToEdge(const std::vector<Pt>& poly, double x, double y) {
    double best = 1e30;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const double ax = poly[j][0], ay = poly[j][1], bx = poly[i][0], by = poly[i][1];
        const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        const double u = len2 > 0 ? std::clamp(((x - ax) * dx + (y - ay) * dy) / len2, 0.0, 1.0) : 0;
        best = std::min(best, std::hypot(x - (ax + u * dx), y - (ay + u * dy)));
    }
    return best;
}

void sampleInto(const Image& src, double x, double y, float* out) {
    x = std::clamp(x - 0.5, 0.0, double(src.width - 1));
    y = std::clamp(y - 0.5, 0.0, double(src.height - 1));
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, src.width - 1), y1 = std::min(y0 + 1, src.height - 1);
    const float fx = float(x - x0), fy = float(y - y0);
    const float *a = src.at(x0, y0), *b = src.at(x1, y0), *c = src.at(x0, y1), *d = src.at(x1, y1);
    for (int k = 0; k < 4; ++k) out[k] = (a[k] * (1 - fx) + b[k] * fx) * (1 - fy) + (c[k] * (1 - fx) + d[k] * fx) * fy;
}

// A copy of part of a picture, read in the picture's own coordinates (clamped to the picture's edges, as sampleInto).
struct Region {
    Image img;
    int ox = 0, oy = 0, fullW = 0, fullH = 0;
    void sample(double x, double y, float* out) const {
        x = std::clamp(x, 0.5, fullW - 0.5) - ox, y = std::clamp(y, 0.5, fullH - 0.5) - oy;
        sampleInto(img, x, y, out);
    }
    const float* at(int x, int y) const { return img.at(std::clamp(x - ox, 0, img.width - 1), std::clamp(y - oy, 0, img.height - 1)); }
};

Region copyRegion(const Image& img, int x0, int y0, int x1, int y1) {
    Region r;
    r.fullW = img.width, r.fullH = img.height;
    x0 = std::clamp(x0, 0, img.width - 1), x1 = std::clamp(x1, 0, img.width - 1);
    y0 = std::clamp(y0, 0, img.height - 1), y1 = std::clamp(y1, 0, img.height - 1);
    r.ox = x0, r.oy = y0;
    r.img = Image(x1 - x0 + 1, y1 - y0 + 1, Image::Uninitialized{});
    for (int y = y0; y <= y1; ++y) std::copy(img.at(x0, y), img.at(x1, y) + 4, r.img.at(0, y - y0));
    return r;
}

void dot(Image& img, double x, double y, double r, const float rgb[3]) {
    for (int py = int(std::floor(y - r)); py <= int(std::ceil(y + r)); ++py)
        for (int px = int(std::floor(x - r)); px <= int(std::ceil(x + r)); ++px) {
            if (px < 0 || py < 0 || px >= img.width || py >= img.height || std::hypot(px + 0.5 - x, py + 0.5 - y) > r) continue;
            float* p = img.at(px, py);
            p[0] = rgb[0], p[1] = rgb[1], p[2] = rgb[2], p[3] = 1;
        }
}

}  // namespace

bool FaceShapeSettings::any() const {
    for (double v : {faceWidth, jaw, chin, forehead, noseWidth, noseLength, eyeSize, eyeDistance, mouthWidth, mouthHeight})
        if (std::fabs(v) > 1e-4) return true;
    return false;
}

std::vector<Pt> eyeOutline(const FaceMesh& m, bool left) {
    std::vector<Pt> out;
    for (int i : left ? kLeftEye : kRightEye) out.push_back({m.x(i), m.y(i)});
    return out;
}

std::vector<Pt> faceShapeTargets(const FaceMesh& m, const FaceShapeSettings& s) {
    std::vector<Pt> to(kFaceMeshPoints, Pt{0, 0});
    if (!m.valid()) return to;
    for (int i = 0; i < kFaceMeshPoints; ++i) to[size_t(i)] = {m.x(i), m.y(i)};
    const Frame f = faceFrame(m);
    const double top = f.down(m.x(10), m.y(10));
    auto move = [&](int i, double a, double b) {  // across and down, pixels
        to[size_t(i)][0] += float(a * f.ex[0] + b * f.ey[0]);
        to[size_t(i)][1] += float(a * f.ex[1] + b * f.ey[1]);
    };
    // The outline: cheeks out or in at their widest, the jaw at its corners, the chin and forehead up or down.
    for (int i : kOval) {
        const double s0 = f.across(m.x(i), m.y(i)), t = (f.down(m.x(i), m.y(i)) - top) / f.height;
        const double side = s0 < 0 ? -1 : 1, off = std::min(1.0, std::fabs(s0) / (0.25 * f.width));
        move(i, side * s.faceWidth * 0.12 * f.width / 2 * bump(t, 0.5, 0.38), 0);
        move(i, side * s.jaw * 0.14 * f.width / 2 * bump(t, 0.82, 0.22) * off, 0);
        move(i, 0, s.chin * 0.08 * f.height * std::max(0.0, 1 - std::fabs(s0) / (0.32 * f.width)) * (t > 0.7 ? 1 : 0));
        move(i, 0, -s.forehead * 0.08 * f.height * std::max(0.0, 1 - std::fabs(s0) / (0.5 * f.width)) * (t < 0.3 ? 1 : 0));
    }
    // The nose: its wings toward or away from its middle, its tip down or up (longer or shorter).
    const double noseMid = f.across(m.x(1), m.y(1)), noseLength = std::max(1.0, f.down(m.x(1), m.y(1)));
    for (int i : kNoseWings) move(i, (f.across(m.x(i), m.y(i)) - noseMid) * 0.3 * s.noseWidth, 0);
    for (int i : kNoseTip) move(i, 0, s.noseLength * 0.15 * noseLength);
    // The eyes: larger or smaller about their middles, further apart or closer.
    for (bool left : {false, true}) {
        const int* eye = left ? kLeftEye : kRightEye;
        double ex = 0, ey = 0;
        for (int k = 0; k < 16; ++k) ex += m.x(eye[k]) / 16, ey += m.y(eye[k]) / 16;
        const double spread = f.across(ex, ey);
        for (int k = 0; k < 16; ++k) {
            const int i = eye[k];
            const double gx = (m.x(i) - ex) * 0.25 * s.eyeSize, gy = (m.y(i) - ey) * 0.25 * s.eyeSize;
            to[size_t(i)][0] += float(gx), to[size_t(i)][1] += float(gy);
            move(i, spread * 0.1 * s.eyeDistance, 0);
        }
    }
    // The lips: wider or narrower, taller or shorter, about the mouth's middle.
    const double mx = (m.x(61) + m.x(291)) / 2, my = (m.y(61) + m.y(291)) / 2;
    const double ma = f.across(mx, my), md = f.down(mx, my);
    for (int i : kLips) move(i, (f.across(m.x(i), m.y(i)) - ma) * 0.2 * s.mouthWidth, (f.down(m.x(i), m.y(i)) - md) * 0.25 * s.mouthHeight);
    return to;
}

void warpPoints(Image& img, const std::vector<Pt>& from, const std::vector<Pt>& to, double cx, double cy, double radius) {
    if (img.empty() || from.size() != to.size() || from.empty() || !(radius > 1)) return;
    // Handles: the points (where they go -> where they were, for each output pixel's source) and a still ring.
    std::vector<Pt> p = to, q = from;
    for (int k = 0; k < 24; ++k) {
        const double a = 2 * M_PI * k / 24;
        const Pt r = {float(cx + radius * std::cos(a)), float(cy + radius * std::sin(a))};
        p.push_back(r), q.push_back(r);
    }
    const int x0 = std::max(0, int(std::floor(cx - radius))), x1 = std::min(img.width - 1, int(std::ceil(cx + radius)));
    const int y0 = std::max(0, int(std::floor(cy - radius))), y1 = std::min(img.height - 1, int(std::ceil(cy + radius)));
    if (x1 < x0 || y1 < y0) return;
    // The move worked out on a grid (about 200 nodes across, whatever the size) and spread between its nodes.
    const double step = std::max(1.0, radius / 100);
    const int gw = int((x1 - x0) / step) + 2, gh = int((y1 - y0) / step) + 2;
    std::vector<float> dx(size_t(gw) * size_t(gh), 0.f), dy(dx.size(), 0.f);
    const double eps = std::pow(0.01 * radius, 2);
    parallelRows(gh, [&](int r0, int r1) {
        std::vector<double> w(p.size());
        for (int gy = r0; gy < r1; ++gy)
            for (int gx = 0; gx < gw; ++gx) {
                const double vx = x0 + gx * step + 0.5, vy = y0 + gy * step + 0.5;
                const double dist = std::hypot(vx - cx, vy - cy);
                if (dist >= radius) continue;
                // Affine moving least squares (Schaefer, McPhail and Warren 2006).
                double sw = 0, px = 0, py = 0, qx = 0, qy = 0;
                for (size_t i = 0; i < p.size(); ++i) {
                    w[i] = 1 / ((p[i][0] - vx) * (p[i][0] - vx) + (p[i][1] - vy) * (p[i][1] - vy) + eps);
                    sw += w[i];
                    px += w[i] * p[i][0], py += w[i] * p[i][1], qx += w[i] * q[i][0], qy += w[i] * q[i][1];
                }
                px /= sw, py /= sw, qx /= sw, qy /= sw;
                double a = 0, b = 0, d = 0, m00 = 0, m01 = 0, m10 = 0, m11 = 0;
                for (size_t i = 0; i < p.size(); ++i) {
                    const double hx = p[i][0] - px, hy = p[i][1] - py, kx = q[i][0] - qx, ky = q[i][1] - qy;
                    a += w[i] * hx * hx, b += w[i] * hx * hy, d += w[i] * hy * hy;
                    m00 += w[i] * hx * kx, m01 += w[i] * hx * ky, m10 += w[i] * hy * kx, m11 += w[i] * hy * ky;
                }
                const double det = a * d - b * b;
                if (std::fabs(det) < 1e-12) continue;
                // (row vector (v - p*) times (sum w p^T p)^-1 (sum w p^T q))
                const double i00 = d / det, i01 = -b / det, i11 = a / det;
                const double M00 = i00 * m00 + i01 * m10, M01 = i00 * m01 + i01 * m11;
                const double M10 = i01 * m00 + i11 * m10, M11 = i01 * m01 + i11 * m11;
                const double ux = vx - px, uy = vy - py;
                const double fx = ux * M00 + uy * M10 + qx, fy = ux * M01 + uy * M11 + qy;
                // (nothing moved at the ring and beyond: eased out over its outer fifth)
                const double fade = std::clamp((radius - dist) / (0.2 * radius), 0.0, 1.0);
                const size_t n = size_t(gy) * size_t(gw) + size_t(gx);
                dx[n] = float((fx - vx) * fade), dy[n] = float((fy - vy) * fade);
            }
    });
    // Only the part that is read is kept: the circle and as far past it as anything reaches.
    float reach = 0;
    for (size_t n = 0; n < dx.size(); ++n) reach = std::max(reach, std::max(std::fabs(dx[n]), std::fabs(dy[n])));
    const Region src = copyRegion(img, x0 - int(reach) - 2, y0 - int(reach) - 2, x1 + int(reach) + 2, y1 + int(reach) + 2);
    parallelRows(y1 - y0 + 1, [&](int r0, int r1) {
        for (int y = y0 + r0; y < y0 + r1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double gx = (x - x0) / step, gy = (y - y0) / step;
                const int ax = std::min(int(gx), gw - 2), ay = std::min(int(gy), gh - 2);
                const float fx = float(gx - ax), fy = float(gy - ay);
                auto at = [&](const std::vector<float>& v) {
                    const size_t n = size_t(ay) * size_t(gw) + size_t(ax);
                    return (v[n] * (1 - fx) + v[n + 1] * fx) * (1 - fy) + (v[n + size_t(gw)] * (1 - fx) + v[n + size_t(gw) + 1] * fx) * fy;
                };
                const float mx = at(dx), my = at(dy);
                if (std::fabs(mx) < 1e-3f && std::fabs(my) < 1e-3f) continue;
                src.sample(x + 0.5 + mx, y + 0.5 + my, img.at(x, y));
            }
    });
}

void reshapeFaces(Image& img, const std::vector<FaceMesh>& meshes, const FaceShapeSettings& s) {
    for (const FaceMesh& m : meshes) {
        if (!m.valid()) continue;
        const std::vector<Pt> to = faceShapeTargets(m, s);
        if (s.showMesh) {
            const float green[3] = {0.1f, 1, 0.2f};
            const double r = std::max(1.0, faceFrame(m).width / 150);
            for (const Pt& p : to) dot(img, p[0], p[1], r, green);
            continue;
        }
        if (!s.any()) continue;
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        for (int i = 0; i < kFaceMeshPoints; ++i) {
            x0 = std::min(x0, double(m.x(i))), x1 = std::max(x1, double(m.x(i)));
            y0 = std::min(y0, double(m.y(i))), y1 = std::max(y1, double(m.y(i)));
        }
        const double extent = std::max(x1 - x0, y1 - y0);
        // The points that move, and of those that don't only the ones well away from them (a still point beside a
        // moving one would pinch the skin between them; what is near gives way smoothly instead).
        std::vector<Pt> from, onto, still;
        for (int i = 0; i < kFaceMeshPoints; ++i) {
            const Pt p = {m.x(i), m.y(i)};
            if (std::hypot(to[size_t(i)][0] - p[0], to[size_t(i)][1] - p[1]) > 1e-3) {
                from.push_back(p), onto.push_back(to[size_t(i)]);
            } else {
                still.push_back(p);
            }
        }
        if (from.empty()) continue;
        const double clear = 0.12 * extent;
        for (const Pt& p : still) {
            bool nearMoving = false;
            for (const Pt& q : from)
                if (std::hypot(q[0] - p[0], q[1] - p[1]) < clear) {
                    nearMoving = true;
                    break;
                }
            if (!nearMoving) from.push_back(p), onto.push_back(p);
        }
        // (the ring well outside the outline, so what is round the face gives way smoothly)
        warpPoints(img, from, onto, (x0 + x1) / 2, (y0 + y1) / 2, 0.85 * extent);
    }
}

namespace {

struct EyePixel {
    float x, y, luma, side;  // side: +1 the iris's (dark), -1 the white's, between at the boundary
};

// The pixels inside the lids, each with how much it looks like iris; false when too few show.
bool eyePixels(const Image& img, const std::vector<Pt>& lids, std::vector<EyePixel>& out) {
    out.clear();
    if (lids.size() < 3 || img.empty()) return false;
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (const Pt& p : lids) bx0 = std::min(bx0, p[0]), bx1 = std::max(bx1, p[0]), by0 = std::min(by0, p[1]), by1 = std::max(by1, p[1]);
    if (bx1 - bx0 < 6 || by1 - by0 < 2) return false;
    const int stride = std::max(1, int((bx1 - bx0) / 60));  // (a large eye read sparsely: its iris is big too)
    for (int py = std::max(0, int(by0)); py <= std::min(img.height - 1, int(by1)); py += stride)
        for (int px = std::max(0, int(bx0)); px <= std::min(img.width - 1, int(bx1)); px += stride)
            if (insidePolygon(lids, px + 0.5, py + 0.5)) {
                const float* p = img.at(px, py);
                const float a = p[3] > 1e-6f ? p[3] : 1;
                out.push_back({px + 0.5f, py + 0.5f, (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / a, 0});
            }
    if (out.size() < 12) return false;
    // Iris or white by a threshold between the white (the brighter part of what shows) and the darkest part, eased
    // over the boundary's width.
    std::vector<float> l;
    for (const auto& p : out) l.push_back(p.luma);
    std::sort(l.begin(), l.end());
    const float white = l[l.size() * 85 / 100];
    const float dark = l[l.size() * 5 / 100];
    if (white - dark < 0.04f) return false;  // (no iris shows: all one tone)
    const float mid = 0.5f * (white + dark), ease = 0.25f * (white - dark);
    for (auto& p : out) p.side = std::clamp((mid - p.luma) / ease, -1.0f, 1.0f);
    return true;
}

// The hole's pixels (RGBA, w x h) made the smoothest blend of what is round them: each the mean of its four neighbours,
// solved on a coarser copy first so that a large hole settles in a few passes.
void membrane(std::vector<float>& v, const std::vector<uint8_t>& hole, int w, int h) {
    int passes = 0;
    if (w > 24 && h > 24) {
        const int cw = (w + 1) / 2, chh = (h + 1) / 2;
        std::vector<float> cv(size_t(cw) * size_t(chh) * 4, 0.f);
        std::vector<uint8_t> chole(size_t(cw) * size_t(chh), 1);
        for (int j = 0; j < chh; ++j)
            for (int i = 0; i < cw; ++i) {
                double sum[4] = {0, 0, 0, 0};
                int n = 0;
                for (int b = 2 * j; b < std::min(h, 2 * j + 2); ++b)
                    for (int a = 2 * i; a < std::min(w, 2 * i + 2); ++a) {
                        const size_t at = size_t(b) * size_t(w) + size_t(a);
                        if (hole[at]) continue;
                        for (int c = 0; c < 4; ++c) sum[c] += v[at * 4 + size_t(c)];
                        ++n;
                    }
                const size_t at = size_t(j) * size_t(cw) + size_t(i);
                if (n == 0) continue;
                chole[at] = 0;
                for (int c = 0; c < 4; ++c) cv[at * 4 + size_t(c)] = float(sum[c] / n);
            }
        membrane(cv, chole, cw, chh);
        for (int j = 0; j < h; ++j)
            for (int i = 0; i < w; ++i) {
                const size_t at = size_t(j) * size_t(w) + size_t(i);
                if (!hole[at]) continue;
                const double gx = std::clamp((i + 0.5) / 2 - 0.5, 0.0, cw - 1.0), gy = std::clamp((j + 0.5) / 2 - 0.5, 0.0, chh - 1.0);
                const int ax = std::min(int(gx), cw - 2), ay = std::min(int(gy), chh - 2);
                const float fx = float(gx - ax), fy = float(gy - ay);
                const size_t n = size_t(ay) * size_t(cw) + size_t(ax);
                for (size_t c = 0; c < 4; ++c)
                    v[at * 4 + c] = (cv[n * 4 + c] * (1 - fx) + cv[(n + 1) * 4 + c] * fx) * (1 - fy) +
                                    (cv[(n + size_t(cw)) * 4 + c] * (1 - fx) + cv[(n + size_t(cw) + 1) * 4 + c] * fx) * fy;
            }
        passes = 12;
    } else {
        double sum[4] = {0, 0, 0, 0}, n = 0;
        for (size_t at = 0; at < hole.size(); ++at)
            if (!hole[at]) {
                for (int c = 0; c < 4; ++c) sum[c] += v[at * 4 + size_t(c)];
                n += 1;
            }
        for (size_t at = 0; at < hole.size(); ++at)
            if (hole[at])
                for (int c = 0; c < 4; ++c) v[at * 4 + size_t(c)] = float(sum[c] / std::max(1.0, n));
        passes = 3 * std::max(w, h) + 20;
    }
    // (over-relaxed Gauss-Seidel; a hole never touches the border)
    for (int pass = 0; pass < passes; ++pass)
        for (int j = 1; j < h - 1; ++j)
            for (int i = 1; i < w - 1; ++i) {
                const size_t at = size_t(j) * size_t(w) + size_t(i);
                if (!hole[at]) continue;
                for (size_t c = 0; c < 4; ++c) {
                    const float m = 0.25f * (v[(at - 1) * 4 + c] + v[(at + 1) * 4 + c] + v[(at - size_t(w)) * 4 + c] + v[(at + size_t(w)) * 4 + c]);
                    v[at * 4 + c] += 1.8f * (m - v[at * 4 + c]);
                }
            }
}

double eyeWidthOf(const std::vector<Pt>& lids) {
    // (the outline runs from the outer corner round to the inner one halfway)
    return std::hypot(lids[lids.size() / 2][0] - lids[0][0], lids[lids.size() / 2][1] - lids[0][1]);
}

}  // namespace

void moveIris(Image& img, const std::vector<Pt>& lids, float x, float y, float radius, float tx, float ty, double amount) {
    if (lids.size() < 3 || img.empty() || !(amount > 0) || !(radius > 0.5f)) return;
    std::vector<EyePixel> px;
    if (!eyePixels(img, lids, px)) return;
    const double ex = x + (tx - x) * amount, ey = y + (ty - y) * amount;  // where the iris ends up
    const double r = radius;
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
    for (const Pt& p : lids) bx0 = std::min(bx0, p[0]), bx1 = std::max(bx1, p[0]), by0 = std::min(by0, p[1]), by1 = std::max(by1, p[1]);
    // (what is read: the eye and round the iris, not the whole picture)
    const Region src = copyRegion(img, int(std::min(double(bx0), x - r)) - 4, int(std::min(double(by0), y - r)) - 4,
                                  int(std::max(double(bx1), x + r)) + 4, int(std::max(double(by1), y + r)) + 4);
    auto sample = [&](double sx, double sy, float* out) { src.sample(sx, sy, out); };
    // What fills where the iris was: the smoothest blend of what is round it (the white beside it, the lids' shadow
    // above), a membrane stretched over the hole.
    const int fx0 = std::max(0, int(std::floor(x - r - 3))), fy0 = std::max(0, int(std::floor(y - r - 3)));
    const int fx1 = std::min(img.width - 1, int(std::ceil(x + r + 3))), fy1 = std::min(img.height - 1, int(std::ceil(y + r + 3)));
    const int fw = fx1 - fx0 + 1, fh = fy1 - fy0 + 1;
    std::vector<float> fill;
    std::vector<uint8_t> hole;
    if (fw > 2 && fh > 2) {
        fill.resize(size_t(fw) * size_t(fh) * 4);
        hole.assign(size_t(fw) * size_t(fh), 0);
        for (int j = 0; j < fh; ++j)
            for (int i = 0; i < fw; ++i) {
                const double px_ = fx0 + i + 0.5, py_ = fy0 + j + 0.5;
                const size_t at = size_t(j) * size_t(fw) + size_t(i);
                std::copy(src.at(fx0 + i, fy0 + j), src.at(fx0 + i, fy0 + j) + 4, &fill[at * 4]);
                hole[at] = i > 0 && j > 0 && i < fw - 1 && j < fh - 1 && std::hypot(px_ - x, py_ - y) < r + 1 && insidePolygon(lids, px_, py_);
            }
        membrane(fill, hole, fw, fh);
    }
    // The iris's tone at each distance from its middle (the pupil, the iris, its darker rim), from what shows of it
    // but its highlights, for the part that was under a lid.
    const int rings = std::max(3, int(std::ceil(r)));
    std::vector<std::array<double, 5>> profile(size_t(rings), {0, 0, 0, 0, 0});
    for (const auto& p : px) {
        const double d = std::hypot(p.x - x, p.y - y);
        if (d >= r || p.side < -0.5f) continue;
        auto& ring = profile[size_t(std::min(rings - 1, int(d / r * rings)))];
        const float* c = src.at(int(p.x), int(p.y));
        for (int k = 0; k < 4; ++k) ring[size_t(k)] += c[k];
        ring[4] += 1;
    }
    for (int i = 0; i < rings; ++i)  // (an empty ring takes its nearest neighbour's)
        if (profile[size_t(i)][4] <= 0)
            for (int j = 1; j < rings; ++j) {
                const int a = i - j, b = i + j;
                const auto* n = a >= 0 && profile[size_t(a)][4] > 0 ? &profile[size_t(a)] : b < rings && profile[size_t(b)][4] > 0 ? &profile[size_t(b)] : nullptr;
                if (n) {
                    const double f = 1 / (*n)[4];
                    profile[size_t(i)] = {(*n)[0] * f, (*n)[1] * f, (*n)[2] * f, (*n)[3] * f, 1};
                    break;
                }
            }
    for (auto& ring : profile)
        if (ring[4] > 1)
            for (int k = 0; k < 4; ++k) ring[size_t(k)] /= ring[4];
    if (profile[0][4] <= 0) return;  // (no iris shows)
    // (the lids' edges, their lashes and shadow, kept as they are: what is drawn eases in over a margin inside them)
    const double margin = std::max(1.0, 0.05 * eyeWidthOf(lids));
    for (int py = std::max(0, int(by0)); py <= std::min(img.height - 1, int(by1)); ++py)
        for (int qx = std::max(0, int(bx0)); qx <= std::min(img.width - 1, int(bx1)); ++qx) {
            const double cx = qx + 0.5, cy = py + 0.5;
            if (!insidePolygon(lids, cx, cy)) continue;
            const float inside = float(std::clamp(distanceToEdge(lids, cx, cy) / margin - 0.5, 0.0, 1.0));
            if (inside <= 0) continue;
            const double dOld = std::hypot(cx - x, cy - y), dNew = std::hypot(cx - ex, cy - ey);
            const float oldIris = float(std::clamp(r + 1 - dOld, 0.0, 1.0)), newIris = float(std::clamp(r + 0.5 - dNew, 0.0, 1.0));
            if (oldIris <= 0 && newIris <= 0) continue;
            float out[4];
            std::copy(src.at(qx, py), src.at(qx, py) + 4, out);
            if (qx >= fx0 && qx <= fx1 && py >= fy0 && py <= fy1 && !fill.empty()) {
                const size_t at = size_t(py - fy0) * size_t(fw) + size_t(qx - fx0);
                if (hole[at]) std::copy(&fill[at * 4], &fill[at * 4] + 4, out);
            }
            if (newIris > 0) {
                // The iris carried over; its part that was under a lid drawn from its tone at that distance.
                float iris[4];
                const double sx = cx - ex + x, sy = cy - ey + y;
                if (insidePolygon(lids, sx, sy)) {
                    sample(sx, sy, iris);
                } else {
                    const auto& ring = profile[size_t(std::min(rings - 1, int(dNew / r * rings)))];
                    for (int ch = 0; ch < 4; ++ch) iris[ch] = float(ring[size_t(ch)]);
                }
                for (int ch = 0; ch < 4; ++ch) out[ch] = out[ch] * (1 - newIris) + iris[ch] * newIris;
            }
            float* d = img.at(qx, py);
            for (int ch = 0; ch < 4; ++ch) d[ch] = d[ch] * (1 - inside) + out[ch] * inside;
        }
}

bool gazeTarget(const FaceMesh& m, bool left, float& tx, float& ty, double* eyeballRadius) {
    if (!m.valid()) return false;
    const int outer = left ? 263 : 33, inner = left ? 362 : 133, upper = left ? 386 : 159, lower = left ? 374 : 145;
    const double width = std::hypot(m.x(outer) - m.x(inner), m.y(outer) - m.y(inner));
    const double open = std::hypot(m.x(upper) - m.x(lower), m.y(upper) - m.y(lower));
    if (width < 6 || open < 0.15 * width) return false;  // (shut, or blinking: left alone)
    // Looking at the lens: the iris about the middle of the eye, moved against the head's turn by the eyeball's radius
    // (about 0.4 of the eye's width) times the sine of the turn.
    const Frame f = faceFrame(m);
    const double r = 0.4 * width;
    const double across = -r * std::sin(m.yaw * M_PI / 180), down = r * std::sin(m.pitch * M_PI / 180);
    // Up or down only as far as leaves the iris showing as much as it can between the lids (they would follow the
    // eye, but are not moved): its middle within a band about theirs, at least a third of its radius either way.
    float ix = 0, iy = 0, ir = 0;
    m.iris(left, ix, iy, ir);
    const double mid = f.down((m.x(upper) + m.x(lower)) / 2, (m.y(upper) + m.y(lower)) / 2);
    const double slack = std::max(0.3 * ir, open / 2 - 0.6 * ir);
    const double now = f.down(ix, iy) - mid;
    double to = std::clamp(down, -slack, slack);
    if (std::fabs(now) > slack) to = std::clamp(to, std::min(now, 0.0), std::max(now, 0.0));  // (never further out)
    // (looking ahead, an iris sits a little toward the outer corner of the eye's middle: about a twentieth of its
    // width, as measured on portraits looking at the lens)
    const double side = (left ? 1 : -1) * 0.05 * width;
    const double cx = (m.x(outer) + m.x(inner)) / 2 + side * f.ex[0], cy = (m.y(upper) + m.y(lower)) / 2 + side * f.ex[1];
    tx = float(cx + across * f.ex[0] + to * f.ey[0]);
    ty = float(cy + across * f.ex[1] + to * f.ey[1]);
    if (eyeballRadius) *eyeballRadius = r;
    return true;
}

int lookAtLens(Image& img, const std::vector<FaceMesh>& meshes, const LookAtLensSettings& s) {
    int moved = 0;
    for (const FaceMesh& m : meshes) {
        for (bool left : {false, true}) {
            float tx = 0, ty = 0;
            double r = 0;
            if (!gazeTarget(m, left, tx, ty, &r)) continue;
            const std::vector<Pt> lids = eyeOutline(m, left);
            float ix = 0, iy = 0, ir = 0;
            m.iris(left, ix, iy, ir);
            const double off = std::hypot(ix - tx, iy - ty);
            const double angle = std::asin(std::min(1.0, off / r)) * 180 / M_PI;
            const double a = s.amount * std::clamp((s.limit + 10 - angle) / 10, 0.0, 1.0);
            if (s.showIrises) {
                const float red[3] = {1, 0.1f, 0.1f}, green[3] = {0.1f, 1, 0.2f};
                dot(img, ix, iy, std::max(1.0, r / 8), red);
                dot(img, tx, ty, std::max(1.0, r / 8), green);
                continue;
            }
            if (a <= 0 || off < 0.25) continue;
            moveIris(img, lids, ix, iy, ir, tx, ty, a);
            ++moved;
        }
    }
    return moved;
}

}  // namespace montage
