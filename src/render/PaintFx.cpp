#include "PaintFx.h"

#include "render/Compositor.h"
#include "render/Processing.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace montage {

namespace {

// The most samples a Kuwahara brush takes along its radius (past it, its ellipse is sampled every few pixels).
constexpr double kKuwaharaSamples = 8;

// A box of 2r + 1 across `n`-channel rows (along x) or down columns (along y), edges held, `in` to `out`: a running
// sum, so its cost does not grow with r. Each row (or column) on its own, so any number of threads gives the same.
void boxPass(const std::vector<float>& in, std::vector<float>& out, int w, int h, int n, int r, bool alongY) {
    const double scale = 1.0 / (2 * r + 1);
    if (!alongY) {
        parallelRows(h, [&](int y0, int y1) {
            std::vector<double> acc(static_cast<size_t>(n), 0.0);
            for (int y = y0; y < y1; ++y) {
                const float* row = &in[size_t(y) * size_t(w) * size_t(n)];
                float* o = &out[size_t(y) * size_t(w) * size_t(n)];
                auto at = [&](int x, int c) { return double(row[size_t(std::clamp(x, 0, w - 1)) * size_t(n) + size_t(c)]); };
                for (int c = 0; c < n; ++c) {
                    acc[size_t(c)] = 0;
                    for (int i = -r; i <= r; ++i) acc[size_t(c)] += at(i, c);
                }
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < n; ++c) {
                        o[size_t(x) * size_t(n) + size_t(c)] = float(acc[size_t(c)] * scale);
                        acc[size_t(c)] += at(x + r + 1, c) - at(x - r, c);
                    }
            }
        });
        return;
    }
    // (columns in bands, each band walked down row by row)
    parallelRows(w, [&](int x0, int x1) {
        const size_t span = size_t(x1 - x0) * size_t(n);
        std::vector<double> acc(span, 0.0);
        auto row = [&](int y) { return &in[(size_t(std::clamp(y, 0, h - 1)) * size_t(w) + size_t(x0)) * size_t(n)]; };
        for (int i = -r; i <= r; ++i) {
            const float* q = row(i);
            for (size_t k = 0; k < span; ++k) acc[k] += q[k];
        }
        for (int y = 0; y < h; ++y) {
            float* o = &out[(size_t(y) * size_t(w) + size_t(x0)) * size_t(n)];
            const float* add = row(y + r + 1);
            const float* drop = row(y - r);
            for (size_t k = 0; k < span; ++k) {
                o[k] = float(acc[k] * scale);
                acc[k] += double(add[k]) - double(drop[k]);
            }
        }
    });
}

// A Gaussian blur of `n`-channel rows (in place), edges held: separable, each output from the input alone. Wide ones
// (past a sigma of 3) as three boxes each way (Kovesi's widths, "Fast Almost-Gaussian Filtering", 2010), whose cost
// does not grow with the radius.
void gaussian(std::vector<float>& v, int w, int h, int n, double sigma) {
    if (sigma <= 0 || w <= 0 || h <= 0) return;
    if (sigma > 3) {
        const double ideal = std::sqrt(4 * sigma * sigma + 1);  // (12 sigma^2 / 3 boxes, + 1)
        int lo = int(std::floor(ideal));
        if (lo % 2 == 0) --lo;
        const int m = int(std::lround((12 * sigma * sigma - 3.0 * lo * lo - 12.0 * lo - 9) / (-4.0 * lo - 4)));
        std::vector<float> tmp(v.size());
        for (bool alongY : {false, true})
            for (int pass = 0; pass < 3; ++pass) {
                const int r = (pass < m ? lo : lo + 2) / 2;
                boxPass(v, tmp, w, h, n, r, alongY);
                v.swap(tmp);
            }
        return;
    }
    const int r = std::max(1, int(std::ceil(3 * sigma)));
    std::vector<float> k(size_t(2 * r + 1));
    double sum = 0;
    for (int i = -r; i <= r; ++i) sum += (k[size_t(i + r)] = float(std::exp(-0.5 * i * i / (sigma * sigma))));
    for (float& x : k) x = float(x / sum);
    std::vector<float> tmp(v.size());
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < n; ++c) {
                    float acc = 0;
                    for (int i = -r; i <= r; ++i) acc += k[size_t(i + r)] * v[(size_t(y) * size_t(w) + size_t(std::clamp(x + i, 0, w - 1))) * size_t(n) + size_t(c)];
                    tmp[(size_t(y) * size_t(w) + size_t(x)) * size_t(n) + size_t(c)] = acc;
                }
    });
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < n; ++c) {
                    float acc = 0;
                    for (int i = -r; i <= r; ++i) acc += k[size_t(i + r)] * tmp[(size_t(std::clamp(y + i, 0, h - 1)) * size_t(w) + size_t(x)) * size_t(n) + size_t(c)];
                    v[(size_t(y) * size_t(w) + size_t(x)) * size_t(n) + size_t(c)] = acc;
                }
    });
}

