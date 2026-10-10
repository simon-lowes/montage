#include "VideoFx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "Processing.h"

namespace montage::vfx {

namespace {

// Bilinear sample at (x, y) (pixel centres at +0.5); transparent outside the frame.
void sample(const Image& img, double x, double y, float out[4]) {
    x -= 0.5;
    y -= 0.5;
    const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    const float fx = float(x - x0), fy = float(y - y0);
    auto px = [&](int xi, int yi, int k) -> float {
        return xi < 0 || yi < 0 || xi >= img.width || yi >= img.height ? 0.f : img.at(xi, yi)[k];
    };
    for (int k = 0; k < 4; ++k) {
        const float top = px(x0, y0, k) + (px(x0 + 1, y0, k) - px(x0, y0, k)) * fx;
        const float bottom = px(x0, y0 + 1, k) + (px(x0 + 1, y0 + 1, k) - px(x0, y0 + 1, k)) * fx;
        out[k] = top + (bottom - top) * fy;
    }
}

// Each pixel from wherever `from` says, in parallel rows.
template <class F>
void remap(Image& img, F from) {
    const Image src = img;
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                double sx, sy;
                if (!from(x + 0.5, y + 0.5, sx, sy)) {
                    std::fill(p, p + 4, 0.f);
                    continue;
                }
                sample(src, sx, sy, p);
            }
        }
    });
}

// Colour work on straight (not premultiplied) values.
template <class F>
void straight(Image& img, F fn) {
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                const float a = p[3];
                if (a <= 0) continue;
                for (int c = 0; c < 3; ++c) p[c] = fn(p[c] / a) * a;
            }
        }
    });
}

uint32_t hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

}  // namespace

void levels(const Effect& e, FrameTime t, Image& img) {
    const float inB = float(e.p("in_black", t, 0)), inW = float(e.p("in_white", t, 1));
    const float gamma = float(std::max(0.05, e.p("gamma", t, 1)));
    const float outB = float(e.p("out_black", t, 0)), outW = float(e.p("out_white", t, 1));
    const float span = std::max(1e-4f, inW - inB);
    straight(img, [&](float v) {
        float u = std::clamp((v - inB) / span, 0.f, 1.f);
        u = std::pow(u, 1.f / gamma);
        return outB + u * (outW - outB);
    });
}

void glow(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const float threshold = float(e.p("threshold", t, 0.7));
    const double radius = e.p("radius", t, 20) * pixelScale;
    const float intensity = float(e.p("intensity", t, 1));
    // What is brighter than the threshold, blurred and added back.
    Image bright = img;
    parallelRows(bright.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = bright.row(y);
            for (int x = 0; x < bright.width; ++x, p += 4) {
                const float a = p[3];
                const float l = a > 0 ? (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / a : 0.f;
                const float k = l > threshold ? (l - threshold) / std::max(1e-4f, 1 - threshold) : 0.f;
                for (int c = 0; c < 3; ++c) p[c] *= k;
                p[3] *= k;
            }
        }
    });
    if (radius > 0.5) gaussianBlur(bright, radius);
    for (size_t i = 0; i < img.px.size(); i += 4) {
        for (int c = 0; c < 3; ++c) img.px[i + c] += bright.px[i + c] * intensity;
        img.px[i + 3] = std::min(1.f, img.px[i + 3] + bright.px[i + 3] * intensity);
    }
}

