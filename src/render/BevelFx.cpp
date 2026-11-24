// Montage — bevels on a layer's alpha (see BevelFx.h).
#include "BevelFx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace montage::bevel {

namespace {

constexpr double kDeg = M_PI / 180;
constexpr float kFull = 1.0f - 1.0f / 512, kNone = 1.0f / 512;

// One line of the distance transform (the lower envelope of parabolas): for each place q the source p for which
// (q - p)^2 + f[p] is least, and that value. `v` and `z` are scratch of n and n + 1.
void nearestOnLine(const double* f, int n, double* d, int* arg, int* v, double* z) {
    constexpr double kFar = 1e30;
    int k = 0;
    v[0] = 0;
    z[0] = -kFar, z[1] = kFar;
    for (int q = 1; q < n; ++q) {
        double s = 0;
        for (;;) {
            const int p = v[k];
            s = ((f[q] + double(q) * q) - (f[p] + double(p) * p)) / (2.0 * q - 2.0 * p);
            if (s <= z[k]) --k;  // (never past the first: its start is further off than any crossing)
            else break;
        }
        ++k;
        v[k] = q;
        z[k] = s, z[k + 1] = kFar;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < q) ++k;
        const int p = v[k];
        d[q] = double(q - p) * (q - p) + f[p];
        arg[q] = p;
    }
}

// For every pixel of a w x h grid, the nearest pixel for which `source` holds (its index; -1 when there is none).
void nearest(const std::vector<float>& alpha, int w, int h, bool inside, std::vector<int>& at) {
    constexpr double kBig = 1e18;
    auto source = [&](size_t i) { return inside ? alpha[i] < kFull : alpha[i] > kNone; };
    std::vector<double> g(size_t(w) * size_t(h));
    std::vector<int> row(size_t(w) * size_t(h));
    parallelRows(w, [&](int x0, int x1) {
        const size_t n = size_t(h);
        std::vector<double> f(n), d(n), z(n + 1);
        std::vector<int> arg(n), v(n);
        for (int x = x0; x < x1; ++x) {
            for (int y = 0; y < h; ++y) f[size_t(y)] = source(size_t(y) * size_t(w) + size_t(x)) ? 0.0 : kBig;
            nearestOnLine(f.data(), h, d.data(), arg.data(), v.data(), z.data());
            for (int y = 0; y < h; ++y) {
                g[size_t(y) * size_t(w) + size_t(x)] = d[size_t(y)];
                row[size_t(y) * size_t(w) + size_t(x)] = arg[size_t(y)];
            }
        }
    });
    at.assign(size_t(w) * size_t(h), -1);
    parallelRows(h, [&](int y0, int y1) {
        const size_t n = size_t(w);
        std::vector<double> d(n), z(n + 1);
        std::vector<int> arg(n), v(n);
        for (int y = y0; y < y1; ++y) {
            const size_t o = size_t(y) * size_t(w);
            nearestOnLine(g.data() + o, w, d.data(), arg.data(), v.data(), z.data());
            for (int x = 0; x < w; ++x) {
                if (d[size_t(x)] >= kBig) continue;
                const int sx = arg[size_t(x)], sy = row[o + size_t(sx)];
                at[o + size_t(x)] = sy * w + sx;
            }
        }
    });
}

