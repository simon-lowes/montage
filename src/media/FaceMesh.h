// Montage — the face mesh: 468 points over a face and 5 on each iris (MediaPipe's Face Mesh V2, Apache-2.0, converted
// to ONNX, 4.9 MB, downloaded on first use), fitted to each face YuNet finds (media/Faces.h). Face Shape and Look at
// Lens (render/FaceShape.h) move its points. The points are numbered as MediaPipe's canonical mesh (1 the nose tip, 33
// and 263 the eyes' outer corners, 152 the chin; 468 the middle of the iris of the eye the picture shows on the left,
// 473 the other's, each followed by 4 points round it).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Faces.h"
#include "Image.h"
#include "ModelFiles.h"

namespace montage {

// In the face model's folder ($MONTAGE_FACE_MODEL), beside YuNet.
const ModelPack& faceMeshModel();
bool faceMeshAvailable();  // built with ONNX Runtime

constexpr int kFaceMeshPoints = 468;     // the face's
constexpr int kFaceMeshAllPoints = 478;  // with the irises'

struct FaceMesh {
    std::vector<float> points;  // kFaceMeshAllPoints x (x, y, z): pixels of the picture; z the same scale, nearer < 0
    float score = 0;  // 0..1, how much it looks like a face
    double yaw = 0, pitch = 0, roll = 0;  // degrees: turned to the picture's right, up, and the eyes' tilt (clockwise)
    float x(int i) const { return points[size_t(i) * 3]; }
    float y(int i) const { return points[size_t(i) * 3 + 1]; }
    float z(int i) const { return points[size_t(i) * 3 + 2]; }
    bool valid() const { return points.size() >= size_t(kFaceMeshAllPoints) * 3; }
    // An iris (`left`: the eye the picture shows on the right): its middle and radius, pixels.
    void iris(bool left, float& x, float& y, float& radius) const;
};

// The face's head pose (yaw, pitch, roll) from its points.
void faceMeshPose(FaceMesh& m);

// The mesh of the face `face` (as cachedFaces gives it) in `img` (premultiplied); false without the model, or when
// what is there does not look like a face.
bool fitFaceMesh(const Image& img, const FaceBox& face, FaceMesh& out, std::string* error = nullptr);

// Each face's mesh (those that fit), the last few pictures' remembered by content.
std::shared_ptr<const std::vector<FaceMesh>> cachedFaceMeshes(const Image& img, const std::vector<FaceBox>& faces);

}  // namespace montage
