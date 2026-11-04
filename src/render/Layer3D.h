// Montage — a clip as a card in 3D (the "layer_3d" effect): its picture, as its own transform leaves it on the
// sequence's frame, laid on a plane in the scene, turned about a pivot and moved, then seen through a camera (its own,
// the 3D Camera Tracker's solve, a file's, or the scene's Camera clip; render/ModelClip.h). Each pixel of the output
// looks along the camera's ray to the card and takes the picture where it meets it, with the depth there, so cards
// are merged by depth with each other and with models and deep pictures (cutting through where they meet).
//
// Lit, the card is a 3D Model's plane carrying the picture, lit by a sun, point or spot light and ambient light, taking
// the shadows of other cards that cast them and its own (where the pictures' alpha is at least a half) and casting
// them onto a ground; with depth of field on, it is blurred by the circle of confusion of a thin lens focused at a
// distance, gathered by depth as Lens Blur (Depth) gathers.
//
// The card is the whole frame: at z = 0 with nothing turned, seen through the camera that matches the 2D view (its own,
// or a Camera clip left as it is), each pixel lands on itself and the picture is unchanged.
#pragma once

#include <array>
#include <functional>
#include <memory>
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

// Another card casting its shadow: where it lies, and its picture (its alpha is where it casts).
struct Layer3DCaster {
    Layer3DPlace place;
    std::shared_ptr<const ModelTexture> picture;
    bool ownGround = false;  // lit with a ground of its own, which takes its shadow (so this card's ground does not)
};

// The card's plane in the world: a 3D Model's plane shape (the frame's shape, its picture's top left at uv 0, 0) laid
// where `place` puts the frame, `rough` its roughness (0..1).
PosedTriangles layer3dTriangles(const Layer3DPlace& place, std::shared_ptr<const Model>& model, float rough = 0.5f);

// The card lit as a 3D Model's plane is (render/ModelRender.h): the picture (`layer`, as renderLayer3D takes it, in
// the sequence's working space) its base colour and its alpha its coverage, lit by the light effect `e`'s settings
// make at its frame `lt` (render/ModelClip.h settingsLight), shadowed by `casters` and itself, its shadows on a ground
// at ground_y when "ground" is on. The output is in the working space again; `depth` as renderLayer3D gives it.
Image renderLitLayer3D(const Sequence& seq, const Effect& e, FrameTime lt, const Image& layer, const Layer3DPlace& place,
                       const ModelCamera& cam, const std::vector<Layer3DCaster>& casters, std::vector<float>* depth,
                       bool serial = false);

// The thin-lens circle of confusion's radius (output pixels) at depth z in front of `cam` focused at `focus` (the same
// units), at f-stop `fstop`, with `unitsPerMetre` scene units a metre and a 36 mm wide sensor across `width` pixels;
// positive nearer than the focus, negative farther.
double layer3dCoc(const ModelCamera& cam, int width, double z, double focus, double fstop, double unitsPerMetre);

// `img` (premultiplied, working space) blurred by each pixel's circle of confusion at `depth` (0: nothing there, kept
// sharp), gathered by depth as Lens Blur (Depth) does (render/LensBlur.h); left exactly as it is where every circle is
// under half a pixel. Where the blur spreads onto pixels with nothing there, they take the depth of the nearest that
// had something (so it is merged with the rest by depth).
void layer3dDepthOfField(Image& img, std::vector<float>& depth, const ModelCamera& cam, double focus, double fstop,
                         double unitsPerMetre);

// Clip c's picture `layer` through its 3D layer `e` at sequence frame t: false (with `error`) when its camera is not
// there (a solve missing, a file without a camera). Lit, it takes the shadows of the other cards at t that cast them
// through the same kind of camera, their pictures from `cardLayer` (none without it); with depth of field on, blurred.
bool renderClipLayer3D(const Project& p, const Sequence& seq, const Clip& c, const Effect& e, FrameTime t, const Image& layer, double scale,
                       Image& out, std::vector<float>* depth, std::string* error = nullptr,
                       const std::function<Image(const Clip&)>& cardLayer = {});

}  // namespace montage
