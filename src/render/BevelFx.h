// Montage — bevels on a layer's alpha (Effects › Stylize): Edge Bevel lights and shades a chamfer cut along the
// alpha's edge, and Layer Bevel raises the layer from what is behind it (an inner or outer bevel, an emboss across
// the edge, or a pillow emboss pressed in along it), with a colour and a blend mode each for its lit and its shaded
// side. Both work from the distance to the alpha's edge, to a fraction of a pixel, so a bevel is as wide as it is set
// wherever the edge runs. Each works in place on a premultiplied frame; `pixelScale` is the frame's pixels per source
// pixel, so sizes given in pixels look the same in previews at lower resolution. A frame's own border counts as an
// edge: an opaque picture is bevelled along its sides.
//
// The GPU compositor has no pass of its own for these: a layer with one is drawn by this code and handed over as a
// texture, as for the other effects render/Processing.cpp does not list as GPU filters, so both paths give the same
// pixels.
#pragma once

#include <vector>

#include "core/Model.h"
#include "media/Image.h"

namespace montage::bevel {

// The distance from each pixel's centre to the alpha's edge (where the alpha crosses a half), in pixels: positive
// inside, negative outside, width x height values. Outside the frame is outside the layer.
std::vector<float> edgeDistance(const Image& img);

void edgeBevel(const Effect& e, FrameTime t, Image& img, double pixelScale);
void layerBevel(const Effect& e, FrameTime t, Image& img, double pixelScale);

}  // namespace montage::bevel