// Index `i` within [lo, hi) as `edge` says (-1: transparent).
int edgeIndex(int i, int lo, int hi, DisplaceEdge edge) {
    const int n = hi - lo;
    switch (edge) {
        case DisplaceEdge::Wrap: return lo + (((i - lo) % n) + n) % n;
        case DisplaceEdge::Clamp: return std::clamp(i, lo, hi - 1);
        case DisplaceEdge::Transparent: return i < lo || i >= hi ? -1 : i;
    }
    return -1;
}

// Where a picture's own pixels are within its frame: [x0, x1) x [y0, y1).
struct Bounds {
    int x0, y0, x1, y1;
};

// The picture at (x, y) (pixel centres at +0.5), bilinear or bicubic (Catmull-Rom), outside `b` as `edge` says.
void sampleAt(const Image& img, const Bounds& b, double x, double y, DisplaceEdge edge, bool bicubic, float out[4]) {
    x -= 0.5;
    y -= 0.5;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    double acc[4] = {0, 0, 0, 0};
    auto pixel = [&](int xi, int yi) -> const float* {
        const int X = edgeIndex(xi, b.x0, b.x1, edge), Y = edgeIndex(yi, b.y0, b.y1, edge);
        return X < 0 || Y < 0 ? nullptr : img.at(X, Y);
    };
    auto add = [&](int xi, int yi, double w) {
        if (w == 0) return;
        if (const float* p = pixel(xi, yi))
            for (int c = 0; c < 4; ++c) acc[c] += w * p[c];
    };
    if (bicubic) {
        auto cr = [](double t, double ws[4]) {
            const double t2 = t * t, t3 = t2 * t;
            ws[0] = -0.5 * t3 + t2 - 0.5 * t;
            ws[1] = 1.5 * t3 - 2.5 * t2 + 1;
            ws[2] = -1.5 * t3 + 2 * t2 + 0.5 * t;
            ws[3] = 0.5 * t3 - 0.5 * t2;
        };
        double wx[4], wy[4];
        cr(fx, wx);
        cr(fy, wy);
        for (int j = 0; j < 4; ++j)
            for (int i = 0; i < 4; ++i) add(x0 - 1 + i, y0 - 1 + j, wx[i] * wy[j]);
    } else {
        add(x0, y0, (1 - fx) * (1 - fy));
        add(x0 + 1, y0, fx * (1 - fy));
        add(x0, y0 + 1, (1 - fx) * fy);
        add(x0 + 1, y0 + 1, fx * fy);
    }
    if (bicubic) {
        // (Catmull-Rom overshoots at hard edges: every channel kept within the four nearest pixels' values, as bilinear
        // would be, and coverage within 0 and 1)
        static const float zero[4] = {0, 0, 0, 0};
        double lo[4], hi[4];
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const float* p = pixel(x0 + i, y0 + j);
                if (!p) p = zero;
                for (int c = 0; c < 4; ++c) {
                    lo[c] = i + j == 0 ? p[c] : std::min(lo[c], double(p[c]));
                    hi[c] = i + j == 0 ? p[c] : std::max(hi[c], double(p[c]));
                }
            }
        for (int c = 0; c < 4; ++c) acc[c] = std::clamp(acc[c], lo[c], hi[c]);
        acc[3] = std::clamp(acc[3], 0.0, 1.0);
    }
    for (int c = 0; c < 4; ++c) out[c] = float(acc[c]);
}

