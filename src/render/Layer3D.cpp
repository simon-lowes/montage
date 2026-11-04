#include "Layer3D.h"

#include <algorithm>
#include <cmath>

#include "core/Model.h"
#include "render/ColorSpace.h"
#include "render/LensBlur.h"
#include "render/ModelClip.h"
#include "render/Processing.h"
#include "render/TransferTables.h"

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

PosedTriangles layer3dTriangles(const Layer3DPlace& pl, std::shared_ptr<const Model>& model, float rough) {
    // (where the picture is clear the card is not there at all: what is behind it, a ground's shadow, shows)
    auto plane = std::make_shared<Model>(*shapeModel(1, pl.seqWidth / std::max(1.0, pl.seqHeight), {1, 1, 1, 1}, 0, rough));
    plane->materials[0].alpha = ModelMaterial::Alpha::Mask;
    plane->materials[0].alphaCutoff = 1e-4f;
    model = plane;
    PosedTriangles tr = posedTriangles(*model, poseModel(*model, -1, 0));
    // The plane is a unit tall: the frame's height on the card.
    const double k = pl.seqHeight * pl.unit;
    auto onCard = [&](float* v, bool point) {
        const V3 q{v[0] * k, v[1] * k, v[2] * k};
        for (int a = 0; a < 3; ++a)
            v[a] = float((point ? pl.origin[size_t(a)] : 0) + pl.axisU[size_t(a)] * q[0] + pl.axisV[size_t(a)] * q[1] + pl.normal[size_t(a)] * q[2]);
    };
    for (size_t i = 0; i < tr.positions.size() / 3; ++i) {
        onCard(&tr.positions[i * 3], true);
        if (tr.normals.size() >= i * 3 + 3) {
            onCard(&tr.normals[i * 3], false);
            const double l = std::sqrt(double(tr.normals[i * 3]) * tr.normals[i * 3] + double(tr.normals[i * 3 + 1]) * tr.normals[i * 3 + 1] +
                                       double(tr.normals[i * 3 + 2]) * tr.normals[i * 3 + 2]);
            for (int a = 0; a < 3 && l > 0; ++a) tr.normals[i * 3 + size_t(a)] = float(tr.normals[i * 3 + size_t(a)] / l);
        }
        if (tr.tangents.size() >= i * 4 + 4) {
            onCard(&tr.tangents[i * 4], false);
            const double l = std::sqrt(double(tr.tangents[i * 4]) * tr.tangents[i * 4] + double(tr.tangents[i * 4 + 1]) * tr.tangents[i * 4 + 1] +
                                       double(tr.tangents[i * 4 + 2]) * tr.tangents[i * 4 + 2]);
            for (int a = 0; a < 3 && l > 0; ++a) tr.tangents[i * 4 + size_t(a)] = float(tr.tangents[i * 4 + size_t(a)] / l);
        }
    }
    return tr;
}

Image renderLitLayer3D(const Sequence& seq, const Effect& e, FrameTime lt, const Image& layer, const Layer3DPlace& pl,
                       const ModelCamera& cam, const std::vector<Layer3DCaster>& casters, std::vector<float>* depth, bool serial) {
    const int W = layer.width, H = layer.height;
    auto P = [&](const char* name, double def) { return e.p(name, lt, def); };
    std::shared_ptr<const Model> plane;
    ModelScene scene;
    scene.triangles = layer3dTriangles(pl, plane, float(std::clamp(P("rough", 50) / 100, 0.0, 1.0)));
    scene.model = plane.get();
    scene.picture = pictureTexture(layer, seq, 4096);  // (UHD cards at their own size)
    float ambient = 0;
    ModelLight light = settingsLight(e, lt, pl.origin, pl.seqWidth * pl.unit, &ambient);
    light.shadows = true;  // (it takes the casters' shadows, and casts its own on the ground when it casts them)
    scene.castsShadows = P("shadows", 0) > 0.5;
    scene.ambient = {ambient, ambient, ambient};
    if (P("light_strength", 100) > 0) scene.lights.push_back(light);
    for (const Layer3DCaster& k : casters) {
        std::shared_ptr<const Model> m;
        scene.casters.push_back({layer3dTriangles(k.place, m), k.picture, !k.ownGround});
    }
    scene.ground = P("ground", 0) > 0.5;
    scene.groundHeight = P("ground_y", 0);
    scene.groundStrength = float(std::clamp(P("ground_strength", 80) / 100, 0.0, 1.0));
    ModelRenderOptions ro;
    ro.serial = serial;
    std::vector<float> d;
    Image out = renderModel(scene, cam, W, H, &d, ro);
    // Linear Rec. 709 light into the working space (as pictureTexture took it out of it).
    const SceneGrading* sg = currentSceneGrading();
    const ColorSpace& to = sg ? *sg->linear : sequenceColorSpace(seq);
    double mtx[9];
    primariesMatrix(Primaries::Bt709, to.primaries, mtx);
    auto rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                float* q = out.at(x, y);
                if (!(q[3] > 0)) {
                    q[0] = q[1] = q[2] = q[3] = 0;
                    d[size_t(y) * size_t(W) + size_t(x)] = 0;
                    continue;
                }
                const double l[3] = {q[0] / q[3], q[1] / q[3], q[2] / q[3]};
                for (int k = 0; k < 3; ++k)
                    q[k] = float(fromLinear(to.transfer, std::max(0.0, mtx[k * 3] * l[0] + mtx[k * 3 + 1] * l[1] + mtx[k * 3 + 2] * l[2]))) * q[3];
            }
    };
    if (serial) rows(0, H);
    else parallelRows(H, rows);
    if (depth) *depth = std::move(d);
    return out;
}

