// Montage — placing a clip's picture in the frame: where it lies (its cropped box as drawn), and lining it up with
// the frame's centre, edges and corners inside a margin (CapCut's and Descript's canvas alignment, Premiere's
// Align in the Essential Graphics panel).
#pragma once

#include <array>
#include <string>

#include "core/EditOps.h"
#include "core/Model.h"

namespace montage {

// The clip's picture, cropped, as drawn at timeline frame t: its corners in sequence pixels (top left, top right,
// bottom right, bottom left). False if it has no picture.
bool clipFrameQuad(const Project& p, const Sequence& s, const Clip& c, FrameTime t, std::array<double, 4>& xs, std::array<double, 4>& ys);

enum class Align { Center, Top, Bottom, Left, Right, TopLeft, TopRight, BottomLeft, BottomRight };
// "center", "top", "bottom", "left", "right", "top_left", "top_right", "bottom_left", "bottom_right".
bool parseAlign(const std::string& name, Align& out);

namespace edit {

// Moves the clip so its picture's bounds sit at `where` in the frame, `inset` (a share of the frame's height, 0.05
// for action safe) in from the edges it is lined up with. Keyed at timeline frame t when Position is animated.
Result alignClip(Project& p, Sequence& s, Id clip, FrameTime t, Align where, double inset = 0.05);

}  // namespace edit

}  // namespace montage
