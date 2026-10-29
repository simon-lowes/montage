#include "ModelClip.h"

#include <QCryptographicHash>
#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <mutex>

#include "media/CameraSolve.h"
#include "core/Expression.h"
#include "render/ClipAnalysis.h"
#include "render/ColorSpace.h"
#include "render/Compositor.h"

namespace montage {

namespace {

constexpr double kDeg = 3.14159265358979323846 / 180;

const Clip* clipOn(const Track& track, FrameTime t) {
    for (const Clip& c : track.clips)
        if (c.enabled && t >= c.start && t < c.end()) return &c;
    return nullptr;
}

int trackOf(const Sequence& seq, const Clip& c) {
    for (size_t i = 0; i < seq.videoTracks.size(); ++i)
        for (const Clip& k : seq.videoTracks[i].clips)
            if (k.id == c.id) return int(i);
    return -1;
}

// Solves parsed once while their text is the same.
std::shared_ptr<const CameraSolve> parsedSolve(const Clip& c) {
    static std::mutex m;
    static std::list<std::pair<QByteArray, std::shared_ptr<const CameraSolve>>> recent;
    std::string text;
    for (const Effect& e : c.effects)
        if (e.type == "camera_track" && e.enabled) text = e.s("solve");
    if (text.empty()) return nullptr;
    const QByteArray key = QCryptographicHash::hash(QByteArray::fromStdString(text), QCryptographicHash::Md5);
    std::lock_guard lock(m);
    for (auto it = recent.begin(); it != recent.end(); ++it)
        if (it->first == key) {
            recent.splice(recent.begin(), recent, it);
            return it->second;
        }
    auto s = std::make_shared<CameraSolve>();
    if (!cameraSolveFromString(text, *s) || !(s->focal > 0) || s->width <= 0 || s->height <= 0) return nullptr;
    recent.emplace_front(key, s);
    if (recent.size() > 4) recent.pop_back();
    return s;
}

// A rotation turning by Y (turn), then X (tilt), then Z (roll), degrees, as a matrix.
Mat4 turned(double rx, double ry, double rz) {
    auto q = [](double ax, double ay, double az, double deg) {
        const double h = deg * kDeg / 2, s = std::sin(h);
        return std::array<double, 4>{ax * s, ay * s, az * s, std::cos(h)};
    };
    return trsMatrix({0, 0, 0}, q(0, 1, 0, ry), {1, 1, 1}) * trsMatrix({0, 0, 0}, q(1, 0, 0, rx), {1, 1, 1}) *
           trsMatrix({0, 0, 0}, q(0, 0, 1, rz), {1, 1, 1});
}

// The picture on a track as linear Rec.709 light for image-based lighting, at most 512 wide, kept while it is the
// same.
std::shared_ptr<const ModelEnvironment> environmentFrom(const Image& layer, const Sequence& seq) {
    if (layer.empty()) return nullptr;
    const int w = std::min(512, layer.width), h = std::max(1, int(std::lround(double(layer.height) * w / layer.width)));
    std::vector<float> rgb(size_t(w) * size_t(h) * 3);
    const ColorSpace& from = sequenceColorSpace(seq);
    double mtx[9];
    primariesMatrix(from.primaries, Primaries::Bt709, mtx);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int x0 = int(int64_t(x) * layer.width / w), x1 = std::max(x0 + 1, int(int64_t(x + 1) * layer.width / w));
            const int y0 = int(int64_t(y) * layer.height / h), y1 = std::max(y0 + 1, int(int64_t(y + 1) * layer.height / h));
            double s[3] = {0, 0, 0};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const float* q = layer.at(sx, sy);
                    const float a = q[3] > 1e-6f ? q[3] : 1;
                    for (int k = 0; k < 3; ++k) s[k] += toLinear(from.transfer, q[k] / a);
                }
            const double n = double(x1 - x0) * (y1 - y0);
            const double l[3] = {s[0] / n, s[1] / n, s[2] / n};
            for (int k = 0; k < 3; ++k)
                rgb[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(k)] = float(std::max(0.0, mtx[k * 3] * l[0] + mtx[k * 3 + 1] * l[1] + mtx[k * 3 + 2] * l[2]));
        }
    const QByteArray key = QCryptographicHash::hash(QByteArray(reinterpret_cast<const char*>(rgb.data()), qsizetype(rgb.size() * sizeof(float))),
                                                    QCryptographicHash::Md5);
    static std::mutex m;
    static std::list<std::pair<QByteArray, std::shared_ptr<const ModelEnvironment>>> recent;
    std::lock_guard lock(m);
    for (auto it = recent.begin(); it != recent.end(); ++it)
        if (it->first == key) {
            recent.splice(recent.begin(), recent, it);
            return it->second;
        }
    auto e = std::make_shared<ModelEnvironment>();
    e->width = w, e->height = h, e->rgb = std::move(rgb);
    recent.emplace_front(key, e);
    if (recent.size() > 2) recent.pop_back();
    return e;
}

// A dome light's picture as image light (tinted by its colour), kept while its model is.
std::shared_ptr<const ModelEnvironment> domeEnvironment(const std::shared_ptr<const Model>& model, size_t light) {
    const ModelLightInfo& l = model->lights[light];
    if (l.picture.empty()) return nullptr;
    static std::mutex m;
    struct Kept {
        std::shared_ptr<const Model> model;
        size_t light;
        std::shared_ptr<const ModelEnvironment> env;
    };
    static std::list<Kept> recent;
    std::lock_guard lock(m);
    for (auto it = recent.begin(); it != recent.end(); ++it)
        if (it->model == model && it->light == light) {
            recent.splice(recent.begin(), recent, it);
            return it->env;
        }
    auto e = std::make_shared<ModelEnvironment>();
    e->width = l.pictureWidth, e->height = l.pictureHeight, e->rgb = l.picture;
    for (size_t i = 0; i < e->rgb.size(); ++i) e->rgb[i] *= l.color[i % 3];
    recent.push_front({model, light, e});
    if (recent.size() > 2) recent.pop_back();
    return e;
}

