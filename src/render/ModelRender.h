// Montage — drawing 3D models (media/Model3D.h) into the picture: a z-buffered rasteriser with 2 x 2 samples a pixel
// (each triangle shaded once a pixel at the centre of the samples it covers, perspective-correct), the glTF 2.0
// metallic-roughness shading (GGX microfacets with the height-correlated Smith visibility, Schlick's Fresnel and a
// Lambert diffuse, as the glTF specification's appendix B gives it), base colour, metallic-roughness, normal and
// emissive textures sampled with mip levels and up to 8 taps along the longer axis of the pixel's footprint (so
// textures stay sharp at grazing angles), directional, point and spot lights (shadow maps for directional and spot
// lights, filtered over 3 x 3 texels), and light from a picture of the scene (a latitude-longitude HDR image: its
// diffuse light from spherical harmonics, its reflections from copies blurred for each roughness, with an analytic
// split-sum term). A ground plane can catch the model's shadows alone, as a picture to lay over the footage.
// Only the nearest surface is kept at each sample, so a blended (see-through) material shows what is behind the model,
// not the model's own faces behind it; point lights cast no shadows, and masked materials cast them whole.
#pragma once

#include <array>
#include <memory>
#include <vector>

#include "media/Image.h"
#include "media/Model3D.h"

namespace montage {

// A pinhole camera: a world point X is at R (X - C) in the camera's space (x right, y down, z forward), and with
// x' = x / z, y' = y / z at (screen[0] x' + screen[1] y' + screen[2], screen[3] x' + screen[4] y' + screen[5]) in the
// picture (pixels).
struct ModelCamera {
    std::array<double, 9> R{1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major, world to camera
    std::array<double, 3> C{0, 0, 0};
    std::array<double, 6> screen{1, 0, 0, 0, 1, 0};
    double nearZ = 1e-3;  // nothing nearer than this is drawn
    // Where X is in the picture; false when it is not in front of the camera. `z`: its depth in front.
    bool project(const std::array<double, 3>& X, double& px, double& py, double* z = nullptr) const;
    // The ray's direction (world, not unit length) through picture point (px, py).
    std::array<double, 3> ray(double px, double py) const;
};

struct ModelLight {
    enum class Kind { Directional, Point, Spot } kind = Kind::Directional;
    std::array<double, 3> direction{0, -1, 0};  // the way the light travels (directional, spot)
    std::array<double, 3> position{0, 0, 0};    // point, spot
    std::array<float, 3> color{1, 1, 1};
    double intensity = 3.14159265358979;  // directional: illuminance (pi lights a white diffuse face to 1); point
                                          // and spot: the same at 1 unit away
    double innerCone = 0, outerCone = 0.7854;  // spot: radians from its axis
    bool shadows = true;                        // (directional and spot)
};

// A latitude-longitude picture of the light all round (linear RGB, top row up): the middle of the picture lies along
// -z, its left and right edges along +z.
struct ModelEnvironment {
    int width = 0, height = 0;
    std::vector<float> rgb;
    // Prepared once (its harmonics and blurred copies), when first drawn with.
    struct Prepared;
    mutable std::shared_ptr<const Prepared> prepared;
};

struct ModelScene {
    PosedTriangles triangles;  // in the world
    const Model* model = nullptr;  // their materials and textures (null: the default material)
    std::vector<ModelLight> lights;
    std::array<float, 3> ambient{0, 0, 0};  // light from all round, where there is no environment
    std::shared_ptr<const ModelEnvironment> environment;
    double environmentRotation = 0;  // radians about +y
    float environmentIntensity = 1;
    // A ground plane (y = groundHeight) that shows only the shadows falling on it, as black at the share of light
    // they take away (times groundStrength).
    bool ground = false;
    double groundHeight = 0;
    float groundStrength = 1;
    float opacity = 1;
};

struct ModelRenderOptions {
    bool serial = false;  // on this thread alone (the result is the same)
    int shadowSize = 2048;
};

// The scene through the camera, w x h, linear light, premultiplied. `depth` (w x h): the nearest drawn surface's
// depth in front of the camera (the ground's where only its shadow is), 0 where nothing is drawn.
Image renderModel(const ModelScene& scene, const ModelCamera& camera, int w, int h, std::vector<float>* depth = nullptr,
                  const ModelRenderOptions& options = {});

}  // namespace montage
