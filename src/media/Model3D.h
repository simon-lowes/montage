// Montage — 3D models: glTF 2.0 (.gltf with its buffers and images, or .glb, read by cgltf), OBJ with its MTL
// materials (read by tinyobjloader) and USD scenes (USDA, USDC and USDZ, read by media/Usd.h), kept as meshes with
// metallic-roughness materials (base colour, metallic and roughness, normal and emissive, each with its texture), the
// node tree, skins, morph targets, animations, cameras and lights. A model is posed at a time of one of its
// animations into triangles in its own space (render/ModelRender.h draws them). OBJ materials are taken as the MTL's
// diffuse colour and texture, with its PBR extension (Pr, Pm) when there, else a roughness from its specular
// exponent. USD's UsdPreviewSurface materials map onto the same metallic-roughness model.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace montage {

// Column-major, as glTF writes them: element (row r, column c) at [c * 4 + r].
using Mat4 = std::array<double, 16>;
Mat4 identity4();
Mat4 operator*(const Mat4& a, const Mat4& b);
// A point (w = 1) and a direction (w = 0) through a matrix.
std::array<double, 3> transformPoint(const Mat4& m, const std::array<double, 3>& p);
std::array<double, 3> transformDirection(const Mat4& m, const std::array<double, 3>& d);
Mat4 inverse4(const Mat4& m);  // (affine)
// Translation, rotation (a unit quaternion x, y, z, w) and scale, applied scale first.
Mat4 trsMatrix(const std::array<double, 3>& t, const std::array<double, 4>& r, const std::array<double, 3>& s);

// A texture's pixels, 8 bits a channel (RGBA, top row first; colour in sRGB when `srgb`, alpha linear), with its mip
// levels, each half the last (averaged in linear light).
struct ModelTexture {
    struct Level {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };
    std::vector<Level> levels;
    bool srgb = false;
    // Channel c (0..3) of an 8-bit texel in linear light.
    float linear(uint8_t v, int c) const;
    enum class Wrap { Repeat, Clamp, Mirror } wrapS = Wrap::Repeat, wrapT = Wrap::Repeat;
};

struct ModelMaterial {
    std::string name;
    std::array<float, 4> baseColor{1, 1, 1, 1};  // linear
    float metallic = 1, roughness = 1;
    std::array<float, 3> emissive{0, 0, 0};       // linear
    float normalScale = 1;
    // Textures (indices into Model::textures, -1: none): base colour and emissive hold sRGB colours (made linear
    // when read), metallicRoughness roughness in G and metallic in B.
    int baseTexture = -1, metallicRoughnessTexture = -1, normalTexture = -1, emissiveTexture = -1;
    enum class Alpha { Opaque, Mask, Blend } alpha = Alpha::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};

struct ModelPrimitive {
    std::vector<float> positions;   // 3 a vertex
    std::vector<float> normals;     // 3 a vertex (made flat from the faces when the file has none)
    std::vector<float> uvs;         // 2 a vertex, or empty
    std::vector<float> tangents;    // 4 a vertex (w the bitangent's sign), or empty (made when a normal map needs them)
    std::vector<float> colors;      // 4 a vertex (COLOR_0, linear), or empty
    std::vector<uint16_t> joints;   // 4 a vertex, or empty
    std::vector<float> weights;     // 4 a vertex
    struct Target {
        std::vector<float> positions, normals;  // displacements, 3 a vertex (either may be empty)
    };
    std::vector<Target> targets;
    std::vector<uint32_t> indices;  // triangles
    int material = -1;              // -1: the default material
};

struct ModelMesh {
    std::string name;
    std::vector<ModelPrimitive> primitives;
    std::vector<float> weights;  // the morph targets' default weights
};

// One of a USD prim's transform operations, applied in the order the node lists them (the first outermost).
struct ModelXformOp {
    enum class Kind { Translate, Scale, RotateX, RotateY, RotateZ, RotateXYZ, RotateXZY, RotateYXZ, RotateYZX, RotateZXY, RotateZYX, Orient, Transform };
    Kind kind = Kind::Translate;
    bool inverse = false;
    // Translate and scale: x, y, z; a rotation: degrees (one or three, in the order its name gives); orient: a
    // quaternion (x, y, z, w); transform: a matrix (Mat4's order).
    std::vector<double> value;
    Mat4 matrix() const;
};

struct ModelNode {
    std::string name;
    int parent = -1;
    std::vector<int> children;
    std::array<double, 3> translation{0, 0, 0}, scale{1, 1, 1};
    std::array<double, 4> rotation{0, 0, 0, 1};
    bool hasMatrix = false;
    Mat4 matrix = identity4();  // when hasMatrix (not animated)
    int mesh = -1, skin = -1;
    std::vector<float> weights;  // morph weights for its mesh (empty: the mesh's)
    std::vector<ModelXformOp> ops;  // (USD) its transform as these, when it has any, in place of the above
    bool resetsParent = false;      // (USD's !resetXformStack!) placed in the world, not under its parent
};

struct ModelSkin {
    std::vector<int> joints;
    std::vector<Mat4> inverseBind;
};

