// Montage — what a Shape layer holds when it is more than one shape (render/ShapeLayers draws it): groups, each with
// its own transform, holding shapes (rectangles, ellipses, polygons, stars and drawn Bézier paths), paints (fills and
// strokes, solid or gradients) and operators applied to the paths above them (combining, growing, rounding, pinching,
// twisting, zig-zags, wiggles, trims and repeats), in order.
//
// The structure (which items, in which order, in which groups, and a path's open or closed, an imported group's
// matrix and a gradient's stops) is JSON in the generator's "contents" string. Every number an item has is a param of
// the generator named "<item id>.<name>" (a path's points "<id>.p<i>.x" / ".y" / ".ix" / ".iy" / ".ox" / ".oy", as
// drawn masks keep theirs), so each one keys, animates and has expressions like any other, and two keyed shapes of
// a path with the same points morph from one to the other. A param that is not there has the kind's default.
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Effects.h"
#include "MaskPath.h"
#include "Model.h"

namespace montage {

// A kind of item: its type ("group", "rect", "ellipse", "polygon", "star", "path", "fill", "stroke", "merge",
// "offset", "round", "pucker", "twist", "zigzag", "wiggle", "trim", "repeater"), the name it is shown with, and its
// settings.
struct ShapeItemKind {
    std::string type, label;
    std::vector<ParamInfo> params;
};
const std::vector<ShapeItemKind>& shapeItemKinds();
// How deep groups go (those deeper are not drawn).
inline constexpr int kShapeDepth = 32;
const ShapeItemKind* shapeItemKind(const std::string& type);

struct ShapeItem {
    std::string type, id, name;
    bool enabled = true;
    std::vector<ShapeItem> items;  // a group's, first on top
    bool closed = true;            // a path
    // A group's fixed transform before its own (an imported SVG's, as QTransform's m11 m12 m21 m22 dx dy).
    bool hasMatrix = false;
    std::array<double, 6> matrix{1, 0, 0, 1, 0, 0};
    // A gradient's stops (offset, r, g, b, a; empty: its two colours), how it repeats past its ends (0 pad, 1 reflect,
    // 2 repeat), its focal point (radial; none: the centre) and its own transform.
    std::vector<std::array<double, 5>> stops;
    int spread = 0;
    bool hasFocal = false;
    double focalX = 0, focalY = 0;
    bool hasGradientMatrix = false;
    std::array<double, 6> gradientMatrix{1, 0, 0, 1, 0, 0};
};

// The generator's contents (empty: it is the one shape its own settings give).
std::vector<ShapeItem> shapeContents(const Effect& g);
std::string shapeContentsToString(const std::vector<ShapeItem>& items);
void setShapeContents(Effect& g, const std::vector<ShapeItem>& items);

// An item's setting at t (the kind's default when it has none).
double shapeItemParam(const Effect& g, const ShapeItem& item, const std::string& name, FrameTime t);

// An id not yet used in the contents (the type and a number: "rect3").
std::string newShapeItemId(const std::vector<ShapeItem>& items, const std::string& type);
// The item with this id anywhere in the contents, with the list it is in and where (null when there is none).
ShapeItem* findShapeItem(std::vector<ShapeItem>& items, const std::string& id, std::vector<ShapeItem>** list = nullptr, size_t* index = nullptr);
const ShapeItem* findShapeItem(const std::vector<ShapeItem>& items, const std::string& id);
// Removes an item's params (and those of everything in it) from the generator.
void eraseShapeItemParams(Effect& g, const ShapeItem& item);
// Every item in order, depth first, with its depth (for listing).
void eachShapeItem(const std::vector<ShapeItem>& items, const std::function<void(const ShapeItem&, int depth)>& fn);

// A path item's points at t (layer pixels: x right, y down, from the layer's middle) and the same set: keyed at every
// key when the path is animated (its first point's x keyed), else fixed.
std::vector<PathPoint> shapePathPoints(const Effect& g, const std::string& id, FrameTime t);
void setShapePathPoints(Effect& g, const std::string& id, FrameTime t, const std::vector<PathPoint>& pts, bool key = false);
// A drawn (Bézier) mask's path as a path item (its points set on `g`, keyed as the mask's are), the mask on a frameW x
// frameH frame and taken back through the layer's own position and rotation; its id new among `items`. Nothing when
// the mask is not a drawn path. (The pen draws masks; this makes what it drew a shape.)
std::optional<ShapeItem> shapePathFromMask(Effect& g, const std::vector<ShapeItem>& items, const Effect& mask, double frameW, double frameH);
// Whether a param name is one of a path's points (shown together, not each on its own).
bool isShapePathParam(const std::string& name);

}  // namespace montage
