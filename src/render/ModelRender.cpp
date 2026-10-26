#include "ModelRender.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>

namespace montage {

namespace {

constexpr int kSamples = 2;          // each way, a pixel
constexpr int kBand = 16;            // output rows drawn together
constexpr double kPi = 3.14159265358979323846;

using V3 = std::array<double, 3>;
V3 operator+(const V3& a, const V3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
V3 operator-(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
V3 operator*(const V3& a, double k) { return {a[0] * k, a[1] * k, a[2] * k}; }
double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 cross(const V3& a, const V3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
V3 unit(const V3& a) {
    const double l = std::sqrt(dot(a, a));
    return l > 1e-300 ? a * (1 / l) : V3{0, 0, 1};
}
double luma(const float* c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }

// Runs fn(band) for bands [0, n): on the worker pool, or here alone.
void eachBand(int n, bool serial, const std::function<void(int)>& fn) {
    if (serial) {
        for (int b = 0; b < n; ++b) fn(b);
        return;
    }
    parallelRows(n, [&](int b0, int b1) {
        for (int b = b0; b < b1; ++b) fn(b);
    });
}

// Pixel rows and columns from coordinates that may lie far off the picture (kept within int's range).
int pixelFloor(double v) { return int(std::floor(std::clamp(v, -1e8, 1e8))); }
int pixelCeil(double v) { return int(std::ceil(std::clamp(v, -1e8, 1e8))); }

// ---- Textures ------------------------------------------------------------------------------------------------------

double wrapCoord(double t, ModelTexture::Wrap w) {
    if (w == ModelTexture::Wrap::Clamp) return std::clamp(t, 0.0, 1.0);
    if (w == ModelTexture::Wrap::Mirror) {
        const double f = t - 2 * std::floor(t / 2);
        return f > 1 ? 2 - f : f;
    }
    return t - std::floor(t);
}

void bilinear(const ModelTexture& tex, int level, double u, double v, float* out) {
    const ModelTexture::Level& l = tex.levels[size_t(level)];
    u = wrapCoord(u, tex.wrapS) * l.width - 0.5;
    v = wrapCoord(v, tex.wrapT) * l.height - 0.5;
    const double fu = std::floor(u), fv = std::floor(v);
    const double au = u - fu, av = v - fv;
    auto texel = [&](int x, int y) {
        if (tex.wrapS == ModelTexture::Wrap::Repeat) x = ((x % l.width) + l.width) % l.width;
        else x = std::clamp(x, 0, l.width - 1);
        if (tex.wrapT == ModelTexture::Wrap::Repeat) y = ((y % l.height) + l.height) % l.height;
        else y = std::clamp(y, 0, l.height - 1);
        return l.rgba.data() + (size_t(y) * size_t(l.width) + size_t(x)) * 4;
    };
    const int x0 = int(fu), y0 = int(fv);
    const uint8_t *a = texel(x0, y0), *b = texel(x0 + 1, y0), *c = texel(x0, y0 + 1), *d = texel(x0 + 1, y0 + 1);
    for (int k = 0; k < 4; ++k)
        out[k] = float((tex.linear(a[k], k) * (1 - au) + tex.linear(b[k], k) * au) * (1 - av) +
                       (tex.linear(c[k], k) * (1 - au) + tex.linear(d[k], k) * au) * av);
}

// The texture over a pixel's footprint (its uv derivatives across one pixel each way): mip level from the footprint's
// shorter axis, up to 8 trilinear taps along its longer one.
void sampleTexture(const ModelTexture& tex, double u, double v, double dux, double dvx, double duy, double dvy, float* out) {
    if (tex.levels.empty()) {
        out[0] = out[1] = out[2] = out[3] = 1;
        return;
    }
    const double W = tex.levels[0].width, H = tex.levels[0].height;
    double lx = std::hypot(dux * W, dvx * H), ly = std::hypot(duy * W, dvy * H);
    if (!std::isfinite(lx) || !std::isfinite(ly)) lx = ly = 1e9, dux = dvx = duy = dvy = 0;  // (towards the horizon: the coarsest)
    const double major = std::max(lx, ly), minor = std::min(lx, ly);
    const int taps = std::clamp(int(std::ceil(major / std::max(minor, 1e-9))), 1, 8);
    const double lod = std::clamp(std::log2(std::max({minor, major / 8, 1e-9})), 0.0, double(tex.levels.size() - 1));
    const int l0 = int(std::floor(lod));
    const int l1 = std::min(int(tex.levels.size()) - 1, l0 + 1);
    const float f = float(lod - l0);
    const double au = lx >= ly ? dux : duy, av = lx >= ly ? dvx : dvy;
    float sum[4] = {0, 0, 0, 0};
    for (int i = 0; i < taps; ++i) {
        const double s = taps > 1 ? (i + 0.5) / taps - 0.5 : 0;
        float a[4], b[4];
        bilinear(tex, l0, u + au * s, v + av * s, a);
        if (l1 != l0 && f > 0) bilinear(tex, l1, u + au * s, v + av * s, b);
        else std::copy(a, a + 4, b);
        for (int k = 0; k < 4; ++k) sum[k] += a[k] + (b[k] - a[k]) * f;
    }
    for (int k = 0; k < 4; ++k) out[k] = sum[k] / float(taps);
}

// ---- Environment ---------------------------------------------------------------------------------------------------

// A direction's place in a latitude-longitude picture (0..1 each way).
void directionToLatLong(const V3& d, double rotation, double& u, double& v) {
    const double c = std::cos(rotation), s = std::sin(rotation);
    const V3 r{c * d[0] - s * d[2], d[1], s * d[0] + c * d[2]};  // (turned back by the rotation)
    u = 0.5 + std::atan2(r[0], -r[2]) / (2 * kPi);
    v = std::acos(std::clamp(r[1], -1.0, 1.0)) / kPi;
}
V3 latLongToDirection(double u, double v) {
    const double phi = (u - 0.5) * 2 * kPi, theta = v * kPi;
    return {std::sin(theta) * std::sin(phi), std::cos(theta), -std::sin(theta) * std::cos(phi)};
}

}  // namespace

struct ModelEnvironment::Prepared {
    std::array<std::array<double, 3>, 9> sh{};  // radiance's spherical harmonics (bands 0 to 2)
    // Copies of the picture blurred for roughness 0, 0.2 ... 1, smaller as they blur.
    struct Map {
        int w = 0, h = 0;
        std::vector<float> rgb;
    };
    std::array<Map, 6> maps;
};

namespace {

void lookup(const ModelEnvironment::Prepared::Map& m, double u, double v, float* out) {
    double x = (u - std::floor(u)) * m.w - 0.5, y = std::clamp(v, 0.0, 1.0) * m.h - 0.5;
    const double fx = std::floor(x), fy = std::floor(y), ax = x - fx, ay = y - fy;
    auto at = [&](int px, int py) {
        px = ((px % m.w) + m.w) % m.w;
        py = std::clamp(py, 0, m.h - 1);
        return m.rgb.data() + (size_t(py) * size_t(m.w) + size_t(px)) * 3;
    };
    const float *a = at(int(fx), int(fy)), *b = at(int(fx) + 1, int(fy)), *c = at(int(fx), int(fy) + 1), *d = at(int(fx) + 1, int(fy) + 1);
    for (int k = 0; k < 3; ++k) out[k] = float((a[k] * (1 - ax) + b[k] * ax) * (1 - ay) + (c[k] * (1 - ax) + d[k] * ax) * ay);
}

ModelEnvironment::Prepared::Map shrink(const ModelEnvironment& e, int w, int h) {
    ModelEnvironment::Prepared::Map m;
    m.w = w, m.h = h;
    m.rgb.assign(size_t(w) * size_t(h) * 3, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int x0 = int(int64_t(x) * e.width / w), x1 = std::max(x0 + 1, int(int64_t(x + 1) * e.width / w));
            const int y0 = int(int64_t(y) * e.height / h), y1 = std::max(y0 + 1, int(int64_t(y + 1) * e.height / h));
            double s[3] = {0, 0, 0};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx)
                    for (int k = 0; k < 3; ++k) s[k] += e.rgb[(size_t(sy) * size_t(e.width) + size_t(sx)) * 3 + size_t(k)];
            const double n = double(x1 - x0) * (y1 - y0);
            for (int k = 0; k < 3; ++k) m.rgb[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(k)] = float(s[k] / n);
        }
    return m;
}

// (Null for a picture with too few pixels. Worked out with no lock held: two threads may both work it out the first
// time, the first kept.)
std::shared_ptr<const ModelEnvironment::Prepared> prepare(const ModelEnvironment& e, bool serial) {
    static std::mutex m;
    {
        std::lock_guard lock(m);
        if (e.prepared) return e.prepared;
    }
    if (e.width <= 0 || e.height <= 0 || e.rgb.size() < size_t(e.width) * size_t(e.height) * 3) return nullptr;
    auto p = std::make_shared<ModelEnvironment::Prepared>();
    // Harmonics from a 128 x 64 copy, each texel by its solid angle.
    const ModelEnvironment::Prepared::Map base = shrink(e, std::min(e.width, 256), std::min(e.height, 128));
    const ModelEnvironment::Prepared::Map small = shrink(e, 128, 64);
    for (int y = 0; y < small.h; ++y)
        for (int x = 0; x < small.w; ++x) {
            const double v = (y + 0.5) / small.h, u = (x + 0.5) / small.w;
            const V3 d = latLongToDirection(u, v);
            const double dw = (2 * kPi / small.w) * (kPi / small.h) * std::sin(v * kPi);
            const double b[9] = {0.282095, 0.488603 * d[1], 0.488603 * d[2], 0.488603 * d[0], 1.092548 * d[0] * d[1],
                                 1.092548 * d[1] * d[2], 0.315392 * (3 * d[2] * d[2] - 1), 1.092548 * d[0] * d[2], 0.546274 * (d[0] * d[0] - d[1] * d[1])};
            const float* c = small.rgb.data() + (size_t(y) * size_t(small.w) + size_t(x)) * 3;
            for (int i = 0; i < 9; ++i)
                for (int k = 0; k < 3; ++k) p->sh[size_t(i)][size_t(k)] += c[k] * b[i] * dw;
        }
    // Reflections: each rougher copy averages the 128 x 64 copy over a lobe like GGX's (a Phong lobe of the same
    // width), for its own directions at 64 x 32.
    p->maps[0] = base;
    std::vector<V3> dirs(size_t(small.w) * size_t(small.h));
    std::vector<double> solid(dirs.size());
    for (int y = 0; y < small.h; ++y)
        for (int x = 0; x < small.w; ++x) {
            dirs[size_t(y) * size_t(small.w) + size_t(x)] = latLongToDirection((x + 0.5) / small.w, (y + 0.5) / small.h);
            solid[size_t(y) * size_t(small.w) + size_t(x)] = std::sin((y + 0.5) / small.h * kPi);
        }
    for (int level = 1; level < 6; ++level) {
        const double r = level / 5.0, a = std::max(1e-3, r * r);
        const double power = std::max(0.0, 2 / (a * a) - 2);
        ModelEnvironment::Prepared::Map& out = p->maps[size_t(level)];
        out.w = 64, out.h = 32;
        out.rgb.assign(size_t(out.w) * size_t(out.h) * 3, 0);
        auto rows = [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < out.w; ++x) {
                    const V3 n = latLongToDirection((x + 0.5) / out.w, (y + 0.5) / out.h);
                    double s[3] = {0, 0, 0}, wsum = 0;
                    for (size_t i = 0; i < dirs.size(); ++i) {
                        const double c = dot(n, dirs[i]);
                        if (c <= 0) continue;
                        const double w = std::pow(c, power) * solid[i];
                        if (w < 1e-12) continue;
                        wsum += w;
                        for (int k = 0; k < 3; ++k) s[k] += w * small.rgb[i * 3 + size_t(k)];
                    }
                    for (int k = 0; k < 3; ++k) out.rgb[(size_t(y) * size_t(out.w) + size_t(x)) * 3 + size_t(k)] = float(wsum > 0 ? s[k] / wsum : 0);
                }
        };
        if (serial) rows(0, out.h);
        else parallelRows(out.h, rows);
    }
    std::lock_guard lock(m);
    if (!e.prepared) e.prepared = p;
    return e.prepared;
}

// Irradiance from the harmonics (Ramamoorthi and Hanrahan's cosine convolution), divided by pi: the radiance a white
// diffuse face facing `n` gives back.
void diffuseLight(const ModelEnvironment::Prepared& p, const V3& d, double rotation, float* out) {
    const double c = std::cos(rotation), sn = std::sin(rotation);
    const V3 n{c * d[0] - sn * d[2], d[1], sn * d[0] + c * d[2]};  // (turned back by the rotation)
    const double A[9] = {kPi, 2 * kPi / 3, 2 * kPi / 3, 2 * kPi / 3, kPi / 4, kPi / 4, kPi / 4, kPi / 4, kPi / 4};
    const double b[9] = {0.282095, 0.488603 * n[1], 0.488603 * n[2], 0.488603 * n[0], 1.092548 * n[0] * n[1],
                         1.092548 * n[1] * n[2], 0.315392 * (3 * n[2] * n[2] - 1), 1.092548 * n[0] * n[2], 0.546274 * (n[0] * n[0] - n[1] * n[1])};
    for (int k = 0; k < 3; ++k) {
        double e = 0;
        for (int i = 0; i < 9; ++i) e += A[i] * p.sh[size_t(i)][size_t(k)] * b[i];
        out[k] = float(std::max(0.0, e) / kPi);
    }
}

void reflectedLight(const ModelEnvironment::Prepared& p, const V3& r, double roughness, double rotation, float* out) {
    double u, v;
    directionToLatLong(r, rotation, u, v);
    const double l = std::clamp(roughness, 0.0, 1.0) * 5;
    const int l0 = std::min(4, int(std::floor(l)));
    const double f = l - l0;
    float a[3], b[3];
    lookup(p.maps[size_t(l0)], u, v, a);
    lookup(p.maps[size_t(l0 + 1)], u, v, b);
    for (int k = 0; k < 3; ++k) out[k] = float(a[k] + (b[k] - a[k]) * f);
}

// The split-sum's scale and bias on F0 for reflected light (Karis's fit to the GGX integral).
void environmentBrdf(double roughness, double NdotV, double& A, double& B) {
    const double r0 = roughness * -1 + 1, r1 = roughness * -0.0275 + 0.0425, r2 = roughness * -0.572 + 1.04, r3 = roughness * 0.022 - 0.04;
    const double a004 = std::min(r0 * r0, std::exp2(-9.28 * NdotV)) * r0 + r1;
    A = a004 * -1.04 + r2;
    B = a004 * 1.04 + r3;
}

// ---- Shadow maps ---------------------------------------------------------------------------------------------------

struct ShadowMap {
    bool perspective = false;
    V3 origin, right, up, forward;
    double extent = 1;  // orthographic: half the width covered; perspective: tan of half the angle
    int size = 0;
    std::vector<float> depth;  // nearest along `forward`; +inf where nothing
    double texelWorld = 0;     // orthographic: a texel's width
    bool map(const V3& P, double& u, double& v, double& d) const {
        const V3 q = P - origin;
        d = dot(q, forward);
        double x = dot(q, right), y = dot(q, up);
        if (perspective) {
            if (d <= 1e-6) return false;
            x /= d * extent, y /= d * extent;
        } else {
            x /= extent, y /= extent;
        }
        u = (x * 0.5 + 0.5) * size, v = (0.5 - y * 0.5) * size;
        return true;
    }
};

V3 basisUp(const V3& f) { return std::fabs(f[1]) < 0.99 ? V3{0, 1, 0} : V3{1, 0, 0}; }

ShadowMap buildShadow(const ModelScene& s, const ModelLight& light, const V3& centre, double radius, int size, bool serial) {
    ShadowMap m;
    m.size = size;
    m.forward = unit(light.direction);
    m.right = unit(cross(m.forward, basisUp(m.forward)));
    m.up = cross(m.right, m.forward);
    if (light.kind == ModelLight::Kind::Spot) {
        m.perspective = true;
        m.origin = light.position;
        m.extent = std::tan(std::clamp(light.outerCone, 0.01, 1.5));
    } else {
        m.origin = centre - m.forward * (radius * 2);
        m.extent = radius * 1.01;
        m.texelWorld = 2 * m.extent / size;
    }
    m.depth.assign(size_t(size) * size_t(size), std::numeric_limits<float>::infinity());
    // The triangles in the map's pixels, both sides, the nearest kept.
    const PosedTriangles& t = s.triangles;
    struct P {
        double u, v, d;
        bool ok;
    };
    std::vector<P> pts(t.positions.size() / 3);
    for (size_t i = 0; i < pts.size(); ++i) {
        P& p = pts[i];
        p.ok = m.map({t.positions[i * 3], t.positions[i * 3 + 1], t.positions[i * 3 + 2]}, p.u, p.v, p.d);
    }
    const int bands = (size + 63) / 64;
    eachBand(bands, serial, [&](int b) {
        const int y0 = b * 64, y1 = std::min(size, y0 + 64);
        for (size_t i = 0; i + 2 < t.indices.size(); i += 3) {
            const P &a = pts[t.indices[i]], &bb = pts[t.indices[i + 1]], &c = pts[t.indices[i + 2]];
            if (!a.ok || !bb.ok || !c.ok) continue;
            const double area = (bb.u - a.u) * (c.v - a.v) - (bb.v - a.v) * (c.u - a.u);
            if (std::fabs(area) < 1e-12) continue;
            const int ty0 = std::max(y0, pixelFloor(std::min({a.v, bb.v, c.v}))), ty1 = std::min(y1 - 1, pixelCeil(std::max({a.v, bb.v, c.v})));
            const int tx0 = std::max(0, pixelFloor(std::min({a.u, bb.u, c.u}))), tx1 = std::min(size - 1, pixelCeil(std::max({a.u, bb.u, c.u})));
            for (int y = ty0; y <= ty1; ++y)
                for (int x = tx0; x <= tx1; ++x) {
                    const double px = x + 0.5, py = y + 0.5;
                    const double w0 = ((bb.u - px) * (c.v - py) - (bb.v - py) * (c.u - px)) / area;
                    const double w1 = ((c.u - px) * (a.v - py) - (c.v - py) * (a.u - px)) / area;
                    const double w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    // (depth along the light: straight across an orthographic map, its reciprocal across a perspective one)
                    const float d = float(m.perspective ? 1 / (w0 / a.d + w1 / bb.d + w2 / c.d) : w0 * a.d + w1 * bb.d + w2 * c.d);
                    float& z = m.depth[size_t(y) * size_t(size) + size_t(x)];
                    if (d < z) z = d;
                }
        }
    });
    return m;
}

// How much of the light reaches P (0..1), over 3 x 3 texels, with a bias against the surface's own shadow.
double lightVisible(const ShadowMap& m, const V3& P, double NdotL) {
    double u, v, d;
    if (!m.map(P, u, v, d)) return 1;
    const double texel = m.perspective ? 2 * m.extent * d / m.size : m.texelWorld;
    const double slope = std::sqrt(std::max(0.0, 1 - NdotL * NdotL)) / std::max(0.1, NdotL);
    const double bias = texel * (1.5 + 1.5 * std::min(slope, 10.0));
    double lit = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int x = int(std::floor(u)) + dx, y = int(std::floor(v)) + dy;
            if (x < 0 || y < 0 || x >= m.size || y >= m.size) {
                lit += 1;
                continue;
            }
            lit += m.depth[size_t(y) * size_t(m.size) + size_t(x)] + bias >= d ? 1 : 0;
        }
    return lit / 9;
}

// ---- Lights --------------------------------------------------------------------------------------------------------

// The light's direction towards it from P and what it brings (rgb); false when none reaches.
bool incoming(const ModelLight& l, const V3& P, V3& L, double* E) {
    double k = l.intensity;
    if (l.kind == ModelLight::Kind::Directional) {
        L = unit(l.direction * -1);
    } else {
        const V3 d = l.position - P;
        const double d2 = std::max(1e-12, dot(d, d));
        L = d * (1 / std::sqrt(d2));
        k /= d2;
        if (l.kind == ModelLight::Kind::Spot) {
            const double cd = dot(unit(l.direction), L * -1);
            const double co = std::cos(l.outerCone), ci = std::cos(std::min(l.innerCone, l.outerCone));
            const double t = std::clamp((cd - co) / std::max(1e-4, ci - co), 0.0, 1.0);
            k *= t * t;
        }
    }
    for (int c = 0; c < 3; ++c) E[c] = l.color[size_t(c)] * k;
    return k > 0;
}

}  // namespace

bool ModelCamera::project(const std::array<double, 3>& X, double& px, double& py, double* z) const {
    const V3 q = X - C;
    const double x = R[0] * q[0] + R[1] * q[1] + R[2] * q[2], y = R[3] * q[0] + R[4] * q[1] + R[5] * q[2],
                 d = R[6] * q[0] + R[7] * q[1] + R[8] * q[2];
    if (z) *z = d;
    if (!(d > nearZ)) return false;
    const double a = x / d, b = y / d;
    px = screen[0] * a + screen[1] * b + screen[2];
    py = screen[3] * a + screen[4] * b + screen[5];
    return true;
}

std::array<double, 3> ModelCamera::ray(double px, double py) const {
    const double det = screen[0] * screen[4] - screen[1] * screen[3];
    const double ix = px - screen[2], iy = py - screen[5];
    const double a = (screen[4] * ix - screen[1] * iy) / det, b = (-screen[3] * ix + screen[0] * iy) / det;
    // R's transpose takes the camera's (a, b, 1) into the world.
    return {R[0] * a + R[3] * b + R[6], R[1] * a + R[4] * b + R[7], R[2] * a + R[5] * b + R[8]};
}

namespace {

// A triangle on screen: corners in pixels with 1 / depth, and for each corner its weights on the model triangle's own
// corners (cut by the near plane, a corner can lie along an edge).
struct ScreenTri {
    uint32_t tri = 0;  // the model triangle
    double x[3], y[3], iz[3];
    double b[3][3];    // corner k's weights on the triangle's corners
    double area = 0;
    int y0 = 0, y1 = 0;  // sample rows
};

// The model triangle's weights at a picture point (perspective-correct), from one of its screen triangles.
void weightsAt(const ScreenTri& t, double px, double py, double* w, double* iz = nullptr) {
    const double l0 = ((t.x[1] - px) * (t.y[2] - py) - (t.y[1] - py) * (t.x[2] - px)) / t.area;
    const double l1 = ((t.x[2] - px) * (t.y[0] - py) - (t.y[2] - py) * (t.x[0] - px)) / t.area;
    const double l2 = 1 - l0 - l1;
    const double z = l0 * t.iz[0] + l1 * t.iz[1] + l2 * t.iz[2];
    if (iz) *iz = z;
    const double k0 = l0 * t.iz[0] / z, k1 = l1 * t.iz[1] / z, k2 = l2 * t.iz[2] / z;
    for (int c = 0; c < 3; ++c) w[c] = k0 * t.b[0][c] + k1 * t.b[1][c] + k2 * t.b[2][c];
}

const ModelMaterial& defaultMaterial() {
    static const ModelMaterial m = [] {
        ModelMaterial d;
        d.metallic = 0, d.roughness = 0.5f;
        return d;
    }();
    return m;
}

}  // namespace

Image renderModel(const ModelScene& s, const ModelCamera& cam, int w, int h, std::vector<float>* depthOut, const ModelRenderOptions& o) {
    w = std::max(0, w), h = std::max(0, h);
    Image out(w, h);
    if (depthOut) depthOut->assign(size_t(w) * size_t(h), 0.0f);
    if (w == 0 || h == 0) return out;
    // Every vertex with a normal, uv, tangent and colour (posedTriangles gives them all; a hand-made set may not).
    const size_t nv = s.triangles.positions.size() / 3;
    PosedTriangles filled;
    const bool whole = s.triangles.normals.size() == nv * 3 && s.triangles.uvs.size() == nv * 2 &&
                       s.triangles.tangents.size() == nv * 4 && s.triangles.colors.size() == nv * 4;
    if (!whole) {
        filled = s.triangles;
        filled.normals.resize(nv * 3, 0.0f), filled.uvs.resize(nv * 2, 0.0f), filled.tangents.resize(nv * 4, 0.0f), filled.colors.resize(nv * 4, 1.0f);
    }
    const PosedTriangles& T = whole ? s.triangles : filled;
    const size_t nt = T.indices.size() / 3;
    for (const uint32_t i : T.indices)
        if (i >= nv) return out;  // (indices past the vertices: nothing drawn)
    auto material = [&](uint32_t tri) -> const ModelMaterial& {
        const int m = tri < T.materials.size() ? T.materials[tri] : -1;
        return s.model && m >= 0 && size_t(m) < s.model->materials.size() ? s.model->materials[size_t(m)] : defaultMaterial();
    };
    auto texture = [&](int id) -> const ModelTexture* {
        return s.model && id >= 0 && size_t(id) < s.model->textures.size() ? &s.model->textures[size_t(id)] : nullptr;
    };
    auto position = [&](uint32_t v) { return V3{T.positions[v * 3], T.positions[v * 3 + 1], T.positions[v * 3 + 2]}; };

    // The camera's view of every corner, then each triangle cut by the near plane and put on screen.
    std::vector<V3> cs(nv);
    for (size_t i = 0; i < nv; ++i) {
        const V3 q = position(uint32_t(i)) - cam.C;
        cs[i] = {cam.R[0] * q[0] + cam.R[1] * q[1] + cam.R[2] * q[2], cam.R[3] * q[0] + cam.R[4] * q[1] + cam.R[5] * q[2],
                 cam.R[6] * q[0] + cam.R[7] * q[1] + cam.R[8] * q[2]};
    }
    std::vector<ScreenTri> tris;
    const double sw = w * kSamples, sh = h * kSamples;
    for (size_t t = 0; t < nt; ++t) {
        const uint32_t ix[3] = {T.indices[t * 3], T.indices[t * 3 + 1], T.indices[t * 3 + 2]};
        const V3 p[3] = {cs[ix[0]], cs[ix[1]], cs[ix[2]]};
        const ModelMaterial& mat = material(uint32_t(t));
        // Facing away (counter-clockwise faces front, as glTF has it): hidden unless two-sided.
        if (!mat.doubleSided && dot(cross(p[1] - p[0], p[2] - p[0]), p[0]) >= 0) continue;
        // Cut by the near plane (Sutherland-Hodgman, one plane), corners carrying their weights.
        struct Cv {
            V3 p;
            double b[3];
        };
        std::vector<Cv> poly, kept;
        for (int k = 0; k < 3; ++k) {
            Cv c{p[k], {0, 0, 0}};
            c.b[k] = 1;
            poly.push_back(c);
        }
        for (size_t i = 0; i < poly.size(); ++i) {
            const Cv &a = poly[i], &b = poly[(i + 1) % poly.size()];
            const double da = a.p[2] - cam.nearZ, db = b.p[2] - cam.nearZ;
            if (da >= 0) kept.push_back(a);
            if ((da >= 0) != (db >= 0)) {
                const double f = da / (da - db);
                Cv m;
                m.p = a.p + (b.p - a.p) * f;
                for (int k = 0; k < 3; ++k) m.b[k] = a.b[k] + (b.b[k] - a.b[k]) * f;
                kept.push_back(m);
            }
        }
        for (size_t k = 1; k + 1 < kept.size(); ++k) {
            ScreenTri st;
            st.tri = uint32_t(t);
            const Cv* c[3] = {&kept[0], &kept[k], &kept[k + 1]};
            double ymin = 1e300, ymax = -1e300, xmin = 1e300, xmax = -1e300;
            for (int v = 0; v < 3; ++v) {
                const double a = c[v]->p[0] / c[v]->p[2], b = c[v]->p[1] / c[v]->p[2];
                st.x[v] = cam.screen[0] * a + cam.screen[1] * b + cam.screen[2];
                st.y[v] = cam.screen[3] * a + cam.screen[4] * b + cam.screen[5];
                st.iz[v] = 1 / c[v]->p[2];
                std::copy(c[v]->b, c[v]->b + 3, st.b[v]);
                ymin = std::min(ymin, st.y[v]), ymax = std::max(ymax, st.y[v]);
                xmin = std::min(xmin, st.x[v]), xmax = std::max(xmax, st.x[v]);
            }
            st.area = (st.x[1] - st.x[0]) * (st.y[2] - st.y[0]) - (st.y[1] - st.y[0]) * (st.x[2] - st.x[0]);
            if (std::fabs(st.area) < 1e-12 || xmax < 0 || xmin > w || ymax < 0 || ymin > h) continue;
            st.y0 = std::max(0, pixelFloor(ymin * kSamples - 0.5));
            st.y1 = std::min(int(sh) - 1, pixelCeil(ymax * kSamples - 0.5));
            if (st.y1 < st.y0) continue;
            tris.push_back(st);
        }
    }
    (void)sw;

    // Shadow maps for the lights that cast them, over the model's bounds.
    V3 centre{0, 0, 0};
    double radius = 1;
    {
        std::array<double, 3> lo, hi;
        if (modelBounds(T, lo, hi)) {
            centre = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
            radius = std::max(1e-6, std::sqrt(dot(V3{hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}, V3{hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]})) / 2);
        }
    }
    std::vector<std::unique_ptr<ShadowMap>> shadows(s.lights.size());
    for (size_t i = 0; i < s.lights.size(); ++i)
        if (s.lights[i].shadows && s.lights[i].kind != ModelLight::Kind::Point && nt > 0)
            shadows[i] = std::make_unique<ShadowMap>(buildShadow(s, s.lights[i], centre, radius, std::max(64, o.shadowSize), o.serial));
    const std::shared_ptr<const ModelEnvironment::Prepared> env =
        s.environment ? prepare(*s.environment, o.serial) : nullptr;
    const V3 eye = cam.C;