struct ModelAnimation {
    std::string name;
    struct Sampler {
        std::vector<float> times;
        std::vector<float> values;  // components a key (CUBICSPLINE: in-tangent, value, out-tangent for each)
        int components = 3;
        enum class Interpolation { Linear, Step, CubicSpline } interpolation = Interpolation::Linear;
    };
    struct Channel {
        int node = -1;
        enum class Path { Translation, Rotation, Scale, Weights, Op } path = Path::Translation;
        int sampler = 0;
        int op = 0;  // Op: which of the node's transform operations
    };
    std::vector<Sampler> samplers;
    std::vector<Channel> channels;
    double duration = 0;  // seconds: the last key's time
};

// A camera in the file: it looks along its node's -z with +y up. Its lens as USD gives it: the focal length and the
// film back's size and offset in the same units (millimetres, or tenths of a scene unit; only their ratios count).
struct ModelCameraInfo {
    std::string name;
    int node = -1;
    bool orthographic = false;
    double focalLength = 50, horizontalAperture = 20.955, verticalAperture = 15.2908;
    double horizontalOffset = 0, verticalOffset = 0;
    double nearClip = 1, farClip = 1e6;  // scene units
    std::vector<double> focalTimes, focalLengths;  // seconds into its animation, when the focal length changes
    double focalAt(double seconds) const;
};

// A light in the file, at its node (shining along the node's -z where it has a direction). Its colour is linear,
// times its intensity (and 2 to the power of its exposure).
struct ModelLightInfo {
    std::string name;
    int node = -1;
    // Point: a glTF point or spot light, its colour the light's intensity (illuminance 1 unit away). The others are
    // USD's (UsdLux): their colour is the luminance of their surface (of the sun's disc for a distant light).
    enum class Kind { Distant, Point, Sphere, Rect, Disk, Cylinder, Dome } kind = Kind::Distant;
    std::array<float, 3> color{1, 1, 1};
    bool normalize = false;  // the light's power stays the same whatever its size
    double radius = 0.5, width = 1, height = 1, length = 1;
    double angle = 0;  // a distant light's angular size (degrees; 0: a point in the sky)
    double coneAngle = 180, coneSoftness = 0;  // degrees from its axis (180: no cone)
    bool shadows = true;
    // A dome's picture of the light all round (linear RGB, latitude-longitude, top row up; empty: its colour alone).
    int pictureWidth = 0, pictureHeight = 0;
    std::vector<float> picture;
};

struct Model {
    std::string path;
    std::vector<ModelMesh> meshes;
    std::vector<ModelMaterial> materials;
    std::vector<ModelTexture> textures;
    std::vector<ModelNode> nodes;
    std::vector<int> roots;  // the scene's top nodes
    std::vector<ModelSkin> skins;
    std::vector<ModelAnimation> animations;
    std::vector<ModelCameraInfo> cameras;
    std::vector<ModelLightInfo> lights;
    std::vector<std::string> notes;  // what the file holds that is not read (listed on import)
    size_t triangles() const;
};

// glTF 2.0 or GLB (by its contents), else OBJ. False with `error` when it cannot be read.
bool loadModel(const std::string& path, Model& out, std::string* error = nullptr);
// The same, kept while it and the files it was read from are unchanged (the last few models asked for).
std::shared_ptr<const Model> cachedModel(const std::string& path, std::string* error = nullptr);
// Whether `path` names a model file Montage reads (by extension: .gltf, .glb, .obj, .usd, .usda, .usdc, .usdz).
bool isModelPath(const std::string& path);

// Each node's transform from the model's space at `seconds` into animation `animation` (-1: none, the rest pose;
// past its end: its last keys), and the morph weights of each node with a mesh.
struct ModelPose {
    std::vector<Mat4> world;                   // per node
    std::vector<std::vector<float>> weights;   // per node
};
ModelPose poseModel(const Model& m, int animation, double seconds);

// A file's camera on a w x h picture of square pixels: its focal length and the picture point straight ahead of it
// (pixels), the film back fitted so all of it shows (matched across the picture's height when the picture is wider
// than the film back, else across its width) and shifted by its offsets.
void modelCameraLens(const ModelCameraInfo& c, double seconds, double w, double h, double& f, double& cx, double& cy);
// Where a file's camera is in this pose (model space): its position and its right, up and backward axes (unit
// length; it looks along -backward). False when its node is missing or flat.
bool modelCameraPlace(const ModelPose& pose, const ModelCameraInfo& c, std::array<double, 3>& position,
                      std::array<std::array<double, 3>, 3>& axes);

// The model's triangles posed: positions, normals, uvs and tangents in the model's space, a material each.
struct PosedTriangles {
    // As ModelPrimitive's, flattened over meshes, every one filled (uvs 0, tangents 0 and colours 1 where a primitive
    // has none).
    std::vector<float> positions, normals, uvs, tangents, colors;
    std::vector<uint32_t> indices;
    std::vector<int> materials;  // one a triangle (-1: the default material)
};
PosedTriangles posedTriangles(const Model& m, const ModelPose& pose);

// The box the posed model fills (min, max), its own space; false when it has no triangles.
bool modelBounds(const PosedTriangles& t, std::array<double, 3>& lo, std::array<double, 3>& hi);

// For the readers: a model's file read whole (as the model cache tracks it; empty when it cannot be), a picture's bytes
// decoded into a texture, and a USD scene read (media/UsdModel.cpp).
std::string readModelFile(const std::string& path);
bool decodeModelTexture(const std::string& bytes, bool srgb, ModelTexture& out);
void buildModelTextureMips(ModelTexture& t);  // its smaller levels made again from its first
bool loadUsdModel(const std::string& path, Model& out, std::string* error);

}  // namespace montage