// The file's lights (USD's or glTF's) in the world, placed with the model by `place` (scaling it by `k`), and its dome
// light as image light.
void addFileLights(const std::shared_ptr<const Model>& model, const ModelPose& pose, const Mat4& place, double k, ModelScene& scene) {
    constexpr double kPi = 3.14159265358979323846;
    for (size_t i = 0; i < model->lights.size(); ++i) {
        const ModelLightInfo& l = model->lights[i];
        if (l.node < 0 || size_t(l.node) >= pose.world.size()) continue;
        const Mat4 W = place * pose.world[size_t(l.node)];
        if (l.kind == ModelLightInfo::Kind::Dome) {
            if (scene.environment) continue;  // (one image light)
            scene.environment = domeEnvironment(model, i);
            if (!scene.environment) {  // its colour alone, from all round
                for (int c = 0; c < 3; ++c) scene.ambient[size_t(c)] += l.color[size_t(c)];
                continue;
            }
            // Turned as its node turns about the vertical; its picture's middle faces +z (ours faces -z).
            const std::array<double, 3> x = transformDirection(W, {1, 0, 0});
            scene.environmentRotation = std::atan2(-x[2], x[0]) + kPi;
            scene.environmentIntensity = 1;
            continue;
        }
        ModelLight ml;
        const std::array<double, 3> at = transformPoint(W, {0, 0, 0}), way = transformDirection(W, {0, 0, -1});
        const double wl = std::sqrt(way[0] * way[0] + way[1] * way[1] + way[2] * way[2]);
        ml.position = at;
        if (wl > 0) ml.direction = {way[0] / wl, way[1] / wl, way[2] / wl};
        ml.color = l.color;
        ml.shadows = l.shadows;
        // Intensity: illuminance for a distant light; else what it gives 1 unit away (its luminance times the area
        // it shows, or spread over its whole surface when normalised), the units scaled with the model.
        double intensity = 1;
        const double r = std::max(0.0, l.radius);
        switch (l.kind) {
        case ModelLightInfo::Kind::Distant: {
            const double half = std::clamp(l.angle * kPi / 360, 0.0, kPi), s2 = std::sin(half) * std::sin(half);
            intensity = l.normalize || half == 0 ? 1 : half <= kPi / 2 ? kPi * s2 : (2 - s2) * kPi;
            break;
        }
        case ModelLightInfo::Kind::Point: break;
        case ModelLightInfo::Kind::Sphere: intensity = r == 0 ? 1 : l.normalize ? 0.25 : kPi * r * r; break;
        case ModelLightInfo::Kind::Rect: intensity = l.normalize ? 1 : std::max(0.0, l.width * l.height); break;
        case ModelLightInfo::Kind::Disk: intensity = l.normalize ? 1 : kPi * r * r; break;
        case ModelLightInfo::Kind::Cylinder: intensity = l.normalize ? 1 / kPi : 2 * r * std::max(0.0, l.length); break;
        case ModelLightInfo::Kind::Dome: break;
        }
        if (l.kind == ModelLightInfo::Kind::Distant) {
            ml.kind = ModelLight::Kind::Directional;
            ml.intensity = intensity;
        } else {
            ml.intensity = intensity * k * k;
            const bool face = l.kind == ModelLightInfo::Kind::Rect || l.kind == ModelLightInfo::Kind::Disk;
            if (face || l.coneAngle < 180) {  // (a rect or disk lights the side it faces)
                ml.kind = ModelLight::Kind::Spot;
                ml.outerCone = std::clamp(std::min(l.coneAngle, face ? 89.0 : 180.0), 0.1, 179.0) * kDeg;
                ml.innerCone = face && l.coneAngle >= 180 ? 0 : ml.outerCone * (1 - std::clamp(l.coneSoftness, 0.0, 1.0));
            } else {
                ml.kind = ModelLight::Kind::Point;
            }
        }
        scene.lights.push_back(ml);
    }
}

// How much a model or splat clip scales its file: 1 seen through a file's camera, its scale (%) in a solved scene,
// else its size (pixels) across its largest extent.
double clipScale(const Effect& g, FrameTime lt, long mode, const std::array<double, 3>& lo, const std::array<double, 3>& hi) {
    if (mode == 2) return 1;
    if (mode == 1) return g.p("scale", lt, 100) / 100;
    return g.p("size", lt, 400) / std::max({1e-9, hi[1] - lo[1], hi[0] - lo[0], hi[2] - lo[2]});
}

// Where it puts its file in the world: centred on the bounds (standing on their bottom in a solved scene when asked),
// scaled by k, turned and moved; where the file has it when seen through a file's camera.
Mat4 clipPlace(const Effect& g, FrameTime lt, long mode, double k, const std::array<double, 3>& lo, const std::array<double, 3>& hi) {
    if (mode == 2) return identity4();
    auto P = [&](const char* name, double def) { return g.p(name, lt, def); };
    const bool stand = mode == 1 && P("stand", 1) > 0.5;
    const Mat4 centre = trsMatrix({-k * (lo[0] + hi[0]) / 2, -k * (stand ? lo[1] : (lo[1] + hi[1]) / 2), -k * (lo[2] + hi[2]) / 2}, {0, 0, 0, 1}, {k, k, k});
    return trsMatrix({P("pos_x", 0), P("pos_y", 0), P("pos_z", 0)}, {0, 0, 0, 1}, {1, 1, 1}) * turned(P("rot_x", 0), P("rot_y", 0), P("rot_z", 0)) * centre;
}

