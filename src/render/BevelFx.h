// Montage — bevels on a layer's alpha (Effects › Stylize): Edge Bevel lights and shades a chamfer cut along the
// alpha's edge, and Layer Bevel raises the layer from what is behind it (an inner or outer bevel, an emboss across
// the edge, or a pillow emboss pressed in along it), with a colour and a blend mode each for its lit and its shaded
// side. Both work from the distance to the alpha's edge, to a fraction of a pixel, so a bevel is as wide as it is set
// wherever the edge runs. Each works in place on a premultiplied frame; `pixelScale` is the frame's pixels per source
// pixel, so sizes given in pixels look the same in previews at lower resolution. A frame's own border counts as an
// edge: an opaque picture is bevelled along its sides.
//
// On the GPU (render/shaders/gpufilter.frag's modes 78 and 79) the surface is made here, from the picture read back
// when the step runs, and lit there.
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
// Each one's surface (its height in pixels), with a ring of a pixel round the frame: (width + 2) x (height + 2)
// values; empty where the bevel changes nothing.
std::vector<float> edgeBevelSurface(const Effect& e, FrameTime t, const Image& img, double pixelScale);
std::vector<float> layerBevelSurface(const Effect& e, FrameTime t, const Image& img, double pixelScale);
// Layer Bevel's light (its direction, x and y across the frame, z up) for an angle and altitude in degrees.
void bevelLight(double angleDeg, double altitudeDeg, double out[3]);

}  // namespace montage::bevel
