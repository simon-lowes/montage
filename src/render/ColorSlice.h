// Montage — film-like density and texture (Phase 6 #10), as Resolve's ColorSlice, Mid-tone Detail and Texture Pop.
//
// Colour Slice changes colours by hue vector (red, yellow, green, cyan, blue, magenta, and skin, whose centre and
// width can move): a hue shift, saturation and density each. It works in a log encoding of the light (ACEScct's
// curve), where saturation can be subtractive as film's dyes are (the brightest channel stays and the others fall,
// so richer colours get darker) and density darkens each colour by how saturated it is. Highlights can be protected.
// It is colour-only, so exported LUTs hold it.
//
// Texture Pop and Mid-tone Detail add (or take away) local contrast in brightness only, so colours do not fringe: Mid-tone
// Detail at a wide scale through an edge-preserving filter (so strong edges get no halo), weighted to the mid-tones;
// Texture Pop at a fine scale, above a noise floor. Sizes are in sequence pixels, scaled to the preview.
#pragma once

#include "ColorSpace.h"
#include "core/Model.h"
#include "media/Image.h"

namespace montage {

// The hue vectors' names (parameter prefixes) and centres in degrees on the log encoding's hue circle; the skin vector's
// centre and width are its own parameters.
struct HueVector {
    const char* name;
    const char* label;
    double centre;
};
const std::vector<HueVector>& colorSliceVectors();

// ACEScct's encoding of linear light, and back.
float acesCct(float linear);
float acesCctToLinear(float cct);

// Colour Slice's settings at a time, as its pixels take them (gpufilter.frag takes the same): the six vectors that change
// something, the skin vector, the whole picture's saturation and density, and whether anything changes.
struct ColorSliceSetup {
    struct Vec {
        float centre = 0, width = 60, hue = 0, sat = 1, density = 0;
    };
    std::vector<Vec> vecs;
    Vec skin;
    float globalSat = 1, globalDensity = 0, depth = 1, protect = 0.5f, mix = 1;
    bool subtractive = true, any = false;
};
ColorSliceSetup colorSliceSetup(const Effect& e, FrameTime t);
void colorSlice(const Effect& e, FrameTime t, const ColorSpace& space, Image& img);
void textureDetail(const Effect& e, FrameTime t, const ColorSpace& space, Image& img, double pixelScale);

}  // namespace montage