void filmGrain(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const float amount = float(e.p("amount", t, 0.15));
    const double size = std::max(0.5, e.p("size", t, 1.5) * pixelScale);
    const bool colour = e.p("color", t) > 0.5;
    // A noise grid at the grain's size, new every frame, with a little blur to round it.
    const int gw = std::max(1, int(std::ceil(img.width / size))), gh = std::max(1, int(std::ceil(img.height / size)));
    Image noise(gw, gh);
    const uint32_t seed = hash(uint32_t(t) * 2654435761U + 12345U);
    for (int y = 0; y < gh; ++y)
        for (int x = 0; x < gw; ++x) {
            float* n = noise.at(x, y);
            for (int c = 0; c < 3; ++c) {
                const uint32_t cell = uint32_t(x) * 73856093U ^ uint32_t(y) * 19349663U ^ uint32_t(colour ? c : 0) * 83492791U;
                const uint32_t h = hash(seed ^ hash(cell));
                n[c] = float(h & 0xffff) / 65535.f - 0.5f;
            }
            n[3] = 1;
        }
    const double sx = double(gw) / img.width, sy = double(gh) / img.height;
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                if (p[3] <= 0) continue;
                float g[4];
                sample(noise, (x + 0.5) * sx, (y + 0.5) * sy, g);
                // Strongest in the mid-tones, as film's is.
                const float l = std::clamp((0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / p[3], 0.f, 1.f);
                const float k = amount * (0.4f + 2.4f * l * (1 - l)) * p[3];
                for (int c = 0; c < 3; ++c) p[c] = std::max(0.f, p[c] + g[c] * k);
            }
        }
    });
}

void directionalBlur(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const double length = e.p("length", t, 20) * pixelScale;
    if (length < 0.5) return;
    const double ang = e.p("angle", t, 0) * M_PI / 180;
    const double dx = std::cos(ang), dy = std::sin(ang);
    const int taps = std::clamp(int(std::ceil(length)), 2, 96);
    const Image src = img;
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                float acc[4] = {0, 0, 0, 0};
                for (int k = 0; k < taps; ++k) {
                    const double s = (double(k) / (taps - 1) - 0.5) * length;
                    float v[4];
                    sample(src, x + 0.5 + dx * s, y + 0.5 + dy * s, v);
                    for (int c = 0; c < 4; ++c) acc[c] += v[c];
                }
                for (int c = 0; c < 4; ++c) p[c] = acc[c] / float(taps);
            }
        }
    });
}

void chromaticAberration(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const double amount = e.p("amount", t, 3) * pixelScale;
    if (std::fabs(amount) < 0.01) return;
    const double cx = img.width / 2.0, cy = img.height / 2.0, half = std::hypot(cx, cy);
    const double k = amount / half;  // red spreads out by `amount` at the corners, blue in
    const Image src = img;
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                const double ox = x + 0.5 - cx, oy = y + 0.5 - cy;
                float r[4], b[4];
                sample(src, cx + ox / (1 + k), cy + oy / (1 + k), r);
                sample(src, cx + ox / (1 - k), cy + oy / (1 - k), b);
                p[0] = r[0];
                p[2] = b[2];
                p[3] = std::max({p[3], r[3], b[3]});
            }
        }
    });
}

void lensDistortion(const Effect& e, FrameTime t, Image& img) {
    // Positive: barrel (the middle bulges out); negative: pincushion.
    const double amount = std::clamp(e.p("amount", t, 0) / 100.0, -1.0, 1.0);
    if (std::fabs(amount) < 1e-4) return;
    const double cx = img.width / 2.0, cy = img.height / 2.0, norm = std::hypot(cx, cy);
    remap(img, [&](double x, double y, double& sx, double& sy) {
        const double nx = (x - cx) / norm, ny = (y - cy) / norm, r2 = nx * nx + ny * ny;
        // Barrel samples further out the further from the middle, which pulls the edges in.
        const double f = 1 + amount * 0.5 * r2;
        sx = cx + nx * f * norm;
        sy = cy + ny * f * norm;
        return sx >= 0 && sy >= 0 && sx <= img.width && sy <= img.height;
    });
}

