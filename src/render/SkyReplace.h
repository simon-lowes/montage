// Montage — Sky Replacement: finds the sky in a picture (bright, smooth and open to the top of the frame, and far away
// when the depth model is installed), keys it with edges refined against the picture so branches and hair stay, and
// puts another sky in its place: a clip on another track (a still or a moving sky) or a gradient. The new sky can
// follow the camera (the clip's analysed camera motion, or its 3D camera solve, turned only, as a sky at infinity
// moves), and the foreground is tied in by matching its colour to the new sky and wrapping the sky's light round its
// edges. Handled when tracks are composited (render/Compositor.cpp), like Track Matte Key.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/Model.h"
#include "media/Image.h"
#include "media/Tracking.h"

namespace montage {

struct ValueMap;

// Where the sky is in `img` (premultiplied; transparent pixels are never sky), 0..1 a pixel. `sensitivity` (0..1) lets
// more through as sky (less smooth, darker, nearer); `lowest` (0..1 of the height) is the lowest it reaches; `refine`
// (0..1) is how far its edges follow the picture's own (fine detail against the sky). `depth` (0 far, 1 near), when
// given, keeps near things out.
std::vector<float> skyMatte(const Image& img, const ValueMap* depth, double sensitivity, double lowest, double refine);

// The clip a sky is replaced in, for a sky held steady through the shot: `frame(offset, layer, depth)` gives its layer
// `offset` frames from the one being replaced, with its depth over that layer (null: found in it), false beyond the
// clip; `key(offset)` says what decides that frame ("": nothing kept), so what is found in each frame is kept for the
// frames around it.
struct SkyShot {
    std::function<bool(int offset, Image& layer, std::shared_ptr<const ValueMap>& depth)> frame;
    std::function<std::string(int offset)> key;
};

// The sky in `img` held steady through the shot: the sky found in up to three frames either side carried onto this
// one along the optical flow from frame to frame, and averaged with its own (nearer frames counting for more, and
// only where the pictures agree once carried, not where something moved in front), then its edges put on this
// picture's own. A sky that flickers in and out of a hazy horizon or through branches keeps still.
std::vector<float> steadySkyMatte(const Image& img, const ValueMap* depth, const SkyShot& shot, double sensitivity, double lowest,
                                  double refine);

// A sky graded from `zenith` at the top to `horizon` at row `horizonRow` and below (opaque).
Image skyGradient(int w, int h, const float zenith[3], const float horizon[3], double horizonRow);
// The lowest row the sky reaches in `matte` (most of it above), for a gradient's horizon.
double skyHorizonRow(const std::vector<float>& matte, int w, int h);

// `sky` moved by `h` (from its picture to the frame, pixels); beyond its edges it is mirrored.
Image warpSky(const Image& sky, const Homography& h);

// `sky` (premultiplied, the layer's size) put in `layer` where `matte` says, after moving the foreground's colour
// `match` (0..1) of the way the new sky differs from the old, and wrapping `wrap` (0..1) of the sky's light
// `wrapWidth` pixels into the foreground's edges.
void composeSky(Image& layer, const Image& sky, const std::vector<float>& matte, double match, double wrap, double wrapWidth);

// How the sky moves from the clip's middle frame to clip-local frame `local` in a w x h picture (pixels): the camera
// motion the effect's analysis measured (its strings["motion"]), or the clip's 3D camera solve (rotation only), scaled
// by Follow Amount. False when it does not move (Follow Camera off, or nothing analysed or solved there).
bool skyMotion(const Sequence& seq, const Clip& c, const Effect& e, FrameTime local, int w, int h, Homography& out);

// The whole effect on `layer` (the clip's composited layer at clip-local `local`), with `skyLayer` the picture on the
// sky track (null or empty: nothing to put in). It works within the clip's picture (its opaque bounds on the canvas),
// with depth when Use Depth is on: `depth` over the whole layer when given (the clip's analysed depth, carried through
// its transform), else found in the layer when the model is installed. `scale` is pixels per sequence pixel;
// `sourceSeconds` the media time. `mask` is the effect's mask over the whole layer (made over the clip's frame and
// carried through its transform); null: made over the layer as it is. With Steady Through the Shot on, `shot` gives
// the frames around it (steadySkyMatte).
void applySkyReplacement(const Sequence& seq, const Clip& c, const Effect& e, FrameTime local, Image& layer, const Image* skyLayer,
                         double scale, double sourceSeconds, const std::vector<float>* mask = nullptr, const ValueMap* depth = nullptr,
                         const SkyShot& shot = {});

}  // namespace montage
