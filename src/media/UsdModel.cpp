// Montage — USD scenes as models (media/Model3D.h): the stage's prims become the node tree (their transform
// operations kept as they are, time samples as the scene's one animation), meshes and the simple shapes (cube, sphere,
// cylinder, cone, capsule, plane) become meshes with their normals, uvs and colours, UsdPreviewSurface materials
// become metallic-roughness materials, and cameras and lights (UsdLux) are kept for the model clip to look through
// and light by. UsdSkel skeletons skin their meshes, with their animation and blend shapes. A Z-up stage is turned to
// stand Y-up, as glTF does. What is not read is listed in the model's notes.
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <tuple>

#include "Exr.h"
#include "Model3D.h"
#include "Usd.h"

namespace montage {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kMaxCorners = size_t(1) << 25;  // face corners a model may unroll to
constexpr size_t kMaxMorphValues = size_t(1) << 26;
constexpr int kMaxDomeWidth = 1024;

// A number from the file as an int (-1 when it is not one, or out of range).
int toInt(double d) { return d >= -2147483648.0 && d <= 2147483647.0 ? int(d) : -1; }

// A primvar: its values, how they spread over the mesh, and its indices when it has them.
struct Primvar {
    usd::Value values;
    std::vector<int> indices;
    std::string interpolation = "constant";
    int elementSize = 1;
    bool valid() const { return values.kind == usd::Value::Kind::Number && !values.numbers.empty(); }
    // The element at one face corner (face, its point, the corner's place in all corners), or null (also when it has
    // fewer than `need` numbers).
    const double* at(size_t face, size_t point, size_t corner, size_t need) const {
        size_t e = interpolation == "constant" ? 0 : interpolation == "uniform" ? face : interpolation == "faceVarying" ? corner : point;
        if (!indices.empty()) {
            if (e >= indices.size() || indices[e] < 0) return nullptr;
            e = size_t(indices[e]);
        }
        const size_t w = size_t(std::max(1, values.width)) * size_t(std::clamp(elementSize, 1, 64));
        return w >= need && e < values.numbers.size() / w ? &values.numbers[e * w] : nullptr;
    }
};

// A mesh as polygons, ready to be cut into triangles: per point positions, per corner normals, uvs and colours (any
// may be empty), per face a material.
struct Polygons {
    std::vector<double> points;
    std::vector<int> counts, indices;
    std::vector<float> normals, uvs, colors;  // 3, 2 and 4 a corner
    std::vector<int> faceMaterial;
    std::vector<char> hole;
    bool smooth = true;  // with no normals: smooth (shared points) or flat
    std::vector<std::vector<double>> pointSamples;  // animated points: each time sample's positions
    std::vector<double> sampleTimes;                // their times (seconds)
};

// A colour temperature's light (Kelvin) in linear Rec.709, at luminance 1: the Planckian locus by Kim et al.'s
// cubic fit, as USD's lights use it.
std::array<float, 3> blackbody(double kelvin) {
    const double T = std::clamp(kelvin, 1667.0, 25000.0);
    const double x = T <= 4000 ? -0.2661239e9 / (T * T * T) - 0.2343589e6 / (T * T) + 0.8776956e3 / T + 0.179910
                               : -3.0258469e9 / (T * T * T) + 2.1070379e6 / (T * T) + 0.2226347e3 / T + 0.240390;
    double y;
    if (T <= 2222) y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683;
    else if (T <= 4000) y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
    else y = 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
    const double X = x / y, Y = 1, Z = (1 - x - y) / y;
    const double r = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z, g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z,
                 b = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
    return {float(std::max(0.0, r)), float(std::max(0.0, g)), float(std::max(0.0, b))};
}

// A Radiance .hdr picture (RGBE, flat or run-length encoded, top row first) as linear RGB.
bool decodeRgbe(const std::string& b, int& w, int& h, std::vector<float>& rgb) {
    size_t p = b.find("\n\n");
    if (b.compare(0, 2, "#?") != 0 || p == std::string::npos) return false;
    p += 2;
    const size_t eol = b.find('\n', p);
    if (eol == std::string::npos) return false;
    char ya[3] = {}, xa[3] = {};
    if (std::sscanf(b.substr(p, eol - p).c_str(), "%2s %d %2s %d", ya, &h, xa, &w) != 4 || std::string(ya) != "-Y" || std::string(xa) != "+X") return false;
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768 || int64_t(w) * h > (int64_t(1) << 26)) return false;
    p = eol + 1;
    // (no smaller than its rows can be, run-length encoded: nothing is set aside for a picture the file cannot hold)
    if (b.size() - p < size_t(h) * (w >= 8 ? 4 + 8 * size_t((w + 127) / 128) : size_t(w) * 4)) return false;
    rgb.assign(size_t(w) * size_t(h) * 3, 0);
    std::vector<uint8_t> line(size_t(w) * 4);
    auto u = [&](size_t i) { return i < b.size() ? uint8_t(b[i]) : uint8_t(0); };
    for (int y = 0; y < h; ++y) {
        if (w >= 8 && w < 32768 && u(p) == 2 && u(p + 1) == 2 && !(u(p + 2) & 0x80)) {
            p += 4;
            for (int c = 0; c < 4; ++c)
                for (int x = 0; x < w;) {
                    if (p >= b.size()) return false;
                    int n = u(p++);
                    if (n > 128) {
                        n -= 128;
                        const uint8_t v = u(p++);
                        for (int k = 0; k < n && x < w; ++k) line[size_t(x++) * 4 + size_t(c)] = v;
                    } else {
                        for (int k = 0; k < n && x < w; ++k) line[size_t(x++) * 4 + size_t(c)] = u(p++);
                    }
                }
        } else {
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c) line[size_t(x) * 4 + size_t(c)] = u(p++);
        }
        if (p > b.size()) return false;
        for (int x = 0; x < w; ++x) {
            const uint8_t* q = &line[size_t(x) * 4];
            const float f = q[3] ? std::ldexp(1.0f, int(q[3]) - 136) : 0.0f;
            for (int c = 0; c < 3; ++c) rgb[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(c)] = float(q[c]) * f;
        }
    }
    return true;
}

class Converter {
public:
    Converter(const usd::Stage& st, Model& m) : st_(st), m_(m) {
        tcps_ = st.layer.timeCodesPerSecondOr24();
        start_ = st.layer.hasTimeRange ? st.layer.startTimeCode : 0;
    }

    bool run(std::string* error) {
        int top = -1;
        if (st_.layer.upAxis == "Z") {  // stood Y up: +z becomes +y
            ModelNode n;
            n.name = "Z up";
            n.rotation = {-std::sqrt(0.5), 0, 0, std::sqrt(0.5)};
            m_.nodes.push_back(n);
            top = 0;
            m_.roots.push_back(0);
        }
        for (const usd::Prim& p : st_.layer.root.children) walk(p, "/" + p.name, top, Inherited{}, 0);
        skins();
        if (animation_ >= 0) {
            ModelAnimation& a = m_.animations[size_t(animation_)];
            if (st_.layer.hasTimeRange && st_.layer.endTimeCode > st_.layer.startTimeCode)
                a.duration = std::max(a.duration, (st_.layer.endTimeCode - st_.layer.startTimeCode) / tcps_);
        }
        for (const std::string& n : st_.notes) note(n);
        if (corners_ > kMaxCorners) {
            if (error) *error = st_.path + " is larger than Montage can load";
            return false;
        }
        return true;
    }

private:
    const usd::Stage& st_;
    Model& m_;
    double tcps_ = 24, start_ = 0;
    int animation_ = -1;
    size_t corners_ = 0;
    std::map<std::pair<std::string, bool>, int> textures_;
    struct MaterialInfo {
        int index = -1;
        std::string st = "st";
        bool transform = false;
        double rotation = 0, scale[2] = {1, 1}, translation[2] = {0, 0};
    };
    std::map<std::string, MaterialInfo> materials_;
    std::map<std::pair<int, bool>, int> sided_;
    std::map<std::string, int> plain_;  // materials made for a display colour
    std::map<std::string, int> nodeOf_;  // prim path to node
    struct SkinnedMesh {
        int node = -1, mesh = -1;
        std::string skeleton;  // its Skeleton's path
        std::vector<int> jointIndices;
        std::vector<float> jointWeights;
        int influences = 0;
        Mat4 geomBind = identity4();
        std::vector<std::string> blendShapes, blendTargets;
        std::vector<int> cornerPoints;  // each unrolled vertex's point
        std::string path;
    };
    std::vector<SkinnedMesh> skinned_;
    std::map<std::string, const usd::Prim*> skeletons_;  // path to Skeleton prim
    std::map<std::string, int> skeletonNodes_;
    std::map<std::string, std::string> skelAnimation_;  // Skeleton path to its SkelAnimation path
    // Each mesh's vertices in the order makeMesh() made them: (primitive, vertex) for every kept corner, face by face.
    std::map<int, std::vector<std::pair<size_t, size_t>>> cornerOrder_;

    struct Inherited {
        std::string binding;
        std::string skeleton, animation;
        std::string purpose = "default";
    };