// A map's channel as a straight value, within 0 and 1 (0.5, no move, where the map is transparent and the channel is a
// colour, or where it is not a number).
float channelOf(const float* p, DisplaceChannel ch) {
    float v = 0.5f;
    if (ch == DisplaceChannel::Alpha) {
        v = p[3];
    } else if (p[3] > 0) {
        const float inv = 1.0f / p[3];
        switch (ch) {
            case DisplaceChannel::Luma: v = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) * inv; break;
            case DisplaceChannel::Red: v = p[0] * inv; break;
            case DisplaceChannel::Green: v = p[1] * inv; break;
            case DisplaceChannel::Blue: v = p[2] * inv; break;
            default: break;
        }
    }
    // (an HDR map's light past 1 moves no further than white; NaN moves nothing)
    return std::isnan(v) ? 0.5f : std::clamp(v, 0.0f, 1.0f);
}

// The flow of a picture's structure: at each pixel the unit tangent (along its edges and contours, across its
// gradient) and the anisotropy (0 where there is no direction, towards 1 along a clean edge), from the structure
// tensor of its luma (Sobel), smoothed by a Gaussian of `sigma`.
struct Flow {
    std::vector<float> tx, ty, anisotropy;
};
Flow structureFlow(const Image& img, double sigma) {
    const int W = img.width, H = img.height;
    std::vector<float> luma(size_t(W) * size_t(H));
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* p = img.at(x, y);
            luma[size_t(y) * size_t(W) + size_t(x)] = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        }
    auto L = [&](int x, int y) { return luma[size_t(std::clamp(y, 0, H - 1)) * size_t(W) + size_t(std::clamp(x, 0, W - 1))]; };
    std::vector<float> tensor(size_t(W) * size_t(H) * 3);
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const float gx = ((L(x + 1, y - 1) + 2 * L(x + 1, y) + L(x + 1, y + 1)) - (L(x - 1, y - 1) + 2 * L(x - 1, y) + L(x - 1, y + 1))) / 4;
                const float gy = ((L(x - 1, y + 1) + 2 * L(x, y + 1) + L(x + 1, y + 1)) - (L(x - 1, y - 1) + 2 * L(x, y - 1) + L(x + 1, y - 1))) / 4;
                float* t = &tensor[(size_t(y) * size_t(W) + size_t(x)) * 3];
                t[0] = gx * gx, t[1] = gx * gy, t[2] = gy * gy;
            }
    });
    gaussian(tensor, W, H, 3, sigma);
    Flow f;
    f.tx.resize(size_t(W) * size_t(H));
    f.ty.resize(f.tx.size());
    f.anisotropy.resize(f.tx.size());
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * size_t(W) + size_t(x);
                const double E = tensor[i * 3], F = tensor[i * 3 + 1], G = tensor[i * 3 + 2];
                const double root = std::sqrt((E - G) * (E - G) + 4 * F * F);
                const double l1 = (E + G + root) / 2, l2 = (E + G - root) / 2;
                double tx = l1 - E, ty = -F;
                const double len = std::hypot(tx, ty);
                if (len > 1e-12) {
                    tx /= len, ty /= len;
                } else {
                    // (no direction, or an edge straight across: the tangent then runs down when it is vertical)
                    tx = E >= G && E > 0 ? 0 : 1, ty = E >= G && E > 0 ? 1 : 0;
                }
                f.tx[i] = float(tx), f.ty[i] = float(ty);
                f.anisotropy[i] = l1 + l2 > 1e-12 ? float((l1 - l2) / (l1 + l2)) : 0.0f;
            }
    });
    return f;
}

}  // namespace

