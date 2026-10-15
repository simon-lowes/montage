#include "LensBlur.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "TransferTables.h"

namespace montage {

namespace {

constexpr double kPi = 3.14159265358979323846;

double luma(const float* p) { return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]; }

// Highlights lifted before the blur and brought back after it: light above the threshold raised by a quadratic (an
// in-focus pixel comes back as it was; a highlight spread over a disc keeps more of its light there), and its inverse.
struct Lift {
    double thr = 1, a = 0;
    double up(double y) const {
        if (a <= 0 || y <= thr) return y;
        const double u = y - thr;
        return y + a * u * u;
    }
    double down(double y) const {
        if (a <= 0 || y <= thr) return y;
        return thr + (-1 + std::sqrt(1 + 4 * a * (y - thr))) / (2 * a);
    }
    void apply(float* p, bool lift) const {
        if (a <= 0 || p[3] <= 1e-6f) return;
        const double y = luma(p) / p[3];
        if (y <= thr) return;
        const double to = lift ? up(y) : down(y);
        for (int k = 0; k < 3; ++k) p[k] = float(p[k] * to / y);
    }
};

// Fills what is behind: where `cov` (the coverage of what has been laid down so far) is under 1, the colour of what is
// round about (a pyramid of coverage-weighted means, coarse filling fine), so the layers behind carry on under a nearer
// one whose blurred edge lets them show.
// Only within the box (rx0, ry0) to (rx1, ry1), where the next layer goes.
void fillBehind(std::vector<float>& col, std::vector<float>& cov, int W, int rx0, int ry0, int rx1, int ry1) {
    struct Level {
        int w = 0, h = 0;
        std::vector<float> c, a;  // colour sums and weights; after the pass down, the mean colour (a > 0 where any)
    };
    std::vector<Level> lv;
    const int RW = rx1 - rx0 + 1, RH = ry1 - ry0 + 1;
    int w = RW, h = RH;
    const std::vector<float>* pc = &col;
    const std::vector<float>* pa = &cov;
    bool top = true;  // reading the full picture's box (else a level's own arrays)
    while (w > 1 || h > 1) {
        Level n;
        n.w = (w + 1) / 2, n.h = (h + 1) / 2;
        n.c.assign(size_t(n.w) * n.h * 4, 0);
        n.a.assign(size_t(n.w) * n.h, 0);
        parallelRows(n.h, [&](int ya, int yb) {
            for (int Y = ya; Y < yb; ++Y)
                for (int y = 2 * Y; y < std::min(h, 2 * Y + 2); ++y)
                    for (int x = 0; x < w; ++x) {
                        const size_t i = top ? size_t(y + ry0) * W + x + rx0 : size_t(y) * w + x, j = size_t(Y) * n.w + x / 2;
                        const float a = (*pa)[i];
                        if (a <= 0) continue;
                        n.a[j] += a;
                        for (int k = 0; k < 4; ++k) n.c[j * 4 + k] += (*pc)[i * 4 + k];
                    }
        });
        lv.push_back(std::move(n));
        top = false;
        pc = &lv.back().c, pa = &lv.back().a;
        w = lv.back().w, h = lv.back().h;
    }
    if (lv.empty() || lv.back().a[0] <= 1e-6f) return;  // nothing laid down yet
    // Down the pyramid: each level's mean colour, or its parent's where it has none.
    for (size_t L = lv.size(); L-- > 0;) {
        Level& n = lv[L];
        const Level* parent = L + 1 < lv.size() ? &lv[L + 1] : nullptr;
        for (int y = 0; y < n.h; ++y)
            for (int x = 0; x < n.w; ++x) {
                const size_t j = size_t(y) * n.w + x;
                if (n.a[j] > 1e-6f) {
                    for (int k = 0; k < 4; ++k) n.c[j * 4 + k] /= n.a[j];
                    n.a[j] = 1;
                } else if (parent) {
                    const size_t q = size_t(y / 2) * parent->w + x / 2;
                    for (int k = 0; k < 4; ++k) n.c[j * 4 + k] = parent->c[q * 4 + k];
                    n.a[j] = 1;
                }
            }
    }
    const Level& first = lv[0];
    parallelRows(RH, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < RW; ++x) {
                const size_t i = size_t(y + ry0) * W + x + rx0;
                const float missing = 1 - std::min(1.0f, cov[i]);
                if (missing <= 1e-4f) continue;
                const size_t q = size_t(y / 2) * first.w + x / 2;
                for (int k = 0; k < 4; ++k) col[i * 4 + k] += missing * first.c[q * 4 + k];
                cov[i] = 1;
            }
    });
}

struct Tap {
    int dx, dy;     // where the source pixel is, from the one gathered
    double kx, ky;  // the aperture's position (source to output)
    float w;
};

}  // namespace

