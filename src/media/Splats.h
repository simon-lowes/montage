// Montage — Gaussian splats: captured 3D scenes kept as clouds of oriented 3D Gaussians (Kerbl et al., "3D Gaussian
// Splatting for Real-Time Radiance Field Rendering", SIGGRAPH 2023), each with a position, three scales and a
// rotation, an opacity, and a colour that changes with the direction it is seen from (spherical harmonics up to degree
// 3). Read from PLY files in the layout the training tools write (binary, one vertex a splat: x y z, f_dc_0..2,
// f_rest_*, opacity as a logit, scale_0..2 as logarithms, rot_0..3 as w x y z) and from SPZ files (versions 1 to 4,
// gzip or zstd; read after Niantic's spz library, see THIRD_PARTY_NOTICES.md), and written as either. A cloud is kept
// y up and z back, as SPZ files and Montage's 3D world are; PLY files hold y down and z ahead, as the cameras that
// trained them, and are turned 180° about x when read and written. A numbered run of files (scene_0001.ply to
// scene_0240.ply) plays as an animated splat, a file a frame.
#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace montage {

struct SplatCloud {
    std::string path;
    size_t count = 0;
    int shDegree = 0;                // of the colour's spherical harmonics, 0..3
    std::vector<float> positions;    // 3 a splat
    std::vector<float> scales;       // 3 a splat: its standard deviations along its own axes
    std::vector<float> rotations;    // 4 a splat: a unit quaternion x, y, z, w
    std::vector<float> opacities;    // 0..1
    std::vector<float> colors;       // 3 a splat: the harmonics' constant term (the colour seen from anywhere)
    std::vector<float> sh;           // shCoefficients() x 3 a splat: degree 1 up, each coefficient's R, G and B together
    bool antialiased = false;        // trained with the screen-space filter's opacity made up for (an SPZ flag)
    // Where the middle 90% of the centres lie on each axis, widened by three times the median splat's largest scale.
    std::array<float, 3> lo{0, 0, 0}, hi{0, 0, 0};
    std::vector<std::string> notes;  // what the file holds that is not used
    int shCoefficients() const { return (shDegree + 1) * (shDegree + 1) - 1; }
};

// The constant term of the harmonics times this, plus 0.5, is the colour.
constexpr double kSplatSh0 = 0.28209479177387814;

// A file taken as Gaussian splats: named .spz or .ply (a PLY of another kind is refused when read).
bool isSplatFile(const std::string& path);
bool loadSplats(const std::string& path, SplatCloud& out, std::string* error = nullptr);
// As PLY (binary, little-endian) or SPZ (by the name's suffix; `spzVersion` 2 to 4, 4 only in builds that can write
// zstd, otherwise 3).
bool saveSplats(const SplatCloud& cloud, const std::string& path, std::string* error = nullptr, int spzVersion = 4);
// Kept while the file is the same (a few at a time); a file that would not load is kept with its error.
std::shared_ptr<const SplatCloud> cachedSplats(const std::string& path, std::string* error = nullptr);

// Splat i's colour (display-referred, as the photographs it was trained on) seen along `dir` (a unit vector in the
// cloud's space, from the eye towards the splat).
std::array<float, 3> splatColor(const SplatCloud& cloud, size_t i, const std::array<double, 3>& dir);

// The file of a numbered run that `file` belongs to that is showing `seconds` in, at `fps` files a second, from the
// run's first (looping, or holding the last); `file` itself when it is not in a run. `frames`: the run's length (1).
std::string splatFrameFile(const std::string& file, double seconds, double fps, bool loop, int* frames = nullptr,
                           std::string* first = nullptr);

}  // namespace montage