namespace {
// Displace within a picture turned at an angle (DisplaceSettings::turned): each pixel whose centre lies in it takes its
// colour from where it is moved to, that place wrapped, held or ended at the picture's own edges, found in the
// picture's own axes (and, to wrap or hold, kept clear of its edges by the sampling's reach, so no pixel from outside it
// comes in).
void displaceTurned(Image& img, const Image& m, const DisplaceSettings& s, bool h, bool v) {
    const double* o = s.origin;
    const double* A = s.across;
    const double* D = s.down;
    const double det = A[0] * D[1] - A[1] * D[0];
    if (!(std::fabs(det) > 1e-9)) return;
    // (a frame point in the picture's own axes: 0 to 1 across and down it)
    auto local = [&](double x, double y, double& a, double& b) {
        const double rx = x - o[0], ry = y - o[1];
        a = (rx * D[1] - ry * D[0]) / det;
        b = (A[0] * ry - A[1] * rx) / det;
    };
    const double reach = s.bicubic ? 3.0 : 1.5;  // (frame pixels a sample reads about it, turned)
    const double ia = std::min(0.5, reach / std::max(1e-9, std::hypot(A[0], A[1])));
    const double ib = std::min(0.5, reach / std::max(1e-9, std::hypot(D[0], D[1])));
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (double a : {0.0, 1.0})
        for (double b : {0.0, 1.0}) {
            const double X = o[0] + a * A[0] + b * D[0], Y = o[1] + a * A[1] + b * D[1];
            x0 = std::min(x0, X), x1 = std::max(x1, X), y0 = std::min(y0, Y), y1 = std::max(y1, Y);
        }
    const int c0 = int(std::clamp(std::floor(x0), 0.0, double(img.width))), c1 = int(std::clamp(std::ceil(x1), 0.0, double(img.width)));
    const int r0 = int(std::clamp(std::floor(y0), 0.0, double(img.height))), r1 = int(std::clamp(std::ceil(y1), 0.0, double(img.height)));
    if (c1 <= c0 || r1 <= r0) return;
    const Bounds whole{0, 0, img.width, img.height};
    const Image src = img;
    parallelRows(r1 - r0, [&](int q0, int q1) {
        for (int y = r0 + q0; y < r0 + q1; ++y)
            for (int x = c0; x < c1; ++x) {
                double a, b;
                local(x + 0.5, y + 0.5, a, b);
                if (a < 0 || a >= 1 || b < 0 || b >= 1) continue;  // (not the picture's: left as it is)
                const float* mp = m.at(x, y);
                const double dx = h ? s.horizontalAmount * (2.0 * channelOf(mp, s.horizontal) - 1) : 0;
                const double dy = v ? s.verticalAmount * (2.0 * channelOf(mp, s.vertical) - 1) : 0;
                if (dx == 0 && dy == 0) continue;
                local(x + 0.5 + dx, y + 0.5 + dy, a, b);
                float* out = img.at(x, y);
                if (s.edge == DisplaceEdge::Transparent) {
                    if (a < 0 || a >= 1 || b < 0 || b >= 1) {
                        out[0] = out[1] = out[2] = out[3] = 0;
                        continue;
                    }
                } else {
                    if (s.edge == DisplaceEdge::Wrap) a -= std::floor(a), b -= std::floor(b);
                    a = std::clamp(a, ia, 1 - ia), b = std::clamp(b, ib, 1 - ib);
                }
                sampleAt(src, whole, o[0] + a * A[0] + b * D[0], o[1] + a * A[1] + b * D[1], DisplaceEdge::Clamp, s.bicubic, out);
            }
    });
}
}  // namespace

