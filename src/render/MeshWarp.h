// Montage — Mesh Warp and Grid Warp. Mesh Warp lays a triangle mesh over the picture (a square lattice, each square
// split in two, over the picture's alpha or the whole frame) and bends it by pins as rigidly as it can, in the two linear
// steps of Igarashi, Moscovich and Hughes' as-rigid-as-possible shape manipulation (SIGGRAPH 2005): first each triangle
// kept similar to itself, then each one's rotation taken from that and its size kept. Both steps' matrices are factored
// once for a mesh and its pins (core/LinearSolve.h SparseCholesky) and kept, so moving a pin solves only the right-hand
// sides. Position pins are moved (keyframed); Stiff pins make an area hold its shape (one rotation for all of it, and
// weighted far above the rest); Bend pins turn and scale the area round them; Overlap pins say which part is drawn in
// front where the mesh folds over itself. The picture is resampled triangle by triangle (bilinear, as the transforms
// are) and motion blur comes from the mesh's own motion through the shutter. Grid Warp moves the points of a grid of
// rows and columns, the picture following smoothly (Catmull-Rom between the points) or straight between them.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"

namespace montage {

using MeshPoint = std::array<double, 2>;

struct WarpMesh {
    std::vector<MeshPoint> rest;           // vertices, pixels
    std::vector<std::array<int, 3>> tris;  // vertex indices
};
// A lattice of about `vertices` vertices over the w x h frame: all of it, or (with `alpha`) the squares that hold a pixel
// whose alpha is above 1/255 and those within `grow` pixels of them (in whole squares). The lattice does not depend on the picture, so a moving
// alpha keeps its vertices where they were.
WarpMesh buildWarpMesh(int w, int h, int vertices, const Image* alpha = nullptr, double grow = 0);

enum class PinKind { Position, Stiff, Bend, Overlap };
struct WarpPin {
    PinKind kind = PinKind::Position;
    MeshPoint at{};               // where it sits on the picture at rest (pixels)
    MeshPoint to{};               // Position: where it is moved to
    double radius = 80;           // Stiff, Bend, Overlap: how far it reaches (pixels)
    double amount = 1;            // Stiff: 0..1
    double angle = 0, scale = 1;  // Bend: degrees, factor
    double depth = 0;             // Overlap: in front (+) or behind (-)
};

// Where each of the mesh's vertices goes for the pins. Rigid: as rigid as it can (the effect). Plain: each triangle's
// edges kept as they were without turning, which spreads the pins' moves smoothly (a plain triangle-by-triangle warp,
// for comparison). A part of the mesh with no Position pin stays where it is (turned and scaled about its Bend pins);
// one with a single pin is moved with it.
enum class WarpSolve { Rigid, Plain };
std::vector<MeshPoint> solveWarp(const WarpMesh& m, const std::vector<WarpPin>& pins, WarpSolve how = WarpSolve::Rigid);
// Each triangle's depth: the deepest-in-front Overlap pin that reaches its middle at rest, else 0.
std::vector<double> warpDepths(const WarpMesh& m, const std::vector<WarpPin>& pins);
// `src` warped from the mesh at rest to `moved`, drawn over `out` (the same size) triangle by triangle, the furthest
// back first (by `depth`, then in order). Every pixel is drawn by at most one triangle of a seamless part.
void drawWarp(const Image& src, const WarpMesh& m, const std::vector<MeshPoint>& moved, const std::vector<double>& depth, Image& out);

// The effects (Mesh Warp's pins read from its params at clip frame t; positions as fractions of the picture, sizes in
// sequence pixels times `pixelScale`).
std::vector<WarpPin> meshWarpPins(const Effect& e, double t, int w, int h, double pixelScale);
void meshWarp(const Effect& e, FrameTime t, Image& img, double pixelScale);
void gridWarp(const Effect& e, FrameTime t, Image& img);
// Points on the picture at rest, where Mesh Warp `e` on clip frame t puts them (pixels of a w x h picture, on the mesh
// it was last drawn with, or one over the whole frame), and the point at rest it puts at `shown` (false where nothing of the mesh is there): for
// showing and placing pins on the monitor.
std::vector<MeshPoint> meshWarpMoved(const Effect& e, double t, int w, int h, double pixelScale, const std::vector<MeshPoint>& rest);
bool meshWarpRestOf(const Effect& e, double t, int w, int h, double pixelScale, const MeshPoint& shown, MeshPoint& rest);
// Where a Grid Warp's grid point (r, c) is on clip frame t (fractions of the picture).
MeshPoint gridWarpPointAt(const Effect& e, double t, int r, int c);

// Keys for each Position pin of the Mesh Warp `e` on video clip `c` that keep it on what is under it in the footage,
// tracked from clip frame `at` to both ends of the clip: (to_x, to_y) for each pin's k. `progress` (0..1) may return
// false to stop. False on failure (not a video clip, no Position pins, lost at once, cancelled).
bool trackMeshPins(const Project& p, const Sequence& seq, const Clip& c, const Effect& e, FrameTime at,
                   std::map<int, std::pair<Param, Param>>& keys, const std::function<bool(double)>& progress = {},
                   std::string* error = nullptr);

}  // namespace montage
