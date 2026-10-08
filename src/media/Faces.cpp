#include "Faces.h"

#include <QString>
#include <algorithm>
#include <cmath>
#include <mutex>

#include "Decoder.h"
#include "core/FaceIndex.h"

#ifdef MONTAGE_WITH_ONNXRUNTIME
#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#else
#include <onnxruntime/onnxruntime_cxx_api.h>
#endif
#include "OrtSupport.h"
#endif

namespace montage {

const ModelPack& faceModel() {
    static const ModelPack pack = [] {
        ModelPack p;
        p.id = "faces";
        p.title = "face model";
        p.directoryEnv = "MONTAGE_FACE_MODEL";
        p.urlEnv = "MONTAGE_FACE_MODEL_URL";
        p.files = {
            {"face_detection_yunet_2023mar.onnx",
             "https://huggingface.co/opencv/face_detection_yunet/resolve/3cc26e7f1014a5ee5d74a42acee58bafc9d0a310/"
             "face_detection_yunet_2023mar.onnx",
             "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4", 232589},
            {"face_recognition_sface_2021dec.onnx",
             "https://huggingface.co/opencv/face_recognition_sface/resolve/3d7082438a6e4551e840c9b2bb60b71e8da4b524/"
             "face_recognition_sface_2021dec.onnx",
             "0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79", 38696353},
        };
        return p;
    }();
    return pack;
}

namespace {

constexpr int kDetect = 640;  // YuNet's fixed input
constexpr int kAlign = 112;   // SFace's

// A pixel of a straight-alpha RGBA16 frame, bilinear, 0..255, at pixel-centre coordinates.
void sample(const Frame16& f, double x, double y, float rgb[3]) {
    x = std::clamp(x, 0.0, double(f.width - 1));
    y = std::clamp(y, 0.0, double(f.height - 1));
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, f.width - 1), y1 = std::min(y0 + 1, f.height - 1);
    const double fx = x - x0, fy = y - y0;
    auto at = [&](int xx, int yy, int c) { return double(f.px[(size_t(yy) * size_t(f.width) + size_t(xx)) * 4 + size_t(c)]); };
    for (int c = 0; c < 3; ++c) {
        const double top = at(x0, y0, c) + (at(x1, y0, c) - at(x0, y0, c)) * fx;
        const double bottom = at(x0, y1, c) + (at(x1, y1, c) - at(x0, y1, c)) * fx;
        rgb[c] = float((top + (bottom - top) * fy) / 257.0);
    }
}

}  // namespace

std::vector<float> alignFace(const Frame16& frame, const DetectedFace& face) {
    // The standard landmark positions SFace was trained with (OpenCV's FaceRecognizerSF).
    static const float dst[10] = {38.2946f, 51.6963f, 73.5318f, 51.5014f, 56.0252f, 71.7366f, 41.5493f, 92.3655f, 70.7299f, 92.2041f};
    // Least-squares similarity (rotation, scale, shift) from the landmarks to the standard ones.
    double mx = 0, my = 0, nx = 0, ny = 0;
    for (int i = 0; i < 5; ++i) {
        mx += face.landmarks[2 * i], my += face.landmarks[2 * i + 1];
        nx += dst[2 * i], ny += dst[2 * i + 1];
    }
    mx /= 5, my /= 5, nx /= 5, ny /= 5;
    double num1 = 0, num2 = 0, den = 0;
    for (int i = 0; i < 5; ++i) {
        const double ax = face.landmarks[2 * i] - mx, ay = face.landmarks[2 * i + 1] - my;
        const double bx = dst[2 * i] - nx, by = dst[2 * i + 1] - ny;
        num1 += ax * bx + ay * by;
        num2 += ax * by - ay * bx;
        den += ax * ax + ay * ay;
    }
    if (den <= 0) return {};
    const double a = num1 / den, b = num2 / den;  // q = [a -b; b a] p + t
    const double tx = nx - (a * mx - b * my), ty = ny - (b * mx + a * my);
    const double s2 = a * a + b * b;
    std::vector<float> out(size_t(3) * kAlign * kAlign);
    for (int v = 0; v < kAlign; ++v)
        for (int u = 0; u < kAlign; ++u) {
            // Back to the frame: p = [a b; -b a] (q - t) / (a² + b²).
            const double qx = u - tx, qy = v - ty;
            const double px = (a * qx + b * qy) / s2, py = (-b * qx + a * qy) / s2;
            float rgb[3];
            sample(frame, px, py, rgb);
            for (int c = 0; c < 3; ++c) out[size_t(c) * kAlign * kAlign + size_t(v) * kAlign + size_t(u)] = rgb[c];
        }
    return out;
}

