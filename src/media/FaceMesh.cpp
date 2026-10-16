#include "FaceMesh.h"

#include <QString>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

namespace montage {

const ModelPack& faceMeshModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "faces";  // (beside YuNet, which finds the faces it fits)
        p.title = "face mesh model";
        p.directoryEnv = "MONTAGE_FACE_MODEL";
        p.urlEnv = "MONTAGE_FACE_MODEL_URL";
        // (MediaPipe's Face Mesh V2, from its Face Landmarker bundle, converted to ONNX with its weights unchanged)
        p.files = {
            {"face_landmarks.onnx",
             "https://huggingface.co/naklitechie/face-landmarks-onnx/resolve/575c33816c840c5b156398adb485e4f8d138adc2/face_landmarks.onnx",
             "f38c3321ceffbc9e95103480ad38cc3f52e7e1bde2bcee7cd9355d0b9138ac0c", 4920995},
        };
        return p;
    }();
    return pack;
}

void faceMeshPose(FaceMesh& m) {
    if (!m.valid()) return;
    // Across the eyes (outer corners) and down the face (forehead to chin); the face looks along their cross product.
    double r[3] = {m.x(263) - m.x(33), m.y(263) - m.y(33), m.z(263) - m.z(33)};
    double d[3] = {m.x(152) - m.x(10), m.y(152) - m.y(10), m.z(152) - m.z(10)};
    auto unit = [](double* v) {
        const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-9)
            for (int k = 0; k < 3; ++k) v[k] /= l;
    };
    unit(r), unit(d);
    // (x right, y down, z away: r x d points away from the camera for a face looking at it)
    double f[3] = {-(r[1] * d[2] - r[2] * d[1]), -(r[2] * d[0] - r[0] * d[2]), -(r[0] * d[1] - r[1] * d[0])};
    unit(f);
    m.yaw = std::atan2(f[0], -f[2]) * 180 / M_PI;
    m.pitch = std::atan2(-f[1], -f[2]) * 180 / M_PI;
    m.roll = std::atan2(r[1], r[0]) * 180 / M_PI;
}

void FaceMesh::iris(bool left, float& x, float& y, float& radius) const {
    const int c = left ? 473 : 468;
    x = this->x(c), y = this->y(c);
    double r = 0;
    for (int k = 1; k <= 4; ++k) r += std::hypot(this->x(c + k) - x, this->y(c + k) - y) / 4;
    radius = float(r);
}

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool faceMeshAvailable() { return false; }
bool fitFaceMesh(const Image&, const FaceBox&, FaceMesh&, std::string* error) {
    if (error) *error = "This build of Montage has no ONNX Runtime";
    return false;
}

#else

bool faceMeshAvailable() { return true; }

namespace {

constexpr int kSize = 256;  // the model's square input

struct Model {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-facemesh"};
    std::unique_ptr<OrtModel> session;
};

std::shared_ptr<Model> loadModel(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<Model>();  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    const ModelPack& pack = faceMeshModel();
    if (!pack.installed()) {
        if (error) *error = "The face mesh model is not downloaded";
        return nullptr;
    }
    try {
        auto model = std::make_shared<Model>();
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        so.SetIntraOpNumThreads(1);  // (a small model, run for one face at a time)
#ifdef _WIN32
        const std::wstring path = QString::fromStdString(pack.path(pack.files[0])).toStdWString();
#else
        const std::string path = pack.path(pack.files[0]);
#endif
        model->session = openModel(model->env, path.c_str(), so, ModelRuns::OnProcessor);
        *cached = model;
        return model;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The face mesh model could not be loaded: ") + e.what();
        return nullptr;
    }
}

// A square of `side` pixels round (cx, cy), turned `angle` radians (the face upright in it), as the model reads it;
// then its points back in the picture.
bool runCrop(Model& model, const Image& img, double cx, double cy, double side, double angle, FaceMesh& out, std::string* error) {
    const double c = std::cos(angle), s = std::sin(angle), k = side / kSize;
    std::vector<float> input(size_t(kSize) * kSize * 3);  // (rows of RGB pixels)
    for (int v = 0; v < kSize; ++v)
        for (int u = 0; u < kSize; ++u) {
            const double ox = (u + 0.5 - kSize / 2.0) * k, oy = (v + 0.5 - kSize / 2.0) * k;
            const double x = std::clamp(cx + c * ox - s * oy - 0.5, 0.0, double(img.width - 1));
            const double y = std::clamp(cy + s * ox + c * oy - 0.5, 0.0, double(img.height - 1));
            const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
            const float fx = float(x - x0), fy = float(y - y0);
            float p[4];
            for (int ch = 0; ch < 4; ++ch)
                p[ch] = (img.at(x0, y0)[ch] * (1 - fx) + img.at(x1, y0)[ch] * fx) * (1 - fy) +
                        (img.at(x0, y1)[ch] * (1 - fx) + img.at(x1, y1)[ch] * fx) * fy;
            const float a = p[3] > 1e-6f ? p[3] : 1.0f;
            for (int ch = 0; ch < 3; ++ch) input[(size_t(v) * kSize + size_t(u)) * 3 + size_t(ch)] = std::clamp(p[ch] / a, 0.0f, 1.0f);
        }
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const char* inNames[] = {"input_12"};
        const char* outNames[] = {"Identity", "Identity_1"};
        const int64_t shape[4] = {1, kSize, kSize, 3};
        Ort::Value tensor = Ort::Value::CreateTensor<float>(cpu, input.data(), input.size(), shape, 4);
        auto result = model.session->Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1, outNames, 2);
        if (result[0].GetTensorTypeAndShapeInfo().GetElementCount() < size_t(kFaceMeshAllPoints) * 3) {
            if (error) *error = "The face mesh model gave too few points";
            return false;
        }
        out.score = 1.0f / (1.0f + std::exp(-result[1].GetTensorData<float>()[0]));  // (a logit)
        const float* l = result[0].GetTensorData<float>();
        out.points.resize(size_t(kFaceMeshAllPoints) * 3);
        for (int i = 0; i < kFaceMeshAllPoints; ++i) {
            // (pixels of the square, z on the same scale as x)
            const double ox = (l[i * 3] - kSize / 2.0) * k, oy = (l[i * 3 + 1] - kSize / 2.0) * k;
            out.points[size_t(i) * 3] = float(cx + c * ox - s * oy);
            out.points[size_t(i) * 3 + 1] = float(cy + s * ox + c * oy);
            out.points[size_t(i) * 3 + 2] = float(l[i * 3 + 2] * k);
        }
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The face mesh failed: ") + e.what();
        return false;
    }
    return true;
}

}  // namespace