    void note(const std::string& n) {
        if (std::find(m_.notes.begin(), m_.notes.end(), n) == m_.notes.end() && m_.notes.size() < 64) m_.notes.push_back(n);
    }
    double seconds(double tc) const { return (tc - start_) / tcps_; }
    ModelAnimation& animation() {
        if (animation_ < 0) {
            ModelAnimation a;
            a.name = "Scene";
            m_.animations.push_back(a);
            animation_ = int(m_.animations.size()) - 1;
        }
        return m_.animations[size_t(animation_)];
    }
    // A sampler for these time samples (each `components` long), in the scene's animation.
    int sampler(const std::vector<std::pair<double, usd::Value>>& samples, size_t components) {
        ModelAnimation& a = animation();
        ModelAnimation::Sampler s;
        s.components = int(components);
        for (const auto& [t, v] : samples) {
            if (v.numbers.size() < components) continue;
            s.times.push_back(float(seconds(t)));
            for (size_t k = 0; k < components; ++k) s.values.push_back(float(v.numbers[k]));
        }
        if (s.times.empty()) return -1;
        a.duration = std::max(a.duration, double(s.times.back()));
        a.samplers.push_back(std::move(s));
        return int(a.samplers.size()) - 1;
    }
    static const usd::Attribute* attr(const usd::Prim& p, const std::string& name) {
        const usd::Attribute* a = p.attribute("inputs:" + name);
        return a ? a : p.attribute(name);
    }
    double number(const usd::Prim& p, const std::string& name, double fallback) const {
        const usd::Attribute* a = attr(p, name);
        return a ? a->at(start_).number(0, fallback) : fallback;
    }
    std::string token(const usd::Prim& p, const std::string& name, const std::string& fallback = {}) const {
        const usd::Attribute* a = attr(p, name);
        const std::string t = a ? a->at(start_).text() : std::string();
        return t.empty() ? fallback : t;
    }
    std::vector<double> numbers(const usd::Prim& p, const std::string& name) const {
        const usd::Attribute* a = p.attribute(name);
        return a ? a->at(start_).numbers : std::vector<double>{};
    }
    Primvar primvar(const usd::Prim& p, const std::string& name, const char* defaultInterpolation = "constant") const {
        Primvar v;
        const usd::Attribute* a = p.attribute("primvars:" + name);
        if (!a) return v;
        v.values = a->at(start_);
        v.interpolation = a->metaText("interpolation", defaultInterpolation);
        if (const auto it = a->meta.find("elementSize"); it != a->meta.end()) v.elementSize = std::clamp(toInt(it->second.number(0, 1)), 1, 64);
        if (const usd::Attribute* ix = p.attribute("primvars:" + name + ":indices"))
            for (double d : ix->at(start_).numbers) v.indices.push_back(toInt(d));
        return v;
    }

    // ---- The tree ---------------------------------------------------------------------------------------------------

    void walk(const usd::Prim& p, const std::string& path, int parent, Inherited inh, int depth) {
        if (depth > 256 || !p.active || p.specifier != usd::Prim::Specifier::Def) return;  // (an over never defined is not drawn)
        const std::string& type = p.typeName;
        if (type == "Material" || type == "Shader" || type == "NodeGraph" || type == "GeomSubset" || type == "SkelAnimation" || type == "BlendShape") return;
        if (token(p, "visibility") == "invisible") return;
        if (const std::string purpose = token(p, "purpose"); !purpose.empty()) inh.purpose = purpose;
        if (inh.purpose == "guide" || inh.purpose == "proxy") return;
        for (const char* rel : {"material:binding:preview", "material:binding"})
            if (const auto* r = p.relationship(rel); r && !r->empty()) {
                inh.binding = r->front();
                break;
            }
        for (const auto& [name, targets] : p.relationships)
            if (name.rfind("material:binding:collection", 0) == 0) note("Collection-based material bindings are not read");
        if (const auto* r = p.relationship("skel:skeleton"); r && !r->empty()) inh.skeleton = r->front();
        if (const auto* r = p.relationship("skel:animationSource"); r && !r->empty()) inh.animation = r->front();

        const int node = int(m_.nodes.size());
        ModelNode n;
        n.name = p.name;
        n.parent = parent;
        m_.nodes.push_back(n);
        nodeOf_[path] = node;
        if (parent >= 0) m_.nodes[size_t(parent)].children.push_back(node);
        else m_.roots.push_back(node);
        transform(p, node);

        if (type == "Mesh") mesh(p, path, node, inh);
        else if (type == "Cube" || type == "Sphere" || type == "Cylinder" || type == "Cone" || type == "Capsule" || type == "Plane") shape(p, node, inh);
        else if (type == "Camera") camera(p, node);
        else if (type.size() > 5 && type.compare(type.size() - 5, 5, "Light") == 0) light(p, node, type);
        else if (type.rfind("DomeLight", 0) == 0) light(p, node, "DomeLight");
        else if (type == "Skeleton") {
            skeletons_[path] = &p;
            skeletonNodes_[path] = node;
            if (!inh.animation.empty()) skelAnimation_[path] = inh.animation;
            if (const auto* r = p.relationship("skel:animationSource"); r && !r->empty()) skelAnimation_[path] = r->front();
        } else if (type == "PointInstancer") note("Point instancers are not drawn");
        else if (type == "Points" || type == "BasisCurves" || type == "NurbsCurves" || type == "NurbsPatch" || type == "HermiteCurves" || type == "TetMesh" || type == "Volume")
            note(type + " prims are not drawn");
        for (const usd::Prim& c : p.children) walk(c, path + "/" + c.name, node, inh, depth + 1);
    }

    void transform(const usd::Prim& p, int node) {
        const usd::Attribute* order = p.attribute("xformOpOrder");
        if (!order) return;
        ModelNode& n = m_.nodes[size_t(node)];
        for (std::string name : order->at(start_).texts) {
            if (name == "!resetXformStack!") {  // placed in the world (stood up still when the stage is Z up)
                n.resetsParent = true;
                n.ops.clear();
                if (animation_ >= 0) {
                    auto& ch = m_.animations[size_t(animation_)].channels;
                    ch.erase(std::remove_if(ch.begin(), ch.end(), [&](const ModelAnimation::Channel& c) { return c.node == node && c.path == ModelAnimation::Channel::Path::Op; }), ch.end());
                }
                if (st_.layer.upAxis == "Z") {
                    ModelXformOp up;
                    up.kind = ModelXformOp::Kind::Orient;
                    up.value = {-std::sqrt(0.5), 0, 0, std::sqrt(0.5)};
                    n.ops.push_back(up);
                }
                continue;
            }
            ModelXformOp op;
            if (name.rfind("!invert!", 0) == 0) op.inverse = true, name = name.substr(8);
            if (name.rfind("xformOp:", 0) != 0) continue;
            std::string kind = name.substr(8);
            kind = kind.substr(0, kind.find(':'));
            static const std::map<std::string, ModelXformOp::Kind> kinds = {
                {"translate", ModelXformOp::Kind::Translate}, {"scale", ModelXformOp::Kind::Scale},
                {"rotateX", ModelXformOp::Kind::RotateX}, {"rotateY", ModelXformOp::Kind::RotateY}, {"rotateZ", ModelXformOp::Kind::RotateZ},
                {"rotateXYZ", ModelXformOp::Kind::RotateXYZ}, {"rotateXZY", ModelXformOp::Kind::RotateXZY}, {"rotateYXZ", ModelXformOp::Kind::RotateYXZ},
                {"rotateYZX", ModelXformOp::Kind::RotateYZX}, {"rotateZXY", ModelXformOp::Kind::RotateZXY}, {"rotateZYX", ModelXformOp::Kind::RotateZYX},
                {"orient", ModelXformOp::Kind::Orient}, {"transform", ModelXformOp::Kind::Transform}};
            const auto k = kinds.find(kind);
            if (k == kinds.end()) {
                note("A transform operation (" + kind + ") is not read");
                continue;
            }
            op.kind = k->second;
            const usd::Attribute* a = p.attribute(name);
            if (!a) continue;  // (no value: no change)
            op.value = a->at(start_).numbers;
            if (op.kind == ModelXformOp::Kind::Scale && op.value.size() == 1) op.value.assign(3, op.value[0]);
            n.ops.push_back(op);
            if (a->animated()) {
                const size_t components = op.value.size();
                const int s = sampler(a->samples, components);
                if (s >= 0) {
                    ModelAnimation::Channel c;
                    c.node = node;
                    c.sampler = s;
                    c.path = ModelAnimation::Channel::Path::Op;
                    c.op = int(n.ops.size()) - 1;
                    animation().channels.push_back(c);
                }
            }
        }
    }

    // ---- Materials --------------------------------------------------------------------------------------------------

    // Where a shader input comes from: a value, or another shader's output (following a material's or node graph's
    // inputs and outputs it is connected through).
    struct Source {
        const usd::Prim* shader = nullptr;
        std::string output;
        usd::Value value;
        bool has() const { return shader || !value.empty(); }
    };
    Source source(const usd::Prim& prim, const std::string& property, int depth) const {
        Source s;
        if (depth > 16) return s;
        const usd::Attribute* a = prim.attribute(property);
        if (!a) return s;
        if (!a->connections.empty()) {
            const std::string& to = a->connections.front();
            const size_t dot = to.rfind('.');
            const usd::Prim* t = dot == std::string::npos ? nullptr : st_.prim(to.substr(0, dot));
            if (t) {
                const std::string name = to.substr(dot + 1);
                if (t->typeName == "Shader" && name.rfind("outputs:", 0) == 0) {
                    s.shader = t;
                    s.output = name.substr(8);
                    return s;
                }
                return source(*t, name, depth + 1);  // (through a material's or node graph's interface)
            }
        }
        s.value = a->at(start_);
        return s;
    }
    std::string shaderId(const usd::Prim& s) const { return token(s, "info:id"); }

