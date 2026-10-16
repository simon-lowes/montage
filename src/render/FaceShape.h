// Montage — Face Shape and Look at Lens, on the face mesh (media/FaceMesh.h). Face Shape moves groups of the mesh's
// points (the cheeks, jaw, chin and forehead along the face's outline, the nose's wings and tip, the eyes, the lips)
// by its sliders, in the face's own directions, and warps the picture to follow by moving least squares (an affine
// fit to the points, each weighted by its nearness), the rest of the mesh and a ring round the face holding still.
// Look at Lens takes each open eye's iris from the mesh and draws it again where it would be looking at the lens for
// the head's pose, filling in where it was from what is round it and leaving the lids and skin alone; past a gaze angle
// (a script far from the lens) it fades out.
#pragma once

#include <array>
#include <vector>

#include "media/FaceMesh.h"
#include "media/Image.h"

namespace montage {

struct FaceShapeSettings {
    // Each -1..1 (narrower or smaller below 0, wider or larger above).
    double faceWidth = 0, jaw = 0, chin = 0, forehead = 0;
    double noseWidth = 0, noseLength = 0;
    double eyeSize = 0, eyeDistance = 0;
    double mouthWidth = 0, mouthHeight = 0;
    bool showMesh = false;  // the mesh's points drawn (where they go) instead of warping
    bool any() const;
};

// Where each of the mesh's points goes (x, y pixels, kFaceMeshPoints of them).
std::vector<std::array<float, 2>> faceShapeTargets(const FaceMesh& m, const FaceShapeSettings& s);

// The picture warped so that each point `from` lands on `to` (pixels), within `radius` of (cx, cy) and nothing moved
// outside it (the edge of that circle holds still).
void warpPoints(Image& img, const std::vector<std::array<float, 2>>& from, const std::vector<std::array<float, 2>>& to, double cx,
                double cy, double radius);

// Each face reshaped.
void reshapeFaces(Image& img, const std::vector<FaceMesh>& meshes, const FaceShapeSettings& s);

// The iris at (x, y) of `radius`, inside an eye's lids (`lids`, its outline in pixels, from the outer corner), moved `amount` of the way to (tx, ty): drawn there within the lids (what was under a
// lid taken from its other side), the white filled in where it was.
void moveIris(Image& img, const std::vector<std::array<float, 2>>& lids, float x, float y, float radius, float tx, float ty,
              double amount);

struct LookAtLensSettings {
    double amount = 1;  // 0..1
    double limit = 20;  // degrees of gaze away from the lens it corrects fully; it fades out over the next 10
    bool showIrises = false;  // each iris found (red) and where it goes (green) instead
};
// Where an eye's iris sits looking at the lens, for the head's pose (and the eyeball's radius in pixels); false for an
// eye shut or too small.
bool gazeTarget(const FaceMesh& m, bool left, float& tx, float& ty, double* eyeballRadius = nullptr);
// Each open eye of each face turned to the lens; returns how many were moved.
int lookAtLens(Image& img, const std::vector<FaceMesh>& meshes, const LookAtLensSettings& s);

// The outlines of a mesh's eyes (right as the picture shows it first, then left), 16 points each.
std::vector<std::array<float, 2>> eyeOutline(const FaceMesh& m, bool left);

}  // namespace montage