void magnify(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const bool square = e.p("shape", t, 0) >= 0.5;
    const double cx = e.p("center_x", t, 0.5) * img.width, cy = e.p("center_y", t, 0.5) * img.height;
    const double radius = std::max(1.0, e.p("size", t, 40) / 100.0 * img.height * 0.5);
    const double mag = std::max(1.0, e.p("magnification", t, 200) / 100.0);
    const double feather = std::max(0.0, e.p("feather", t, 0) * pixelScale), border = std::max(0.0, e.p("border", t, 2) * pixelScale);
    const float bc[3] = {float(e.p("border_color.r", t, 1)), float(e.p("border_color.g", t, 1)), float(e.p("border_color.b", t, 1))};
    const float opacity = float(std::clamp(e.p("opacity", t, 100) / 100.0, 0.0, 1.0));
    if (opacity <= 0) return;
    const Image src = img;
    const double reach = radius + border + 1;
    const int y0 = std::max(0, int(cy - reach)), y1 = std::min(img.height, int(cy + reach) + 1);
    const int x0 = std::max(0, int(cx - reach)), x1 = std::min(img.width, int(cx + reach) + 1);
    parallelRows(y1 - y0, [&](int r0, int r1) {
        for (int y = y0 + r0; y < y0 + r1; ++y) {
            float* p = img.row(y) + size_t(x0) * 4;
            for (int x = x0; x < x1; ++x, p += 4) {
                const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
                const double d = square ? std::max(std::fabs(dx), std::fabs(dy)) : std::hypot(dx, dy);
                // The lens: everything seen from the centre, `mag` times larger; a soft (or anti-aliased) edge.
                const double cover = feather > 0 ? std::clamp((radius - d) / feather, 0.0, 1.0) : std::clamp(radius - d + 0.5, 0.0, 1.0);
                if (cover > 0) {
                    float m[4];
                    sample(src, cx + dx / mag, cy + dy / mag, m);
                    const float a = float(cover) * opacity;
                    for (int k = 0; k < 4; ++k) p[k] += (m[k] - p[k]) * a;
                }
                if (border > 0) {
                    const double ring = std::clamp(std::min(d - radius + 0.5, radius + border - d + 0.5), 0.0, 1.0);
                    if (ring > 0) {
                        const float a = float(ring) * opacity;
                        for (int k = 0; k < 3; ++k) p[k] += (bc[k] - p[k]) * a;
                        p[3] += (1.0f - p[3]) * a;
                    }
                }
            }
        }
    });
}

void channelBlur(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    const char* names[4] = {"red", "green", "blue", "alpha"};
    const int dims = int(std::lround(e.p("dimensions", t, 0)));  // both, horizontal, vertical
    for (int c = 0; c < 4; ++c) {
        const double r = std::max(0.0, e.p(names[c], t, 0)) * pixelScale;
        if (r < 0.05) continue;
        Image blurred = img;
        gaussianBlur(blurred, r, dims != 2, dims != 1);
        for (size_t i = size_t(c); i < img.px.size(); i += 4) img.px[i] = blurred.px[i];
    }
}

namespace {
// A well-mixed 32-bit hash of a pixel, frame and channel (for noise that is the same each time a frame is drawn).
inline uint32_t mix(uint32_t x, uint32_t y, uint32_t t, uint32_t c) {
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ t * 0xcb1ab31fu ^ c * 0x165667b1u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
}  // namespace

void noise(const Effect& e, FrameTime t, Image& img) {
    const float amount = float(std::clamp(e.p("amount", t, 20) / 100.0, 0.0, 1.0));
    if (amount <= 0) return;
    const bool colour = e.p("color", t, 1) >= 0.5, clip = e.p("clip", t, 1) >= 0.5;
    const uint32_t frame = uint32_t(t);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                const float a = p[3];
                if (a <= 0) continue;
                for (int c = 0; c < 3; ++c) {
                    const float n = (float(mix(uint32_t(x), uint32_t(y), frame, colour ? uint32_t(c) : 0u)) / 4294967296.0f - 0.5f) * amount;
                    float v = p[c] / a + n;  // on straight values
                    if (clip) v = std::clamp(v, 0.0f, 1.0f);
                    p[c] = v * a;
                }
            }
        }
    });
}