// Camera `camera_index` (1-based) of `model` posed by `pose` at `sec`, on the sequence's picture scaled by `scale`.
bool fileCameraOf(const Model& model, const ModelPose& pose, double sec, const std::string& file, const Effect& g, FrameTime lt,
                  const Sequence& seq, double scale, ModelCamera& cam, std::string* error) {
    const int index = int(std::lround(g.p("camera_index", lt, 1))) - 1;
    std::array<double, 3> at;
    std::array<std::array<double, 3>, 3> axes;
    if (index < 0 || size_t(index) >= model.cameras.size() || !modelCameraPlace(pose, model.cameras[size_t(index)], at, axes)) {
        if (error) *error = model.cameras.empty() ? file + " has no camera" : file + " has no camera " + std::to_string(index + 1);
        return false;
    }
    const ModelCameraInfo& fc = model.cameras[size_t(index)];
    // Rows: right, down and ahead.
    cam.R = {axes[0][0], axes[0][1], axes[0][2], -axes[1][0], -axes[1][1], -axes[1][2], -axes[2][0], -axes[2][1], -axes[2][2]};
    cam.C = at;
    double f, cx, cy;
    modelCameraLens(fc, sec, seq.width, seq.height, f, cx, cy);
    cam.screen = {f * scale, 0, cx * scale, 0, f * scale, cy * scale};
    cam.nearZ = std::max(1e-6, fc.nearClip);
    return true;
}

// The video track whose solved camera an effect `g` on a clip on `track` uses at t (its local time `lt`).
int cameraTrackOf(const Sequence& seq, size_t track, const Effect& g, FrameTime lt, FrameTime t) {
    const int v = int(std::lround(g.p("camera_track", lt, 0)));
    if (v > 0) return v - 1 < int(seq.videoTracks.size()) && v - 1 != int(track) ? v - 1 : -1;
    for (int k = int(track) - 1; k >= 0; --k)
        if (const Clip* f = clipOn(seq.videoTracks[size_t(k)], t); f && parsedSolve(*f)) return k;
    return -1;
}

// The camera in front of the frame with a field of view `fov` (degrees across its width): the plane z = 0 at one
// sequence pixel a unit, y up, the camera on +z looking back.
void frontCamera(const Sequence& seq, double fov, double scale, ModelCamera& cam) {
    const double SW = seq.width, SH = seq.height;
    const double f = (SW / 2) / std::tan(std::clamp(fov, 1.0, 170.0) * kDeg / 2);
    cam.R = {1, 0, 0, 0, -1, 0, 0, 0, -1};
    cam.C = {0, 0, f};
    cam.screen = {f * scale, 0, SW / 2 * scale, 0, f * scale, SH / 2 * scale};
    cam.nearZ = 1;
}

// The camera a model, splat or 3D layer (`g`, on clip `c`) is seen through at t: its own in front of the frame (mode
// 0), the one the 3D Camera Tracker solved (1; `onGround`: whether that scene has a ground, at y = 0), one in the file
// named for the camera (2), moving as that file's first animation moves it, or the scene's Camera clip (3).
bool clipCamera(const Project& p, const Sequence& seq, const Clip& c, const Effect& g, FrameTime t, double scale, int ti, long mode,
                ModelCamera& cam, double& groundY, bool& onGround, std::string* error) {
    const FrameTime lt = t - c.start;
    auto P = [&](const char* name, double def) { return g.p(name, lt, def); };
    const bool solved = mode == 1, fileCamera = mode == 2;
    onGround = true;
    if (solved) {
        const int ct = ti >= 0 ? cameraTrackOf(seq, size_t(ti), g, lt, t) : -1;
        const Clip* footage = ct >= 0 ? clipOn(seq.videoTracks[size_t(ct)], t) : nullptr;
        const std::shared_ptr<const CameraSolve> solve = footage ? parsedSolve(*footage) : nullptr;
        const int frame = solve ? cameraSolveFrame(seq, *footage, *solve, t - footage->start) : -1;
        if (frame < 0 || !solve->cameras[size_t(frame)].valid) {
            if (error) *error = "The 3D Camera Tracker's camera is not solved here (on the footage below the model, or the track named)";
            return false;
        }
        const SolvedCamera& sc = solve->cameras[size_t(frame)];
        cam.R = sc.R;
        cam.C = {sc.C.x, sc.C.y, sc.C.z};
        // The solve's picture (pixels of its size) to the footage's frame (fractions), then to the sequence through the
        // footage's transform, then to the output.
        double x0, y0, x1, y1, x2, y2;
        if (!clipFrameToSequence(p, seq, *footage, t, 0, 0, x0, y0) || !clipFrameToSequence(p, seq, *footage, t, 1, 0, x1, y1) ||
            !clipFrameToSequence(p, seq, *footage, t, 0, 1, x2, y2)) {
            if (error) *error = "The footage with the solved camera has no picture here";
            return false;
        }
        const double fu = solve->focal / solve->width, fv = solve->focal / solve->height;
        // seq = (x0, y0) + (x1 - x0) u + (x2 - x0) v, with u = fu X + 0.5 and v = fv Y + 0.5.
        cam.screen = {(x1 - x0) * fu * scale, (x2 - x0) * fv * scale, (x0 + (x1 - x0) * 0.5 + (x2 - x0) * 0.5) * scale,
                      (y1 - y0) * fu * scale, (y2 - y0) * fv * scale, (y0 + (y1 - y0) * 0.5 + (y2 - y0) * 0.5) * scale};
        cam.nearZ = 1e-3;
        groundY = 0;
        onGround = solve->oriented;  // (a scene not stood on its ground has no ground to catch shadows)
    } else if (fileCamera) {
        const std::string file = g.s("camera_file");
        const std::shared_ptr<const Model> model = file.empty() ? nullptr : cachedModel(file, error);
        if (!model) {
            if (error && file.empty()) *error = "Choose the file to take the camera from";
            return false;
        }
        // At the clip's time sped up, offset and looped as the model's animation is, so it keeps step with it.
        double sec = lt / seq.fpsValue() * P("anim_speed", 100) / 100 + P("anim_offset", 0);
        const double d = model->animations.empty() ? 0 : model->animations[0].duration;
        if (P("loop", 1) > 0.5 && d > 0) sec = sec - d * std::floor(sec / d);
        if (!fileCameraOf(*model, poseModel(*model, model->animations.empty() ? -1 : 0, sec), sec, file, g, lt, seq, scale, cam, error)) return false;
    } else if (mode == 3) {
        // The scene's Camera clip (on the track named, else the highest with one at t); where there is none, one with a
        // 35° lens in front of the frame, as a Camera clip that is left as it is.
        const int named = int(std::lround(P("camera_track", 0))) - 1;
        const Clip* cc = sceneCameraClip(seq, t, named);
        if (!cc && named >= 0) cc = sceneCameraClip(seq, t, -1);
        if (cc) sceneCamera(p, seq, *cc, t, scale, cam);
        else frontCamera(seq, 35, scale, cam);
    } else {
        frontCamera(seq, P("fov", 35), scale, cam);
    }
    return true;
}