double layer3dCoc(const ModelCamera& cam, int width, double z, double focus, double fstop, double unitsPerMetre) {
    if (!(z > 0) || !(focus > 0) || !(fstop > 0) || !(unitsPerMetre > 0) || width <= 0) return 0;
    // The sensor (36 mm across the picture) in scene units; the lens's focal length there from the camera's in pixels.
    const double sensor = 0.036 * unitsPerMetre, pitch = sensor / width;
    const double f = std::hypot(cam.screen[0], cam.screen[3]) * pitch;
    if (!(focus > f)) return 0;
    const double aperture = f / fstop;
    // The thin lens: a point at z focuses off the sensor; its circle's diameter there A f |zf - z| / (z (zf - f)).
    const double across = aperture * f * std::fabs(focus - z) / (z * (focus - f));
    const double r = across / 2 / pitch;
    return z < focus ? r : -r;
}

void layer3dDepthOfField(Image& img, std::vector<float>& depth, const ModelCamera& cam, double focus, double fstop,
                         double unitsPerMetre) {
    const int W = img.width, H = img.height;
    const size_t N = size_t(W) * size_t(H);
    if (depth.size() != N || N == 0) return;
    std::vector<float> coc(N, 0.0f);
    double widest = 0;
    for (size_t i = 0; i < N; ++i) {
        if (!(depth[i] > 0)) continue;
        // (at most a twentieth of the picture's width: a card running up to the lens is not blurred past that)
        const double cap = 0.05 * W;
        coc[i] = float(std::clamp(layer3dCoc(cam, W, depth[i], focus, fstop, unitsPerMetre), -cap, cap));
        widest = std::max(widest, double(std::fabs(coc[i])));
    }
    if (widest < 0.5) return;  // (in focus: as it was)
    // As Lens Blur (Depth) takes it: 0.5 in focus, nearer up to 1, farther down to 0, the blur growing straight from
    // the focus to `widest` at either end, so each pixel's circle is its own.
    LensBlurSettings ls;
    ls.focus = 0.5, ls.half = 0, ls.falloff = 0.5, ls.radius = widest, ls.blurNear = true;
    // (pixels with nothing there are put with the farthest of the card, so its blur spreads over them as a nearer
    // layer's does: none of them is a sharp subject the blur must stay behind)
    std::vector<float> d(N);
    float farthest = 1;
    for (size_t i = 0; i < N; ++i) {
        d[i] = float(0.5 + coc[i] / (2 * widest));
        if (depth[i] > 0) farthest = std::min(farthest, d[i]);
    }
    for (size_t i = 0; i < N; ++i)
        if (!(depth[i] > 0)) d[i] = farthest;
    // (in the space the card was drawn in: scene-referred grading's linear one when it is on)
    const SceneGrading* sg = currentSceneGrading();
    const ColorSpace& working = sg ? *sg->linear : currentWorkingSpace();
    std::unique_ptr<TransferTables> tables;
    if (working.transfer != Transfer::Linear) tables = std::make_unique<TransferTables>(working.transfer);
    lensBlur(img, d, ls, tables.get());
    // The depth spread out with the blur: each empty pixel the blur reached takes the depth of the nearest that had
    // something (a chamfer distance transform, forward then back).
    std::vector<float> grown = depth, dist(N);
    for (size_t i = 0; i < N; ++i) dist[i] = depth[i] > 0 ? 0.0f : 1e30f;
    auto relax = [&](int x, int y, int dx, int dy, float step) {
        const int nx = x + dx, ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= W || ny >= H) return;
        const size_t i = size_t(y) * size_t(W) + size_t(x), n = size_t(ny) * size_t(W) + size_t(nx);
        if (dist[n] + step < dist[i]) dist[i] = dist[n] + step, grown[i] = grown[n];
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) relax(x, y, -1, 0, 1), relax(x, y, -1, -1, 1.4142f), relax(x, y, 0, -1, 1), relax(x, y, 1, -1, 1.4142f);
    for (int y = H - 1; y >= 0; --y)
        for (int x = W - 1; x >= 0; --x) relax(x, y, 1, 0, 1), relax(x, y, 1, 1, 1.4142f), relax(x, y, 0, 1, 1), relax(x, y, -1, 1, 1.4142f);
    for (size_t i = 0; i < N; ++i)
        if (!(depth[i] > 0) && img.px[i * 4 + 3] > 0 && dist[i] < 1e29f) depth[i] = grown[i];
}

