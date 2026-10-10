// Montage — 360° video: views out of equirectangular footage (GoPro's Reframe, Insta360 Studio's keyframes,
// Premiere's VR Projection) and the whole sphere as a little planet or a tunnel.
#pragma once

#include <optional>

#include "core/EditOps.h"
#include "core/Model.h"
#include "media/Image.h"

namespace montage {

enum class SphereView { Flat = 0, LittlePlanet = 1, Tunnel = 2 };

// A view of `equirect` (the whole sphere, premultiplied RGBA, longitude across, latitude down) at w x h: looking
// `yaw` degrees right of the picture's centre and `pitch` degrees up, turned `roll` degrees clockwise, `fov`
// degrees across the width. Flat is a rectilinear camera (up to 170°); the little planet looks down at the ground
// with the sky around it, the tunnel up at the sky (stereographic, up to 330°). `span` is the longitude the picture
// covers: 360 for the whole sphere, 180 for VR180 footage (the half in front; outside it the view is transparent).
Image reframeEquirect(const Image& equirect, double yaw, double pitch, double roll, double fov, SphereView view, int w,
                      int h, double span = 360);
// The longitude a 360° media item's picture covers ("vr180": 180, else 360).
double projectionSpan(const std::string& projection);

// Where a direction (degrees right of centre, degrees up) falls in an equirectangular picture of w x h.
void equirectPoint(double yaw, double pitch, int w, int h, double& x, double& y);

// Settings of a Reframe 360° effect to change; those left empty stay as they are.
struct ReframeView {
    std::optional<double> yaw, pitch, roll, fov;
    std::optional<SphereView> projection;
};

namespace edit {

// Aims a picture clip's Reframe 360° (adding one if it has none): the settings given, as smooth keys at clip-local
// frame `key`, or with key < 0 as its constant values (their keys removed). The projection is never keyed.
Result setReframe360(Project& p, Sequence& s, Id clip, const ReframeView& view, FrameTime key = -1);

}  // namespace edit

}  // namespace montage
