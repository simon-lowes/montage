// Montage — Extend Clip (a local counterpart to Premiere's Generative Extend): a shot that ends too soon carries on
// past its last frame. The last frame is held while the camera's motion over the shot's final second (a pan, a
// drift, a slow push) carries on and settles, so the picture does not stop dead; the sound under it is the room's
// own tone (render/RoomTone.h), added by the caller as it needs a file.
#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "core/Model.h"

namespace montage {

// How the picture was moving at the end of a clip, per frame: in sequence pixels, as a scale factor's logarithm and
// in degrees.
struct EndMotion {
    double dx = 0, dy = 0;
    double zoom = 0;
    double turn = 0;
    int samples = 0;  // frames measured
};
// The camera's motion over the clip's last second (at most), measured from its media.
bool measureEndMotion(const Project& p, const Sequence& s, const Clip& c, EndMotion& out, std::string* error = nullptr,
                      const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

// The extension of a video clip: `frames` long, starting where the clip ends, holding its last frame (as Frame
// Hold does) with its effects as they are there, its position, scale and rotation carrying on the motion and easing
// to rest (velocity falling as the square of the time left), scaled up as it drifts so a picture that filled the
// frame still does. Named "<name> (extended)".
Clip makeExtension(Project& p, const Sequence& s, const Clip& c, FrameTime frames, const EndMotion& motion);

// Places the extension after the clip: in empty space, or, with `ripple`, pushing what follows (on sync-locked tracks
// too) to make room. False with `error` if the space is taken and not rippling. `made` receives the new clip's id.
bool extendClip(Project& p, Sequence& s, Id clip, FrameTime frames, const EndMotion& motion, bool ripple, Id* made = nullptr,
                std::string* error = nullptr);

}  // namespace montage
