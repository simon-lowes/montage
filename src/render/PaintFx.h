// Montage — pictures moved by another picture, and painterly looks.
//
// Displace by Layer: each pixel takes its colour from a place moved across and down by a channel of a map (a track's
// picture, or the clip's own): mid-grey moves nothing, white moves by the full amount, black by as much the other way.
//
// Paint: an oil-paint look by the anisotropic Kuwahara filter (Kyprianidis, Kang and Döllner, "Image and Video
// Abstraction by Anisotropic Kuwahara Filtering", 2009, with the polynomial sector weights of Kyprianidis's 2010
// follow-up): the picture's local structure (its smoothed structure tensor) shapes an ellipse round each pixel, split
// into eight sectors, and each sector's mean colour is weighted by how little it varies, so flat areas stay flat and
// edges stay sharp. Or a smear along the picture's contours: line integral convolution along the structure tensor's
// flow, posterised to a few levels, as a brush would drag paint along the shapes.
//
// All work in place on premultiplied frames, each pixel from the input alone (so any number of threads gives the
// same picture).
#pragma once

#include "core/Model.h"
#include "media/Image.h"

#include <atomic>

namespace montage {

enum class DisplaceChannel { Luma, Red, Green, Blue, Alpha, Off };
enum class DisplaceEdge { Wrap, Clamp, Transparent };

struct DisplaceSettings {
    DisplaceChannel horizontal = DisplaceChannel::Red, vertical = DisplaceChannel::Green;
    double horizontalAmount = 20, verticalAmount = 20;  // pixels a channel at 1 moves (at 0, as far the other way)
    DisplaceEdge edge = DisplaceEdge::Clamp;
    bool bicubic = false;  // else bilinear
    double mapBlur = 0;    // the map blurred first (a Gaussian's radius in pixels)
    // Where the picture's own pixels are in the frame, [left, right) x [top, bottom) (a scaled or letterboxed clip's
    // layer): only they move, and the edges are theirs. Empty: the whole frame.
    int left = 0, top = 0, right = 0, bottom = 0;
};
// `img` displaced by `map` (the same size; straight values read from its premultiplied ones). A pixel moved by
// (dx, dy) takes the colour from (x + dx, y + dy).
void displaceByMap(Image& img, const Image& map, const DisplaceSettings& settings);
DisplaceSettings displaceSettings(const Effect& e, FrameTime t, double pixelScale);

struct KuwaharaSettings {
    double radius = 6;      // the ellipse's radius in pixels (along both axes where there is no structure)
    double sharpness = 8;   // q: how strongly sectors that vary are left out
    double anisotropy = 1;  // alpha: how far the ellipse stretches along the structure (smaller stretches more)
    double tensorBlur = 2;  // the structure tensor's smoothing (Gaussian sigma, pixels)
};
// False, the picture as it was, when `cancel` stopped it part way.
bool anisotropicKuwahara(Image& img, const KuwaharaSettings& settings, const std::atomic<bool>* cancel = nullptr);

struct SmearSettings {
    double length = 12;     // pixels each way along the flow
    int levels = 8;         // colour levels after (0: none)
    double tensorBlur = 3;  // the structure tensor's smoothing (Gaussian sigma, pixels)
};
bool contourSmear(Image& img, const SmearSettings& settings, const std::atomic<bool>* cancel = nullptr);

// The Painterly effect.
void paintEffect(const Effect& e, FrameTime t, Image& img, double pixelScale);

}  // namespace montage
