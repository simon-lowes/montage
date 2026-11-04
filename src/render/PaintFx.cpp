#include "PaintFx.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace montage {

namespace {

// A Gaussian blur of `n`-channel rows (in place), edges held: separable, each output from the input alone.
void gaussian(std::vector<float>& v, int w, int h, int n, double sigma) {
    if (sigma <= 0 || w <= 0 || h <= 0) return;
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

int edgeIndex(int i, int n, DisplaceEdge edge) {
    switch (edge) {
        case DisplaceEdge::Wrap: return ((i % n) + n) % n;
        case DisplaceEdge::Clamp: return std::clamp(i, 0, n - 1);
        case DisplaceEdge::Transparent: return i < 0 || i >= n ? -1 : i;
    }
    return -1;
}

// The picture at (x, y) (pixel centres at +0.5), bilinear or bicubic (Catmull-Rom), outside as `edge` says.
void sampleAt(const Image& img, double x, double y, DisplaceEdge edge, bool bicubic, float out[4]) {
    x -= 0.5;
    y -= 0.5;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const double fx = x - x0, fy = y - y0;
    double acc[4] = {0, 0, 0, 0};
    auto add = [&](int xi, int yi, double w) {
        if (w == 0) return;
        const int X = edgeIndex(xi, img.width, edge), Y = edgeIndex(yi, img.height, edge);
        if (X < 0 || Y < 0) return;
        const float* p = img.at(X, Y);
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
    // (Catmull-Rom overshoots at hard edges: coverage kept within 0 and 1)
    if (bicubic) acc[3] = std::clamp(acc[3], 0.0, 1.0);
    for (int c = 0; c < 4; ++c) out[c] = float(acc[c]);
}

// A map's channel as a straight value (0.5, no move, where the map is transparent and the channel is a colour).
float channelOf(const float* p, DisplaceChannel ch) {
    if (ch == DisplaceChannel::Alpha) return p[3];
    if (p[3] <= 0) return 0.5f;
    const float inv = 1.0f / p[3];
    switch (ch) {
        case DisplaceChannel::Luma: return (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) * inv;
        case DisplaceChannel::Red: return p[0] * inv;
        case DisplaceChannel::Green: return p[1] * inv;
        case DisplaceChannel::Blue: return p[2] * inv;
        default: return 0.5f;
    }
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
    const Image src = img;
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                const float* mp = m->at(x, y);
                const double dx = h ? s.horizontalAmount * (2.0 * channelOf(mp, s.horizontal) - 1) : 0;
                const double dy = v ? s.verticalAmount * (2.0 * channelOf(mp, s.vertical) - 1) : 0;
                if (dx == 0 && dy == 0) continue;  // (left exactly as it was)
                sampleAt(src, x + 0.5 + dx, y + 0.5 + dy, s.edge, s.bicubic, img.at(x, y));
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

void anisotropicKuwahara(Image& img, const KuwaharaSettings& s) {
    if (img.empty() || s.radius <= 0) return;
    const int W = img.width, H = img.height;
    const Flow flow = structureFlow(img, s.tensorBlur);
    const Image src = img;
    const double alpha = std::max(0.01, s.anisotropy), q = std::max(0.0, s.sharpness);
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * size_t(W) + size_t(x);
                const double A = flow.anisotropy[i];
                const double a = s.radius * std::clamp((alpha + A) / alpha, 0.1, 2.0);
                const double b = s.radius * std::clamp(alpha / (alpha + A), 0.1, 2.0);
                const double c = flow.tx[i], sn = flow.ty[i];
                const int maxX = int(std::sqrt(a * a * c * c + b * b * sn * sn));
                const int maxY = int(std::sqrt(a * a * sn * sn + b * b * c * c));
                double m[8][5] = {}, sq[8][3] = {};
                for (int j = -maxY; j <= maxY; ++j)
                    for (int k = -maxX; k <= maxX; ++k) {
                        // The offset in the ellipse's frame, scaled to the disc of radius 1/2.
                        double vx = (c * k + sn * j) * 0.5 / a, vy = (-sn * k + c * j) * 0.5 / b;
                        const double d2 = vx * vx + vy * vy;
                        if (d2 > 0.25) continue;
                        const float* p = src.at(std::clamp(x + k, 0, W - 1), std::clamp(y + j, 0, H - 1));
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
    });
}

void contourSmear(Image& img, const SmearSettings& s) {
    if (img.empty() || s.length <= 0) return;
    const int W = img.width, H = img.height;
    const Flow flow = structureFlow(img, s.tensorBlur);
    const Image src = img;
    const int steps = std::max(1, int(std::lround(s.length)));
    const double sigma = std::max(0.5, s.length / 2);
    auto tangent = [&](double x, double y, double& tx, double& ty) {
        const int X = std::clamp(int(std::floor(x)), 0, W - 1), Y = std::clamp(int(std::floor(y)), 0, H - 1);
        const size_t i = size_t(Y) * size_t(W) + size_t(X);
        tx = flow.tx[i], ty = flow.ty[i];
    };
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
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
                        sampleAt(src, px, py, DisplaceEdge::Clamp, false, c);
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
    });
}

void paintEffect(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const double mix = std::clamp(e.p("mix", t, 100) / 100, 0.0, 1.0);
    if (img.empty() || mix <= 0) return;
    const Image before = mix < 1 ? img : Image();
    // (within the ranges the effect offers, whatever was stored; brush and smear capped in pixels on big sources)
    const double detail = std::clamp(e.p("detail", t, 2), 0.5, 20.0) * pixelScale;
    if (std::lround(e.p("style", t, 0)) == 1) {
        SmearSettings s;
        s.length = std::clamp(std::clamp(e.p("length", t, 12), 1.0, 100.0) * pixelScale, 1.0, 200.0);
        s.levels = int(std::lround(std::clamp(e.p("levels", t, 8), 0.0, 64.0)));
        s.tensorBlur = detail;
        contourSmear(img, s);
    } else {
        KuwaharaSettings s;
        s.radius = std::clamp(std::clamp(e.p("radius", t, 6), 1.0, 40.0) * pixelScale, 1.0, 80.0);
        s.sharpness = std::clamp(e.p("sharpness", t, 8), 1.0, 18.0);
        s.anisotropy = std::clamp(e.p("roundness", t, 1), 0.1, 10.0);
        s.tensorBlur = detail;
        anisotropicKuwahara(img, s);
    }
    if (mix < 1)
        for (size_t i = 0; i < img.px.size(); ++i) img.px[i] = float(before.px[i] + (img.px[i] - before.px[i]) * mix);
}

}  // namespace montage