double signedCoc(const LensBlurSettings& s, double depth) {
    const double off = std::fabs(depth - s.focus) - s.half;
    if (off <= 0 || s.radius <= 0) return 0;
    const double r = s.radius * std::min(1.0, off / std::max(1e-6, s.falloff));
    if (depth > s.focus) return s.blurNear ? r : 0;
    return -r;
}

float apertureWeight(const Aperture& a, double dx, double dy, double r, double ox, double oy) {
    if (r <= 0.5) return std::fabs(dx) < 0.5 && std::fabs(dy) < 0.5 ? 1.0f : 0.0f;
    const double sq = std::max(1.0, a.squeeze);
    const double x = dx, y = dy / sq;
    const double rot = a.rotation * kPi / 180, c = std::cos(rot), s = std::sin(rot);
    const double xr = x * c + y * s, yr = -x * s + y * c;
    const double dist = std::hypot(xr, yr);
    double edge = r;
    if (a.blades >= 3) {
        // A regular polygon in the circle: a vertex at the rotation's angle.
        const double seg = 2 * kPi / a.blades;
        double m = std::fmod(std::atan2(yr, xr), seg);
        if (m < 0) m += seg;
        edge = r * std::cos(kPi / a.blades) / std::cos(m - kPi / a.blades);
    }
    double w = std::clamp(edge - dist + 0.5, 0.0, 1.0);
    if (ox != 0 || oy != 0) w = std::min(w, std::clamp(r - std::hypot(dx - ox, (dy - oy) / sq) + 0.5, 0.0, 1.0));
    return float(w);
}

double depthAt(const std::vector<float>& depth, int w, int h, double u, double v) {
    if (w <= 0 || h <= 0 || depth.size() != size_t(w) * h) return 0.5;
    const int cx = std::clamp(int(u * w), 0, w - 1), cy = std::clamp(int(v * h), 0, h - 1);
    const int r = std::max(1, std::min(w, h) / 100);
    std::vector<float> around;
    for (int y = std::max(0, cy - r); y <= std::min(h - 1, cy + r); ++y)
        for (int x = std::max(0, cx - r); x <= std::min(w - 1, cx + r); ++x) around.push_back(depth[size_t(y) * w + x]);
    std::nth_element(around.begin(), around.begin() + long(around.size() / 2), around.end());
    return around[around.size() / 2];
}

void breathe(Image& img, double scale) {
    if (img.empty() || std::fabs(scale - 1) < 1e-4 || scale <= 0) return;
    const Image src = img;
    const int W = img.width, H = img.height;
    const double cx = W / 2.0, cy = H / 2.0;
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const double sx = std::clamp(cx + (x + 0.5 - cx) / scale - 0.5, 0.0, double(W - 1));
                const double sy = std::clamp(cy + (y + 0.5 - cy) / scale - 0.5, 0.0, double(H - 1));
                const int x0 = int(sx), y0i = int(sy), x1 = std::min(x0 + 1, W - 1), y1i = std::min(y0i + 1, H - 1);
                const float fx = float(sx - x0), fy = float(sy - y0i);
                const float *a = src.at(x0, y0i), *b = src.at(x1, y0i), *c = src.at(x0, y1i), *d = src.at(x1, y1i);
                float* o = img.at(x, y);
                for (int k = 0; k < 4; ++k) {
                    const float top = a[k] + (b[k] - a[k]) * fx, bottom = c[k] + (d[k] - c[k]) * fx;
                    o[k] = top + (bottom - top) * fy;
                }
            }
    });
}