// A shape made in place of a file's model (1 plane, the frame's shape a unit tall, facing +z; 2 box; 3 sphere; 4
// cylinder, each a unit across), in one material; a picture's top left at uv (0, 0), the right way round from outside.
std::shared_ptr<const Model> shapeModel(int shape, double aspect, const std::array<float, 4>& color, float metal, float rough) {
    auto model = std::make_shared<Model>();
    Model& m = *model;
    m.path = "shape";
    ModelMaterial mat;
    mat.name = "Shape";
    mat.baseColor = color;
    mat.metallic = metal, mat.roughness = rough;
    mat.doubleSided = shape == 1;
    m.materials.push_back(mat);
    ModelPrimitive pr;
    pr.material = 0;
    using V = std::array<double, 3>;
    auto vertex = [&](const V& q, const V& n, double u, double v) {
        for (int k = 0; k < 3; ++k) pr.positions.push_back(float(q[size_t(k)])), pr.normals.push_back(float(n[size_t(k)]));
        pr.uvs.push_back(float(u)), pr.uvs.push_back(float(v));
        return uint32_t(pr.positions.size() / 3 - 1);
    };
    // A face of corners a b c d (top left, top right, bottom left, bottom right as seen from outside).
    auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) { pr.indices.insert(pr.indices.end(), {a, c, b, b, c, d}); };
    // A flat rectangle: its middle, outward normal, right and up (half sizes in them).
    auto face = [&](const V& mid, const V& n, const V& r, const V& up) {
        uint32_t i[4];
        for (int k = 0; k < 4; ++k) {
            const double sx = k & 1 ? 1 : -1, sy = k & 2 ? -1 : 1;
            i[k] = vertex({mid[0] + r[0] * sx + up[0] * sy, mid[1] + r[1] * sx + up[1] * sy, mid[2] + r[2] * sx + up[2] * sy}, n, k & 1 ? 1 : 0,
                          k & 2 ? 1 : 0);
        }
        quad(i[0], i[1], i[2], i[3]);
    };
    constexpr double pi = 3.14159265358979323846;
    if (shape == 1) {
        face({0, 0, 0}, {0, 0, 1}, {std::max(1e-6, aspect) / 2, 0, 0}, {0, 0.5, 0});
    } else if (shape == 2) {
        face({0, 0, 0.5}, {0, 0, 1}, {0.5, 0, 0}, {0, 0.5, 0});
        face({0, 0, -0.5}, {0, 0, -1}, {-0.5, 0, 0}, {0, 0.5, 0});
        face({0.5, 0, 0}, {1, 0, 0}, {0, 0, -0.5}, {0, 0.5, 0});
        face({-0.5, 0, 0}, {-1, 0, 0}, {0, 0, 0.5}, {0, 0.5, 0});
        face({0, 0.5, 0}, {0, 1, 0}, {0.5, 0, 0}, {0, 0, -0.5});
        face({0, -0.5, 0}, {0, -1, 0}, {0.5, 0, 0}, {0, 0, 0.5});
    } else if (shape == 3 || shape == 4) {
        // Round the side from the back (u 0) through the front (u 0.5, facing +z) to the back again.
        const int around = 64, down = shape == 3 ? 32 : 1;
        const uint32_t first = uint32_t(pr.positions.size() / 3);
        for (int i = 0; i <= down; ++i)
            for (int j = 0; j <= around; ++j) {
                const double u = double(j) / around, v = double(i) / down, phi = 2 * pi * (u - 0.5);
                if (shape == 3) {
                    const double th = pi * v;
                    const V n{std::sin(th) * std::sin(phi), std::cos(th), std::sin(th) * std::cos(phi)};
                    vertex({n[0] / 2, n[1] / 2, n[2] / 2}, n, u, v);
                } else {
                    const V n{std::sin(phi), 0, std::cos(phi)};
                    vertex({n[0] / 2, 0.5 - v, n[2] / 2}, n, u, v);
                }
            }
        for (int i = 0; i < down; ++i)
            for (int j = 0; j < around; ++j) {
                const uint32_t a = first + uint32_t(i * (around + 1) + j);
                quad(a, a + 1, a + uint32_t(around + 1), a + uint32_t(around + 2));
            }
        if (shape == 4)  // its ends, the picture laid across them
            for (const double y : {0.5, -0.5}) {
                const uint32_t mid = vertex({0, y, 0}, {0, y * 2, 0}, 0.5, 0.5);
                const uint32_t ring = uint32_t(pr.positions.size() / 3);
                for (int j = 0; j <= around; ++j) {
                    const double phi = 2 * pi * j / around;
                    vertex({std::sin(phi) / 2, y, std::cos(phi) / 2}, {0, y * 2, 0}, 0.5 + std::sin(phi) / 2, 0.5 + std::cos(phi) / 2 * (y > 0 ? 1 : -1));
                }
                for (int j = 0; j < around; ++j) pr.indices.insert(pr.indices.end(), {mid, ring + uint32_t(j), ring + uint32_t(j + 1)});
            }
    }
    // Every triangle wound to face out (anticlockwise seen from the side its normals face), its slivers at the poles left out.
    std::vector<uint32_t> kept;
    for (size_t t = 0; t + 2 < pr.indices.size(); t += 3) {
        uint32_t i[3] = {pr.indices[t], pr.indices[t + 1], pr.indices[t + 2]};
        auto P = [&](uint32_t v) { return V{pr.positions[v * 3], pr.positions[v * 3 + 1], pr.positions[v * 3 + 2]}; };
        const V a = P(i[0]), b = P(i[1]), c = P(i[2]);
        const V e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const V n{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        double out = 0;
        for (const uint32_t v : i) out += n[0] * pr.normals[v * 3] + n[1] * pr.normals[v * 3 + 1] + n[2] * pr.normals[v * 3 + 2];
        if (n[0] * n[0] + n[1] * n[1] + n[2] * n[2] < 1e-18) continue;
        if (out < 0) std::swap(i[1], i[2]);
        kept.insert(kept.end(), i, i + 3);
    }
    pr.indices = std::move(kept);
    ModelMesh mesh;
    mesh.name = "Shape";
    mesh.primitives.push_back(std::move(pr));
    m.meshes.push_back(std::move(mesh));
    ModelNode node;
    node.name = "Shape";
    node.mesh = 0;
    m.nodes.push_back(node);
    m.roots.push_back(0);
    return model;
}

