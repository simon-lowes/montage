// Montage — a clip as a card in 3D (the "layer_3d" effect): its picture, as its own transform leaves it on the
// sequence's frame, laid on a plane in the scene, turned about a pivot and moved, then seen through a camera (its own,
// the 3D Camera Tracker's solve, a file's, or the scene's Camera clip; render/ModelClip.h). Each pixel of the output
// looks along the camera's ray to the card and takes the picture where it meets it, with the depth there, so cards
// are merged by depth with each other and with models and deep pictures (cutting through where they meet).
//
// Lit, the card is a 3D Model's plane carrying the picture, lit by a sun, point or spot light and ambient light. The
// cards and models seen through the same kind of camera at a frame are one scene of shadows (SceneShadows): one shadow
// map a light for all of them, so each card and model takes the shadows of the others that cast them and its own
// (where the pictures' alpha is at least a half) and each caster's shadow falls on one ground. With depth of field on,
// the card is blurred by the circle of confusion of a thin lens focused at a distance, gathered by depth as Lens Blur
// (Depth) gathers; seen through the scene's Camera clip with its lens's depth of field on, the camera's settings (its
// aperture's blades, their turn and an anamorphic squeeze too) blur the whole scene instead (cameraDepthOfField).
//
// The card is the whole frame: at z = 0 with nothing turned, seen through the camera that matches the 2D view (its own,
// or a Camera clip left as it is), each pixel lands on itself and the picture is unchanged.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"
#include "render/LensBlur.h"
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

// The scene of shadows that cards and models seen through one kind of camera make at a frame: its casters in one
// ModelShadowWorld (each clip's index there, for those that cast), and for each clip with a ground of its own the
// casters whose shadows fall on it. A caster with no ground of its own shadows one ground: the nearest below its
// middle (with none below it, the nearest above); of grounds as high, that on the nearest track under its own (else
// the nearest over it).
struct SceneShadows {
    std::shared_ptr<const ModelShadowWorld> world;
    std::map<Id, int> caster;
    std::map<Id, std::vector<int>> ground;
    std::set<Id> members;  // every clip in it, casting or not
};

// Which scene of shadows effect `g` (a model's generator, or a 3D Layer) on clip c sees through at sequence frame t:
// "front" for its own camera and the scene's Camera clip (both a unit a pixel about the frame), "solve:<track>" for
// the solve on a track, "file:<file>" for a file's camera. (render/ModelClip.cpp)
std::string shadowWorldOf(const Sequence& seq, const Clip& c, const Effect& g, FrameTime t);

// The scene of shadows `world` at t: every enabled model (not splats) and every card (by the first 3D Layer it is
// drawn by) on the unmuted video tracks seen through that kind of camera, cards' pictures from `cardLayer` (a card
// with none casts nothing), models' texture tracks from `trackLayer` (without it they cast whole). `self` is put in
// it when it is not on a track. Inside a SceneShadowScope for the same project and sequence, made once a frame and
// world (with the scope's pictures), and given again.
SceneShadows sceneShadows(const Project& p, const Sequence& seq, FrameTime t, const std::string& world,
                          const std::function<Image(const Clip&)>& cardLayer = {},
                          const std::function<Image(int track)>& trackLayer = {}, const Clip* self = nullptr);

// While one lives (on this thread), the scenes of shadows of `seq` are made once a frame: a sequence frame's render
// opens one, so a light's shadow map is made once for every card and model in it. Pictures come from `cardLayer`
// (a clip's picture at a frame, before its 3D Layer) and `trackLayer` (a track's at a frame).
class SceneShadowScope {
public:
    SceneShadowScope(const Project& p, const Sequence& seq, std::function<Image(const Clip&, FrameTime)> cardLayer,
                     std::function<Image(int track, FrameTime)> trackLayer);
    ~SceneShadowScope();
    SceneShadowScope(const SceneShadowScope&) = delete;
    SceneShadowScope& operator=(const SceneShadowScope&) = delete;
    struct State;

private:
    State* state_;
    State* outer_;
};

