// Montage — faces in footage, for finding people: YuNet (Shiqi Yu, MIT)
// finds faces and their five landmarks, and SFace (Yaoyao Zhong,
// Apache-2.0) turns each face, aligned to a standard pose, into 128 numbers
// that are close for the same person. Both run on ONNX Runtime; the models
// (39 MB) are downloaded on first use.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Image.h"
#include "ModelFiles.h"

namespace montage {

struct FaceIndex;

// $MONTAGE_FACE_MODEL names another folder, $MONTAGE_FACE_MODEL_URL a mirror.
const ModelPack& faceModel();
bool faceSearchAvailable();  // built with ONNX Runtime

struct DetectedFace {
    float x = 0, y = 0, w = 0, h = 0;  // pixels of the frame
    float score = 0;
    float landmarks[10] = {};  // right eye, left eye, nose tip, right and left mouth corners (x, y)
};

class FaceModel {
public:
    static std::shared_ptr<FaceModel> load(std::string* error = nullptr);
    ~FaceModel();

    // Faces in a frame (straight-alpha RGBA16, any size), best first.
    std::vector<DetectedFace> detect(const Frame16& frame, float threshold = 0.8f) const;
    // A face's identity: unit length, 128 values; empty on failure. Faces of one person score
    // above about 0.36 against each other (cosine), different people below.
    std::vector<float> embed(const Frame16& frame, const DetectedFace& face) const;

    struct Impl;

private:
    FaceModel();
    std::unique_ptr<Impl> d_;
};

// The face aligned for SFace: 112 x 112 RGB (0..255, row by row), its landmarks
// moved onto the standard ones by the closest rotation, scale and shift.
std::vector<float> alignFace(const Frame16& frame, const DetectedFace& face);

// The faces in a picture (premultiplied RGBA), as fractions of it, best first; the last few
// pictures' are remembered by content. Null without the model.
struct FaceBox {
    float x = 0, y = 0, w = 0, h = 0;
    float score = 0;
    float landmarks[10] = {};  // as DetectedFace's, in fractions
};
std::shared_ptr<const std::vector<FaceBox>> cachedFaces(const Image& img);

// Faces every `step` seconds of a video (0: every 2 s, or every second for
// clips under 20 s), or in a still; at most `perFrame` per frame, at least
// `minSize` pixels tall.
bool indexFaces(const std::string& path, double duration, FaceIndex& out, double step = 0, int perFrame = 8, int minSize = 32,
                const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr,
                std::string* error = nullptr);

}  // namespace montage