    // The triangles reaching each band of rows, in order.
    const int bands = (h + kBand - 1) / kBand;
    std::vector<std::vector<uint32_t>> inBand(static_cast<size_t>(bands));
    for (uint32_t i = 0; i < tris.size(); ++i) {
        const int b0 = tris[i].y0 / (kBand * kSamples), b1 = std::min(bands - 1, tris[i].y1 / (kBand * kSamples));
        for (int b = b0; b <= b1; ++b) inBand[size_t(b)].push_back(i);
    }

    // A surface's light towards the camera at weights `wt` on model triangle `tri`, with uv derivatives for its
    // textures; rgb and alpha (straight).
    auto shade = [&](uint32_t tri, const double* wt, const double* wx, const double* wy, float* rgba) {
        const uint32_t ix[3] = {T.indices[size_t(tri) * 3], T.indices[size_t(tri) * 3 + 1], T.indices[size_t(tri) * 3 + 2]};
        auto lerp = [&](const std::vector<float>& a, size_t n, size_t k, const double* ww) {
            return ww[0] * a[ix[0] * n + k] + ww[1] * a[ix[1] * n + k] + ww[2] * a[ix[2] * n + k];
        };
        const ModelMaterial& m = material(tri);
        const V3 P{lerp(T.positions, 3, 0, wt), lerp(T.positions, 3, 1, wt), lerp(T.positions, 3, 2, wt)};
        const double u = lerp(T.uvs, 2, 0, wt), v = lerp(T.uvs, 2, 1, wt);
        const double dux = lerp(T.uvs, 2, 0, wx) - u, dvx = lerp(T.uvs, 2, 1, wx) - v, duy = lerp(T.uvs, 2, 0, wy) - u, dvy = lerp(T.uvs, 2, 1, wy) - v;
        auto tex = [&](int id, float* c) {
            const ModelTexture* t = texture(id);
            if (!t) return false;
            sampleTexture(*t, u, v, dux, dvx, duy, dvy, c);
            return true;
        };
        float base[4] = {m.baseColor[0], m.baseColor[1], m.baseColor[2], m.baseColor[3]}, c[4];
        if (tex(m.baseTexture, c))
            for (int k = 0; k < 4; ++k) base[k] *= c[k];
        for (int k = 0; k < 4; ++k) base[k] *= float(lerp(T.colors, 4, size_t(k), wt));
        double metallic = m.metallic, rough = m.roughness;
        if (tex(m.metallicRoughnessTexture, c)) rough *= c[1], metallic *= c[2];
        rough = std::clamp(rough, 0.03, 1.0), metallic = std::clamp(metallic, 0.0, 1.0);
        const V3 V = unit(eye - P);
        const V3 pa = position(ix[0]), pb = position(ix[1]), pc = position(ix[2]);
        const V3 geo = unit(cross(pb - pa, pc - pa));
        const bool back = dot(geo, V) < 0;
        V3 N = unit(V3{lerp(T.normals, 3, 0, wt), lerp(T.normals, 3, 1, wt), lerp(T.normals, 3, 2, wt)});
        const bool turned = back && m.doubleSided;  // (the back of a two-sided face: its whole frame turned over)
        if (turned) N = N * -1;
        if (tex(m.normalTexture, c) && !T.tangents.empty()) {
            V3 Tg = V3{lerp(T.tangents, 4, 0, wt), lerp(T.tangents, 4, 1, wt), lerp(T.tangents, 4, 2, wt)} * (turned ? -1 : 1);
            const double sign = (T.tangents[ix[0] * 4 + 3] < 0 ? -1 : 1) * (turned ? -1 : 1);
            if (dot(Tg, Tg) > 1e-12) {
                Tg = unit(Tg - N * dot(N, Tg));
                const V3 B = cross(N, Tg) * sign;
                const double nx = (c[0] * 2 - 1) * m.normalScale, ny = (c[1] * 2 - 1) * m.normalScale, nz = c[2] * 2 - 1;
                N = unit(Tg * nx + B * ny + N * nz);
            }
        }
        const double NdotV = std::max(1e-4, dot(N, V));
        const double cd[3] = {base[0] * (1 - metallic), base[1] * (1 - metallic), base[2] * (1 - metallic)};
        const double f0[3] = {0.04 + (base[0] - 0.04) * metallic, 0.04 + (base[1] - 0.04) * metallic, 0.04 + (base[2] - 0.04) * metallic};
        const double a = rough * rough, a2 = a * a;
        double col[3] = {0, 0, 0};
        for (size_t li = 0; li < s.lights.size(); ++li) {
            V3 L;
            double E[3];
            if (!incoming(s.lights[li], P, L, E)) continue;
            const double NdotL = dot(N, L);
            if (NdotL <= 0) continue;
            double vis = 1;
            if (shadows[li]) vis = lightVisible(*shadows[li], P, std::max(0.0, dot(geo * (back ? -1.0 : 1.0), L)));
            if (vis <= 0) continue;
            const V3 H = unit(L + V);
            const double NdotH = std::max(0.0, dot(N, H)), VdotH = std::max(0.0, dot(V, H));
            const double dd = NdotH * NdotH * (a2 - 1) + 1;
            const double D = a2 / (kPi * dd * dd);
            const double Vis = 0.5 / (NdotL * std::sqrt(NdotV * NdotV * (1 - a2) + a2) + NdotV * std::sqrt(NdotL * NdotL * (1 - a2) + a2));
            const double fw = std::pow(1 - VdotH, 5);
            for (int k = 0; k < 3; ++k) {
                const double F = f0[k] + (1 - f0[k]) * fw;
                col[k] += ((1 - F) * cd[k] / kPi + F * D * Vis) * E[k] * NdotL * vis;
            }
        }
        // Light from all round: the picture of the scene, else the ambient colour.
        double A, B;
        environmentBrdf(rough, NdotV, A, B);
        float diff[3], spec[3];
        if (env) {
            diffuseLight(*env, N, s.environmentRotation, diff);
            reflectedLight(*env, N * (2 * dot(N, V)) - V, rough, s.environmentRotation, spec);
            for (int k = 0; k < 3; ++k) diff[k] *= s.environmentIntensity, spec[k] *= s.environmentIntensity;
        } else {
            for (int k = 0; k < 3; ++k) diff[k] = spec[k] = s.ambient[size_t(k)];
        }
        float em[3] = {m.emissive[0], m.emissive[1], m.emissive[2]};
        if (tex(m.emissiveTexture, c))
            for (int k = 0; k < 3; ++k) em[k] *= c[k];
        for (int k = 0; k < 3; ++k) {
            const double ks = f0[k] * A + B;
            rgba[k] = float(col[k] + cd[k] * diff[k] * (1 - ks) + spec[k] * ks + em[k]);
        }
        rgba[3] = m.alpha == ModelMaterial::Alpha::Blend ? std::clamp(base[3], 0.0f, 1.0f) : 1.0f;
    };
    // What a masked material lets through at a sample.
    auto maskPasses = [&](uint32_t tri, const double* wt) {
        const ModelMaterial& m = material(tri);
        if (m.alpha != ModelMaterial::Alpha::Mask) return true;
        const uint32_t ix[3] = {T.indices[size_t(tri) * 3], T.indices[size_t(tri) * 3 + 1], T.indices[size_t(tri) * 3 + 2]};
        double alpha = m.baseColor[3] * (wt[0] * T.colors[ix[0] * 4 + 3] + wt[1] * T.colors[ix[1] * 4 + 3] + wt[2] * T.colors[ix[2] * 4 + 3]);
        if (const ModelTexture* t = texture(m.baseTexture)) {
            float c[4];
            const double u = wt[0] * T.uvs[ix[0] * 2] + wt[1] * T.uvs[ix[1] * 2] + wt[2] * T.uvs[ix[2] * 2];
            const double v = wt[0] * T.uvs[ix[0] * 2 + 1] + wt[1] * T.uvs[ix[1] * 2 + 1] + wt[2] * T.uvs[ix[2] * 2 + 1];
            bilinear(*t, 0, u, v, c);
            alpha *= c[3];
        }
        return alpha >= m.alphaCutoff;
    };
    // The shadows on the ground at P: the share of the light they take away.
    auto groundShadow = [&](const V3& P) {
        const V3 up{0, 1, 0};
        double all = 0, lit = 0;
        for (size_t li = 0; li < s.lights.size(); ++li) {
            V3 L;
            double E[3];
            if (!incoming(s.lights[li], P, L, E)) continue;
            const double NdotL = dot(up, L);
            if (NdotL <= 0) continue;
            const float Ef[3] = {float(E[0]), float(E[1]), float(E[2])};
            const double e = luma(Ef) * NdotL;
            all += e;
            lit += e * (shadows[li] ? lightVisible(*shadows[li], P, NdotL) : 1);
        }
        float amb[3];
        if (env) {
            diffuseLight(*env, up, s.environmentRotation, amb);
            for (float& k : amb) k *= s.environmentIntensity * float(kPi);
        } else {
            for (int k = 0; k < 3; ++k) amb[k] = s.ambient[size_t(k)] * float(kPi);
        }
        all += luma(amb), lit += luma(amb);
        return all > 0 ? std::clamp(1 - lit / all, 0.0, 1.0) : 0.0;
    };