void displaceByMap(Image& img, const Image& map, const DisplaceSettings& s) {
    if (img.empty() || map.width != img.width || map.height != img.height) return;
    const bool h = s.horizontal != DisplaceChannel::Off && s.horizontalAmount != 0;
    const bool v = s.vertical != DisplaceChannel::Off && s.verticalAmount != 0;
    if (!h && !v) return;
    const Image* m = &map;
    Image blurred;
    if (s.mapBlur > 0) {
        blurred = map;
        std::vector<float> data(blurred.px.begin(), blurred.px.end());
        gaussian(data, map.width, map.height, 4, s.mapBlur / 2);  // (a radius of about two sigmas)
        std::copy(data.begin(), data.end(), blurred.px.begin());
        m = &blurred;
    }
    if (s.turned) {
        displaceTurned(img, *m, s, h, v);
        return;
    }
    // (the picture's own pixels, where it is smaller than the frame: its edges wrap, hold or end there)
    Bounds b{0, 0, img.width, img.height};
    if (s.right > s.left && s.bottom > s.top)
        b = Bounds{std::clamp(s.left, 0, img.width), std::clamp(s.top, 0, img.height), std::clamp(s.right, 0, img.width),
                   std::clamp(s.bottom, 0, img.height)};
    if (b.x1 <= b.x0 || b.y1 <= b.y0) return;
    const Image src = img;
    parallelRows(b.y1 - b.y0, [&](int r0, int r1) {
        for (int y = b.y0 + r0; y < b.y0 + r1; ++y)
            for (int x = b.x0; x < b.x1; ++x) {
                const float* mp = m->at(x, y);
                const double dx = h ? s.horizontalAmount * (2.0 * channelOf(mp, s.horizontal) - 1) : 0;
                const double dy = v ? s.verticalAmount * (2.0 * channelOf(mp, s.vertical) - 1) : 0;
                if (dx == 0 && dy == 0) continue;  // (left exactly as it was)
                sampleAt(src, b, x + 0.5 + dx, y + 0.5 + dy, s.edge, s.bicubic, img.at(x, y));
            }
    });
}

DisplaceSettings displaceSettings(const Effect& e, FrameTime t, double pixelScale) {
    DisplaceSettings s;
    auto channel = [&](const char* name, int def) { return DisplaceChannel(std::clamp(int(std::lround(e.p(name, t, def))), 0, 5)); };
    s.horizontal = channel("h_channel", 1);
    s.vertical = channel("v_channel", 2);
    // (within the ranges the effect offers, whatever was stored)
    s.horizontalAmount = std::clamp(e.p("h_amount", t, 20), -4000.0, 4000.0) * pixelScale;
    s.verticalAmount = std::clamp(e.p("v_amount", t, 20), -4000.0, 4000.0) * pixelScale;
    s.edge = DisplaceEdge(std::clamp(int(std::lround(e.p("edges", t, 1))), 0, 2));
    s.bicubic = std::lround(e.p("sampling", t, 0)) == 1;
    s.mapBlur = std::clamp(e.p("map_blur", t, 0), 0.0, 200.0) * pixelScale;
    return s;
}