// The signed distance on the frame with a ring of empty pixels round it (the frame's border is an edge), the ring
// included: (width + 2) x (height + 2) values.
std::vector<float> signedDistance(const Image& img, bool wantInside, bool wantOutside) {
    const int w = img.width, h = img.height, pw = w + 2, ph = h + 2;
    std::vector<float> alpha(size_t(pw) * size_t(ph), 0.0f);
    for (int y = 0; y < h; ++y) {
        const float* s = img.row(y);
        float* a = alpha.data() + size_t(y + 1) * size_t(pw) + 1;
        for (int x = 0; x < w; ++x) a[x] = std::clamp(s[x * 4 + 3], 0.0f, 1.0f);
    }
    std::vector<int> in, out;
    if (wantInside) nearest(alpha, pw, ph, true, in);
    if (wantOutside) nearest(alpha, pw, ph, false, out);
    auto al = [&](int x, int y) { return alpha[size_t(std::clamp(y, 0, ph - 1)) * size_t(pw) + size_t(std::clamp(x, 0, pw - 1))]; };
    // How far pixel (x, y) is from the edge as it runs through pixel q, the nearest the edge touches: the edge lies in
    // q as far across as q's alpha says, at right angles to the way the alpha climbs there, and is taken as straight
    // for three quarters of a pixel either way (beyond that, round its end: a corner).
    auto fromEdge = [&](int x, int y, int q) {
        const int qx = q % pw, qy = q / pw;
        const double a = al(qx, qy);
        // (the way the alpha climbs: into the layer)
        double nx = (al(qx + 1, qy - 1) + 2 * al(qx + 1, qy) + al(qx + 1, qy + 1)) - (al(qx - 1, qy - 1) + 2 * al(qx - 1, qy) + al(qx - 1, qy + 1));
        double ny = (al(qx - 1, qy + 1) + 2 * al(qx, qy + 1) + al(qx + 1, qy + 1)) - (al(qx - 1, qy - 1) + 2 * al(qx, qy - 1) + al(qx + 1, qy - 1));
        const double len = std::hypot(nx, ny), dx = x - qx, dy = y - qy;
        if (len < 1e-9) return std::hypot(dx, dy) + std::fabs(a - 0.5);
        nx /= len, ny /= len;
        const double across = dx * nx + dy * ny + (a - 0.5), along = std::max(0.0, std::fabs(dx * ny - dy * nx) - 0.75);
        return std::hypot(across, along);
    };
    // The nearest pixel the edge touches is found on whole pixels, and the edge may pass closer through one beside it
    // (where it runs at a slant, or its pixel there is nearly empty or nearly full): the least over it and the pixels
    // round it that the edge crosses.
    auto nearEdge = [&](int x, int y, int q) {
        double best = fromEdge(x, y, q);
        const int qx = q % pw, qy = q / pw;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = qx + dx, ny = qy + dy;
                if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= pw || ny >= ph) continue;
                const float an = alpha[size_t(ny) * size_t(pw) + size_t(nx)];
                if (an > kNone && an < kFull) best = std::min(best, fromEdge(x, y, ny * pw + nx));
            }
        return best;
    };
    std::vector<float> sd(size_t(pw) * size_t(ph));
    parallelRows(ph, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < pw; ++x) {
                const size_t i = size_t(y) * size_t(pw) + size_t(x);
                const float a = alpha[i];
                float d;
                if (a >= kFull) d = !wantInside ? 1e9f : in[i] < 0 ? 1e9f : float(nearEdge(x, y, in[i]));
                else if (a <= kNone) d = !wantOutside ? -1e9f : out[i] < 0 ? -1e9f : -float(nearEdge(x, y, out[i]));
                else d = a - 0.5f;
                sd[i] = d;
            }
    });
    return sd;
}

// A height field blurred (a Gaussian of `sigma` pixels, edges repeated).
void soften(std::vector<float>& z, int w, int h, double sigma) {
    if (sigma < 0.25) return;
    const int r = std::max(1, int(std::ceil(sigma * 3)));
    std::vector<float> k(size_t(r) * 2 + 1);
    double sum = 0;
    for (int i = -r; i <= r; ++i) sum += k[size_t(i + r)] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
    for (float& v : k) v = float(v / sum);
    std::vector<float> tmp(z.size());
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                float acc = 0;
                for (int i = -r; i <= r; ++i) acc += k[size_t(i + r)] * z[size_t(y) * size_t(w) + size_t(std::clamp(x + i, 0, w - 1))];
                tmp[size_t(y) * size_t(w) + size_t(x)] = acc;
            }
    });
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                float acc = 0;
                for (int i = -r; i <= r; ++i) acc += k[size_t(i + r)] * tmp[size_t(std::clamp(y + i, 0, h - 1)) * size_t(w) + size_t(x)];
                z[size_t(y) * size_t(w) + size_t(x)] = acc;
            }
    });
}