void lensBlur(Image& img, const std::vector<float>& depth, const LensBlurSettings& s, const TransferTables* tf) {
    const int W = img.width, H = img.height;
    if (img.empty() || depth.size() != size_t(W) * H || s.radius < 0.5) return;
    const size_t N = size_t(W) * H;
    // Each pixel's blur, signed (behind the focus negative), and the layers: blur radii from the farthest to the
    // nearest a step apart, each pixel shared between the two either side of its own.
    std::vector<float> coc(N);
    double lo = 0, hi = 0;
    for (size_t i = 0; i < N; ++i) {
        coc[i] = float(signedCoc(s, depth[i]));
        lo = std::min(lo, double(coc[i])), hi = std::max(hi, double(coc[i]));
    }
    if (hi - lo < 0.5) return;
    const double widest = std::max(-lo, hi);
    const int perSide = std::clamp(int(std::ceil(widest / 2)), 1, 10);
    const double step = widest / perSide;
    const int first = -int(std::ceil(-lo / step - 1e-9)), last = int(std::ceil(hi / step - 1e-9));
    // In light, highlights lifted.
    Lift lift;
    lift.thr = tf ? tf->decode(float(s.threshold)) : s.threshold;
    lift.a = 12 * std::clamp(s.highlights, 0.0, 1.0);
    std::vector<float> lin(N * 4);
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                const float* p = img.at(x, y);
                float* q = &lin[i * 4];
                q[3] = p[3];
                for (int k = 0; k < 3; ++k) q[k] = tf && p[3] > 1e-6f ? tf->decode(p[k] / p[3]) * p[3] : p[k];
                lift.apply(q, true);
            }
    });
    // Each pixel's two layers: the nearer-side share of the one past bin0.
    std::vector<int16_t> bin0(N);
    std::vector<float> share(N);
    const int bins = last - first + 1;
    std::vector<int> bx0(size_t(bins), W), by0(size_t(bins), H), bx1(size_t(bins), -1), by1(size_t(bins), -1);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + x;
            const double pos = std::clamp(coc[i] / step, double(first), double(last));
            const int k0 = std::min(last - 1, int(std::floor(pos)));
            bin0[i] = int16_t(k0);
            share[i] = float(pos - k0);
            for (int k = k0; k <= k0 + 1; ++k) {
                if ((k == k0 ? 1 - share[i] : share[i]) <= 0) continue;
                const size_t b = size_t(k - first);
                bx0[b] = std::min(bx0[b], x), bx1[b] = std::max(bx1[b], x), by0[b] = std::min(by0[b], y), by1[b] = std::max(by1[b], y);
            }
        }
    std::vector<float> acc(N * 4, 0), accA(N, 0);
    // Pixels wholly in the sharp layer that nothing blurred covers keep their own value (no round trip to light).
    std::vector<uint8_t> keep(N, 0);
    const double cx = W / 2.0, cy = H / 2.0, corner = std::hypot(cx, cy);
    const Aperture& ap = s.aperture;
    const double sq = std::max(1.0, ap.squeeze);
    bool any = false;
    for (int k = first; k <= last; ++k) {
        const double centre = k * step, r = std::fabs(centre);
        auto weight = [&](size_t i) { return bin0[i] == k ? 1 - share[i] : bin0[i] + 1 == k ? share[i] : 0.0f; };
        const size_t b = size_t(k - first);
        if (bx1[b] < 0) continue;
        const int bx0k = bx0[b], by0k = by0[b], bx1k = bx1[b], by1k = by1[b];
        if (r < 0.5) {
            // Sharp: laid down as it is.
            for (int y = by0k; y <= by1k; ++y)
                for (int x = bx0k; x <= bx1k; ++x) {
                    const size_t i = size_t(y) * W + x;
                    const float w = weight(i);
                    if (w <= 0) continue;
                    keep[i] = w >= 1;
                    for (int c = 0; c < 4; ++c) acc[i * 4 + c] = lin[i * 4 + c] * w + acc[i * 4 + c] * (1 - w);
                    accA[i] = w + accA[i] * (1 - w);
                }
            any = true;
            continue;
        }
        // Worked on at a scale where the radius is at most 6 pixels, over the layer's box grown by the blur.
        const int f = r > 6 ? int(std::ceil(r / 6)) : 1;
        const double rr = r / f;
        const int grow = int(std::ceil(r * sq)) + 2;
        const int x0 = std::max(0, bx0k - grow), y0 = std::max(0, by0k - grow);
        const int x1 = std::min(W - 1, bx1k + grow), y1 = std::min(H - 1, by1k + grow);
        // What is behind carries on under this layer's blurred edge (filled from a margin round the box, so the
        // colour comes from what surrounds the layer).
        if (any) fillBehind(acc, accA, W, std::max(0, x0 - grow), std::max(0, y0 - grow), std::min(W - 1, x1 + grow), std::min(H - 1, y1 + grow));
        const int lw = (x1 - x0 + f) / f, lh = (y1 - y0 + f) / f;
        std::vector<float> lc(size_t(lw) * lh * 4, 0), la(size_t(lw) * lh, 0);
        const float cell = 1.0f / float(f * f);
        parallelRows(lh, [&](int ya, int yb) {
            for (int Y = ya; Y < yb; ++Y)
                for (int y = y0 + Y * f; y < std::min(y1 + 1, y0 + (Y + 1) * f); ++y)
                    for (int x = x0; x <= x1; ++x) {
                        const size_t i = size_t(y) * W + x, j = size_t(Y) * lw + (x - x0) / f;
                        const float w = weight(i) * cell;
                        if (w <= 0) continue;
                        la[j] += w;
                        for (int c = 0; c < 4; ++c) lc[j * 4 + c] += lin[i * 4 + c] * w;
                    }
        });
        // The aperture as taps: gathering from the source at -k what a scattered disc would put at +k.
        std::vector<Tap> taps;
        double total = 0;
        const int R = int(std::ceil(rr * sq)) + 1;
        for (int ky = -R; ky <= R; ++ky)
            for (int kx = -R; kx <= R; ++kx) {
                const float w = apertureWeight(ap, kx, ky, rr);
                if (w <= 0) continue;
                taps.push_back({-kx, -ky, double(kx), double(ky), w});
                total += w;
            }
        if (taps.empty() || total <= 0) continue;
        const bool catEye = ap.catEye > 0;
        std::vector<float> bc(lc.size(), 0), ba(la.size(), 0);
        parallelRows(lh, [&](int ya, int yb) {
            for (int y = ya; y < yb; ++y)
                for (int x = 0; x < lw; ++x) {
                    double ox = 0, oy = 0;
                    if (catEye) {
                        // Toward the edges a second disc, moved outward, cuts the aperture.
                        const double px = x0 + (x + 0.5) * f, py = y0 + (y + 0.5) * f;
                        ox = ap.catEye * rr * (px - cx) / corner, oy = ap.catEye * rr * sq * (py - cy) / corner;
                    }
                    double sum[4] = {0, 0, 0, 0}, a = 0, norm = 0;
                    for (const Tap& t : taps) {
                        float w = t.w;
                        if (catEye) w = std::min(w, float(std::clamp(rr - std::hypot(t.kx - ox, (t.ky - oy) / sq) + 0.5, 0.0, 1.0)));
                        if (w <= 0) continue;
                        const int sx = x + t.dx, sy = y + t.dy;
                        // Taps past the frame's edge don't count (the edge would fade); past only the window's they
                        // are empty picture and do.
                        if ((sx < 0 && x0 == 0) || (sx >= lw && x1 == W - 1) || (sy < 0 && y0 == 0) || (sy >= lh && y1 == H - 1)) continue;
                        norm += w;
                        if (sx < 0 || sy < 0 || sx >= lw || sy >= lh) continue;
                        const size_t j = size_t(sy) * lw + sx;
                        if (la[j] <= 0) continue;
                        a += w * la[j];
                        for (int c = 0; c < 4; ++c) sum[c] += w * lc[j * 4 + c];
                    }
                    if (norm <= 0) continue;
                    const size_t o = size_t(y) * lw + x;
                    ba[o] = float(a / norm);
                    for (int c = 0; c < 4; ++c) bc[o * 4 + c] = float(sum[c] / norm);
                }
        });
        // Laid over what is behind, back at full size.
        parallelRows(y1 - y0 + 1, [&](int ya, int yb) {
            for (int yy = ya; yy < yb; ++yy) {
                const int y = y0 + yy;
                const double fy = std::clamp((yy + 0.5) / f - 0.5, 0.0, double(lh - 1));
                const int ly0 = int(fy), ly1 = std::min(ly0 + 1, lh - 1);
                const float wy = float(fy - ly0);
                for (int x = x0; x <= x1; ++x) {
                    const double fx = std::clamp((x - x0 + 0.5) / f - 0.5, 0.0, double(lw - 1));
                    const int lx0 = int(fx), lx1 = std::min(lx0 + 1, lw - 1);
                    const float wx = float(fx - lx0);
                    auto at = [&](const std::vector<float>& v, int ch, int stride) {
                        const float a = v[(size_t(ly0) * lw + lx0) * stride + ch], b = v[(size_t(ly0) * lw + lx1) * stride + ch];
                        const float c = v[(size_t(ly1) * lw + lx0) * stride + ch], d = v[(size_t(ly1) * lw + lx1) * stride + ch];
                        const float top = a + (b - a) * wx, bottom = c + (d - c) * wx;
                        return top + (bottom - top) * wy;
                    };
                    const float A = std::min(1.0f, at(ba, 0, 1));
                    if (A <= 0) continue;
                    const size_t i = size_t(y) * W + x;
                    keep[i] = 0;
                    for (int c = 0; c < 4; ++c) acc[i * 4 + c] = at(bc, c, 4) + acc[i * 4 + c] * (1 - A);
                    accA[i] = A + accA[i] * (1 - A);
                }
            }
        });
        any = true;
    }
    // Back from light, highlights brought down.
    parallelRows(H, [&](int ya, int yb) {
        for (int y = ya; y < yb; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                if (keep[i] || accA[i] <= 1e-4f) continue;
                float q[4];
                for (int c = 0; c < 4; ++c) q[c] = acc[i * 4 + c] / accA[i];
                lift.apply(q, false);
                float* p = img.at(x, y);
                p[3] = std::clamp(q[3], 0.0f, 1.0f);
                for (int c = 0; c < 3; ++c) p[c] = tf && q[3] > 1e-6f ? tf->encode(q[c] / q[3]) * p[3] : q[c];
            }
    });
}

}  // namespace montage