bool renderClipLayer3D(const Project& p, const Sequence& seq, const Clip& c, const Effect& e, FrameTime t, const Image& layer, double scale,
                       Image& out, std::vector<float>* depth, std::string* error, const std::function<Image(const Clip&)>& cardLayer) {
    ModelCamera cam;
    if (!effectCamera(p, seq, c, e, t, scale, cam, error)) return false;
    const FrameTime lt = t - c.start;
    const Layer3DPlace pl = layer3dPlace(e, lt, seq.width, seq.height);
    const bool dof = e.p("depth_of_field", lt, 0) > 0.5;
    std::vector<float> own;
    std::vector<float>* d = depth ? depth : dof ? &own : nullptr;
    if (e.p("lit", lt, 0) > 0.5 && !layer.empty() && scale > 0) {
        // The other cards casting shadows through the same kind of camera at t.
        std::vector<Layer3DCaster> casters;
        const long mode = std::lround(e.p("camera", lt, 3));
        if (cardLayer)
            for (const Track& track : seq.videoTracks)
                for (const Clip& k : track.clips) {
                    if (track.muted || &k == &c || k.id == c.id || !k.enabled || t < k.start || t >= k.end()) continue;
                    const Effect* ke = nullptr;  // (its first, as the card is drawn by)
                    for (const Effect& x : k.effects)
                        if (!ke && x.enabled && x.node == 0 && x.type == "layer_3d") ke = &x;
                    if (!ke || ke->p("shadows", t - k.start, 0) <= 0.5 || std::lround(ke->p("camera", t - k.start, 3)) != mode) continue;
                    if (k.isGenerator() && k.generator.type == "model3d") continue;
                    const Image kl = cardLayer(k);
                    if (kl.empty()) continue;
                    const FrameTime kt = t - k.start;
                    casters.push_back({layer3dPlace(*ke, t - k.start, seq.width, seq.height), pictureTexture(kl, seq),
                                       ke->p("lit", kt, 0) > 0.5 && ke->p("ground", kt, 0) > 0.5});
                }
        out = renderLitLayer3D(seq, e, lt, layer, pl, cam, casters, d);
    } else {
        out = renderLayer3D(layer, pl, cam, scale, d);
    }
    if (dof && d) {
        const long mode = std::lround(e.p("camera", lt, 3));
        double units = e.p("units_m", lt, 0);
        if (!(units > 0)) units = mode == 1 || mode == 2 ? 1 : 1000;
        double focus = e.p("focus_distance", lt, 0);
        if (!(focus > 0)) {  // (the card's middle)
            const V3 q{pl.origin[0] - cam.C[0], pl.origin[1] - cam.C[1], pl.origin[2] - cam.C[2]};
            focus = cam.R[6] * q[0] + cam.R[7] * q[1] + cam.R[8] * q[2];
        }
        layer3dDepthOfField(out, *d, cam, focus, e.p("fstop", lt, 2.8), units);
    }
    return true;
}

}  // namespace montage