bool anisotropicKuwahara(Image& img, const KuwaharaSettings& s, const std::atomic<bool>* cancel) {
    if (img.empty() || s.radius <= 0) return true;
    const int W = img.width, H = img.height;
    const Flow flow = structureFlow(img, s.tensorBlur);
    const Image src = img;
    const double alpha = std::max(0.01, s.anisotropy), q = std::max(0.0, s.sharpness);
    // A wide brush samples its ellipse every `step` pixels, from the picture and its squares averaged over a box of about
    // that size (so each sample stands for the pixels about it, and the sectors' means and variances hold): at most
    // about 200 samples a pixel whatever the radius.
    const int step = std::max(1, int(std::ceil(s.radius / kKuwaharaSamples)));
    std::vector<float> boxMean, boxSq;
    if (step > 1) {
        boxMean.assign(src.px.begin(), src.px.end());
        boxSq.resize(size_t(W) * size_t(H) * 3);
        for (size_t i = 0, n = size_t(W) * size_t(H); i < n; ++i)
            for (int ch = 0; ch < 3; ++ch) boxSq[i * 3 + size_t(ch)] = boxMean[i * 4 + size_t(ch)] * boxMean[i * 4 + size_t(ch)];
        std::vector<float> tmp;
        for (bool alongY : {false, true}) {
            tmp.resize(boxMean.size());
            boxPass(boxMean, tmp, W, H, 4, step / 2, alongY);
            boxMean.swap(tmp);
            tmp.resize(boxSq.size());
            boxPass(boxSq, tmp, W, H, 3, step / 2, alongY);
            boxSq.swap(tmp);
        }
    }
    const float* P = step > 1 ? boxMean.data() : src.px.data();
    std::atomic<bool> stopped{false};
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                stopped = true;
                return;
            }
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * size_t(W) + size_t(x);
                const double A = flow.anisotropy[i];
                const double a = s.radius * std::clamp((alpha + A) / alpha, 0.1, 2.0);
                const double b = s.radius * std::clamp(alpha / (alpha + A), 0.1, 2.0);
                const double c = flow.tx[i], sn = flow.ty[i];
                const int maxX = int(std::sqrt(a * a * c * c + b * b * sn * sn)) / step * step;
                const int maxY = int(std::sqrt(a * a * sn * sn + b * b * c * c)) / step * step;
                double m[8][5] = {}, sq[8][3] = {};
                for (int j = -maxY; j <= maxY; j += step)
                    for (int k = -maxX; k <= maxX; k += step) {
                        // The offset in the ellipse's frame, scaled to the disc of radius 1/2.
                        double vx = (c * k + sn * j) * 0.5 / a, vy = (-sn * k + c * j) * 0.5 / b;
                        const double d2 = vx * vx + vy * vy;
                        if (d2 > 0.25) continue;
                        const size_t at = size_t(std::clamp(y + j, 0, H - 1)) * size_t(W) + size_t(std::clamp(x + k, 0, W - 1));
                        const float* p = P + at * 4;
                        // Polynomial sector weights: four sectors on the axes, four between them.
                        double w[8], n = 0;
                        for (int half = 0; half < 2; ++half) {
                            if (half) {
                                const double rx = std::sqrt(0.5) * (vx - vy), ry = std::sqrt(0.5) * (vx + vy);
                                vx = rx, vy = ry;
                            }
                            const double xx = 0.33 - 0.84 * vx * vx, yy = 0.33 - 0.84 * vy * vy;
                            double z;
                            z = std::max(0.0, vy + xx), n += w[half] = z * z;
                            z = std::max(0.0, -vx + yy), n += w[2 + half] = z * z;
                            z = std::max(0.0, -vy + xx), n += w[4 + half] = z * z;
                            z = std::max(0.0, vx + yy), n += w[6 + half] = z * z;
                        }
                        if (n <= 0) continue;
                        const double g = std::exp(-3.125 * d2) / n;
                        for (int sct = 0; sct < 8; ++sct) {
                            const double ws = w[sct] * g;
                            if (ws == 0) continue;
                            for (int ch = 0; ch < 4; ++ch) m[sct][ch] += ws * p[ch];
                            if (step > 1)
                                for (int ch = 0; ch < 3; ++ch) sq[sct][ch] += ws * boxSq[at * 3 + size_t(ch)];
                            else
                                for (int ch = 0; ch < 3; ++ch) sq[sct][ch] += ws * double(p[ch]) * p[ch];
                            m[sct][4] += ws;
                        }
                    }
                double out[4] = {0, 0, 0, 0}, total = 0;
                for (int sct = 0; sct < 8; ++sct) {
                    const double ws = m[sct][4];
                    if (ws <= 0) continue;
                    double sigma2 = 0;
                    for (int ch = 0; ch < 3; ++ch) {
                        const double mean = m[sct][ch] / ws;
                        sigma2 += std::fabs(sq[sct][ch] / ws - mean * mean);
                    }
                    const double weight = 1 / (1 + std::pow(255 * sigma2, 0.5 * q));
                    for (int ch = 0; ch < 4; ++ch) out[ch] += weight * m[sct][ch] / ws;
                    total += weight;
                }
                if (total <= 0) continue;
                float* d = img.at(x, y);
                for (int ch = 0; ch < 4; ++ch) d[ch] = float(out[ch] / total);
            }
        }
    });
    if (!stopped) return true;
    img = src;
    return false;
}

