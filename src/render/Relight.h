// Montage — Relight (like Resolve's Relight): a virtual light added to a
// shot after it was filmed. The surfaces' directions come from the picture's
// depth (media/DepthMap.h): slopes of the smoothed depth give a normal per
// pixel, and each is lit by how much it faces the light.
#pragma once

#include <vector>

#include "media/Image.h"

namespace montage {

struct DepthMap;

struct RelightSettings {
    double azimuth = 135;   // where the light comes from, degrees anticlockwise from the right
    double elevation = 35;  // above the picture plane, degrees (90: straight on)
    float color[3] = {1.0f, 0.95f, 0.85f};
    double intensity = 0.6;  // brightening of surfaces turned towards the light (a surface facing the camera keeps its light)
    double shadows = 0.3;    // darkening of those turned away (0..1)
    double relief = 3;       // how much depth turns into slope
    double smoothness = 8;   // depth smoothing before slopes, in pixels
    double reach = 1;        // 1: everything is lit; less lights only nearer parts (0..1)
    bool showNormals = false;
};

// Normals (x right, y down, z towards the viewer; three floats per pixel) of a w x h picture
// from its depth (near is 1).
std::vector<float> depthNormals(const DepthMap& depth, int w, int h, double relief, double smoothness);

// Lights `img` (premultiplied) in place.
void relight(Image& img, const DepthMap& depth, const RelightSettings& s);

}  // namespace montage
