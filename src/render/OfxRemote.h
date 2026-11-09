// Montage — OpenFX renders in a plug-in host process (audio/PluginHost.h): the effect, the plug-in's description and
// the pictures go to montage-pluginhost, which renders as Montage would (render/Ofx.h) and sends the picture back;
// frames at other times and connected inputs are asked back from Montage while it renders. Internal to Ofx.cpp.
#pragma once

#include <string>

#include "render/Ofx.h"

namespace montage::ofx::remote {

// Whether renders go to a host (plug-ins are isolated, and this is not a host itself).
bool isolated();

enum class Kind { Effect = 0, Transition = 1, Generator = 2 };
// Renders `e` (its plug-in `d`) in its host: an effect on `a`, a transition from `a` to `b` (both w x h), or a
// generator's w x h picture. False, with why, when the plug-in failed (it is then bypassed: the picture stays as it
// came) or is bypassed after failing too often.
bool render(Kind kind, const Effect& e, const PluginDesc& d, double t, double progress, double scale, int w, int h, const Image* a,
            const Image* b, Image& out, const FrameFetch* fetch, std::string* error, const ColorSpace* working);

// Lets go of the channels kept for the next render, so hosts with nothing else in them end (and new ones start with
// the settings as they are then).
void releaseIdle();

}  // namespace montage::ofx::remote