    // A texture's index for this file and colour use (-1 when it cannot be read).
    int texture(const std::string& file, bool srgb, const std::string& wrapS, const std::string& wrapT) {
        const auto key = std::make_pair(file + "|" + wrapS + "|" + wrapT, srgb);
        if (const auto it = textures_.find(key); it != textures_.end()) return it->second;
        std::string bytes;
        ModelTexture t;
        int id = -1;
        if (file.find("<UDIM>") != std::string::npos) note("UDIM textures are not read");
        else if (QFileInfo(QString::fromStdString(file.substr(0, file.find('[')))).suffix().toLower() == "exr") note("EXR textures on surfaces are not read");
        else if (st_.readAsset(file, bytes) && decodeModelTexture(bytes, srgb, t)) {
            auto wrap = [](const std::string& w) {
                return w == "mirror" ? ModelTexture::Wrap::Mirror : (w == "clamp" || w == "black") ? ModelTexture::Wrap::Clamp : ModelTexture::Wrap::Repeat;
            };
            t.wrapS = wrap(wrapS), t.wrapT = wrap(wrapT);
            id = int(m_.textures.size());
            m_.textures.push_back(std::move(t));
        } else {
            note("Cannot read the texture " + file);
        }
        textures_[key] = id;
        return id;
    }

    // A texture input: its file read, the channel the output takes, the texture's scale and its uv reader.
    struct TextureUse {
        int texture = -1;
        int channel = 0;  // r 0, g 1, b 2, a 3 (rgb 0)
        std::array<double, 4> scale{1, 1, 1, 1}, bias{0, 0, 0, 0};
        const usd::Prim* shader = nullptr;
    };
    TextureUse textureUse(const Source& s, bool srgbIfAuto) {
        TextureUse u;
        if (!s.shader || shaderId(*s.shader) != "UsdUVTexture") return u;
        const usd::Prim& t = *s.shader;
        u.shader = &t;
        const std::string file = token(t, "file");
        if (file.empty()) return u;
        const std::string space = token(t, "sourceColorSpace", "auto");
        const bool srgb = space == "sRGB" || (space == "auto" && srgbIfAuto);
        u.texture = texture(file, srgb, token(t, "wrapS", "useMetadata"), token(t, "wrapT", "useMetadata"));
        u.channel = s.output == "g" ? 1 : s.output == "b" ? 2 : s.output == "a" ? 3 : 0;
        const std::vector<double> sc = attr(t, "scale") ? attr(t, "scale")->at(start_).numbers : std::vector<double>{};
        const std::vector<double> bi = attr(t, "bias") ? attr(t, "bias")->at(start_).numbers : std::vector<double>{};
        for (size_t k = 0; k < 4; ++k) {
            if (k < sc.size()) u.scale[k] = sc[k];
            if (k < bi.size()) u.bias[k] = bi[k];
        }
        return u;
    }

    // A texture made of channels taken from others (each scaled), at the largest one's size; G and B by default
    // filled with `fill`. For the metallic-roughness texture glTF's materials use.
    int combined(const std::array<TextureUse, 4>& from, const std::array<double, 4>& fill) {
        int w = 1, h = 1;
        for (const TextureUse& u : from)
            if (u.texture >= 0) w = std::max(w, m_.textures[size_t(u.texture)].levels[0].width), h = std::max(h, m_.textures[size_t(u.texture)].levels[0].height);
        w = std::min(w, 4096), h = std::min(h, 4096);
        ModelTexture t;
        t.srgb = false;
        ModelTexture::Level l;
        l.width = w, l.height = h;
        l.rgba.resize(size_t(w) * size_t(h) * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c) {
                    double v = fill[size_t(c)];
                    const TextureUse& u = from[size_t(c)];
                    if (u.texture >= 0) {
                        const ModelTexture& src = m_.textures[size_t(u.texture)];
                        const ModelTexture::Level& s = src.levels[0];
                        const int sx = std::min(s.width - 1, int(int64_t(x) * s.width / w)), sy = std::min(s.height - 1, int(int64_t(y) * s.height / h));
                        v = src.linear(s.rgba[(size_t(sy) * size_t(s.width) + size_t(sx)) * 4 + size_t(u.channel)], u.channel) * u.scale[size_t(u.channel)] +
                            u.bias[size_t(u.channel)];
                    }
                    l.rgba[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(c)] = uint8_t(std::lround(std::clamp(v, 0.0, 1.0) * 255));
                }
        t.levels.push_back(std::move(l));
        buildModelTextureMips(t);
        for (const TextureUse& u : from)
            if (u.texture >= 0) t.wrapS = m_.textures[size_t(u.texture)].wrapS, t.wrapT = m_.textures[size_t(u.texture)].wrapT;
        m_.textures.push_back(std::move(t));
        return int(m_.textures.size()) - 1;
    }

    // The material at `path` (a UsdPreviewSurface), made once.
    const MaterialInfo& material(const std::string& path) {
        if (const auto it = materials_.find(path); it != materials_.end()) return it->second;
        MaterialInfo& info = materials_[path];
        const usd::Prim* mat = st_.prim(path);
        if (!mat || mat->typeName != "Material") {
            note("A material binding names " + path + ", which is not a material");
            return info;
        }
        // Its surface: the UsdPreviewSurface its outputs lead to.
        const usd::Prim* surface = nullptr;
        for (const auto& [name, a] : mat->attributes) {
            if (name.rfind("outputs:", 0) != 0 || name.find("surface") == std::string::npos) continue;
            const Source s = source(*mat, name, 0);
            if (s.shader && shaderId(*s.shader) == "UsdPreviewSurface" && (!surface || name == "outputs:surface")) surface = s.shader;
        }
        ModelMaterial mm;
        mm.name = mat->name;
        mm.baseColor = {0.18f, 0.18f, 0.18f, 1};  // (UsdPreviewSurface's fallbacks)
        mm.metallic = 0, mm.roughness = 0.5f;
        if (!surface) {
            note("Material " + mat->name + " has no UsdPreviewSurface (MaterialX and other shading are not read)");
            info.index = int(m_.materials.size());
            m_.materials.push_back(mm);
            return info;
        }
        const usd::Prim& sh = *surface;
        auto in = [&](const char* name) { return source(sh, std::string("inputs:") + name, 0); };
        auto uvOf = [&](const TextureUse& u) {  // the primvar the texture reads, and any transform on the way
            if (!u.shader) return;
            Source st = source(*u.shader, "inputs:st", 0);
            for (int k = 0; k < 4 && st.shader; ++k) {
                const std::string id = shaderId(*st.shader);
                if (id == "UsdTransform2d") {
                    info.transform = true;
                    info.rotation = number(*st.shader, "rotation", 0);
                    const std::vector<double> sc = attr(*st.shader, "scale") ? attr(*st.shader, "scale")->at(start_).numbers : std::vector<double>{};
                    const std::vector<double> tr = attr(*st.shader, "translation") ? attr(*st.shader, "translation")->at(start_).numbers : std::vector<double>{};
                    if (sc.size() >= 2) info.scale[0] = sc[0], info.scale[1] = sc[1];
                    if (tr.size() >= 2) info.translation[0] = tr[0], info.translation[1] = tr[1];
                    st = source(*st.shader, "inputs:in", 0);
                    continue;
                }
                if (id.rfind("UsdPrimvarReader", 0) == 0) {
                    const Source var = source(*st.shader, "inputs:varname", 0);
                    if (!var.value.text().empty()) info.st = var.value.text();
                }
                break;
            }
        };
        auto constant = [](const Source& s, size_t n, std::array<float, 4>& out) {
            if (s.shader || s.value.kind != usd::Value::Kind::Number) return false;
            for (size_t k = 0; k < n && k < s.value.numbers.size(); ++k) out[k] = float(s.value.numbers[k]);
            return true;
        };
        std::array<float, 4> v{};
        // Base colour and opacity.
        const Source diffuse = in("diffuseColor");
        TextureUse base;
        if (constant(diffuse, 3, v)) mm.baseColor = {v[0], v[1], v[2], 1};
        else if ((base = textureUse(diffuse, true)).texture >= 0) {
            mm.baseTexture = base.texture;
            mm.baseColor = {float(base.scale[0]), float(base.scale[1]), float(base.scale[2]), 1};
            uvOf(base);
        }
        const Source opacity = in("opacity");
        const double threshold = number(sh, "opacityThreshold", 0);
        bool seeThrough = false;
        if (constant(opacity, 1, v)) {
            mm.baseColor[3] = v[0];
            seeThrough = v[0] < 1;
        } else if (const TextureUse o = textureUse(opacity, false); o.texture >= 0) {
            seeThrough = true;
            if (base.texture >= 0 && o.shader == base.shader && o.channel == 3) {
                mm.baseColor[3] = float(o.scale[3]);
            } else {  // the opacity put in the base texture's alpha
                TextureUse rgb = base;
                std::array<TextureUse, 4> parts{rgb, rgb, rgb, o};
                parts[1].channel = 1, parts[2].channel = 2;
                if (base.texture >= 0) {
                    const int t = combined(parts, {0, 0, 0, 1});
                    m_.textures[size_t(t)].srgb = false;  // (the colour is already linear: kept so)
                    mm.baseTexture = t;
                } else {
                    std::array<TextureUse, 4> alpha{};
                    alpha[3] = o;
                    mm.baseTexture = combined(alpha, {1, 1, 1, 1});
                }
                uvOf(o);
            }
        }
        if (seeThrough) {
            mm.alpha = threshold > 0 ? ModelMaterial::Alpha::Mask : ModelMaterial::Alpha::Blend;
            mm.alphaCutoff = float(threshold);
        }
        // Metallic and roughness.
        const Source metallic = in("metallic"), roughness = in("roughness");
        const TextureUse mt = textureUse(metallic, false), rt = textureUse(roughness, false);
        if (constant(metallic, 1, v)) mm.metallic = v[0];
        if (constant(roughness, 1, v)) mm.roughness = v[0];
        if (mt.texture >= 0 || rt.texture >= 0) {
            std::array<TextureUse, 4> parts{};
            parts[1] = rt, parts[2] = mt;
            mm.metallicRoughnessTexture = combined(parts, {1, mm.roughness, mm.metallic, 1});
            mm.metallic = 1, mm.roughness = 1;
            uvOf(rt.texture >= 0 ? rt : mt);
        }
        if (number(sh, "useSpecularWorkflow", 0) > 0.5) {
            note("Material " + mat->name + " uses the specular workflow: drawn as not metallic");
            mm.metallic = 0;
        }
        // Emission and normals.
        const Source emissive = in("emissiveColor");
        if (constant(emissive, 3, v)) mm.emissive = {v[0], v[1], v[2]};
        else if (const TextureUse e = textureUse(emissive, true); e.texture >= 0) {
            mm.emissiveTexture = e.texture;
            mm.emissive = {float(e.scale[0]), float(e.scale[1]), float(e.scale[2])};
            uvOf(e);
        }
        if (const TextureUse n = textureUse(in("normal"), false); n.texture >= 0) {
            mm.normalTexture = n.texture;
            uvOf(n);
        }
        if (const Source d = in("displacement"); d.shader || d.value.number(0, 0) != 0) note("Displacement is not drawn");
        info.index = int(m_.materials.size());
        m_.materials.push_back(mm);
        return info;
    }
    int sidedMaterial(int index, bool doubleSided) {
        if (index < 0 || !doubleSided) return index;
        const auto key = std::make_pair(index, doubleSided);
        if (const auto it = sided_.find(key); it != sided_.end()) return it->second;
        ModelMaterial copy = m_.materials[size_t(index)];
        copy.doubleSided = true;
        m_.materials.push_back(copy);
        return sided_[key] = int(m_.materials.size()) - 1;
    }
    // A plain material in a display colour (for a mesh with no material), made once a colour.
    int plainMaterial(const std::array<float, 4>& c, bool doubleSided) {
        const std::string key = std::to_string(c[0]) + "," + std::to_string(c[1]) + "," + std::to_string(c[2]) + "," + std::to_string(c[3]) + (doubleSided ? "d" : "");
        if (const auto it = plain_.find(key); it != plain_.end()) return it->second;
        ModelMaterial mm;
        mm.name = "Display colour";
        mm.baseColor = c;
        mm.metallic = 0, mm.roughness = 0.5f;
        mm.doubleSided = doubleSided;
        if (c[3] < 1) mm.alpha = ModelMaterial::Alpha::Blend;
        m_.materials.push_back(mm);
        return plain_[key] = int(m_.materials.size()) - 1;
    }