    eachBand(bands, o.serial, [&](int band) {
        const int oy0 = band * kBand, oy1 = std::min(h, oy0 + kBand);
        const int sy0 = oy0 * kSamples, rows = (oy1 - oy0) * kSamples, cols = w * kSamples;
        std::vector<float> iz(size_t(rows) * size_t(cols), 0.0f);
        std::vector<int32_t> id(iz.size(), -1);
        for (const uint32_t ti : inBand[size_t(band)]) {
            const ScreenTri& t = tris[ti];
            const int y0 = std::max(sy0, t.y0), y1 = std::min(sy0 + rows - 1, t.y1);
            const double xmin = std::min({t.x[0], t.x[1], t.x[2]}), xmax = std::max({t.x[0], t.x[1], t.x[2]});
            const int x0 = std::max(0, pixelFloor(xmin * kSamples - 0.5)), x1 = std::min(cols - 1, pixelCeil(xmax * kSamples - 0.5));
            const bool masked = material(t.tri).alpha == ModelMaterial::Alpha::Mask;
            for (int y = y0; y <= y1; ++y) {
                const double py = (y + 0.5) / kSamples;
                for (int x = x0; x <= x1; ++x) {
                    const double px = (x + 0.5) / kSamples;
                    const double l0 = ((t.x[1] - px) * (t.y[2] - py) - (t.y[1] - py) * (t.x[2] - px)) / t.area;
                    const double l1 = ((t.x[2] - px) * (t.y[0] - py) - (t.y[2] - py) * (t.x[0] - px)) / t.area;
                    const double l2 = 1 - l0 - l1;
                    if (l0 < 0 || l1 < 0 || l2 < 0) continue;
                    const float z = float(l0 * t.iz[0] + l1 * t.iz[1] + l2 * t.iz[2]);
                    const size_t i = size_t(y - sy0) * size_t(cols) + size_t(x);
                    if (z <= iz[i]) continue;
                    if (masked) {
                        double wt[3];
                        weightsAt(t, px, py, wt);
                        if (!maskPasses(t.tri, wt)) continue;
                    }
                    iz[i] = z;
                    id[i] = int32_t(ti);
                }
            }
        }
        // Each pixel: every screen triangle in it shaded once at the middle of its samples, the ground's shadows
        // where none is.
        for (int y = oy0; y < oy1; ++y)
            for (int x = 0; x < w; ++x) {
                int32_t ids[kSamples * kSamples];
                double sx[kSamples * kSamples], sy[kSamples * kSamples];
                float nearest = 0;
                int n = 0;
                for (int j = 0; j < kSamples; ++j)
                    for (int i = 0; i < kSamples; ++i) {
                        const size_t k = size_t((y - oy0) * kSamples + j) * size_t(cols) + size_t(x * kSamples + i);
                        ids[n] = id[k];
                        sx[n] = x + (i + 0.5) / kSamples, sy[n] = y + (j + 0.5) / kSamples;
                        nearest = std::max(nearest, iz[k]);
                        ++n;
                    }
                float acc[4] = {0, 0, 0, 0};
                int empty = 0;
                bool done[kSamples * kSamples] = {};
                for (int a = 0; a < n; ++a) {
                    if (ids[a] < 0) {
                        ++empty;
                        continue;
                    }
                    if (done[a]) continue;
                    double cx = 0, cy = 0;
                    int count = 0;
                    for (int b = a; b < n; ++b)
                        if (ids[b] == ids[a]) cx += sx[b], cy += sy[b], ++count, done[b] = true;
                    cx /= count, cy /= count;
                    const ScreenTri& t = tris[size_t(ids[a])];
                    double wt[3], wx[3], wy[3];
                    weightsAt(t, cx, cy, wt);
                    weightsAt(t, cx + 1, cy, wx);
                    weightsAt(t, cx, cy + 1, wy);
                    float c[4];
                    shade(t.tri, wt, wx, wy, c);
                    const float share = float(count) / n * c[3];
                    for (int k = 0; k < 3; ++k) acc[k] += c[k] * share;
                    acc[3] += share;
                }
                if (empty > 0 && s.ground) {
                    const V3 d = cam.ray(x + 0.5, y + 0.5);
                    if (std::fabs(d[1]) > 1e-12) {
                        const double tt = (s.groundHeight - eye[1]) / d[1];
                        const V3 P = eye + d * tt;
                        double zc;
                        double px, py;
                        if (tt > 0 && cam.project(P, px, py, &zc)) {
                            const float shadow = float(groundShadow(P) * s.groundStrength * float(empty) / float(n));
                            acc[3] += shadow;
                            if (nearest <= 0 && shadow > 0) nearest = float(1 / zc);  // (the ground's depth where it alone is)
                        }
                    }
                }
                float* q = out.at(x, y);
                for (int k = 0; k < 4; ++k) q[k] = acc[k] * s.opacity;
                if (depthOut && nearest > 0) (*depthOut)[size_t(y) * size_t(w) + size_t(x)] = 1 / nearest;
            }
    });
    return out;
}

}  // namespace montage