// How much more (above 0) or less (below) light a height field's surface gets at a pixel than level ground does, each
// way as a share of the most it can: 1 facing the light squarely, -1 turned from it to the dark. The field has the
// ring round the frame: (x, y) is the frame's pixel.
struct Light {
    double lx, ly, lz;
    Light(double angleDeg, double altitudeDeg) {
        const double a = angleDeg * kDeg, el = std::clamp(altitudeDeg, 1.0, 89.0) * kDeg;
        lx = std::cos(a) * std::cos(el);
        ly = -std::sin(a) * std::cos(el);  // (the angle turns anticlockwise, as on the page; rows run down)
        lz = std::sin(el);
    }
    float shade(const std::vector<float>& z, int w, int x, int y) const {
        const size_t pw = size_t(w) + 2, i = size_t(y + 1) * pw + size_t(x + 1);
        const double gx = (z[i + 1] - z[i - 1]) * 0.5, gy = (z[i + pw] - z[i - pw]) * 0.5;
        if (gx == 0 && gy == 0) return 0.0f;
        const double lit = (-gx * lx - gy * ly + lz) / std::sqrt(gx * gx + gy * gy + 1) - lz;
        return float(lit > 0 ? std::min(1.0, lit / (1 - lz)) : std::max(-1.0, lit / lz));
    }
    // The same by how far the slope leans towards the light's side alone: 1 for a slope of one in one (or steeper)
    // facing it, -1 facing away, and nothing for a slope the light runs along.
    float lean(const std::vector<float>& z, int w, int x, int y, double ax, double ay) const {
        const size_t pw = size_t(w) + 2, i = size_t(y + 1) * pw + size_t(x + 1);
        const double gx = (z[i + 1] - z[i - 1]) * 0.5, gy = (z[i + pw] - z[i - pw]) * 0.5;
        if (gx == 0 && gy == 0) return 0.0f;
        return float(std::clamp((-gx * ax - gy * ay) / std::sqrt(gx * gx + gy * gy + 1) * M_SQRT2, -1.0, 1.0));
    }
};

float blendChannel(int mode, float base, float over) {
    switch (mode) {
        case 1: return 1 - (1 - base) * (1 - over);  // screen
        case 2: return base + over;                  // add
        case 3: return base * over;                  // multiply
        case 4: return base < 0.5f ? 2 * base * over : 1 - 2 * (1 - base) * (1 - over);  // overlay
        default: return over;
    }
}

}  // namespace

std::vector<float> edgeDistance(const Image& img) {
    if (img.empty()) return {};
    const std::vector<float> ringed = signedDistance(img, true, true);
    const size_t w = size_t(img.width), h = size_t(img.height);
    std::vector<float> sd(w * h);
    for (size_t y = 0; y < h; ++y) std::copy_n(ringed.begin() + std::ptrdiff_t((y + 1) * (w + 2) + 1), w, sd.begin() + std::ptrdiff_t(y * w));
    return sd;
}

void edgeBevel(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    if (img.empty()) return;
    const double thickness = std::max(0.5, e.p("thickness", t, 8) * pixelScale), height = std::max(0.0, e.p("height", t, 8) * pixelScale);
    const float strength = float(std::clamp(e.p("strength", t, 40) / 100, 0.0, 1.0));
    if (height <= 0 || strength <= 0) return;
    const float light[3] = {float(e.p("light.r", t, 1)), float(e.p("light.g", t, 1)), float(e.p("light.b", t, 1))};
    const int w = img.width, h = img.height;
    // The chamfer: it climbs from the edge to `height` over `thickness`, and is level from there in.
    std::vector<float> z = signedDistance(img, true, false);
    const float slope = float(height / thickness), top = float(height);
    for (float& v : z) v = std::clamp(v * slope, 0.0f, top);
    // (the angle turns anticlockwise, as on the page; rows run down)
    const double angle = e.p("angle", t, 45) * kDeg, ax = std::cos(angle), ay = -std::sin(angle);
    const Light sun(0, 45);
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* px = img.row(y);
            for (int x = 0; x < w; ++x, px += 4) {
                if (px[3] <= 0) continue;
                const float s = sun.lean(z, w, x, y, ax, ay);
                if (s > 0) {
                    // Lit: the light's colour added, as much of it as the pixel is covered.
                    const float k = s * strength * px[3];
                    for (int c = 0; c < 3; ++c) px[c] += light[c] * k;
                } else if (s < 0) {
                    const float k = 1 - std::min(1.0f, -s * strength);
                    for (int c = 0; c < 3; ++c) px[c] *= k;
                }
            }
        }
    });
}