    // ---- Geometry ---------------------------------------------------------------------------------------------------

    void mesh(const usd::Prim& p, const std::string& path, int node, const Inherited& inh) {
        Polygons g;
        const usd::Attribute* pointsAttr = p.attribute("points");
        if (!pointsAttr) return;
        g.points = pointsAttr->at(start_).numbers;
        for (double d : numbers(p, "faceVertexCounts")) g.counts.push_back(toInt(d));
        for (double d : numbers(p, "faceVertexIndices")) g.indices.push_back(toInt(d));
        const size_t np = g.points.size() / 3;
        size_t total = 0;
        for (int c : g.counts) total += size_t(std::max(0, c));
        if (std::any_of(g.counts.begin(), g.counts.end(), [](int c) { return c < 0; }) || total != g.indices.size() || std::any_of(g.indices.begin(), g.indices.end(), [&](int i) { return i < 0 || size_t(i) >= np; })) {
            note("Mesh " + p.name + " has faces that do not match its points: not drawn");
            return;
        }
        if ((corners_ += total) > kMaxCorners) return;
        g.hole.assign(g.counts.size(), 0);
        for (double d : numbers(p, "holeIndices"))
            if (d >= 0 && d < double(g.hole.size())) g.hole[size_t(d)] = 1;
        std::vector<size_t> authored(total);  // each corner's place as authored (left-handed faces are turned round)
        for (size_t k = 0; k < total; ++k) authored[k] = k;
        if (token(p, "orientation") == "leftHanded")
            for (size_t f = 0, at = 0; f < g.counts.size(); at += size_t(g.counts[f++])) {
                std::reverse(g.indices.begin() + long(at), g.indices.begin() + long(at) + g.counts[f]);
                std::reverse(authored.begin() + long(at), authored.begin() + long(at) + g.counts[f]);
            }
        const std::string scheme = token(p, "subdivisionScheme", "catmullClark");
        g.smooth = scheme != "none";
        if (scheme != "none") note("Subdivision surfaces are drawn as their control meshes");

        // Materials: the mesh's, or its face subsets'.
        std::vector<std::string> faceBinding(g.counts.size(), inh.binding);
        for (const usd::Prim& c : p.children) {
            if (c.typeName != "GeomSubset" || token(c, "elementType", "face") != "face") continue;
            std::string binding;
            for (const char* rel : {"material:binding:preview", "material:binding"})
                if (const auto* r = c.relationship(rel); r && !r->empty() && binding.empty()) binding = r->front();
            if (binding.empty()) continue;
            for (double d : numbers(c, "indices"))
                if (d >= 0 && d < double(faceBinding.size())) faceBinding[size_t(d)] = binding;
        }
        const bool doubleSided = number(p, "doubleSided", 0) > 0.5;
        std::string stName;
        g.faceMaterial.assign(g.counts.size(), -1);
        std::map<std::string, std::pair<int, const MaterialInfo*>> chosen;
        std::vector<const MaterialInfo*> faceInfo(g.counts.size(), nullptr);  // (for each face's uv transform)
        for (size_t f = 0; f < g.counts.size(); ++f) {
            const std::string& b = faceBinding[f];
            if (b.empty()) continue;
            auto it = chosen.find(b);
            if (it == chosen.end()) {
                const MaterialInfo& info = material(b);
                if (stName.empty()) stName = info.st;
                it = chosen.emplace(b, std::make_pair(sidedMaterial(info.index, doubleSided), &info)).first;
            }
            g.faceMaterial[f] = it->second.first;
            faceInfo[f] = it->second.second;
        }

        // Normals, uvs and colours, at each corner.
        Primvar normals = primvar(p, "normals", "vertex");
        if (!normals.valid())
            if (const usd::Attribute* a = p.attribute("normals")) normals.values = a->at(start_), normals.interpolation = a->metaText("interpolation", "vertex");
        Primvar uv = primvar(p, stName.empty() ? "st" : stName, "vertex");
        if (!uv.valid())
            for (const auto& [name, a] : p.attributes)
                if (name.rfind("primvars:", 0) == 0 && name.find(":indices") == std::string::npos &&
                    (a.typeName.rfind("texCoord2", 0) == 0 || (a.typeName == "float2[]" && (name == "primvars:st" || name == "primvars:uv")))) {
                    uv = primvar(p, name.substr(9), "vertex");
                    break;
                }
        const Primvar color = primvar(p, "displayColor"), opacity = primvar(p, "displayOpacity");
        const bool colored = color.valid() && color.interpolation != "constant";
        const bool anyMaterial = std::any_of(g.faceMaterial.begin(), g.faceMaterial.end(), [](int m) { return m >= 0; });
        for (size_t f = 0, corner = 0; f < g.counts.size(); ++f)
            for (int k = 0; k < g.counts[f]; ++k, ++corner) {
                const size_t point = size_t(g.indices[corner]), fv = authored[corner];
                if (normals.valid()) {
                    const double* n = normals.at(f, point, fv, 3);
                    g.normals.insert(g.normals.end(), {n ? float(n[0]) : 0.0f, n ? float(n[1]) : 0.0f, n ? float(n[2]) : 1.0f});
                }
                if (uv.valid()) {
                    const double* t = uv.at(f, point, fv, 2);
                    double s = t ? t[0] : 0, v = t ? t[1] : 0;
                    if (const MaterialInfo* mi = faceInfo[f]; mi && mi->transform) {
                        const double a = mi->rotation * kPi / 180, x = s * mi->scale[0], y = v * mi->scale[1];
                        s = x * std::cos(a) - y * std::sin(a) + mi->translation[0];
                        v = x * std::sin(a) + y * std::cos(a) + mi->translation[1];
                    }
                    g.uvs.insert(g.uvs.end(), {float(s), float(1 - v)});  // (USD's t runs up the picture)
                }
                if (colored && !anyMaterial) {
                    const double* c = color.at(f, point, fv, 3);
                    const double* o = opacity.valid() ? opacity.at(f, point, fv, 1) : nullptr;
                    g.colors.insert(g.colors.end(), {c ? float(c[0]) : 1.0f, c ? float(c[1]) : 1.0f, c ? float(c[2]) : 1.0f, o ? float(o[0]) : 1.0f});
                }
            }
        if (std::any_of(g.faceMaterial.begin(), g.faceMaterial.end(), [](int m) { return m < 0; })) {
            // Faces bound to no material: in the display colour (white under colours given point by point).
            std::array<float, 4> c{0.5f, 0.5f, 0.5f, 1};  // (USD's fallback display colour)
            const bool byPoint = colored && !anyMaterial;
            if (byPoint) c = {1, 1, 1, 1};
            else if (color.valid())
                if (const double* d = color.at(0, 0, 0, 3)) c = {float(d[0]), float(d[1]), float(d[2]), 1};
            if (!byPoint && opacity.valid())
                if (const double* d = opacity.at(0, 0, 0, 1)) c[3] = float(d[0]);
            const int plain = plainMaterial(c, doubleSided);
            for (int& m : g.faceMaterial)
                if (m < 0) m = plain;
        }

        // Points that move: kept as morph targets, one a time sample, weighted to blend between neighbours.
        if (pointsAttr->animated()) {
            if (pointsAttr->samples.size() * total * 3 > kMaxMorphValues) {
                note("Mesh " + p.name + " moves its points over too many samples: drawn still");
            } else {
                for (const auto& [t, v] : pointsAttr->samples)
                    if (v.numbers.size() == g.points.size()) g.pointSamples.push_back(v.numbers), g.sampleTimes.push_back(seconds(t));
                if (g.pointSamples.size() < 2) g.pointSamples.clear(), g.sampleTimes.clear();
            }
        }

        const int mesh = makeMesh(g, p.name, node);
        if (mesh < 0) return;
        // Skinned by a skeleton.
        if (!inh.skeleton.empty() && (p.attribute("primvars:skel:jointIndices") || p.relationship("skel:blendShapeTargets"))) {
            SkinnedMesh s;
            s.node = node, s.mesh = mesh, s.skeleton = inh.skeleton, s.path = path;
            const Primvar ji = primvar(p, "skel:jointIndices", "vertex"), jw = primvar(p, "skel:jointWeights", "vertex");
            s.influences = std::clamp(ji.elementSize, 1, 64);
            const bool constantSkin = ji.interpolation == "constant";
            for (double d : ji.values.numbers) s.jointIndices.push_back(toInt(d));
            for (double d : jw.values.numbers) s.jointWeights.push_back(float(d));
            if (constantSkin && !s.jointIndices.empty()) {  // (one set of influences for every point)
                std::vector<int> ji2;
                std::vector<float> jw2;
                for (size_t k = 0; k < np; ++k) {
                    ji2.insert(ji2.end(), s.jointIndices.begin(), s.jointIndices.begin() + std::min<long>(s.influences, long(s.jointIndices.size())));
                    jw2.insert(jw2.end(), s.jointWeights.begin(), s.jointWeights.begin() + std::min<long>(s.influences, long(s.jointWeights.size())));
                }
                s.jointIndices = std::move(ji2), s.jointWeights = std::move(jw2);
            }
            if (const usd::Attribute* gb = p.attribute("primvars:skel:geomBindTransform")) {
                const std::vector<double> v = gb->at(start_).numbers;
                if (v.size() == 16) std::copy(v.begin(), v.end(), s.geomBind.begin());
            }
            if (const usd::Attribute* bs = p.attribute("skel:blendShapes")) s.blendShapes = bs->at(start_).texts;
            if (const auto* r = p.relationship("skel:blendShapeTargets")) s.blendTargets = *r;
            for (size_t f = 0, corner = 0; f < g.counts.size(); ++f)
                for (int k = 0; k < g.counts[f]; ++k, ++corner)
                    if (!g.hole[f] && g.counts[f] >= 3) s.cornerPoints.push_back(g.indices[corner]);
            skinned_.push_back(std::move(s));
        }
    }