bool contourSmear(Image& img, const SmearSettings& s, const std::atomic<bool>* cancel) {
    if (img.empty() || s.length <= 0) return true;
    const int W = img.width, H = img.height;
    const Flow flow = structureFlow(img, s.tensorBlur);
    const Image src = img;
    const int steps = std::max(1, int(std::lround(s.length)));
    const double sigma = std::max(0.5, s.length / 2);
    const Bounds whole{0, 0, W, H};
    auto tangent = [&](double x, double y, double& tx, double& ty) {
        const int X = std::clamp(int(std::floor(x)), 0, W - 1), Y = std::clamp(int(std::floor(y)), 0, H - 1);
        const size_t i = size_t(Y) * size_t(W) + size_t(X);
        tx = flow.tx[i], ty = flow.ty[i];
    };
    std::atomic<bool> stopped{false};
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                stopped = true;
                return;
            }
            for (int x = 0; x < W; ++x) {
                double acc[4] = {0, 0, 0, 0}, total = 0;
                {
                    const float* p = src.at(x, y);
                    for (int c = 0; c < 4; ++c) acc[c] += p[c];
                    total += 1;
                }
                for (int dir = -1; dir <= 1; dir += 2) {
                    double px = x + 0.5, py = y + 0.5, lx = 0, ly = 0;
                    tangent(px, py, lx, ly);
                    lx *= dir, ly *= dir;
                    for (int k = 1; k <= steps; ++k) {
                        // A step of a pixel along the flow, its direction kept from turning back (a tangent has no
                        // sign), at the midpoint's tangent.
                        double tx, ty;
                        tangent(px + lx * 0.5, py + ly * 0.5, tx, ty);
                        if (tx * lx + ty * ly < 0) tx = -tx, ty = -ty;
                        px += tx, py += ty;
                        lx = tx, ly = ty;
                        if (px < 0 || py < 0 || px >= W || py >= H) break;
                        const double w = std::exp(-0.5 * k * k / (sigma * sigma));
                        float c[4];
                        sampleAt(src, whole, px, py, DisplaceEdge::Clamp, false, c);
                        for (int ch = 0; ch < 4; ++ch) acc[ch] += w * c[ch];
                        total += w;
                    }
                }
                float* d = img.at(x, y);
                for (int ch = 0; ch < 4; ++ch) d[ch] = float(acc[ch] / total);
                if (s.levels >= 2 && d[3] > 0) {
                    const float n = float(s.levels - 1);
                    for (int ch = 0; ch < 3; ++ch) {
                        const float straight = std::clamp(d[ch] / d[3], 0.0f, 1.0f);
                        d[ch] = std::round(straight * n) / n * d[3];
                    }
                }
            }
        }
    });
    if (!stopped) return true;
    img = src;
    return false;
}

void paintEffect(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const double mix = std::clamp(e.p("mix", t, 100) / 100, 0.0, 1.0);
    if (img.empty() || mix <= 0) return;
    const Image before = mix < 1 ? img : Image();
    // (within the ranges the effect offers, whatever was stored; brush and smear capped in pixels on big sources)
    const double detail = std::clamp(e.p("detail", t, 2), 0.5, 20.0) * pixelScale;
    // (it runs for seconds a frame on big pictures: stopped with the render)
    const SlowEffects& slow = currentSlowEffects();
    bool finished = true;
    if (std::lround(e.p("style", t, 0)) == 1) {
        SmearSettings s;
        s.length = std::clamp(std::clamp(e.p("length", t, 12), 1.0, 100.0) * pixelScale, 1.0, 200.0);
        s.levels = int(std::lround(std::clamp(e.p("levels", t, 8), 0.0, 64.0)));
        s.tensorBlur = detail;
        finished = contourSmear(img, s, slow.cancel);
    } else {
        KuwaharaSettings s;
        s.radius = std::clamp(std::clamp(e.p("radius", t, 6), 1.0, 40.0) * pixelScale, 1.0, 80.0);
        s.sharpness = std::clamp(e.p("sharpness", t, 8), 1.0, 18.0);
        s.anisotropy = std::clamp(e.p("roundness", t, 1), 0.1, 10.0);
        s.tensorBlur = detail;
        finished = anisotropicKuwahara(img, s, slow.cancel);
    }
    if (!finished) {
        // (the render was stopped part way: the picture as it came, and the frame not final)
        if (slow.issues) slow.issues->note("Painterly was stopped before it finished");
        return;
    }
    if (mix < 1)
        for (size_t i = 0; i < img.px.size(); ++i) img.px[i] = float(before.px[i] + (img.px[i] - before.px[i]) * mix);
}

}  // namespace montage