bool squareToQuad(const double q[4][2], double h[9]) {
    // Heckbert's closed form for the unit square to a quadrilateral.
    const double x0 = q[0][0], y0 = q[0][1], x1 = q[1][0], y1 = q[1][1], x2 = q[2][0], y2 = q[2][1], x3 = q[3][0], y3 = q[3][1];
    const double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    double g = 0, hh = 0;
    if (std::fabs(sx) > 1e-12 || std::fabs(sy) > 1e-12) {
        const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
        const double den = dx1 * dy2 - dx2 * dy1;
        if (std::fabs(den) < 1e-12) return false;
        g = (sx * dy2 - dx2 * sy) / den;
        hh = (dx1 * sy - sx * dy1) / den;
    }
    h[0] = x1 - x0 + g * x1;
    h[1] = x3 - x0 + hh * x3;
    h[2] = x0;
    h[3] = y1 - y0 + g * y1;
    h[4] = y3 - y0 + hh * y3;
    h[5] = y0;
    h[6] = g;
    h[7] = hh;
    h[8] = 1;
    return true;
}

void cornerPin(const Effect& e, FrameTime t, Image& img) {
    // Corners as fractions of the frame: top left, top right, bottom right, bottom left.
    const double W = img.width, H = img.height;
    const double q[4][2] = {{e.p("tl_x", t, 0) * W, e.p("tl_y", t, 0) * H},
                            {e.p("tr_x", t, 1) * W, e.p("tr_y", t, 0) * H},
                            {e.p("br_x", t, 1) * W, e.p("br_y", t, 1) * H},
                            {e.p("bl_x", t, 0) * W, e.p("bl_y", t, 1) * H}};
    double m[9];
    if (!squareToQuad(q, m)) return;
    // Its inverse (the adjugate is enough for a projective map), for output to source.
    const double inv[9] = {m[4] * m[8] - m[5] * m[7], m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4],
                           m[5] * m[6] - m[3] * m[8], m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5],
                           m[3] * m[7] - m[4] * m[6], m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3]};
    remap(img, [&](double x, double y, double& sx, double& sy) {
        const double w = inv[6] * x + inv[7] * y + inv[8];
        if (std::fabs(w) < 1e-12) return false;
        const double u = (inv[0] * x + inv[1] * y + inv[2]) / w, v = (inv[3] * x + inv[4] * y + inv[5]) / w;
        if (u < 0 || v < 0 || u > 1 || v > 1) return false;
        sx = u * W;
        sy = v * H;
        return true;
    });
}

void letterbox(const Effect& e, FrameTime t, Image& img) {
    static const double ratios[] = {2.39, 2.0, 1.85, 4.0 / 3.0, 1.0, 9.0 / 16.0};
    const int choice = std::clamp(int(e.p("aspect", t, 0)), 0, int(std::size(ratios)) - 1);
    const double want = ratios[choice], have = double(img.width) / std::max(1, img.height);
    const float op = float(std::clamp(e.p("opacity", t, 100) / 100.0, 0.0, 1.0));
    auto darken = [&](int x0, int y0, int x1, int y1) {
        for (int y = std::max(0, y0); y < std::min(img.height, y1); ++y)
            for (int x = std::max(0, x0); x < std::min(img.width, x1); ++x) {
                float* p = img.at(x, y);
                for (int c = 0; c < 3; ++c) p[c] *= 1 - op;
                p[3] = p[3] * (1 - op) + op;
            }
    };
    if (want > have) {  // wider: bars top and bottom
        const int bar = int(std::lround((img.height - img.width / want) / 2));
        darken(0, 0, img.width, bar);
        darken(0, img.height - bar, img.width, img.height);
    } else if (want < have) {  // narrower: bars at the sides
        const int bar = int(std::lround((img.width - img.height * want) / 2));
        darken(0, 0, bar, img.height);
        darken(img.width - bar, 0, img.width, img.height);
    }
}

void posterize(const Effect& e, FrameTime t, Image& img) {
    const float n = float(std::clamp(e.p("levels", t, 6), 2.0, 64.0)) - 1;
    straight(img, [&](float v) { return std::round(std::clamp(v, 0.f, 1.f) * n) / n; });
}

}  // namespace montage::vfx
