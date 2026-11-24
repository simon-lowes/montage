// Montage — Gaussian splats: captured 3D scenes kept as clouds of oriented 3D Gaussians (Kerbl et al., "3D Gaussian
// Splatting for Real-Time Radiance Field Rendering", SIGGRAPH 2023), each with a position, three scales and a
// rotation, an opacity, and a colour that changes with the direction it is seen from (spherical harmonics up to degree
// 3). Read from PLY files in the layout the training tools write (binary, one vertex a splat: x y z, f_dc_0..2,
// f_rest_*, opacity as a logit, scale_0..2 as logarithms, rot_0..3 as w x y z), from SPZ files (versions 1 to 4,
// gzip or zstd; read after Niantic's spz library, see THIRD_PARTY_NOTICES.md) and from glTF files (.gltf, .glb) whose
// point primitives carry Khronos's KHR_gaussian_splatting extension (its ellipse kernel: positions, rotations, linear
// scales and opacities, harmonics to degree 3, in sRGB or linear Rec. 709), and written as any of them. A cloud is kept
// y up and z back, as SPZ files and Montage's 3D world are; PLY files hold y down and z ahead, as the cameras that
// trained them, and are turned 180° about x when read and written. A numbered run of files (scene_0001.ply to
// scene_0240.ply) plays as an animated splat, a file a frame.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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
    bool linearColor = false;        // its colours are linear light (a glTF file that says so), not display values
    // Where the middle 90% of the centres lie on each axis, widened by three times the median splat's largest scale.
    std::array<float, 3> lo{0, 0, 0}, hi{0, 0, 0};
    std::vector<std::string> notes;  // what the file holds that is not used
    int shCoefficients() const { return (shDegree + 1) * (shDegree + 1) - 1; }
};

// The constant term of the harmonics times this, plus 0.5, is the colour.
constexpr double kSplatSh0 = 0.28209479177387814;

// A file taken as Gaussian splats: named .spz or .ply (a PLY of another kind is refused when read), or a .gltf or
// .glb file that names the KHR_gaussian_splatting extension (looked for once while the file is the same).
bool isSplatFile(const std::string& path);
bool loadSplats(const std::string& path, SplatCloud& out, std::string* error = nullptr);
// As PLY (binary, little-endian), SPZ (`spzVersion` 2 to 4, 4 only in builds that can write zstd, otherwise 3) or
// glTF (.glb, or .gltf with its buffer beside it as <name>.bin: one point primitive with the extension, every value a
// float, a COLOR_0 for readers without the extension), by the name's suffix.
bool saveSplats(const SplatCloud& cloud, const std::string& path, std::string* error = nullptr, int spzVersion = 4);
// The splats of a glTF file's primitives that carry the extension, each placed by its node (in media/Model3D.cpp,
// beside the model reader). A node that turns its splats turns their harmonics with them.
bool loadGltfSplats(const std::string& path, SplatCloud& out, std::string* error = nullptr);
// The harmonics of `cloud` (degrees 1 up) turned by the rotation `r` (row-major, cloud to world), so that the colour
// seen along r·d is what was seen along d.
void rotateSplatHarmonics(SplatCloud& cloud, const std::array<double, 9>& r);
// Kept while the file is the same (a few at a time); a file that would not load is kept with its error.
std::shared_ptr<const SplatCloud> cachedSplats(const std::string& path, std::string* error = nullptr);

// Splat i's colour (display-referred, as the photographs it was trained on) seen along `dir` (a unit vector in the
// cloud's space, from the eye towards the splat).
std::array<float, 3> splatColor(const SplatCloud& cloud, size_t i, const std::array<double, 3>& dir);

// Cleaning a capture: which of a cloud's splats are kept. Every test that is on must pass.
struct SplatFilter {
    float minOpacity = 0;         // splats fainter than this are dropped
    float maxSize = 0;            // splats whose largest scale is above this are dropped (0: none)
    // Splats with fewer than `neighbours` other splats' centres within this distance are dropped (0: none).
    float neighbourDistance = 0;
    int neighbours = 3;
    int box = 0, sphere = 0;      // 1: only the splats whose centres are inside are kept, 2: only those outside
    std::array<float, 3> boxLo{0, 0, 0}, boxHi{0, 0, 0};
    std::array<float, 3> sphereCentre{0, 0, 0};
    float sphereRadius = 0;
    // 1: splats whose colour (the constant term's, 0..1) is within `colorRange` of `colorValue` on every channel are
    // dropped, 2: only those are kept.
    int color = 0;
    std::array<float, 3> colorValue{0, 0, 0};
    float colorRange = 0;
    std::string removed;          // splats taken out by hand, as splatRanges writes them
    // Of the splats that pass, the share drawn (1: all), the most opaque and largest first (a draft).
    double share = 1;
    bool any() const;
    std::string key() const;      // equal for filters that keep the same splats of a cloud
};
// How many splats each test dropped (a splat counted for the first test it fails, in the order below).
struct SplatFilterCounts {
    size_t opacity = 0, size = 0, neighbours = 0, box = 0, sphere = 0, color = 0, removed = 0, thinned = 0, kept = 0;
};
// One byte a splat: 1 where it is kept.
std::vector<uint8_t> splatKeep(const SplatCloud& cloud, const SplatFilter& filter, SplatFilterCounts* counts = nullptr);
// The same, kept while the cloud lives and the filter is the same (a few at a time); null when the filter keeps all.
std::shared_ptr<const std::vector<uint8_t>> cachedSplatKeep(const std::shared_ptr<const SplatCloud>& cloud, const SplatFilter& filter,
                                                            SplatFilterCounts* counts = nullptr);
// A cloud of the kept splats only, in their order.
SplatCloud keptSplats(const SplatCloud& cloud, const std::vector<uint8_t>& keep);
// A set of splats (one byte each, non-zero where in the set) as index ranges ("3-7,12,40-41"), and back (indices at
// or past `count` left out; anything unreadable ends the list there).
std::string splatRanges(const std::vector<uint8_t>& set);
std::vector<uint8_t> splatRangeSet(const std::string& ranges, size_t count);

// The file of a numbered run that `file` belongs to that is showing `seconds` in, at `fps` files a second, from
// `file` to the run's last (looping, or holding the last); `file` itself when it is not in a run. `frames`: how many
// files that is (1).
std::string splatFrameFile(const std::string& file, double seconds, double fps, bool loop, int* frames = nullptr);

}  // namespace montage