void layerBevel(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    if (img.empty()) return;
    const int style = std::clamp(int(std::lround(e.p("style", t, 0))), 0, 3);  // inner, outer, emboss, pillow
    const double size = std::max(0.5, e.p("size", t, 5) * pixelScale), depth = std::clamp(e.p("depth", t, 100) / 100, 0.01, 10.0);
    const float hiOp = float(std::clamp(e.p("highlight_opacity", t, 75) / 100, 0.0, 1.0)),
                shOp = float(std::clamp(e.p("shadow_opacity", t, 75) / 100, 0.0, 1.0));
    if (hiOp <= 0 && shOp <= 0) return;
    const int hiMode = std::clamp(int(std::lround(e.p("highlight_mode", t, 1))), 0, 4), shMode = std::clamp(int(std::lround(e.p("shadow_mode", t, 3))), 0, 4);
    const float hi[3] = {float(e.p("highlight.r", t, 1)), float(e.p("highlight.g", t, 1)), float(e.p("highlight.b", t, 1))};
    const float sh[3] = {float(e.p("shadow.r", t, 0)), float(e.p("shadow.g", t, 0)), float(e.p("shadow.b", t, 0))};
    const int w = img.width, h = img.height;
    const bool outside = style != 0;  // (what it draws beyond the layer's edge)
    std::vector<float> z = signedDistance(img, style != 1, outside);
    // The surface, in pixels of height: a climb from the edge inwards, a climb up to the edge from outside, one across
    // the edge, or a groove along it. Depth makes it steeper.
    const float sz = float(size), k = float(depth);
    for (float& v : z) switch (style) {
            case 0: v = std::clamp(v, 0.0f, sz) * k; break;
            case 1: v = std::clamp(v + sz, 0.0f, sz) * k; break;
            case 2: v = std::clamp(v + sz * 0.5f, 0.0f, sz) * k; break;
            default: v = std::min(std::fabs(v), sz * 0.5f) * k; break;
        }
    soften(z, w + 2, h + 2, e.p("soften", t, 0) * pixelScale);
    const Light sun(e.p("angle", t, 120), e.p("altitude", t, 30));
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* px = img.row(y);
            for (int x = 0; x < w; ++x, px += 4) {
                const float a = std::clamp(px[3], 0.0f, 1.0f);
                if (a <= 0 && !outside) continue;
                const float s = sun.shade(z, w, x, y);
                if (s == 0) continue;
                const float amount = s > 0 ? s * hiOp : -s * shOp;
                const float* tint = s > 0 ? hi : sh;
                const int mode = s > 0 ? hiMode : shMode;
                // On the layer: its colour blended with the tint. Beyond it: the tint itself, under the layer's edge
                // (there is nothing of the layer there to blend with).
                float out[3];
                for (int c = 0; c < 3; ++c) {
                    const float base = a > 0 ? px[c] / a : 0.0f;
                    out[c] = (base + (blendChannel(mode, base, tint[c]) - base) * amount) * a;
                }
                if (outside && a < 1) {
                    const float under = amount * (1 - a);
                    for (int c = 0; c < 3; ++c) out[c] += tint[c] * under;
                    px[3] = a + under;
                }
                for (int c = 0; c < 3; ++c) px[c] = out[c];
            }
        }
    });
}

}  // namespace montage::bevel