// A track's picture (premultiplied, the sequence's colour; graded scene-referred, the linear light layers are mixed in)
// as a texture: linear light, Rec. 709 primaries, premultiplied, at most 2048 pixels across (averaged down).
std::shared_ptr<const ModelTexture> pictureTexture(const Image& layer, const Sequence& seq) {
    if (layer.empty()) return nullptr;
    const SceneGrading* sg = currentSceneGrading();
    const ColorSpace& from = sg ? *sg->linear : sequenceColorSpace(seq);
    double mtx[9];
    primariesMatrix(from.primaries, Primaries::Bt709, mtx);
    const int step = std::max(1, (std::max(layer.width, layer.height) + 2047) / 2048);
    const int w = (layer.width + step - 1) / step, h = (layer.height + step - 1) / step;
    std::vector<float> rgba(size_t(w) * size_t(h) * 4);
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                double sum[4] = {0, 0, 0, 0};
                int n = 0;
                for (int sy = y * step; sy < std::min(layer.height, (y + 1) * step); ++sy)
                    for (int sx = x * step; sx < std::min(layer.width, (x + 1) * step); ++sx, ++n) {
                        const float* q = layer.at(sx, sy);
                        const float a = std::clamp(q[3], 0.0f, 1.0f);
                        if (!(a > 1e-6f)) continue;
                        double l[3];
                        for (int k = 0; k < 3; ++k) l[k] = toLinear(from.transfer, q[k] / a);
                        for (int k = 0; k < 3; ++k) sum[k] += std::max(0.0, mtx[k * 3] * l[0] + mtx[k * 3 + 1] * l[1] + mtx[k * 3 + 2] * l[2]) * a;
                        sum[3] += a;
                    }
                float* o = rgba.data() + (size_t(y) * size_t(w) + size_t(x)) * 4;
                for (int k = 0; k < 4; ++k) o[k] = float(sum[k] / std::max(1, n));
            }
    });
    auto t = std::make_shared<ModelTexture>(linearModelTexture(w, h, std::move(rgba)));
    t->wrapS = t->wrapT = ModelTexture::Wrap::Clamp;
    return t;
}


}  // namespace

int modelCameraTrack(const Sequence& seq, size_t track, const Clip& c, FrameTime t) { return cameraTrackOf(seq, track, c.generator, t - c.start, t); }

const Clip* sceneCameraClip(const Sequence& seq, FrameTime t, int track) {
    auto camera = [&](size_t k) -> const Clip* {
        if (k >= seq.videoTracks.size() || seq.videoTracks[k].muted) return nullptr;
        const Clip* c = clipOn(seq.videoTracks[k], t);
        return c && c->enabled && c->isGenerator() && c->generator.type == "camera3d" ? c : nullptr;
    };
    if (track >= 0) return camera(size_t(track));
    for (size_t k = seq.videoTracks.size(); k-- > 0;)
        if (const Clip* c = camera(k)) return c;
    return nullptr;
}

