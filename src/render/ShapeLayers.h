// Montage — a Shape layer's contents drawn (core/ShapeContents.h holds them): each group's items in order, the
// shapes making paths, the operators changing the paths above them in their group, fills and strokes painting them,
// and a repeater copying all that is above it; an item higher in a list is drawn over one lower down, as in the
// motion graphics tools this follows. And an SVG file turned into such contents, to edit.
#pragma once

#include <QBrush>
#include <QPainterPath>
#include <QPen>
#include <QTransform>
#include <array>
#include <string>
#include <vector>

#include "core/ShapeContents.h"
#include "media/Image.h"

namespace montage {

// One thing drawn: a path filled or stroked, its transform into the layer (pixels from its middle, y down) and its
// opacity; in drawing order, the lowest first. Or a group seen through its opacity as a whole: what is in it drawn
// together (their transforms then go on through the group's), then laid down at its opacity.
struct ShapePaint {
    QPainterPath path;
    QTransform transform;
    bool stroke = false;
    QBrush brush;
    QPen pen;
    double opacity = 1;
    std::vector<ShapePaint> group;
    bool isGroup = false;
};
std::vector<ShapePaint> shapePaints(const Effect& g, FrameTime t, double fps);

// A path item's points (core/ShapeContents.h shapePathPoints) to the layer's frame at t: through the groups it is in
// and the layer's own position and rotation, to pixels of a frameW x frameH frame (sequence pixels at its size). False
// when the contents hold no path item `id`.
bool shapePathToFrame(const Effect& g, const std::string& id, FrameTime t, double frameW, double frameH, QTransform& out);
// The path item a name or id picks (the first path when it is empty), "" when there is none.
std::string shapePathItem(const Effect& g, const std::string& nameOrId);

// The layer's one shape (its own Shape, sizes, fill, stroke and trim) made its contents, drawing the same, keys and
// all (to add more to); returns them, or nothing when it already has contents.
std::vector<ShapeItem> plainShapeAsContents(Effect& g);

// An SVG drawing's text, to be a title over its shapes: where its first line starts (or its middle or end, as
// text-anchor says) on the layer (pixels from its middle, y down, at the size the drawing is made), its font, size,
// weight, slant, fill and opacity.
struct SvgText {
    std::string text, font;
    double x = 0, y = 0, size = 16, opacity = 1;
    int anchor = 0;  // 0 start, 1 middle, 2 end
    bool bold = false, italic = false;
    std::array<double, 3> fill{0, 0, 0};
};

// An SVG file as a Shape layer's contents: its groups, paths, rectangles, circles, ellipses, lines and polygons,
// fills and strokes (solid, linear and radial gradients, dash patterns), transforms and opacities (a group's applied
// to it as a whole), styles from attributes, style= and <style> sheets (type, class, id and universal selectors,
// lists and descendants), set on `g` (its contents and their params), the drawing's size made the layer's and its
// middle at the layer's middle. Its text goes to `texts` (when given) to be made titles. `notes` lists what is not
// turned into shapes (pictures, filters, masks, clipping, patterns; text when `texts` is not given). False with
// `error` when it cannot be read or holds nothing to draw.
bool importSvgShapes(const std::string& file, Effect& g, double& width, double& height, std::vector<std::string>* notes = nullptr,
                     std::string* error = nullptr, std::vector<SvgText>* texts = nullptr);
// A title generator ("title") showing an SVG's text where it was on a shape layer placed as imported (its middle at
// the frame's middle), at `scale` sequence pixels a layer pixel.
Effect svgTextTitle(const SvgText& t, double scale = 1);
// Titles for an SVG's text over the shape layer clip `layer` it was imported into, for as long as it, each on the
// first track above it free then (a track added on top when none is); the first 32 (more are noted). Returns how
// many were placed.
int placeSvgTexts(Project& p, Sequence& s, Id layer, const std::vector<SvgText>& texts, std::vector<std::string>* notes = nullptr);

}  // namespace montage
