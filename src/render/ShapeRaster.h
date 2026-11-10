// Montage — a Shape layer's paints made into coverage, the same way on the CPU and on the GPU (gpufilter.frag's 56):
// each path flattened into lines in the frame's pixels and listed by the 16-pixel tiles it reaches, with what lies to a
// tile's left summed into a winding per row (its backdrop), so each pixel works out the exact area of itself each path
// covers (by its fill rule) from the few lines in its tile; then each paint's colour (solid, or a linear or radial
// gradient with its spread and stops) laid down over what is below it, groups seen through their opacity as a whole.
// Both renderers read one table, so they cannot drift apart.
#pragma once

#include <QTransform>
#include <vector>

#include "ShapeLayers.h"
#include "media/Image.h"

namespace montage {

constexpr int kShapeTile = 16;        // a tile's side, in pixels
constexpr int kShapeGpuGroups = 8;    // groups inside groups the GPU's shader keeps (deeper drawings stay on the CPU)
constexpr int kShapeTableWidth = 4096;

struct ShapeRaster {
    int width = 0, height = 0;
    int tilesX = 0, tilesY = 0;
    int depth = 0;  // how deep groups go
    // RGBA texels, read as one run: each tile's commands (start, end) at the tile's index; the paints (5 texels: kind,
    // spread, stops and where they start; the colour or the map into the gradient's space in two; the gradient's
    // points; its scale or radius) and their stops (2 texels: where, the colour premultiplied); then the commands: a
    // path (type 0, its paint's texel, its lines, even-odd), its backdrop in 4 texels (16 rows) and its lines in the
    // tile's own pixels (x0, y0, x1, y1); a group begun (1); a group ended (2, its opacity).
    std::vector<float> table;
};

// The paints drawn through `to` (the layer into the frame's pixels) at `opacity`, as render/ShapeLayers draws them, on
// a w x h frame.
ShapeRaster shapeRaster(const std::vector<ShapePaint>& paints, const QTransform& to, double opacity, int w, int h);
// What it draws, premultiplied (gpufilter.frag's 56 does the same per pixel).
Image drawShapeRaster(const ShapeRaster& r);

// A Shape layer's contents (render/ShapeLayers' renderShapeContents), and the layer whatever it holds (render/Shapes'
// renderShape: its contents, or its one shape), made so.
ShapeRaster shapeContentsRaster(const Effect& g, FrameTime t, int w, int h, double scale, double fps);
ShapeRaster shapeLayerRaster(const Effect& g, FrameTime t, int w, int h, double scale, double fps = 25);

}  // namespace montage