// The card's plane in the world: a 3D Model's plane shape (the frame's shape, its picture's top left at uv 0, 0) laid
// where `place` puts the frame, `rough` its roughness (0..1).
PosedTriangles layer3dTriangles(const Layer3DPlace& place, std::shared_ptr<const Model>& model, float rough = 0.5f);

// The card lit as a 3D Model's plane is (render/ModelRender.h): the picture (`layer`, as renderLayer3D takes it, in
// the sequence's working space) its base colour and its alpha its coverage, lit by the light effect `e`'s settings
// make at its frame `lt` (render/ModelClip.h settingsLight), shadowed by the scene of shadows `shadows` (the card
// itself clip `self` there; without one, by itself alone when it casts), its ground at ground_y when "ground" is on
// taking the shadows that fall on it. The output is in the working space again; `depth` as renderLayer3D gives it.
Image renderLitLayer3D(const Sequence& seq, const Effect& e, FrameTime lt, const Image& layer, const Layer3DPlace& place,
                       const ModelCamera& cam, const SceneShadows* shadows, Id self, std::vector<float>* depth,
                       bool serial = false);

// The thin-lens circle of confusion's radius (output pixels) at depth z in front of `cam` focused at `focus` (the same
// units), at f-stop `fstop`, with `unitsPerMetre` scene units a metre and a 36 mm wide sensor across `width` pixels;
// positive nearer than the focus, negative farther.
double layer3dCoc(const ModelCamera& cam, int width, double z, double focus, double fstop, double unitsPerMetre);

// `img` (premultiplied, working space) blurred by each pixel's circle of confusion at `depth` (0: nothing there, kept
// sharp), gathered by depth as Lens Blur (Depth) does (render/LensBlur.h); left exactly as it is where every circle is
// under half a pixel. Where the blur spreads onto pixels with nothing there, they take the depth of the nearest that
// had something (so it is merged with the rest by depth). The aperture's shape is `aperture`'s (round by default).
void layer3dDepthOfField(Image& img, std::vector<float>& depth, const ModelCamera& cam, double focus, double fstop,
                         double unitsPerMetre, const Aperture& aperture = {});

// A Camera clip's lens ("camera3d": dof, focus_distance (0: what is at the picture's middle), fstop, units_m, blades,
// blade_rotation, squeeze) at sequence frame t: false when its depth of field is off.
struct CameraFocus {
    double focus = 0, fstop = 2.8, unitsPerMetre = 1000;
    Aperture aperture;
};
bool cameraFocus(const Clip& camera, FrameTime t, CameraFocus& focus);
// The scene seen through Camera clip `camera` at t (img and its depth, as layer3dDepthOfField takes them, on the
// sequence's picture scaled by `scale`) blurred by its lens; nothing when its depth of field is off.
void cameraDepthOfField(const Project& p, const Sequence& seq, const Clip& camera, FrameTime t, double scale, Image& img,
                        std::vector<float>& depth);

// Clip c's picture `layer` through its 3D layer `e` at sequence frame t: false (with `error`) when its camera is not
// there (a solve missing, a file without a camera). Lit, it takes the shadows of its scene of shadows (sceneShadows,
// the other cards' pictures from `cardLayer`); with depth of field on, blurred. Seen through the scene's Camera clip
// with the camera's depth of field on, its own is left off: the camera's blurs it, here when `depth` is null (the card
// used flat), else where the deep pictures are merged.
bool renderClipLayer3D(const Project& p, const Sequence& seq, const Clip& c, const Effect& e, FrameTime t, const Image& layer, double scale,
                       Image& out, std::vector<float>* depth, std::string* error = nullptr,
                       const std::function<Image(const Clip&)>& cardLayer = {});

}  // namespace montage
