// Montage — a Shape layer's contents drawn (core/ShapeContents.h holds them): each group's items in order, the
// shapes making paths, the operators changing the paths above them in their group, fills and strokes painting them,
// and a repeater copying all that is above it; an item higher in a list is drawn over one lower down, as in the
// motion graphics tools this follows. And an SVG file turned into such contents, to edit.
#pragma once

#include <QBrush>
#include <QPainterPath>
#include <QPen>
#include <QTransform>
#include <string>
#include <vector>

#include "core/ShapeContents.h"
#include "media/Image.h"

namespace montage {

// One thing drawn: a path filled or stroked, its transform into the layer (pixels from its middle, y down) and its
// opacity; in drawing order, the lowest first.
struct ShapePaint {
    QPainterPath path;
    QTransform transform;
    bool stroke = false;
    QBrush brush;
    QPen pen;
    double opacity = 1;
};
std::vector<ShapePaint> shapePaints(const Effect& g, FrameTime t, double fps);

// The contents drawn on a w x h frame (`scale` output pixels a sequence pixel), through the layer's own position,
// rotation and opacity; premultiplied.
Image renderShapeContents(const Effect& g, FrameTime t, int w, int h, double scale, double fps);

// The layer's one shape (its own Shape, sizes, fill, stroke and trim) made its contents, drawing the same, keys and
// all (to add more to); returns them, or nothing when it already has contents.
std::vector<ShapeItem> plainShapeAsContents(Effect& g);

// An SVG file as a Shape layer's contents: its groups, paths, rectangles, circles, ellipses, lines and polygons,
// fills and strokes (solid, linear and radial gradients), transforms and opacities, set on `g` (its contents and
// their params), the drawing's size made the layer's and its middle at the layer's middle. `notes` lists what is not
// turned into shapes (text, pictures, filters, masks, clipping, patterns, stylesheets). False with `error` when it
// cannot be read or holds nothing to draw.
bool importSvgShapes(const std::string& file, Effect& g, double& width, double& height, std::vector<std::string>* notes = nullptr,
                     std::string* error = nullptr);

}  // namespace montage