    // The polygons cut into triangles (fans), one primitive a material, each corner its own vertex; the mesh's
    // index (-1: nothing to draw).
    int makeMesh(const Polygons& g, const std::string& name, int node) {
        const size_t np = g.points.size() / 3;
        // Smooth normals where none are given: each point's faces' normals summed (by their areas).
        std::vector<double> pointNormals;
        if (g.normals.empty() && g.smooth) {
            pointNormals.assign(np * 3, 0);
            for (size_t f = 0, at = 0; f < g.counts.size(); at += size_t(std::max(0, g.counts[f++]))) {
                if (g.hole[f] || g.counts[f] < 3) continue;
                double n[3] = {0, 0, 0};
                for (int k = 0; k < g.counts[f]; ++k) {  // (Newell's normal)
                    const double* a = &g.points[size_t(g.indices[at + size_t(k)]) * 3];
                    const double* b = &g.points[size_t(g.indices[at + size_t((k + 1) % g.counts[f])]) * 3];
                    n[0] += (a[1] - b[1]) * (a[2] + b[2]), n[1] += (a[2] - b[2]) * (a[0] + b[0]), n[2] += (a[0] - b[0]) * (a[1] + b[1]);
                }
                for (int k = 0; k < g.counts[f]; ++k)
                    for (int c = 0; c < 3; ++c) pointNormals[size_t(g.indices[at + size_t(k)]) * 3 + size_t(c)] += n[c];
            }
        }
        ModelMesh mesh;
        mesh.name = name;
        std::map<int, size_t> prims;
        std::vector<std::pair<size_t, size_t>> order;  // (primitive, vertex) for each corner made
        const bool morph = !g.pointSamples.empty();
        for (size_t f = 0, at = 0; f < g.counts.size(); at += size_t(std::max(0, g.counts[f++]))) {
            if (g.hole[f] || g.counts[f] < 3) continue;
            auto it = prims.find(g.faceMaterial[f]);
            if (it == prims.end()) {
                it = prims.emplace(g.faceMaterial[f], mesh.primitives.size()).first;
                mesh.primitives.emplace_back();
                mesh.primitives.back().material = g.faceMaterial[f];
                if (morph) mesh.primitives.back().targets.resize(g.pointSamples.size());
            }
            ModelPrimitive& prim = mesh.primitives[it->second];
            auto corner = [&](size_t c) {
                const size_t point = size_t(g.indices[at + c]), fv = at + c;
                const uint32_t index = uint32_t(prim.positions.size() / 3);
                const double* P = morph ? &g.pointSamples[0][point * 3] : &g.points[point * 3];
                prim.positions.insert(prim.positions.end(), {float(P[0]), float(P[1]), float(P[2])});
                if (!g.normals.empty()) {
                    prim.normals.insert(prim.normals.end(), g.normals.begin() + long(fv * 3), g.normals.begin() + long(fv * 3 + 3));
                } else if (!pointNormals.empty()) {
                    const double* n = &pointNormals[point * 3];
                    const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    prim.normals.insert(prim.normals.end(), {float(l > 0 ? n[0] / l : 0), float(l > 0 ? n[1] / l : 0), float(l > 0 ? n[2] / l : 1)});
                }
                if (!g.uvs.empty()) prim.uvs.insert(prim.uvs.end(), g.uvs.begin() + long(fv * 2), g.uvs.begin() + long(fv * 2 + 2));
                if (!g.colors.empty()) prim.colors.insert(prim.colors.end(), g.colors.begin() + long(fv * 4), g.colors.begin() + long(fv * 4 + 4));
                for (size_t s = 0; morph && s < g.pointSamples.size(); ++s)
                    for (int k = 0; k < 3; ++k)
                        prim.targets[s].positions.push_back(float(g.pointSamples[s][point * 3 + size_t(k)] - P[k]));
                prim.indices.push_back(index);
                order.emplace_back(it->second, index);
                return index;
            };
            // A fan from the face's first corner.
            std::vector<uint32_t> idx;
            for (int k = 0; k < g.counts[f]; ++k) idx.push_back(corner(size_t(k)));
            std::vector<uint32_t> tris;
            for (size_t k = 1; k + 1 < idx.size(); ++k) tris.insert(tris.end(), {idx[0], idx[k], idx[k + 1]});
            prim.indices.resize(prim.indices.size() - idx.size());
            prim.indices.insert(prim.indices.end(), tris.begin(), tris.end());
        }
        if (mesh.primitives.empty()) return -1;
        for (ModelPrimitive& p : mesh.primitives)
            if (p.normals.size() != p.positions.size()) p.normals.clear();  // (finish() makes them flat)
        if (morph) {
            // Weights one-hot at each sample: linear between them is USD's linear interpolation of the points.
            const size_t n = g.pointSamples.size();
            mesh.weights.assign(n, 0.0f);
            mesh.weights[0] = 1;
            ModelAnimation::Sampler s;
            s.components = int(n);
            for (size_t k = 0; k < n; ++k) {
                s.times.push_back(float(g.sampleTimes[k]));
                for (size_t j = 0; j < n; ++j) s.values.push_back(j == k ? 1.0f : 0.0f);
            }
            ModelAnimation& a = animation();
            a.duration = std::max(a.duration, double(s.times.back()));
            a.samplers.push_back(std::move(s));
            ModelAnimation::Channel c;
            c.node = node, c.sampler = int(a.samplers.size()) - 1, c.path = ModelAnimation::Channel::Path::Weights;
            a.channels.push_back(c);
            // (The base is the first sample, so its weight is a displacement of nothing.)
        }
        m_.meshes.push_back(std::move(mesh));
        const int index = int(m_.meshes.size()) - 1;
        m_.nodes[size_t(node)].mesh = index;
        cornerOrder_[index] = std::move(order);
        return index;
    }

