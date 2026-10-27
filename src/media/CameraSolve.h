// Montage — 3D camera tracking (Resolve Fusion's Camera Tracker, After Effects' 3D Camera Tracker): the camera that
// filmed a shot, solved from the footage alone. Features are followed through the frames (Shi–Tomasi corners and
// pyramidal Lucas–Kanade, media/Tracking.h, topped up as they leave); two keyframes far enough apart give the first
// cameras (the essential matrix from the eight-point algorithm under RANSAC) and points (triangulated); each further
// keyframe's camera is fitted to the points it sees and new points triangulated; bundle adjustment (Levenberg–Marquardt
// with the Schur complement) refines every keyframe camera, every point and the focal length together, starting from
// several fields of view and keeping the one that fits best; then every frame's camera is fitted to the points. The
// scene's scale and placement are arbitrary until orientScene stands it on its ground.
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "media/Tracking.h"

namespace montage {

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

// A pinhole camera: a world point X is at R (X - C) in the camera's coordinates (x right, y down, z forward) and at
// f x / z + cx, f y / z + cy in the picture (pixels of the solved size).
struct SolvedCamera {
    bool valid = false;
    std::array<double, 9> R{1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major, world to camera
    Vec3 C;                                              // where the camera is
    double error = 0;                                    // its RMS reprojection error, pixels
};

struct SolvedPoint {
    Vec3 X;
    int track = 0;      // the feature track it came from
    double error = 0;   // RMS reprojection error, pixels
    int seen = 0;       // frames it is seen in
    int first = 0, last = -1;  // the frames its track spans
};

// A feature followed through frames: its position (pixels) in each frame from `first`.
struct FeatureTrack {
    int first = 0;
    std::vector<Point2> at;
    int last() const { return first + int(at.size()) - 1; }
};

struct CameraSolve {
    int width = 0, height = 0;  // the picture the solve is in (pixels)
    double focal = 0;           // pixels (the same for every frame: no zooms)
    double fps = 0, start = 0;  // the frames are at media time start + i / fps
    std::vector<SolvedCamera> cameras;  // one per frame
    std::vector<SolvedPoint> points;
    double error = 0;  // RMS reprojection error over every frame, pixels
    int keyframes = 0;
    bool oriented = false;  // orientScene stood it on its ground (y up)
    // The horizontal field of view the focal length gives, degrees.
    double fieldOfView() const;
    // Where X is in the picture of frame i (pixels); false when behind the camera or the frame has no camera.
    bool project(int frame, const Vec3& X, Point2& out) const;
};

struct CameraSolveOptions {
    double focal = 0;        // pixels when known (from the lens); 0 = solved
    double fieldOfView = 0;  // or the horizontal field of view, degrees, when known
    int maxFeatures = 400;
    int minTrackLength = 8;  // frames
};

using SolveProgress = std::function<void(double fraction, const char* stage)>;

// Solves the camera from feature tracks over `frames` frames of a width x height picture. False with `error` when it
// cannot (too few tracks, no parallax: a camera that only turns or a flat picture that only slides).
bool solveCamera(const std::vector<FeatureTrack>& tracks, int frames, int width, int height, const CameraSolveOptions& options,
                 CameraSolve& out, const SolveProgress& progress = {}, const std::atomic<bool>* cancel = nullptr,
                 std::string* error = nullptr);

// Feature tracks through the media at `path` from `from` to `to` (seconds), at most `maxWidth` pixels wide; `width`,
// `height` and `fps` get the analysed picture's size and the media's rate.
std::vector<FeatureTrack> trackFeatures(const std::string& path, double from, double to, int maxWidth, int maxFeatures,
                                        int& width, int& height, double& fps, const SolveProgress& progress = {},
                                        const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

// Both: the camera of the media at `path` between `from` and `to`.
bool solveMediaCamera(const std::string& path, double from, double to, const CameraSolveOptions& options, CameraSolve& out,
                      const SolveProgress& progress = {}, const std::atomic<bool>* cancel = nullptr, std::string* error = nullptr);

// Stands the scene on its ground: the plane most points lie on (RANSAC) among those level enough for a floor (seen by
// a first camera tilted at most 60 degrees; any plane when none is) becomes y = 0 with the cameras above it (y up),
// the origin under the middle of its points, the first camera looking along -z, and the scale such that the first
// camera is `height` above the ground. False (scene unchanged) when no plane holds enough points.
bool orientScene(CameraSolve& s, double height = 1.6);

// The four corners (top left, top right, bottom right, bottom left) of a flat rectangle `width` x `height` (scene units)
// centred on `centre`, lying on the ground (facing up, its top edge away from the first camera) or standing upright
// facing the first camera (`upright`), turned `turn` degrees about its normal.
std::array<Vec3, 4> sceneRectangle(const CameraSolve& s, const Vec3& centre, double width, double height, bool upright, double turn = 0);

// Where the ray through `pixel` of frame `frame` meets the ground of an oriented solve (y = 0, in front of the camera);
// false when it does not (the sky, or the scene is not oriented).
bool groundPoint(const CameraSolve& s, int frame, Point2 pixel, Vec3& out);

// Compact text forms, stored with a clip.
std::string cameraSolveToString(const CameraSolve& s);
bool cameraSolveFromString(const std::string& text, CameraSolve& s);

// The camera for other programs: Nuke's .chan (frame, translate, rotate ZXY in degrees, vertical field of view) and the
// points as an ASCII PLY. A scene orientScene could not stand on a ground is turned so the first camera is y up,
// looking along -z. (Nuke reads the field of view through the camera's vertical aperture: set the aperture to the
// footage's shape.)
bool writeNukeChan(const CameraSolve& s, const std::string& path, int firstFrame = 1, std::string* error = nullptr);
bool writePointsPly(const CameraSolve& s, const std::string& path, std::string* error = nullptr);

// A model placed in the solved scene, for writeSolveUsda: triangles in the scene's space, of one material.
struct SolvedSceneMesh {
    std::string name;
    std::vector<float> positions, normals;  // 3 a vertex (normals may be empty)
    std::vector<uint32_t> indices;          // triangles
    std::array<float, 4> color{0.18f, 0.18f, 0.18f, 1};  // linear base colour and opacity
    float metallic = 0, roughness = 0.5f;
    bool doubleSided = false;
};
// The camera, its points and the models placed in its scene as a USD scene (USDA): a camera moving through time codes
// 0, 1, 2... (one a frame, at the footage's frame rate; a 36 mm wide film back), the points as a point cloud and each
// model as a mesh with a plain material. Turned as writeNukeChan turns a scene with no ground.
bool writeSolveUsda(const CameraSolve& s, const std::string& path, const std::vector<SolvedSceneMesh>& models = {},
                    std::string* error = nullptr);

}  // namespace montage