bool fitFaceMesh(const Image& img, const FaceBox& face, FaceMesh& out, std::string* error) {
    out = {};
    if (img.empty() || !(face.w > 0) || !(face.h > 0)) {
        if (error) *error = "No face";
        return false;
    }
    auto model = loadModel(error);
    if (!model) return false;
    // First round the face's box, upright by its eyes; then again round the points found, as MediaPipe tracks a face.
    const double W = img.width, H = img.height;
    double cx = (face.x + face.w / 2) * W, cy = (face.y + face.h / 2) * H;
    double side = 1.5 * std::max(face.w * W, face.h * H);
    // (landmarks: the right eye, as the picture shows it on the left, then the left)
    double angle = std::atan2((face.landmarks[3] - face.landmarks[1]) * H, (face.landmarks[2] - face.landmarks[0]) * W);
    for (int round = 0; round < 2; ++round) {
        if (!runCrop(*model, img, cx, cy, side, angle, out, error)) return false;
        if (out.score < 0.5f) {
            if (error) *error = "No face there";
            return false;
        }
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (int i = 0; i < kFaceMeshPoints; ++i) {
            x0 = std::min(x0, out.x(i)), x1 = std::max(x1, out.x(i));
            y0 = std::min(y0, out.y(i)), y1 = std::max(y1, out.y(i));
        }
        angle = std::atan2(out.y(263) - out.y(33), out.x(263) - out.x(33));
        cx = (x0 + x1) / 2.0, cy = (y0 + y1) / 2.0;
        side = 1.5 * std::max(x1 - x0, y1 - y0);
    }
    faceMeshPose(out);
    return true;
}

#endif

std::shared_ptr<const std::vector<FaceMesh>> cachedFaceMeshes(const Image& img, const std::vector<FaceBox>& faces) {
    if (img.empty()) return nullptr;
    uint64_t key = 1469598103934665603ULL;
    auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ULL; };
    mix(uint64_t(img.width)), mix(uint64_t(img.height));
    const size_t step = std::max<size_t>(1, img.px.size() / 8192);
    for (size_t i = 0; i < img.px.size(); i += step) {
        uint32_t bits;
        std::memcpy(&bits, &img.px[i], 4);
        mix(bits);
    }
    for (const FaceBox& f : faces)
        for (float v : {f.x, f.y, f.w, f.h}) {
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            mix(bits);
        }
    static std::mutex m;
    static std::deque<std::pair<uint64_t, std::shared_ptr<const std::vector<FaceMesh>>>> cache;
    {
        std::lock_guard lock(m);
        for (const auto& [k, v] : cache)
            if (k == key) return v;
    }
    auto meshes = std::make_shared<std::vector<FaceMesh>>();
    for (const FaceBox& f : faces) {
        FaceMesh mesh;
        if (fitFaceMesh(img, f, mesh)) meshes->push_back(std::move(mesh));
    }
    std::lock_guard lock(m);
    cache.emplace_front(key, meshes);
    if (cache.size() > 8) cache.pop_back();
    return meshes;
}

}  // namespace montage