void sceneCamera(const Project& p, const Sequence& seq, const Clip& camera, FrameTime t, double scale, ModelCamera& cam) {
    ExpressionScope expressions(&p, &seq, &camera);  // (its own expressions see its own clip)
    const Effect& g = camera.generator;
    const FrameTime lt = t - camera.start;
    auto P = [&](const char* name, double def) { return g.p(name, lt, def); };
    frontCamera(seq, P("fov", 35), scale, cam);
    // Moved from where the frame fills its view, and turned (or aimed at a point, then rolled).
    const std::array<double, 3> at{P("pos_x", 0), P("pos_y", 0), cam.C[2] + P("pos_z", 0)};
    std::array<double, 3> right, up, back;
    if (P("aim", 0) > 0.5) {
        std::array<double, 3> b{at[0] - P("aim_x", 0), at[1] - P("aim_y", 0), at[2] - P("aim_z", 0)};
        double l = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
        if (l < 1e-9) b = {0, 0, 1}, l = 1;
        for (double& v : b) v /= l;
        // Right is level (across y), unless it looks straight up or down.
        std::array<double, 3> r{b[2], 0, -b[0]};
        double rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
        if (rl < 1e-9) r = {1, 0, 0}, rl = 1;
        for (double& v : r) v /= rl;
        const std::array<double, 3> u{b[1] * r[2] - b[2] * r[1], b[2] * r[0] - b[0] * r[2], b[0] * r[1] - b[1] * r[0]};
        const double roll = P("rot_z", 0) * kDeg, cr = std::cos(roll), sr = std::sin(roll);
        for (int k = 0; k < 3; ++k) right[size_t(k)] = r[size_t(k)] * cr + u[size_t(k)] * sr, up[size_t(k)] = u[size_t(k)] * cr - r[size_t(k)] * sr;
        back = b;
    } else {
        const Mat4 m = turned(P("rot_x", 0), P("rot_y", 0), P("rot_z", 0));
        right = transformDirection(m, {1, 0, 0}), up = transformDirection(m, {0, 1, 0}), back = transformDirection(m, {0, 0, 1});
    }
    cam.R = {right[0], right[1], right[2], -up[0], -up[1], -up[2], -back[0], -back[1], -back[2]};
    cam.C = at;
}

bool effectCamera(const Project& p, const Sequence& seq, const Clip& c, const Effect& g, FrameTime t, double scale, ModelCamera& cam,
                  std::string* error) {
    double groundY = 0;
    bool onGround = false;
    const long mode = std::lround(g.p("camera", t - c.start, 3));
    return clipCamera(p, seq, c, g, t, scale, trackOf(seq, c), mode, cam, groundY, onGround, error);
}

int modelLightTrack(const Sequence& seq, size_t track, const Clip& c, FrameTime t) {
    const int v = int(std::lround(c.generator.p("env_track", t - c.start, 0)));
    return v > 0 && v - 1 < int(seq.videoTracks.size()) && v - 1 != int(track) ? v - 1 : -1;
}

int modelTextureTrack(const Sequence& seq, size_t track, const Clip& c, FrameTime t) {
    const int v = int(std::lround(c.generator.p("texture_track", t - c.start, 0)));
    return v > 0 && v - 1 < int(seq.videoTracks.size()) && v - 1 != int(track) ? v - 1 : -1;
}

