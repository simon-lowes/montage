#include "ModelClip.h"

#include <QCryptographicHash>
#include <algorithm>
#include <cmath>
#include <list>
#include <mutex>

#include "media/CameraSolve.h"
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

}  // namespace

int modelCameraTrack(const Sequence& seq, size_t track, const Clip& c, FrameTime t) {
    const int v = int(std::lround(c.generator.p("camera_track", t - c.start, 0)));
    if (v > 0) return v - 1 < int(seq.videoTracks.size()) && v - 1 != int(track) ? v - 1 : -1;
    for (int k = int(track) - 1; k >= 0; --k)
        if (const Clip* f = clipOn(seq.videoTracks[size_t(k)], t); f && parsedSolve(*f)) return k;
    return -1;
}

int modelLightTrack(const Sequence& seq, size_t track, const Clip& c, FrameTime t) {
    const int v = int(std::lround(c.generator.p("env_track", t - c.start, 0)));
    return v > 0 && v - 1 < int(seq.videoTracks.size()) && v - 1 != int(track) ? v - 1 : -1;
}

bool modelClipScene(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, int w, int h, double scale,
                    const std::function<Image(int track)>& trackLayer, ModelScene& scene, ModelCamera& cam,
                    std::shared_ptr<const Model>& model, std::string* error) {
    const Effect& g = c.generator;
    const FrameTime lt = t - c.start;
    auto P = [&](const char* name, double def) { return g.p(name, lt, def); };
    const std::string file = g.s("file");
    if (file.empty()) {
        if (error) *error = "Choose a model file (glTF, GLB or OBJ)";
        return false;
    }
    model = cachedModel(file, error);
    if (!model) return false;
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
    PosedTriangles posed = posedTriangles(*model, poseModel(*model, animation, sec));

    // Into the world: centred on its rest bounds (standing on their bottom when asked), sized, turned and moved.
    const bool solved = std::lround(P("camera", 0)) == 1;
    const bool stand = solved && P("stand", 1) > 0.5;
    const double modelHeight = std::max(1e-9, hi[1] - lo[1]);
    const double k = solved ? P("scale", 100) / 100 : P("size", 400) / std::max({modelHeight, hi[0] - lo[0], hi[2] - lo[2]});
    const Mat4 centre = trsMatrix({-k * (lo[0] + hi[0]) / 2, -k * (stand ? lo[1] : (lo[1] + hi[1]) / 2), -k * (lo[2] + hi[2]) / 2}, {0, 0, 0, 1}, {k, k, k});
    const Mat4 place = trsMatrix({P("pos_x", 0), P("pos_y", 0), P("pos_z", 0)}, {0, 0, 0, 1}, {1, 1, 1}) *
                       turned(P("rot_x", 0), P("rot_y", 0), P("rot_z", 0)) * centre;
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

    // The camera.
    const double SW = seq.width, SH = seq.height;
    bool onGround = true;
    if (solved) {
        const int ct = ti >= 0 ? modelCameraTrack(seq, size_t(ti), c, t) : -1;
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
    } else {
        // In front of the frame: the plane z = 0 at one sequence pixel a unit, y up, the camera on +z looking back.
        const double f = (SW / 2) / std::tan(std::clamp(P("fov", 35), 1.0, 170.0) * kDeg / 2);
        cam.R = {1, 0, 0, 0, -1, 0, 0, 0, -1};
        cam.C = {0, 0, f};
        cam.screen = {f * scale, 0, SW / 2 * scale, 0, f * scale, SH / 2 * scale};
        cam.nearZ = 1;
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
    if (P("light_strength", 100) > 0) scene.lights.push_back(light);
    const float ambient = float(P("ambient", 30) / 100);
    scene.ambient = {ambient, ambient, ambient};
    if (const int lt2 = ti >= 0 ? modelLightTrack(seq, size_t(ti), c, t) : -1; lt2 >= 0 && trackLayer) {
        scene.environment = environmentFrom(trackLayer(lt2), seq);
        scene.environmentRotation = P("env_rotation", 0) * kDeg;
        scene.environmentIntensity = float(P("env_strength", 100) / 100);
    }
    scene.ground = onGround && P("ground", 1) > 0.5;
    scene.groundHeight = groundY;
    scene.groundStrength = float(P("ground_strength", 80) / 100);
    scene.opacity = float(P("opacity", 100) / 100);
    return true;
}

Image renderModelClip(const Project& p, const Sequence& seq, const Clip& c, FrameTime t, int w, int h, double scale,
                      const std::function<Image(int track)>& trackLayer, std::vector<float>* depth, std::string* error) {
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