    // The simple shapes as polygons, along their axis (Z unless set).
    void shape(const usd::Prim& p, int node, const Inherited& inh) {
        Polygons g;
        const std::string type = p.typeName;
        const std::string axis = type == "Cube" ? "Z" : token(p, "axis", "Z");  // (a cube has no axis)
        auto point = [&](double x, double y, double z) {  // (from Z along the axis: a cyclic turn keeps handedness)
            if (axis == "X") g.points.insert(g.points.end(), {z, x, y});
            else if (axis == "Y") g.points.insert(g.points.end(), {y, z, x});
            else g.points.insert(g.points.end(), {x, y, z});
            return int(g.points.size() / 3) - 1;
        };
        auto nrm = [&](double x, double y, double z) {
            if (axis == "X") g.normals.insert(g.normals.end(), {float(z), float(x), float(y)});
            else if (axis == "Y") g.normals.insert(g.normals.end(), {float(y), float(z), float(x)});
            else g.normals.insert(g.normals.end(), {float(x), float(y), float(z)});
        };
        auto face = [&](const std::vector<int>& pts, const std::vector<std::array<double, 3>>& normals, const std::vector<std::array<double, 2>>& uvs) {
            g.counts.push_back(int(pts.size()));
            for (size_t k = 0; k < pts.size(); ++k) {
                g.indices.push_back(pts[k]);
                nrm(normals[k][0], normals[k][1], normals[k][2]);
                g.uvs.insert(g.uvs.end(), {float(uvs[k][0]), float(1 - uvs[k][1])});
            }
        };
        constexpr int seg = 48, rings = 24;
        if (type == "Cube") {
            const double h = number(p, "size", 2) / 2;
            static const int dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            for (const auto& d : dirs) {
                // Two axes across the face, u and v = n x u, so its corners run anticlockwise seen from outside.
                const double u[3] = {double(d[2] != 0), double(d[0] != 0), double(d[1] != 0)};
                const double v[3] = {d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0]};
                std::vector<int> pts;
                const double cs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
                for (const auto& c : cs)
                    pts.push_back(point(h * (d[0] + c[0] * u[0] + c[1] * v[0]), h * (d[1] + c[0] * u[1] + c[1] * v[1]), h * (d[2] + c[0] * u[2] + c[1] * v[2])));
                const std::array<double, 3> n{double(d[0]), double(d[1]), double(d[2])};
                face(pts, {n, n, n, n}, {{{0, 0}}, {{1, 0}}, {{1, 1}}, {{0, 1}}});
            }
        } else if (type == "Sphere" || type == "Capsule") {
            const bool capsule = type == "Capsule";
            const double r = number(p, "radius", capsule ? 0.5 : 1), half = capsule ? number(p, "height", 1) / 2 : 0;
            // Rows from the bottom pole up; a capsule has its equator twice, the cylinder between them.
            const int rows = capsule ? rings + 2 : rings + 1;
            auto row = [&](int j, double& ph, double& z) {
                const int k = capsule && j > rings / 2 ? j - 1 : j;
                ph = kPi * (double(k) / rings - 0.5);
                z = capsule ? (j > rings / 2 ? half : -half) : 0;
            };
            for (int j = 0; j + 1 < rows; ++j)
                for (int i = 0; i < seg; ++i) {
                    std::vector<int> pts;
                    std::vector<std::array<double, 3>> ns;
                    std::vector<std::array<double, 2>> uvs;
                    for (const auto& [di, dj] : {std::pair{0, 0}, std::pair{1, 0}, std::pair{1, 1}, std::pair{0, 1}}) {
                        double ph, z;
                        row(j + dj, ph, z);
                        const double th = 2 * kPi * (i + di) / seg;
                        const double nx = std::cos(ph) * std::cos(th), ny = std::cos(ph) * std::sin(th), nz = std::sin(ph);
                        pts.push_back(point(r * nx, r * ny, r * nz + z));
                        ns.push_back({nx, ny, nz});
                        uvs.push_back({double(i + di) / seg, double(j + dj) / (rows - 1)});
                    }
                    face(pts, ns, uvs);
                }
        } else if (type == "Cylinder" || type == "Cone") {
            const double r = number(p, "radius", 1), h = number(p, "height", 2) / 2;
            const bool cone = type == "Cone";
            const double slope = cone ? r / (2 * h) : 0, nl = std::sqrt(1 + slope * slope);
            for (int i = 0; i < seg; ++i) {
                std::vector<int> pts;
                std::vector<std::array<double, 3>> ns;
                std::vector<std::array<double, 2>> uvs;
                for (const auto& [di, top] : {std::pair{0, 0}, std::pair{1, 0}, std::pair{1, 1}, std::pair{0, 1}}) {
                    const double th = 2 * kPi * (i + di) / seg, rr = cone && top ? 0 : r;
                    pts.push_back(point(rr * std::cos(th), rr * std::sin(th), top ? h : -h));
                    ns.push_back({std::cos(th) / nl, std::sin(th) / nl, slope / nl});
                    uvs.push_back({double(i + di) / seg, double(top)});
                }
                face(pts, ns, uvs);
            }
            for (int cap = 0; cap < (cone ? 1 : 2); ++cap) {
                const double z = cap == 0 ? -h : h, s = cap == 0 ? -1 : 1;
                std::vector<int> pts;
                std::vector<std::array<double, 3>> ns;
                std::vector<std::array<double, 2>> uvs;
                for (int i = 0; i < seg; ++i) {
                    const double th = 2 * kPi * (cap == 0 ? seg - i : i) / seg;
                    pts.push_back(point(r * std::cos(th), r * std::sin(th), z));
                    ns.push_back({0, 0, s});
                    uvs.push_back({0.5 + 0.5 * std::cos(th), 0.5 + 0.5 * std::sin(th)});
                }
                face(pts, ns, uvs);
            }
        } else if (type == "Plane") {
            const double w = number(p, "width", 2) / 2, l = number(p, "length", 2) / 2;
            const std::array<double, 3> n{0, 0, 1};
            face({point(-w, -l, 0), point(w, -l, 0), point(w, l, 0), point(-w, l, 0)}, {n, n, n, n}, {{{0, 0}}, {{1, 0}}, {{1, 1}}, {{0, 1}}});
        }
        g.hole.assign(g.counts.size(), 0);
        corners_ += g.indices.size();
        const bool doubleSided = number(p, "doubleSided", 0) > 0.5;
        int mat = -1;
        if (!inh.binding.empty()) mat = sidedMaterial(material(inh.binding).index, doubleSided);
        if (mat < 0) {
            std::array<float, 4> c{0.5f, 0.5f, 0.5f, 1};
            const Primvar color = primvar(p, "displayColor");
            if (const double* d = color.valid() ? color.at(0, 0, 0, 3) : nullptr) c = {float(d[0]), float(d[1]), float(d[2]), 1};
            mat = plainMaterial(c, doubleSided);
        }
        g.faceMaterial.assign(g.counts.size(), mat);
        makeMesh(g, p.name, node);
    }

    // ---- Cameras and lights -----------------------------------------------------------------------------------------

    void camera(const usd::Prim& p, int node) {
        ModelCameraInfo c;
        c.name = p.name;
        c.node = node;
        c.orthographic = token(p, "projection", "perspective") == "orthographic";
        c.focalLength = number(p, "focalLength", 50);
        c.horizontalAperture = number(p, "horizontalAperture", 20.955);
        c.verticalAperture = number(p, "verticalAperture", 15.2908);
        c.horizontalOffset = number(p, "horizontalApertureOffset", 0);
        c.verticalOffset = number(p, "verticalApertureOffset", 0);
        const std::vector<double> clip = numbers(p, "clippingRange");
        if (clip.size() >= 2) c.nearClip = clip[0], c.farClip = clip[1];
        if (const usd::Attribute* f = p.attribute("focalLength"); f && f->animated())
            for (const auto& [t, v] : f->samples) c.focalTimes.push_back(seconds(t)), c.focalLengths.push_back(v.number(0, c.focalLength));
        if (c.orthographic) note("Camera " + p.name + " is orthographic: looked through as a perspective camera");
        m_.cameras.push_back(c);
    }

    void light(const usd::Prim& p, int node, const std::string& type) {
        ModelLightInfo l;
        l.name = p.name;
        l.node = node;
        static const std::map<std::string, ModelLightInfo::Kind> kinds = {
            {"DistantLight", ModelLightInfo::Kind::Distant}, {"SphereLight", ModelLightInfo::Kind::Sphere}, {"RectLight", ModelLightInfo::Kind::Rect},
            {"DiskLight", ModelLightInfo::Kind::Disk}, {"CylinderLight", ModelLightInfo::Kind::Cylinder}, {"DomeLight", ModelLightInfo::Kind::Dome}};
        const auto k = kinds.find(type);
        if (k == kinds.end()) {
            note(type + " lights are not read");
            return;
        }
        l.kind = k->second;
        // (a distant light's fallback is the sun's luminance through its 0.53° disc)
        const double scale = number(p, "intensity", l.kind == ModelLightInfo::Kind::Distant ? 50000 : 1) * std::pow(2.0, number(p, "exposure", 0));
        std::array<float, 3> c{1, 1, 1};
        if (const usd::Attribute* a = attr(p, "color"))
            for (size_t i = 0; i < 3 && i < a->at(start_).numbers.size(); ++i) c[i] = float(a->at(start_).numbers[i]);
        if (number(p, "enableColorTemperature", 0) > 0.5) {
            const std::array<float, 3> t = blackbody(number(p, "colorTemperature", 6500));
            for (int i = 0; i < 3; ++i) c[size_t(i)] *= t[size_t(i)];
        }
        for (int i = 0; i < 3; ++i) l.color[size_t(i)] = float(c[size_t(i)] * scale);
        l.normalize = number(p, "normalize", 0) > 0.5;
        l.radius = number(p, "radius", 0.5);
        l.width = number(p, "width", 1), l.height = number(p, "height", 1), l.length = number(p, "length", 1);
        if (number(p, "treatAsPoint", 0) > 0.5) l.radius = 0;
        if (l.kind == ModelLightInfo::Kind::Distant) l.angle = std::clamp(number(p, "angle", 0.53), 0.0, 360.0);
        if (attr(p, "shaping:cone:angle")) {
            l.coneAngle = std::clamp(number(p, "shaping:cone:angle", 90), 0.0, 180.0);
            l.coneSoftness = std::clamp(number(p, "shaping:cone:softness", 0), 0.0, 1.0);
        }
        l.shadows = number(p, "shadow:enable", 1) > 0.5;
        if (l.kind == ModelLightInfo::Kind::Dome) {
            const std::string file = token(p, "texture:file");
            if (!file.empty() && !dome(file, l)) note("Cannot read the dome light's picture " + file);
            if (const std::string fmt = token(p, "texture:format", "automatic"); fmt != "automatic" && fmt != "latlong")
                note("Dome light pictures in the " + fmt + " layout are not read: only latitude-longitude");
        }
        m_.lights.push_back(std::move(l));
    }

    // A dome light's picture (EXR, Radiance HDR, or an 8-bit picture taken as sRGB), at most 1024 wide.
    bool dome(const std::string& file, ModelLightInfo& l) {
        std::string bytes;
        if (!st_.readAsset(file, bytes)) return false;
        int w = 0, h = 0;
        std::vector<float> rgb;
        const QString ext = QFileInfo(QString::fromStdString(file.back() == ']' ? file.substr(file.rfind('[') + 1, file.size() - file.rfind('[') - 2) : file)).suffix().toLower();
        if (ext == "exr") {
            std::string path = file;
            QTemporaryDir tmp;
            if (file.back() == ']') {  // (in a package: read through a file of its own)
                if (!tmp.isValid()) return false;
                path = tmp.filePath("dome.exr").toStdString();
                QFile f(QString::fromStdString(path));
                if (!f.open(QIODevice::WriteOnly) || f.write(bytes.data(), qint64(bytes.size())) != qint64(bytes.size())) return false;
            }
            std::vector<float> rgba;
            if (!readExrLayer(path, "", w, h, rgba)) return false;
            rgb.resize(size_t(w) * size_t(h) * 3);
            for (size_t i = 0; i < size_t(w) * size_t(h); ++i)
                for (int c = 0; c < 3; ++c) rgb[i * 3 + size_t(c)] = rgba[i * 4 + size_t(c)];
        } else if (ext == "hdr") {
            if (!decodeRgbe(bytes, w, h, rgb)) return false;
        } else {
            QImage img = QImage::fromData(QByteArray::fromStdString(bytes));
            if (img.isNull()) return false;
            img = img.convertToFormat(QImage::Format_RGBA64);
            w = img.width(), h = img.height();
            rgb.resize(size_t(w) * size_t(h) * 3);
            for (int y = 0; y < h; ++y) {
                const uint16_t* q = reinterpret_cast<const uint16_t*>(img.constScanLine(y));
                for (int x = 0; x < w; ++x)
                    for (int c = 0; c < 3; ++c) {
                        const double v = q[x * 4 + c] / 65535.0;
                        rgb[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(c)] = float(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
                    }
            }
        }
        if (w <= 0 || h <= 0) return false;
        // Made smaller by averaging, at most kMaxDomeWidth across.
        const int ow = std::min(w, kMaxDomeWidth), oh = std::max(1, int(int64_t(h) * ow / w));
        l.pictureWidth = ow, l.pictureHeight = oh;
        l.picture.assign(size_t(ow) * size_t(oh) * 3, 0);
        for (int y = 0; y < oh; ++y)
            for (int x = 0; x < ow; ++x) {
                const int x0 = int(int64_t(x) * w / ow), x1 = std::max(x0 + 1, int(int64_t(x + 1) * w / ow));
                const int y0 = int(int64_t(y) * h / oh), y1 = std::max(y0 + 1, int(int64_t(y + 1) * h / oh));
                double s[3] = {0, 0, 0};
                for (int sy = y0; sy < y1; ++sy)
                    for (int sx = x0; sx < x1; ++sx)
                        for (int c = 0; c < 3; ++c) s[c] += std::isfinite(rgb[(size_t(sy) * size_t(w) + size_t(sx)) * 3 + size_t(c)]) ? rgb[(size_t(sy) * size_t(w) + size_t(sx)) * 3 + size_t(c)] : 0;
                const double n = double(x1 - x0) * (y1 - y0);
                for (int c = 0; c < 3; ++c) l.picture[(size_t(y) * size_t(ow) + size_t(x)) * 3 + size_t(c)] = float(std::max(0.0, s[c] / n));
            }
        return true;
    }

    // ---- Skeletons --------------------------------------------------------------------------------------------------

    // Each skinned mesh's skeleton made into joint nodes (under the skeleton's node, posed by its animation), and its
    // points bound to them; blend shapes become morph targets weighted by the animation.
    void skins() {
        std::map<std::string, int> skinOf;  // skeleton path to skin
        std::map<std::string, std::vector<int>> jointNodes;
        std::map<std::string, std::vector<std::string>> jointNames;
        for (SkinnedMesh& sm : skinned_) {
            const auto sk = skeletons_.find(sm.skeleton);
            if (sk == skeletons_.end()) {
                note("A skinned mesh's skeleton " + sm.skeleton + " is not found: drawn unskinned");
                continue;
            }
            const usd::Prim& skel = *sk->second;
            if (!skinOf.count(sm.skeleton)) {
                const std::vector<std::string> joints = skel.attribute("joints") ? skel.attribute("joints")->at(start_).texts : std::vector<std::string>{};
                const std::vector<double> bind = numbers(skel, "bindTransforms"), rest = numbers(skel, "restTransforms");
                ModelSkin skin;
                std::vector<int> nodes;
                std::map<std::string, int> byName;
                for (size_t j = 0; j < joints.size(); ++j) {
                    ModelNode n;
                    const std::string& name = joints[j];
                    n.name = name.substr(name.rfind('/') == std::string::npos ? 0 : name.rfind('/') + 1);
                    const size_t slash = name.rfind('/');
                    const auto parent = slash == std::string::npos ? byName.end() : byName.find(name.substr(0, slash));
                    n.parent = parent != byName.end() ? parent->second : skeletonNodes_[sm.skeleton];
                    if (rest.size() >= (j + 1) * 16) {
                        n.hasMatrix = true;
                        std::copy(rest.begin() + long(j * 16), rest.begin() + long(j * 16 + 16), n.matrix.begin());
                        decompose(n);
                    }
                    const int id = int(m_.nodes.size());
                    m_.nodes.push_back(n);
                    m_.nodes[size_t(n.parent)].children.push_back(id);
                    byName[name] = id;
                    nodes.push_back(id);
                    Mat4 b = identity4();
                    if (bind.size() >= (j + 1) * 16) std::copy(bind.begin() + long(j * 16), bind.begin() + long(j * 16 + 16), b.begin());
                    skin.joints.push_back(id);
                    skin.inverseBind.push_back(inverse4(b));
                }
                m_.skins.push_back(std::move(skin));
                skinOf[sm.skeleton] = int(m_.skins.size()) - 1;
                jointNodes[sm.skeleton] = nodes;
                jointNames[sm.skeleton] = joints;
                skelAnimation(sm.skeleton, joints, nodes);
            }
            bind(sm, skinOf[sm.skeleton]);
        }
    }

    // A joint's local matrix as translation, rotation and scale (so its animation can replace parts of it).
    static void decompose(ModelNode& n) {
        const Mat4& m = n.matrix;
        double sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]),
               sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
        const double det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
        if (det < 0) sx = -sx;
        if (sx == 0 || sy == 0 || sz == 0) return;  // (kept as its matrix)
        const double r[9] = {m[0] / sx, m[1] / sx, m[2] / sx, m[4] / sy, m[5] / sy, m[6] / sy, m[8] / sz, m[9] / sz, m[10] / sz};
        // (column-major r: r[c*3+row]) to a quaternion
        const double m00 = r[0], m11 = r[4], m22 = r[8], tr = m00 + m11 + m22;
        double x, y, z, w;
        if (tr > 0) {
            const double s = std::sqrt(tr + 1) * 2;
            w = s / 4, x = (r[5] - r[7]) / s, y = (r[6] - r[2]) / s, z = (r[1] - r[3]) / s;
        } else if (m00 > m11 && m00 > m22) {
            const double s = std::sqrt(1 + m00 - m11 - m22) * 2;
            w = (r[5] - r[7]) / s, x = s / 4, y = (r[3] + r[1]) / s, z = (r[6] + r[2]) / s;
        } else if (m11 > m22) {
            const double s = std::sqrt(1 + m11 - m00 - m22) * 2;
            w = (r[6] - r[2]) / s, x = (r[3] + r[1]) / s, y = s / 4, z = (r[7] + r[5]) / s;
        } else {
            const double s = std::sqrt(1 + m22 - m00 - m11) * 2;
            w = (r[1] - r[3]) / s, x = (r[6] + r[2]) / s, y = (r[7] + r[5]) / s, z = s / 4;
        }
        n.translation = {m[12], m[13], m[14]};
        n.rotation = {x, y, z, w};
        n.scale = {sx, sy, sz};
        n.hasMatrix = false;
    }

    // A SkelAnimation's translations, rotations and scales (arrays over its joints) as channels on the joint nodes,
    // and its blend shape weights kept for the meshes.
    std::map<std::string, std::pair<std::vector<std::string>, const usd::Attribute*>> blendWeights_;  // skeleton: shapes, weights
    void skelAnimation(const std::string& skeleton, const std::vector<std::string>& joints, const std::vector<int>& nodes) {
        const auto it = skelAnimation_.find(skeleton);
        if (it == skelAnimation_.end()) return;
        const usd::Prim* a = st_.prim(it->second);
        if (!a || a->typeName != "SkelAnimation") return;
        const std::vector<std::string> animJoints = a->attribute("joints") ? a->attribute("joints")->at(start_).texts : std::vector<std::string>{};
        if (const usd::Attribute* bs = a->attribute("blendShapes"))
            blendWeights_[skeleton] = {bs->at(start_).texts, a->attribute("blendShapeWeights")};
        for (const auto& [name, path, width] : {std::tuple{"translations", ModelAnimation::Channel::Path::Translation, 3},
                                                std::tuple{"rotations", ModelAnimation::Channel::Path::Rotation, 4},
                                                std::tuple{"scales", ModelAnimation::Channel::Path::Scale, 3}}) {
            const usd::Attribute* attrib = a->attribute(name);
            if (!attrib) continue;
            std::vector<std::pair<double, usd::Value>> samples = attrib->samples;
            if (samples.empty()) samples.emplace_back(start_, attrib->value);
            for (size_t j = 0; j < animJoints.size(); ++j) {
                const auto at = std::find(joints.begin(), joints.end(), animJoints[j]);
                if (at == joints.end()) continue;
                const int node = nodes[size_t(at - joints.begin())];
                std::vector<std::pair<double, usd::Value>> one;
                for (const auto& [t, v] : samples) {
                    if (v.numbers.size() < (j + 1) * size_t(width)) continue;
                    usd::Value e;
                    e.kind = usd::Value::Kind::Number;
                    e.numbers.assign(v.numbers.begin() + long(j * size_t(width)), v.numbers.begin() + long((j + 1) * size_t(width)));
                    one.emplace_back(t, e);
                }
                if (one.empty()) continue;
                ModelNode& n = m_.nodes[size_t(node)];
                n.hasMatrix = false;
                const std::vector<double>& v0 = one.front().second.numbers;
                if (path == ModelAnimation::Channel::Path::Translation) n.translation = {v0[0], v0[1], v0[2]};
                else if (path == ModelAnimation::Channel::Path::Rotation) n.rotation = {v0[0], v0[1], v0[2], v0[3]};
                else n.scale = {v0[0], v0[1], v0[2]};
                if (one.size() < 2) continue;
                const int s = sampler(one, size_t(width));
                if (s < 0) continue;
                ModelAnimation::Channel c;
                c.node = node, c.sampler = s, c.path = path;
                animation().channels.push_back(c);
            }
        }
    }

    // A mesh's vertices given their joints (the four strongest, renormalised) and its points put in the skeleton's
    // bind space; its blend shapes as morph targets.
    void bind(const SkinnedMesh& sm, int skin) {
        ModelNode& node = m_.nodes[size_t(sm.node)];
        node.skin = skin;
        ModelMesh& mesh = m_.meshes[size_t(sm.mesh)];
        // The skeleton's world, not the mesh's, places skinned points: the mesh node is moved under the skeleton.
        const auto skelNode = skeletonNodes_.find(sm.skeleton);
        // Each vertex's point: makeMesh() made the vertices from the kept corners in face order.
        const std::vector<std::pair<size_t, size_t>>& order = cornerOrder_[sm.mesh];
        if (order.size() != sm.cornerPoints.size()) return;
        std::vector<std::vector<int>> pointOf(mesh.primitives.size());
        for (size_t pi = 0; pi < mesh.primitives.size(); ++pi) pointOf[pi].assign(mesh.primitives[pi].positions.size() / 3, 0);
        for (size_t k = 0; k < order.size(); ++k) pointOf[order[k].first][order[k].second] = sm.cornerPoints[k];
        const Mat4 inv = inverse4(sm.geomBind);
        for (size_t pi = 0; pi < mesh.primitives.size(); ++pi) {
            ModelPrimitive& p = mesh.primitives[pi];
            const size_t n = p.positions.size() / 3;
            if (!sm.jointIndices.empty()) {
                p.joints.assign(n * 4, 0);
                p.weights.assign(n * 4, 0.0f);
            }
            for (size_t v = 0; v < n; ++v) {
                const int point = pointOf[pi][v];
                // Bind space.
                const std::array<double, 3> q = transformPoint(sm.geomBind, {p.positions[v * 3], p.positions[v * 3 + 1], p.positions[v * 3 + 2]});
                for (int c = 0; c < 3; ++c) p.positions[v * 3 + size_t(c)] = float(q[size_t(c)]);
                if (!p.normals.empty()) {  // (by the inverse transpose)
                    const float* nn = &p.normals[v * 3];
                    double N[3] = {inv[0] * nn[0] + inv[1] * nn[1] + inv[2] * nn[2], inv[4] * nn[0] + inv[5] * nn[1] + inv[6] * nn[2], inv[8] * nn[0] + inv[9] * nn[1] + inv[10] * nn[2]};
                    const double l = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
                    for (int c = 0; c < 3; ++c) p.normals[v * 3 + size_t(c)] = float(l > 0 ? N[c] / l : 0);
                }
                if (sm.jointIndices.empty()) continue;
                // The strongest four influences.
                std::vector<std::pair<float, int>> inf;
                for (int i = 0; i < sm.influences; ++i) {
                    const size_t at = size_t(point) * size_t(sm.influences) + size_t(i);
                    if (at < sm.jointIndices.size() && at < sm.jointWeights.size() && sm.jointWeights[at] > 0) inf.emplace_back(sm.jointWeights[at], sm.jointIndices[at]);
                }
                std::sort(inf.begin(), inf.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                if (inf.size() > 4) inf.resize(4);
                float total = 0;
                for (const auto& i : inf) total += i.first;
                for (size_t i = 0; i < inf.size(); ++i) {
                    p.joints[v * 4 + i] = uint16_t(std::max(0, inf[i].second));
                    p.weights[v * 4 + i] = total > 0 ? inf[i].first / total : 0;
                }
            }
        }
        if (sm.influences > 4) note("Skinned points with more than four joints keep their four strongest");
        if (skelNode != skeletonNodes_.end()) {
            // The mesh node hangs under the skeleton's node with no transform of its own (the skin places it).
            if (node.parent >= 0) {
                auto& kids = m_.nodes[size_t(node.parent)].children;
                kids.erase(std::remove(kids.begin(), kids.end(), sm.node), kids.end());
            } else {
                m_.roots.erase(std::remove(m_.roots.begin(), m_.roots.end(), sm.node), m_.roots.end());
            }
            node.parent = skelNode->second;
            node.ops.clear();
            m_.nodes[size_t(skelNode->second)].children.push_back(sm.node);
        }
        blendShapes(sm, pointOf);
    }

    void blendShapes(const SkinnedMesh& sm, const std::vector<std::vector<int>>& pointOf) {
        if (sm.blendTargets.empty()) return;
        ModelMesh& mesh = m_.meshes[size_t(sm.mesh)];
        if (std::any_of(mesh.primitives.begin(), mesh.primitives.end(), [](const ModelPrimitive& p) { return !p.targets.empty(); })) return;
        const size_t n = sm.blendTargets.size();
        for (size_t t = 0; t < n; ++t) {
            const usd::Prim* shape = st_.prim(sm.blendTargets[t]);
            std::vector<double> offsets = shape ? numbers(*shape, "offsets") : std::vector<double>{};
            std::vector<double> idx = shape ? numbers(*shape, "pointIndices") : std::vector<double>{};
            std::map<int, size_t> where;  // point to its offset
            for (size_t k = 0; k < offsets.size() / 3; ++k) where[idx.empty() ? int(k) : (k < idx.size() ? int(idx[k]) : -1)] = k;
            if (shape && !shape->children.empty()) note("Blend shapes' in-between shapes are not read");
            for (size_t pi = 0; pi < mesh.primitives.size(); ++pi) {
                ModelPrimitive& p = mesh.primitives[pi];
                p.targets.resize(n);
                std::vector<float>& d = p.targets[t].positions;
                d.assign(p.positions.size(), 0.0f);
                for (size_t v = 0; v < p.positions.size() / 3; ++v)
                    if (const auto w = where.find(pointOf[pi][v]); w != where.end())
                        for (int c = 0; c < 3; ++c)  // (into bind space, as the points were)
                            d[v * 3 + size_t(c)] = float(sm.geomBind[size_t(c)] * offsets[w->second * 3] + sm.geomBind[size_t(4 + c)] * offsets[w->second * 3 + 1] +
                                                         sm.geomBind[size_t(8 + c)] * offsets[w->second * 3 + 2]);
            }
        }
        mesh.weights.assign(n, 0.0f);
        // Weights from the skeleton's animation, by shape name.
        const auto bw = blendWeights_.find(sm.skeleton);
        if (bw == blendWeights_.end() || !bw->second.second) return;
        const std::vector<std::string>& names = bw->second.first;
        const usd::Attribute* weights = bw->second.second;
        std::vector<std::pair<double, usd::Value>> samples = weights->samples;
        if (samples.empty()) samples.emplace_back(start_, weights->value);
        std::vector<std::pair<double, usd::Value>> mapped;
        for (const auto& [t, v] : samples) {
            usd::Value e;
            e.kind = usd::Value::Kind::Number;
            e.numbers.assign(n, 0);
            for (size_t k = 0; k < n && k < sm.blendShapes.size(); ++k) {
                const auto at = std::find(names.begin(), names.end(), sm.blendShapes[k]);
                if (at != names.end() && size_t(at - names.begin()) < v.numbers.size()) e.numbers[k] = v.numbers[size_t(at - names.begin())];
            }
            mapped.emplace_back(t, e);
        }
        for (size_t k = 0; k < n; ++k) mesh.weights[k] = float(mapped.front().second.numbers[k]);
        if (mapped.size() < 2) return;
        const int s = sampler(mapped, n);
        if (s < 0) return;
        ModelAnimation::Channel c;
        c.node = sm.node, c.sampler = s, c.path = ModelAnimation::Channel::Path::Weights;
        animation().channels.push_back(c);
    }

};

}  // namespace

bool loadUsdModel(const std::string& path, Model& out, std::string* error) {
    usd::Stage stage;
    if (!usd::openStage(path, stage, error, &readModelFile)) return false;
    Converter c(stage, out);
    return c.run(error);
}

}  // namespace montage