bool modelClipScene(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, int w, int h, double scale,
                    const std::function<Image(int track)>& trackLayer, ModelScene& scene, ModelCamera& cam,
                    std::shared_ptr<const Model>& model, std::string* error) {
    const Effect& g = c.generator;
    const FrameTime lt = t - c.start;
    auto P = [&](const char* name, double def) { return g.p(name, lt, def); };
    const int shape = int(std::lround(P("shape", 0)));
    const std::string file = shape > 0 ? std::string("shape") : g.s("file");
    if (shape > 0) {
        auto lin = [](double v) { return float(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4)); };
        model = shapeModel(shape, double(seq.width) / std::max(1, seq.height),
                           {lin(P("shape_color.r", 0.8)), lin(P("shape_color.g", 0.8)), lin(P("shape_color.b", 0.8)), 1},
                           float(P("shape_metal", 0) / 100), float(P("shape_rough", 50) / 100));
    } else {
        if (file.empty()) {
            if (error) *error = "Choose a model file (glTF, GLB, OBJ or USD)";
            return false;
        }
        if (isSplatFile(file)) {
            if (error) *error = file + " is Gaussian splats, not a model";
            return false;
        }
        model = cachedModel(file, error);
        if (!model) return false;
    }
    const int ti = trackOf(seq, c);
    const double fps = seq.fpsValue();

    // Its pose: the animation chosen (1-based; 0 none) at the clip's time, its speed and offset, looped.
    const int animation = int(std::lround(P("animation", 1))) - 1;
    double sec = lt / fps * P("anim_speed", 100) / 100 + P("anim_offset", 0);
    if (animation >= 0 && size_t(animation) < model->animations.size()) {
        const double d = model->animations[size_t(animation)].duration;
        if (P("loop", 1) > 0.5 && d > 0) sec = sec - d * std::floor(sec / d);
    }
    const PosedTriangles rest = posedTriangles(*model, poseModel(*model, -1, 0));
    std::array<double, 3> lo, hi;
    if (!modelBounds(rest, lo, hi)) {
        if (error) *error = file + " has nothing to draw";
        return false;
    }
    const ModelPose pose = poseModel(*model, animation, sec);
    PosedTriangles posed = posedTriangles(*model, pose);

    // Into the world: centred on its rest bounds (standing on their bottom when asked), sized, turned and moved; seen
    // through a file's camera, left where the file has it.
    const long mode = std::lround(P("camera", 0));
    const bool fileCamera = mode == 2 && shape == 0;
    const double k = clipScale(g, lt, mode, lo, hi);
    const Mat4 place = clipPlace(g, lt, mode, k, lo, hi);
    const Mat4 normalM = inverse4(place);
    for (size_t i = 0; i < posed.positions.size() / 3; ++i) {
        const std::array<double, 3> q = transformPoint(place, {posed.positions[i * 3], posed.positions[i * 3 + 1], posed.positions[i * 3 + 2]});
        const double n0 = posed.normals[i * 3], n1 = posed.normals[i * 3 + 1], n2 = posed.normals[i * 3 + 2];
        double n[3] = {normalM[0] * n0 + normalM[1] * n1 + normalM[2] * n2, normalM[4] * n0 + normalM[5] * n1 + normalM[6] * n2,
                       normalM[8] * n0 + normalM[9] * n1 + normalM[10] * n2};
        const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const std::array<double, 3> tg = transformDirection(place, {posed.tangents[i * 4], posed.tangents[i * 4 + 1], posed.tangents[i * 4 + 2]});
        for (int a = 0; a < 3; ++a) {
            posed.positions[i * 3 + size_t(a)] = float(q[size_t(a)]);
            posed.normals[i * 3 + size_t(a)] = float(l > 0 ? n[a] / l : 0);
            posed.tangents[i * 4 + size_t(a)] = float(tg[size_t(a)]);
        }
    }
    // The ground under the model's rest pose as placed.
    double groundY = 1e300, size = 0;
    std::array<double, 3> wlo{1e300, 1e300, 1e300}, whi{-1e300, -1e300, -1e300};
    for (int corner = 0; corner < 8; ++corner) {
        const std::array<double, 3> q = transformPoint(place, {corner & 1 ? hi[0] : lo[0], corner & 2 ? hi[1] : lo[1], corner & 4 ? hi[2] : lo[2]});
        groundY = std::min(groundY, q[1]);
        for (int a = 0; a < 3; ++a) wlo[size_t(a)] = std::min(wlo[size_t(a)], q[size_t(a)]), whi[size_t(a)] = std::max(whi[size_t(a)], q[size_t(a)]);
    }
    size = std::max({whi[0] - wlo[0], whi[1] - wlo[1], whi[2] - wlo[2]});
    const std::array<double, 3> mid{(wlo[0] + whi[0]) / 2, (wlo[1] + whi[1]) / 2, (wlo[2] + whi[2]) / 2};

    // The camera: the file's own, unless another file is named for it.
    bool onGround = true;
    if (fileCamera && g.s("camera_file").empty()) {
        if (!fileCameraOf(*model, pose, sec, file, g, lt, seq, scale, cam, error)) return false;
    } else if (!clipCamera(p, seq, c, g, t, scale, ti, mode, cam, groundY, onGround, error)) {
        return false;
    }
    (void)w, (void)h;

    // Its light.
    scene = ModelScene{};
    scene.triangles = std::move(posed);
    scene.model = model.get();
    const double az = P("light_angle", -40) * kDeg, el = P("light_height", 45) * kDeg;
    const std::array<double, 3> towards{std::cos(el) * std::sin(az), std::sin(el), std::cos(el) * std::cos(az)};
    ModelLight light;
    const int kind = int(std::lround(P("light_kind", 0)));
    light.kind = kind == 1 ? ModelLight::Kind::Point : kind == 2 ? ModelLight::Kind::Spot : ModelLight::Kind::Directional;
    light.direction = {-towards[0], -towards[1], -towards[2]};
    const double distance = P("light_distance", 3) * size;
    light.position = {mid[0] + towards[0] * distance, mid[1] + towards[1] * distance, mid[2] + towards[2] * distance};
    light.color = {float(P("light_color.r", 1)), float(P("light_color.g", 1)), float(P("light_color.b", 1))};
    light.intensity = 3.14159265358979323846 * P("light_strength", 100) / 100 * (light.kind == ModelLight::Kind::Directional ? 1 : distance * distance);
    light.outerCone = std::clamp(P("spot_angle", 30), 1.0, 85.0) * kDeg;
    light.innerCone = light.outerCone * 0.8;
    light.shadows = P("shadows", 1) > 0.5;
    const float ambient = float(P("ambient", 30) / 100);
    scene.ambient = {ambient, ambient, ambient};
    if (kind == 3) addFileLights(model, pose, place, k, scene);
    else if (P("light_strength", 100) > 0) scene.lights.push_back(light);
    if (const int lt2 = ti >= 0 ? modelLightTrack(seq, size_t(ti), c, t) : -1; lt2 >= 0 && trackLayer) {
        scene.environment = environmentFrom(trackLayer(lt2), seq);
        scene.environmentRotation = P("env_rotation", 0) * kDeg;
        scene.environmentIntensity = float(P("env_strength", 100) / 100);
    } else if (scene.environment) {  // (the file's dome light)
        scene.environmentRotation += P("env_rotation", 0) * kDeg;
        scene.environmentIntensity = float(P("env_strength", 100) / 100);
    }
    if (const int tt = ti >= 0 ? modelTextureTrack(seq, size_t(ti), c, t) : -1; tt >= 0 && trackLayer) {
        scene.picture = pictureTexture(trackLayer(tt), seq);
        scene.pictureMaterial = int(std::lround(P("texture_material", 0))) - 1;
        scene.pictureAsIs = std::lround(P("texture_light", 0)) == 1;
    }
    scene.ground = onGround && P("ground", 1) > 0.5;
    scene.groundHeight = groundY;
    scene.groundStrength = float(P("ground_strength", 80) / 100);
    scene.opacity = float(P("opacity", 100) / 100);
    return true;
}