#ifndef MONTAGE_WITH_ONNXRUNTIME

bool faceSearchAvailable() { return false; }
struct FaceModel::Impl {};
FaceModel::FaceModel() = default;
FaceModel::~FaceModel() = default;
std::shared_ptr<FaceModel> FaceModel::load(std::string* error) {
    if (error) *error = "This build of Montage cannot find faces (it was built without ONNX Runtime)";
    return nullptr;
}
std::vector<DetectedFace> FaceModel::detect(const Frame16&, float) const { return {}; }
std::vector<float> FaceModel::embed(const Frame16&, const DetectedFace&) const { return {}; }

#else

bool faceSearchAvailable() { return true; }

struct FaceModel::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "montage-faces"};
    std::unique_ptr<Ort::Session> detector, recogniser;
};

FaceModel::FaceModel() : d_(std::make_unique<Impl>()) {}
FaceModel::~FaceModel() = default;

std::shared_ptr<FaceModel> FaceModel::load(std::string* error) {
    static std::mutex m;
    static auto* cached = new std::shared_ptr<FaceModel>();  // kept for the session, never torn down
    std::lock_guard lock(m);
    if (*cached) return *cached;
    if (!ortUsable(error)) return nullptr;
    const ModelPack& pack = faceModel();
    if (!pack.installed()) {
        if (error) *error = "The face model is not downloaded";
        return nullptr;
    }
    try {
        std::shared_ptr<FaceModel> model(new FaceModel());
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(2);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        auto open = [&](const ModelFile& f) {
#ifdef _WIN32
            const std::wstring path = QString::fromStdString(pack.path(f)).toStdWString();
#else
            const std::string path = pack.path(f);
#endif
            return std::make_unique<Ort::Session>(model->d_->env, path.c_str(), so);
        };
        model->d_->detector = open(pack.files[0]);
        model->d_->recogniser = open(pack.files[1]);
        *cached = model;
        return model;
    } catch (const Ort::Exception& e) {
        if (error) *error = std::string("The face model could not be loaded: ") + e.what();
        return nullptr;
    }
}

std::vector<DetectedFace> FaceModel::detect(const Frame16& frame, float threshold) const {
    if (frame.width < 8 || frame.height < 8) return {};
    // The frame fitted into 640 x 640 at the top left (black beyond), as BGR 0..255.
    const double k = double(kDetect) / std::max(frame.width, frame.height);
    const int nw = std::max(1, int(std::lround(frame.width * k))), nh = std::max(1, int(std::lround(frame.height * k)));
    std::vector<float> input(size_t(3) * kDetect * kDetect, 0.0f);
    const size_t plane = size_t(kDetect) * kDetect;
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            float rgb[3];
            sample(frame, (x + 0.5) / k - 0.5, (y + 0.5) / k - 0.5, rgb);
            const size_t i = size_t(y) * kDetect + size_t(x);
            input[i] = rgb[2];
            input[plane + i] = rgb[1];
            input[2 * plane + i] = rgb[0];
        }
    std::vector<DetectedFace> faces;
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t shape[4] = {1, 3, kDetect, kDetect};
        Ort::Value in = Ort::Value::CreateTensor<float>(cpu, input.data(), input.size(), shape, 4);
        const char* inNames[] = {"input"};
        const char* outNames[] = {"cls_8", "cls_16", "cls_32", "obj_8", "obj_16", "obj_32",
                                  "bbox_8", "bbox_16", "bbox_32", "kps_8", "kps_16", "kps_32"};
        auto out = d_->detector->Run(Ort::RunOptions{nullptr}, inNames, &in, 1, outNames, 12);
        // Each stride's grid: a score, a box around the cell and five landmarks.
        const int strides[3] = {8, 16, 32};
        for (int si = 0; si < 3; ++si) {
            const int s = strides[si], cols = kDetect / s, cells = cols * cols;
            const float* cls = out[size_t(si)].GetTensorData<float>();
            const float* obj = out[size_t(3 + si)].GetTensorData<float>();
            const float* box = out[size_t(6 + si)].GetTensorData<float>();
            const float* kps = out[size_t(9 + si)].GetTensorData<float>();
            for (int i = 0; i < cells; ++i) {
                const float score = std::sqrt(std::clamp(cls[i], 0.0f, 1.0f) * std::clamp(obj[i], 0.0f, 1.0f));
                if (score < threshold) continue;
                const int r = i / cols, c = i % cols;
                const float cx = (float(c) + box[i * 4]) * s, cy = (float(r) + box[i * 4 + 1]) * s;
                const float bw = std::exp(box[i * 4 + 2]) * s, bh = std::exp(box[i * 4 + 3]) * s;
                DetectedFace f;
                f.score = score;
                f.x = float((cx - bw / 2) / k);
                f.y = float((cy - bh / 2) / k);
                f.w = float(bw / k);
                f.h = float(bh / k);
                for (int n = 0; n < 5; ++n) {
                    f.landmarks[2 * n] = float((kps[i * 10 + 2 * n] + float(c)) * s / k);
                    f.landmarks[2 * n + 1] = float((kps[i * 10 + 2 * n + 1] + float(r)) * s / k);
                }
                faces.push_back(f);
            }
        }
    } catch (const Ort::Exception&) {
        return {};
    }
    // Overlapping boxes: the best one stays.
    std::sort(faces.begin(), faces.end(), [](const DetectedFace& a, const DetectedFace& b) { return a.score > b.score; });
    std::vector<DetectedFace> kept;
    for (const DetectedFace& f : faces) {
        bool overlaps = false;
        for (const DetectedFace& g : kept) {
            const float ix = std::max(0.0f, std::min(f.x + f.w, g.x + g.w) - std::max(f.x, g.x));
            const float iy = std::max(0.0f, std::min(f.y + f.h, g.y + g.h) - std::max(f.y, g.y));
            const float inter = ix * iy;
            if (inter / (f.w * f.h + g.w * g.h - inter) > 0.3f) overlaps = true;
        }
        if (!overlaps) kept.push_back(f);
    }
    return kept;
}

