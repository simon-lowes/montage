#include "Layer3D.h"

#include <algorithm>
#include <cmath>

#include "render/ModelClip.h"

namespace montage {

namespace {

using V3 = std::array<double, 3>;

constexpr double kDeg = 3.14159265358979323846 / 180;

double dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Turned by Y, then X, then Z (degrees), as models are (a column-major 3 x 3).
std::array<double, 9> turnedYXZ(double rx, double ry, double rz) {
    auto mul = [](const std::array<double, 9>& a, const std::array<double, 9>& b) {
        std::array<double, 9> r{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) r[size_t(j * 3 + i)] += a[size_t(k * 3 + i)] * b[size_t(j * 3 + k)];
        return r;
    };
    const double x = rx * kDeg, y = ry * kDeg, z = rz * kDeg;
    const std::array<double, 9> X{1, 0, 0, 0, std::cos(x), std::sin(x), 0, -std::sin(x), std::cos(x)};
    const std::array<double, 9> Y{std::cos(y), 0, -std::sin(y), 0, 1, 0, std::sin(y), 0, std::cos(y)};
    const std::array<double, 9> Z{std::cos(z), std::sin(z), 0, -std::sin(z), std::cos(z), 0, 0, 0, 1};
    return mul(mul(Y, X), Z);
}

V3 apply(const std::array<double, 9>& m, const V3& v) {
    return {m[0] * v[0] + m[3] * v[1] + m[6] * v[2], m[1] * v[0] + m[4] * v[1] + m[7] * v[2], m[2] * v[0] + m[5] * v[1] + m[8] * v[2]};
}

// A coordinate within a millionth of a pixel of a whole one taken as it (so a card left where it is samples its
// picture exactly).
double snapped(double v) {
    const double r = std::round(v);
    return std::fabs(v - r) < 1e-6 ? r : v;
}

}  // namespace

std::array<double, 3> Layer3DPlace::point(double x, double y) const {
    const double u = (x - seqWidth / 2) * unit, v = (seqHeight / 2 - y) * unit;
    return {origin[0] + axisU[0] * u + axisV[0] * v, origin[1] + axisU[1] * u + axisV[1] * v, origin[2] + axisU[2] * u + axisV[2] * v};
}

Layer3DPlace layer3dPlace(const Effect& e, FrameTime lt, double seqWidth, double seqHeight) {
    auto P = [&](const char* name, double def) { return e.p(name, lt, def); };
    Layer3DPlace pl;
    pl.seqWidth = seqWidth, pl.seqHeight = seqHeight;
    const long mode = std::lround(P("camera", 3));
    pl.unit = mode == 1 || mode == 2 ? std::max(1e-9, P("width", 1)) / std::max(1.0, seqWidth) : 1.0;
    const std::array<double, 9> m = turnedYXZ(P("rot_x", 0), P("rot_y", 0), P("rot_z", 0));
    const V3 pos{P("pos_x", 0), P("pos_y", 0), P("pos_z", 0)};
    const V3 pivot{P("pivot_x", 0) * pl.unit, P("pivot_y", 0) * pl.unit, 0};
    const V3 turnedPivot = apply(m, pivot);
    for (int k = 0; k < 3; ++k) pl.origin[size_t(k)] = pos[size_t(k)] + pivot[size_t(k)] - turnedPivot[size_t(k)];
    pl.axisU = apply(m, {1, 0, 0}), pl.axisV = apply(m, {0, 1, 0}), pl.normal = apply(m, {0, 0, 1});
    return pl;
}

Image renderLayer3D(const Image& layer, const Layer3DPlace& pl, const ModelCamera& cam, double scale, std::vector<float>* depth) {
    const int W = layer.width, H = layer.height;
    Image out(W, H);
    if (depth) depth->assign(size_t(W) * size_t(H), 0.0f);
    if (layer.empty() || !(scale > 0)) return out;
    const V3 ahead{cam.R[6], cam.R[7], cam.R[8]};
    const double reach = dot(V3{pl.origin[0] - cam.C[0], pl.origin[1] - cam.C[1], pl.origin[2] - cam.C[2]}, pl.normal);
    auto texel = [&](int x, int y, int k) { return x < 0 || y < 0 || x >= W || y >= H ? 0.0 : double(layer.at(x, y)[k]); };
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                // Where the pixel's ray meets the card's plane, in front of the camera.
                const V3 d = cam.ray(x + 0.5, y + 0.5);
                const double towards = dot(d, pl.normal);
                if (std::fabs(towards) < 1e-12) continue;
                const double s = reach / towards;
                if (!(s > 0) || !std::isfinite(s)) continue;
                const V3 P{cam.C[0] + d[0] * s, cam.C[1] + d[1] * s, cam.C[2] + d[2] * s};
                const double z = dot(V3{P[0] - cam.C[0], P[1] - cam.C[1], P[2] - cam.C[2]}, ahead);
                if (!(z > cam.nearZ)) continue;
                const V3 q{P[0] - pl.origin[0], P[1] - pl.origin[1], P[2] - pl.origin[2]};
                const double X = dot(q, pl.axisU) / pl.unit + pl.seqWidth / 2, Y = pl.seqHeight / 2 - dot(q, pl.axisV) / pl.unit;
                const double sx = snapped(X * scale - 0.5), sy = snapped(Y * scale - 0.5);
                if (!(sx > -1 && sy > -1 && sx < W && sy < H)) continue;
                const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                const double fx = sx - ix, fy = sy - iy;
                float* o = out.at(x, y);
                for (int k = 0; k < 4; ++k) {
                    const double a = texel(ix, iy, k) + (texel(ix + 1, iy, k) - texel(ix, iy, k)) * fx;
                    const double b = texel(ix, iy + 1, k) + (texel(ix + 1, iy + 1, k) - texel(ix, iy + 1, k)) * fx;
                    o[k] = float(fx == 0 && fy == 0 ? texel(ix, iy, k) : a + (b - a) * fy);
                }
                if (depth && o[3] > 0) (*depth)[size_t(y) * size_t(W) + size_t(x)] = float(z);
            }
    });
    return out;
}

bool renderClipLayer3D(const Project& p, const Sequence& seq, const Clip& c, const Effect& e, FrameTime t, const Image& layer, double scale,
                       Image& out, std::vector<float>* depth, std::string* error) {
    ModelCamera cam;
    if (!effectCamera(p, seq, c, e, t, scale, cam, error)) return false;
    out = renderLayer3D(layer, layer3dPlace(e, t - c.start, seq.width, seq.height), cam, scale, depth);
    return true;
}

}  // namespace montage