bool splatClipScene(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, double scale,
                    std::shared_ptr<const SplatCloud>& cloud, Mat4& place, ModelCamera& cam, SplatRenderOptions& options,
                    std::string* error) {
    const Effect& g = c.generator;
    const FrameTime lt = t - c.start;
    auto P = [&](const char* name, double def) { return g.p(name, lt, def); };
    const std::string file = g.s("file");
    if (file.empty()) {
        if (error) *error = "Choose a splat file (PLY or SPZ)";
        return false;
    }
    // The file of a numbered run showing now (at the run's own rate, sped up and offset as an animation is), sized by
    // the file chosen so that it stays put as it plays.
    const double fps = seq.fpsValue();
    const double sec = lt / fps * P("anim_speed", 100) / 100 + P("anim_offset", 0);
    const double rate = P("frames_fps", 0) > 0 ? P("frames_fps", 0) : fps;
    const std::string now = P("frames", 1) > 0.5 ? splatFrameFile(file, sec, rate, P("loop", 1) > 0.5) : file;
    cloud = cachedSplats(now, error);
    if (!cloud) return false;
    std::shared_ptr<const SplatCloud> base = file == now ? cloud : cachedSplats(file);
    if (!base) base = cloud;
    const std::array<double, 3> lo{base->lo[0], base->lo[1], base->lo[2]}, hi{base->hi[0], base->hi[1], base->hi[2]};
    const long mode = std::lround(P("camera", 0));
    const double k = clipScale(g, lt, mode, lo, hi);
    place = clipPlace(g, lt, mode, k, lo, hi);
    double groundY = 0;
    bool onGround = false;
    if (mode == 2 && g.s("camera_file").empty()) {
        if (error) *error = "Splats have no camera of their own: choose a file to take the camera from (USD or glTF)";
        return false;
    }
    const int ti = trackOf(seq, c);
    if (!clipCamera(p, seq, c, c.generator, t, scale, ti, mode, cam, groundY, onGround, error)) return false;
    options = SplatRenderOptions{};
    options.crop = P("crop", 0) > 0.5;
    const char* names[3][2] = {{"crop_x0", "crop_x1"}, {"crop_y0", "crop_y1"}, {"crop_z0", "crop_z1"}};
    for (int a = 0; a < 3; ++a) {
        const double u = P(names[a][0], -10), v = P(names[a][1], 10);
        options.cropLo[size_t(a)] = std::min(u, v), options.cropHi[size_t(a)] = std::max(u, v);
    }
    options.opacity = float(std::clamp(P("opacity", 100) / 100, 0.0, 1.0));
    return true;
}

std::vector<SolvedSceneMesh> solvedSceneModels(const Project& p, const Sequence& seq, size_t track, const Clip& footage) {
    std::vector<SolvedSceneMesh> out;
    for (size_t ti = 0; ti < seq.videoTracks.size(); ++ti)
        for (const Clip& c : seq.videoTracks[ti].clips) {
            if (!c.enabled || !c.isGenerator() || c.generator.type != "model3d" || std::lround(c.generator.p("camera", 0, 0)) != 1) continue;
            if (modelCameraTrack(seq, ti, c, c.start) != int(track) || clipOn(seq.videoTracks[track], c.start) != &footage) continue;
            ModelScene scene;
            ModelCamera cam;
            std::shared_ptr<const Model> model;
            if (!modelClipScene(p, seq, c, c.start, seq.width, seq.height, 1, {}, scene, cam, model)) continue;
            const PosedTriangles& tr = scene.triangles;
            std::map<int, size_t> byMaterial;
            for (size_t t = 0; t < tr.materials.size() && t * 3 + 2 < tr.indices.size(); ++t) {
                const int mi = tr.materials[t];
                auto it = byMaterial.find(mi);
                if (it == byMaterial.end()) {
                    SolvedSceneMesh m;
                    m.name = c.name.empty() ? model->path : c.name;
                    if (mi >= 0 && size_t(mi) < model->materials.size()) {
                        const ModelMaterial& mm = model->materials[size_t(mi)];
                        m.color = mm.baseColor;
                        m.metallic = mm.metallic, m.roughness = mm.roughness;
                        m.doubleSided = mm.doubleSided;
                    }
                    it = byMaterial.emplace(mi, out.size()).first;
                    out.push_back(std::move(m));
                }
                SolvedSceneMesh& m = out[it->second];
                for (int v = 0; v < 3; ++v) {
                    const uint32_t i = tr.indices[t * 3 + size_t(v)];
                    m.indices.push_back(uint32_t(m.positions.size() / 3));
                    for (int a = 0; a < 3; ++a) {
                        m.positions.push_back(tr.positions[size_t(i) * 3 + size_t(a)]);
                        m.normals.push_back(tr.normals[size_t(i) * 3 + size_t(a)]);
                    }
                }
            }
        }
    return out;
}

Image renderModelClip(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, int w, int h, double scale,
                      const std::function<Image(int track)>& trackLayer, std::vector<float>* depth, std::string* error) {
    if (std::lround(c.generator.p("shape", t - c.start, 0)) == 0 && isSplatFile(c.generator.s("file"))) {
        // (already in the values the splats were trained on, as the other generators draw)
        std::shared_ptr<const SplatCloud> cloud;
        Mat4 place;
        ModelCamera cam;
        SplatRenderOptions options;
        if (!splatClipScene(p, seq, c, t, scale, cloud, place, cam, options, error)) {
            if (depth) depth->assign(size_t(std::max(0, w)) * size_t(std::max(0, h)), 0.0f);
            return Image(std::max(1, w), std::max(1, h));
        }
        return renderSplats(*cloud, place, cam, w, h, depth, options);
    }
    ModelScene scene;
    ModelCamera cam;
    std::shared_ptr<const Model> model;
    if (!modelClipScene(p, seq, c, t, w, h, scale, trackLayer, scene, cam, model, error)) {
        if (depth) depth->assign(size_t(std::max(0, w)) * size_t(std::max(0, h)), 0.0f);
        return Image(std::max(1, w), std::max(1, h));
    }
    Image img = renderModel(scene, cam, w, h, depth);
    // Linear light into Rec.709 (as the other generators draw), straight then premultiplied again.
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < img.width; ++x) {
                float* q = img.at(x, y);
                if (q[3] <= 0) continue;
                for (int k = 0; k < 3; ++k) q[k] = float(fromLinear(Transfer::Bt1886, std::max(0.0, double(q[k] / q[3])))) * q[3];
            }
    });
    return img;
}

}  // namespace montage