std::vector<float> FaceModel::embed(const Frame16& frame, const DetectedFace& face) const {
    std::vector<float> input = alignFace(frame, face);
    if (input.empty()) return {};
    try {
        Ort::MemoryInfo cpu = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        const int64_t shape[4] = {1, 3, kAlign, kAlign};
        Ort::Value in = Ort::Value::CreateTensor<float>(cpu, input.data(), input.size(), shape, 4);
        const char* inNames[] = {"data"};
        const char* outNames[] = {"fc1"};
        auto out = d_->recogniser->Run(Ort::RunOptions{nullptr}, inNames, &in, 1, outNames, 1);
        const float* e = out[0].GetTensorData<float>();
        std::vector<float> v(e, e + 128);
        double len = 0;
        for (float x : v) len += double(x) * x;
        if (len <= 0) return {};
        for (float& x : v) x = float(x / std::sqrt(len));
        return v;
    } catch (const Ort::Exception&) {
        return {};
    }
}

#endif

bool indexFaces(const std::string& path, double duration, FaceIndex& out, double step, int perFrame, int minSize,
                const std::function<void(double)>& progress, const std::atomic<bool>* cancel, std::string* error) {
    std::shared_ptr<FaceModel> model = FaceModel::load(error);
    if (!model) return false;
    VideoDecoder dec;
    if (!dec.open(path, error)) return false;
    if (duration <= 0) duration = dec.duration();
    const bool still = dec.isStill() || duration <= 0;
    if (step <= 0) step = duration < 20 ? 1.0 : 2.0;
    // Decoded with the long side at most 1280: detection works at 640, the faces are aligned from this.
    const int dw = std::max(1, dec.displayWidth()), dh = std::max(1, dec.displayHeight());
    const double k = std::min(1.0, 1280.0 / std::max(dw, dh));
    const int w = std::max(1, int(std::lround(dw * k))), h = std::max(1, int(std::lround(dh * k)));
    FaceIndex index;
    index.model = faceModel().id;
    index.step = still ? 0 : step;
    const int count = still ? 1 : std::max(1, int(std::floor(duration / step)));
    for (int n = 0; n < count; ++n) {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return false;
        }
        const double t = still ? 0.0 : (n + 0.5) * step;
        Frame16Ptr f = dec.frameAt(t, w, h, true);
        if (!f) continue;
        std::vector<DetectedFace> faces = model->detect(*f);
        int added = 0;
        for (const DetectedFace& face : faces) {
            if (face.h < minSize || added >= perFrame) continue;
            const std::vector<float> e = model->embed(*f, face);
            if (e.empty()) continue;
            index.add(t, face.x / float(f->width), face.y / float(f->height), face.w / float(f->width), face.h / float(f->height),
                      face.score, e);
            ++added;
        }
        if (progress) progress(double(n + 1) / count);
    }
    out = std::move(index);
    return true;
}

}  // namespace montage
