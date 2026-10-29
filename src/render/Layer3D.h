// Montage — a clip as a card in 3D (the "layer_3d" effect): its picture, as its own transform leaves it on the
// sequence's frame, laid on a plane in the scene, turned about a pivot and moved, then seen through a camera (its own,
// the 3D Camera Tracker's solve, a file's, or the scene's Camera clip; render/ModelClip.h). Each pixel of the output
// looks along the camera's ray to the card and takes the picture where it meets it, with the depth there, so cards
// are merged by depth with each other and with models and deep pictures (cutting through where they meet).
//
// The card is the whole frame: at z = 0 with nothing turned, seen through the camera that matches the 2D view (its own,
// or a Camera clip left as it is), each pixel lands on itself and the picture is unchanged.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"
#include "render/ModelRender.h"

namespace montage {

// Where the card lies in the world: the frame's middle, and its right, up and front (unit length) axes, with the world
// units a sequence pixel spans.
struct Layer3DPlace {
    std::array<double, 3> origin{0, 0, 0}, axisU{1, 0, 0}, axisV{0, 1, 0}, normal{0, 0, 1};
    double unit = 1;
    double seqWidth = 1920, seqHeight = 1080;
    // A point of the sequence's frame (pixels, y down) on the card, in the world.
    std::array<double, 3> point(double x, double y) const;
};

// The place of effect `e` at its clip's frame `lt` on a sequence's frame: a pixel a unit through its own or the scene's
// camera, its Width across the frame through a solved or a file's camera; Position (y up, z towards the camera),
// turned by Y, then X, then Z about its pivot.
Layer3DPlace layer3dPlace(const Effect& e, FrameTime lt, double seqWidth, double seqHeight);

// `layer` (the clip's picture on the sequence's frame scaled by `scale`, premultiplied) on the card, through `cam`
// (on the same scaled picture): the output, the same size, and the depth in front of the camera (0 where the card is
// not; `depth` may be null).
Image renderLayer3D(const Image& layer, const Layer3DPlace& place, const ModelCamera& cam, double scale, std::vector<float>* depth);

// Clip c's picture `layer` through its 3D layer `e` at sequence frame t: false (with `error`) when its camera is not
// there (a solve missing, a file without a camera).
bool renderClipLayer3D(const Project& p, const Sequence& seq, const Clip& c, const Effect& e, FrameTime t, const Image& layer, double scale,
                       Image& out, std::vector<float>* depth, std::string* error = nullptr);

}  // namespace montage
