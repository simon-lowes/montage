#include "Model3D.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <tuple>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

namespace montage {

// ---- Matrices ------------------------------------------------------------------------------------------------------

Mat4 identity4() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += a[size_t(k * 4 + row)] * b[size_t(c * 4 + k)];
            r[size_t(c * 4 + row)] = s;
        }
    return r;
}

std::array<double, 3> transformPoint(const Mat4& m, const std::array<double, 3>& p) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}

std::array<double, 3> transformDirection(const Mat4& m, const std::array<double, 3>& d) {
    return {m[0] * d[0] + m[4] * d[1] + m[8] * d[2], m[1] * d[0] + m[5] * d[1] + m[9] * d[2], m[2] * d[0] + m[6] * d[1] + m[10] * d[2]};
}

Mat4 inverse4(const Mat4& m) {
    // The 3x3 part inverted by its cofactors, the translation taken back through it.
    const double a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    const double det = a * A + b * B + c * C;
    const double k = std::fabs(det) > 1e-300 ? 1 / det : 0;
    Mat4 r = identity4();
    r[0] = A * k, r[4] = -(b * i - c * h) * k, r[8] = (b * f - c * e) * k;
    r[1] = B * k, r[5] = (a * i - c * g) * k, r[9] = -(a * f - c * d) * k;
    r[2] = C * k, r[6] = -(a * h - b * g) * k, r[10] = (a * e - b * d) * k;
    const std::array<double, 3> t = transformDirection(r, {m[12], m[13], m[14]});
    r[12] = -t[0], r[13] = -t[1], r[14] = -t[2];
    return r;
}

Mat4 trsMatrix(const std::array<double, 3>& t, const std::array<double, 4>& q, const std::array<double, 3>& s) {
    const double x = q[0], y = q[1], z = q[2], w = q[3];
    Mat4 m = identity4();
    m[0] = (1 - 2 * (y * y + z * z)) * s[0];
    m[1] = (2 * (x * y + z * w)) * s[0];
    m[2] = (2 * (x * z - y * w)) * s[0];
    m[4] = (2 * (x * y - z * w)) * s[1];
    m[5] = (1 - 2 * (x * x + z * z)) * s[1];
    m[6] = (2 * (y * z + x * w)) * s[1];
    m[8] = (2 * (x * z + y * w)) * s[2];
    m[9] = (2 * (y * z - x * w)) * s[2];
    m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    m[12] = t[0], m[13] = t[1], m[14] = t[2];
    return m;
}

size_t Model::triangles() const {
    size_t n = 0;
    for (const ModelMesh& m : meshes)
        for (const ModelPrimitive& p : m.primitives) n += p.indices.size() / 3;
    return n;
}

float ModelTexture::linear(uint8_t v, int c) const {
    static const std::array<float, 256> fromSrgb = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float x = float(i) / 255;
            t[size_t(i)] = x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return srgb && c < 3 ? fromSrgb[v] : float(v) * (1.0f / 255);
}

namespace {

// A picture's bytes decoded into a texture with its mip levels (its colour sRGB when `srgb`).
bool decodeTexture(const QByteArray& bytes, bool srgb, ModelTexture& out) {
    QImage img = QImage::fromData(bytes);
    if (img.isNull()) return false;
    img = img.convertToFormat(QImage::Format_RGBA8888);
    out.srgb = srgb;
    ModelTexture::Level l;
    l.width = img.width(), l.height = img.height();
    l.rgba.resize(size_t(l.width) * size_t(l.height) * 4);
    for (int y = 0; y < l.height; ++y) std::memcpy(l.rgba.data() + size_t(y) * size_t(l.width) * 4, img.constScanLine(y), size_t(l.width) * 4);
    out.levels.push_back(std::move(l));
    // Each level averaged from the last in linear light, then put back in 8 bits.
    std::array<uint8_t, 4096> encode{};  // linear 0..1 in 4096 steps to sRGB
    for (int i = 0; i < 4096; ++i) {
        const float v = float(i) / 4095;
        encode[size_t(i)] = uint8_t(std::lround(255 * (v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f)));
    }
    while (out.levels.back().width > 1 || out.levels.back().height > 1) {
        const ModelTexture::Level& a = out.levels.back();
        ModelTexture::Level b;
        b.width = std::max(1, a.width / 2), b.height = std::max(1, a.height / 2);
        b.rgba.resize(size_t(b.width) * size_t(b.height) * 4);
        for (int y = 0; y < b.height; ++y)
            for (int x = 0; x < b.width; ++x)
                for (int c = 0; c < 4; ++c) {
                    float s = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int sx = std::min(a.width - 1, x * 2 + dx), sy = std::min(a.height - 1, y * 2 + dy);
                            s += out.linear(a.rgba[(size_t(sy) * size_t(a.width) + size_t(sx)) * 4 + size_t(c)], c);
                        }
                    s /= 4;
                    b.rgba[(size_t(y) * size_t(b.width) + size_t(x)) * 4 + size_t(c)] =
                        srgb && c < 3 ? encode[size_t(std::lround(std::clamp(s, 0.0f, 1.0f) * 4095))] : uint8_t(std::lround(std::clamp(s, 0.0f, 1.0f) * 255));
                }
        out.levels.push_back(std::move(b));
    }
    return true;
}

// The files read while a model loads (for its cache to see when one changes), when asked for.
thread_local std::vector<std::string>* filesRead = nullptr;

QByteArray readWhole(const QString& path) {
    if (filesRead) filesRead->push_back(path.toStdString());
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Normals for a primitive without them: flat, each triangle its own corners.
void flatNormals(ModelPrimitive& p) {
    auto expand = [&](std::vector<float>& v, size_t n) {
        if (v.empty()) return;
        std::vector<float> out;
        out.reserve(p.indices.size() * n);
        for (uint32_t i : p.indices)
            for (size_t k = 0; k < n; ++k) out.push_back(v[size_t(i) * n + k]);
        v = std::move(out);
    };
    auto expand16 = [&](std::vector<uint16_t>& v) {
        if (v.empty()) return;
        std::vector<uint16_t> out;
        for (uint32_t i : p.indices)
            for (size_t k = 0; k < 4; ++k) out.push_back(v[size_t(i) * 4 + k]);
        v = std::move(out);
    };
    expand(p.positions, 3), expand(p.uvs, 2), expand(p.tangents, 4), expand(p.colors, 4), expand(p.weights, 4), expand16(p.joints);
    for (ModelPrimitive::Target& t : p.targets) expand(t.positions, 3), expand(t.normals, 3);
    for (uint32_t i = 0; i < p.indices.size(); ++i) p.indices[i] = i;
    p.normals.assign(p.positions.size(), 0);
    for (size_t t = 0; t + 2 < p.indices.size(); t += 3) {
        const float* a = &p.positions[t * 3];
        const float *b = a + 3, *c = a + 6;
        const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (float& k : n) k = l > 0 ? k / l : 0;
        for (size_t k = 0; k < 3; ++k) std::copy(n, n + 3, &p.normals[(t + k) * 3]);
    }
}

// Tangents from the uvs' directions across each triangle, averaged at each corner and squared up to its normal.
void makeTangents(ModelPrimitive& p) {
    const size_t n = p.positions.size() / 3;
    if (p.uvs.size() != n * 2 || p.normals.size() != n * 3) return;
    std::vector<double> t(n * 3, 0), b(n * 3, 0);
    for (size_t i = 0; i + 2 < p.indices.size(); i += 3) {
        const uint32_t v[3] = {p.indices[i], p.indices[i + 1], p.indices[i + 2]};
        const float* P[3] = {&p.positions[v[0] * 3], &p.positions[v[1] * 3], &p.positions[v[2] * 3]};
        const float* U[3] = {&p.uvs[v[0] * 2], &p.uvs[v[1] * 2], &p.uvs[v[2] * 2]};
        const double e1[3] = {P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2]};
        const double e2[3] = {P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2]};
        const double du1 = U[1][0] - U[0][0], dv1 = U[1][1] - U[0][1], du2 = U[2][0] - U[0][0], dv2 = U[2][1] - U[0][1];
        const double det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-20) continue;
        const double r = 1 / det;
        for (int k = 0; k < 3; ++k) {
            const double tk = (e1[k] * dv2 - e2[k] * dv1) * r, bk = (e2[k] * du1 - e1[k] * du2) * r;
            for (uint32_t c : v) t[c * 3 + size_t(k)] += tk, b[c * 3 + size_t(k)] += bk;
        }
    }
    p.tangents.assign(n * 4, 0);
    for (size_t i = 0; i < n; ++i) {
        const float* N = &p.normals[i * 3];
        double T[3] = {t[i * 3], t[i * 3 + 1], t[i * 3 + 2]};
        const double d = T[0] * N[0] + T[1] * N[1] + T[2] * N[2];
        for (int k = 0; k < 3; ++k) T[k] -= N[k] * d;
        double l = std::sqrt(T[0] * T[0] + T[1] * T[1] + T[2] * T[2]);
        if (l < 1e-12) {  // (no uv change: any direction across the normal)
            const double a[3] = {std::fabs(N[0]) < 0.9 ? 1.0 : 0.0, std::fabs(N[0]) < 0.9 ? 0.0 : 1.0, 0};
            T[0] = a[1] * N[2] - a[2] * N[1], T[1] = a[2] * N[0] - a[0] * N[2], T[2] = a[0] * N[1] - a[1] * N[0];
            l = std::sqrt(T[0] * T[0] + T[1] * T[1] + T[2] * T[2]);
        }
        const double c[3] = {N[1] * T[2] - N[2] * T[1], N[2] * T[0] - N[0] * T[2], N[0] * T[1] - N[1] * T[0]};
        const double s = c[0] * b[i * 3] + c[1] * b[i * 3 + 1] + c[2] * b[i * 3 + 2];
        for (int k = 0; k < 3; ++k) p.tangents[i * 4 + size_t(k)] = float(T[k] / l);
        p.tangents[i * 4 + 3] = s < 0 ? -1.0f : 1.0f;
    }
}

// Every primitive given normals, and tangents where its material has a normal map.
void finish(Model& m) {
    for (ModelMesh& mesh : m.meshes)
        for (ModelPrimitive& p : mesh.primitives) {
            if (p.normals.size() != p.positions.size()) flatNormals(p);
            const bool mapped = p.material >= 0 && size_t(p.material) < m.materials.size() && m.materials[size_t(p.material)].normalTexture >= 0;
            if (mapped && p.tangents.size() != p.positions.size() / 3 * 4) makeTangents(p);
        }
}

// ---- glTF ----------------------------------------------------------------------------------------------------------

cgltf_result readFile(const cgltf_memory_options*, const cgltf_file_options*, const char* path, cgltf_size* size, void** data) {
    const QByteArray bytes = readWhole(QString::fromUtf8(path));
    if (bytes.isEmpty()) return cgltf_result_file_not_found;
    void* p = std::malloc(size_t(bytes.size()));
    if (!p) return cgltf_result_out_of_memory;
    std::memcpy(p, bytes.constData(), size_t(bytes.size()));
    *size = cgltf_size(bytes.size());
    *data = p;
    return cgltf_result_success;
}
void releaseFile(const cgltf_memory_options*, const cgltf_file_options*, void* data) { std::free(data); }

std::vector<float> floats(const cgltf_accessor* a) {
    if (!a) return {};
    std::vector<float> v(cgltf_accessor_unpack_floats(a, nullptr, 0));
    cgltf_accessor_unpack_floats(a, v.data(), v.size());
    return v;
}

ModelTexture::Wrap wrapOf(int mode) {
    return mode == 33071 ? ModelTexture::Wrap::Clamp : mode == 33648 ? ModelTexture::Wrap::Mirror : ModelTexture::Wrap::Repeat;
}

bool loadGltf(const std::string& path, const QByteArray& bytes, Model& out, std::string* error) {
    cgltf_options options{};
    options.file.read = readFile;
    options.file.release = releaseFile;
    cgltf_data* data = nullptr;
    auto fail = [&](const std::string& why) {
        if (data) cgltf_free(data);
        if (error) *error = why;
        return false;
    };
    if (cgltf_parse(&options, bytes.constData(), cgltf_size(bytes.size()), &data) != cgltf_result_success)
        return fail(path + " is not a glTF model Montage can read");
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) return fail("The buffers of " + path + " cannot be read");
    if (cgltf_validate(data) != cgltf_result_success) return fail(path + " is not a valid glTF model");
    const QDir dir = QFileInfo(QString::fromStdString(path)).absoluteDir();

    // Textures: one for each image and colour use (base colour and emissive are sRGB, the rest linear).
    std::map<std::pair<const cgltf_texture*, bool>, int> made;
    auto texture = [&](const cgltf_texture_view& view, bool srgb) -> int {
        const cgltf_texture* t = view.texture;
        if (!t || !t->image) return -1;
        if (auto it = made.find({t, srgb}); it != made.end()) return it->second;
        const cgltf_image* im = t->image;
        QByteArray b;
        if (im->buffer_view) {
            b = QByteArray(reinterpret_cast<const char*>(cgltf_buffer_view_data(im->buffer_view)), qsizetype(im->buffer_view->size));
        } else if (im->uri && std::strncmp(im->uri, "data:", 5) == 0) {
            const char* comma = std::strchr(im->uri, ',');
            if (comma && std::strstr(im->uri, ";base64,")) b = QByteArray::fromBase64(QByteArray(comma + 1));
        } else if (im->uri) {
            std::string uri = im->uri;
            uri.resize(cgltf_decode_uri(uri.data()));
            b = readWhole(dir.filePath(QString::fromStdString(uri)));
        }
        ModelTexture tex;
        int id = -1;
        if (!b.isEmpty() && decodeTexture(b, srgb, tex)) {
            if (t->sampler) tex.wrapS = wrapOf(t->sampler->wrap_s), tex.wrapT = wrapOf(t->sampler->wrap_t);
            id = int(out.textures.size());
            out.textures.push_back(std::move(tex));
        }
        made[{t, srgb}] = id;
        return id;
    };

    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material& g = data->materials[i];
        ModelMaterial m;
        if (g.name) m.name = g.name;
        if (g.has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness& r = g.pbr_metallic_roughness;
            std::copy(r.base_color_factor, r.base_color_factor + 4, m.baseColor.begin());
            m.metallic = r.metallic_factor, m.roughness = r.roughness_factor;
            m.baseTexture = texture(r.base_color_texture, true);
            m.metallicRoughnessTexture = texture(r.metallic_roughness_texture, false);
        } else if (g.has_pbr_specular_glossiness) {
            const cgltf_pbr_specular_glossiness& s = g.pbr_specular_glossiness;
            std::copy(s.diffuse_factor, s.diffuse_factor + 4, m.baseColor.begin());
            m.metallic = 0, m.roughness = 1 - s.glossiness_factor;
            m.baseTexture = texture(s.diffuse_texture, true);
        }
        std::copy(g.emissive_factor, g.emissive_factor + 3, m.emissive.begin());
        if (g.has_emissive_strength)
            for (float& e : m.emissive) e *= g.emissive_strength.emissive_strength;
        m.emissiveTexture = texture(g.emissive_texture, true);
        m.normalTexture = texture(g.normal_texture, false);
        m.normalScale = g.normal_texture.texture ? g.normal_texture.scale : 1;
        m.alpha = g.alpha_mode == cgltf_alpha_mode_mask ? ModelMaterial::Alpha::Mask
                  : g.alpha_mode == cgltf_alpha_mode_blend ? ModelMaterial::Alpha::Blend
                                                           : ModelMaterial::Alpha::Opaque;
        m.alphaCutoff = g.alpha_cutoff;
        m.doubleSided = g.double_sided;
        out.materials.push_back(m);
    }

    int defaultMaterial = -1;
    for (cgltf_size i = 0; i < data->meshes_count; ++i) {
        const cgltf_mesh& gm = data->meshes[i];
        ModelMesh mesh;
        if (gm.name) mesh.name = gm.name;
        mesh.weights.assign(gm.weights, gm.weights + gm.weights_count);
        for (cgltf_size j = 0; j < gm.primitives_count; ++j) {
            const cgltf_primitive& gp = gm.primitives[j];
            if (gp.type != cgltf_primitive_type_triangles && gp.type != cgltf_primitive_type_triangle_strip &&
                gp.type != cgltf_primitive_type_triangle_fan)
                continue;  // (points and lines: not drawn)
            ModelPrimitive p;
            for (cgltf_size k = 0; k < gp.attributes_count; ++k) {
                const cgltf_attribute& a = gp.attributes[k];
                if (a.type == cgltf_attribute_type_position) p.positions = floats(a.data);
                else if (a.type == cgltf_attribute_type_normal) p.normals = floats(a.data);
                else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) p.uvs = floats(a.data);
                else if (a.type == cgltf_attribute_type_tangent) p.tangents = floats(a.data);
                else if (a.type == cgltf_attribute_type_color && a.index == 0) {
                    const std::vector<float> c = floats(a.data);
                    const size_t n = cgltf_num_components(a.data->type);
                    if (n == 3 || n == 4)
                        for (size_t v = 0; v + n <= c.size(); v += n) p.colors.insert(p.colors.end(), {c[v], c[v + 1], c[v + 2], n == 4 ? c[v + 3] : 1.0f});
                } else if (a.type == cgltf_attribute_type_joints && a.index == 0) {
                    p.joints.resize(a.data->count * 4);
                    for (cgltf_size v = 0; v < a.data->count; ++v) {
                        cgltf_uint u[4] = {0, 0, 0, 0};
                        cgltf_accessor_read_uint(a.data, v, u, 4);
                        for (int c = 0; c < 4; ++c) p.joints[v * 4 + size_t(c)] = uint16_t(u[c]);
                    }
                } else if (a.type == cgltf_attribute_type_weights && a.index == 0) {
                    p.weights = floats(a.data);
                }
            }
            const size_t n = p.positions.size() / 3;
            if (n == 0) continue;
            p.positions.resize(n * 3);
            for (cgltf_size t = 0; t < gp.targets_count; ++t) {
                ModelPrimitive::Target target;
                for (cgltf_size k = 0; k < gp.targets[t].attributes_count; ++k) {
                    const cgltf_attribute& a = gp.targets[t].attributes[k];
                    if (a.type == cgltf_attribute_type_position) target.positions = floats(a.data);
                    else if (a.type == cgltf_attribute_type_normal) target.normals = floats(a.data);
                }
                if (target.positions.size() != n * 3) target.positions.clear();  // (not one a vertex: left out)
                if (target.normals.size() != n * 3) target.normals.clear();
                p.targets.push_back(std::move(target));
            }
            std::vector<uint32_t> order;
            if (gp.indices) {
                order.resize(gp.indices->count);
                for (cgltf_size k = 0; k < gp.indices->count; ++k) order[k] = uint32_t(cgltf_accessor_read_index(gp.indices, k));
            } else {
                order.resize(n);
                for (size_t k = 0; k < n; ++k) order[k] = uint32_t(k);
            }
            if (gp.type == cgltf_primitive_type_triangles) {
                p.indices = std::move(order);
                p.indices.resize(p.indices.size() / 3 * 3);
            } else {
                for (size_t k = 2; k < order.size(); ++k) {
                    if (gp.type == cgltf_primitive_type_triangle_fan) p.indices.insert(p.indices.end(), {order[0], order[k - 1], order[k]});
                    else if (k % 2 == 0) p.indices.insert(p.indices.end(), {order[k - 2], order[k - 1], order[k]});
                    else p.indices.insert(p.indices.end(), {order[k - 1], order[k - 2], order[k]});
                }
            }
            if (std::any_of(p.indices.begin(), p.indices.end(), [&](uint32_t v) { return v >= n; })) return fail(path + " has a mesh whose indices run past its vertices");
            if (p.normals.size() != n * 3) p.normals.clear();
            if (p.uvs.size() != n * 2) p.uvs.clear();
            if (p.tangents.size() != n * 4) p.tangents.clear();
            if (p.colors.size() != n * 4) p.colors.clear();
            if (p.joints.size() != n * 4 || p.weights.size() != n * 4) p.joints.clear(), p.weights.clear();
            if (gp.material) {
                p.material = int(cgltf_material_index(data, gp.material));
            } else {  // glTF's default material (metallic and rough), added once
                if (defaultMaterial < 0) defaultMaterial = int(out.materials.size()), out.materials.push_back(ModelMaterial{});
                p.material = defaultMaterial;
            }
            mesh.primitives.push_back(std::move(p));
        }
        out.meshes.push_back(std::move(mesh));
    }

    out.nodes.resize(data->nodes_count);
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& g = data->nodes[i];
        ModelNode& n = out.nodes[i];
        if (g.name) n.name = g.name;
        n.parent = g.parent ? int(cgltf_node_index(data, g.parent)) : -1;
        for (cgltf_size k = 0; k < g.children_count; ++k) n.children.push_back(int(cgltf_node_index(data, g.children[k])));
        if (g.has_matrix) {
            n.hasMatrix = true;
            std::copy(g.matrix, g.matrix + 16, n.matrix.begin());
        }
        if (g.has_translation) std::copy(g.translation, g.translation + 3, n.translation.begin());
        if (g.has_rotation) std::copy(g.rotation, g.rotation + 4, n.rotation.begin());
        if (g.has_scale) std::copy(g.scale, g.scale + 3, n.scale.begin());
        n.mesh = g.mesh ? int(cgltf_mesh_index(data, g.mesh)) : -1;
        n.skin = g.skin ? int(cgltf_skin_index(data, g.skin)) : -1;
        n.weights.assign(g.weights, g.weights + g.weights_count);
    }
    const cgltf_scene* scene = data->scene ? data->scene : data->scenes_count ? &data->scenes[0] : nullptr;
    if (scene)
        for (cgltf_size k = 0; k < scene->nodes_count; ++k) out.roots.push_back(int(cgltf_node_index(data, scene->nodes[k])));
    else
        for (size_t k = 0; k < out.nodes.size(); ++k)
            if (out.nodes[k].parent < 0) out.roots.push_back(int(k));

    for (cgltf_size i = 0; i < data->skins_count; ++i) {
        const cgltf_skin& g = data->skins[i];
        ModelSkin s;
        for (cgltf_size k = 0; k < g.joints_count; ++k) s.joints.push_back(int(cgltf_node_index(data, g.joints[k])));
        const std::vector<float> ib = floats(g.inverse_bind_matrices);
        for (size_t k = 0; k < s.joints.size(); ++k) {
            Mat4 m = identity4();
            if (ib.size() >= (k + 1) * 16) std::copy(ib.begin() + long(k * 16), ib.begin() + long(k * 16 + 16), m.begin());
            s.inverseBind.push_back(m);
        }
        out.skins.push_back(std::move(s));
    }

    for (cgltf_size i = 0; i < data->animations_count; ++i) {
        const cgltf_animation& g = data->animations[i];
        ModelAnimation a;
        a.name = g.name ? g.name : "Animation " + std::to_string(i + 1);
        for (cgltf_size k = 0; k < g.samplers_count; ++k) {
            const cgltf_animation_sampler& gs = g.samplers[k];
            ModelAnimation::Sampler s;
            s.times = floats(gs.input);
            s.values = floats(gs.output);
            s.interpolation = gs.interpolation == cgltf_interpolation_type_step           ? ModelAnimation::Sampler::Interpolation::Step
                              : gs.interpolation == cgltf_interpolation_type_cubic_spline ? ModelAnimation::Sampler::Interpolation::CubicSpline
                                                                                          : ModelAnimation::Sampler::Interpolation::Linear;
            const size_t keys = std::max<size_t>(1, s.times.size()) * (s.interpolation == ModelAnimation::Sampler::Interpolation::CubicSpline ? 3 : 1);
            s.components = int(s.values.size() / keys);
            if (!s.times.empty()) a.duration = std::max(a.duration, double(s.times.back()));
            a.samplers.push_back(std::move(s));
        }
        for (cgltf_size k = 0; k < g.channels_count; ++k) {
            const cgltf_animation_channel& gc = g.channels[k];
            if (!gc.target_node || !gc.sampler) continue;
            ModelAnimation::Channel c;
            c.node = int(cgltf_node_index(data, gc.target_node));
            c.sampler = int(cgltf_animation_sampler_index(&g, gc.sampler));
            switch (gc.target_path) {
                case cgltf_animation_path_type_translation: c.path = ModelAnimation::Channel::Path::Translation; break;
                case cgltf_animation_path_type_rotation: c.path = ModelAnimation::Channel::Path::Rotation; break;
                case cgltf_animation_path_type_scale: c.path = ModelAnimation::Channel::Path::Scale; break;
                case cgltf_animation_path_type_weights: c.path = ModelAnimation::Channel::Path::Weights; break;
                default: continue;
            }
            a.channels.push_back(c);
        }
        out.animations.push_back(std::move(a));
    }
    cgltf_free(data);
    return true;
}

// ---- OBJ -----------------------------------------------------------------------------------------------------------

bool loadObj(const std::string& path, const QByteArray& bytes, Model& out, std::string* error) {
    const QDir dir = QFileInfo(QString::fromStdString(path)).absoluteDir();
    // Its material libraries read here (so paths in any script work) and handed over as one.
    std::string mtl;
    for (const QByteArray& line : bytes.split('\n')) {
        const QByteArray l = line.trimmed();
        if (l.startsWith("mtllib")) {
            const QByteArray name = l.mid(6).trimmed();
            mtl += readWhole(dir.filePath(QString::fromUtf8(name))).toStdString() + "\n";
        }
    }
    tinyobj::ObjReader reader;
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;
    config.vertex_color = false;
    if (!reader.ParseFromString(bytes.toStdString(), mtl, config) || reader.GetShapes().empty()) {
        if (error) *error = path + " is not an OBJ model Montage can read" + (reader.Error().empty() ? "" : ": " + reader.Error());
        return false;
    }
    std::map<std::string, int> textures;
    auto texture = [&](const std::string& name, bool srgb) -> int {
        if (name.empty()) return -1;
        const std::string key = name + (srgb ? "|s" : "|l");
        if (auto it = textures.find(key); it != textures.end()) return it->second;
        ModelTexture t;
        const int id = decodeTexture(readWhole(dir.filePath(QString::fromStdString(name))), srgb, t) ? int(out.textures.size()) : -1;
        if (id >= 0) out.textures.push_back(std::move(t));
        return textures[key] = id;
    };
    for (const tinyobj::material_t& o : reader.GetMaterials()) {
        ModelMaterial m;
        m.name = o.name;
        m.baseColor = {float(o.diffuse[0]), float(o.diffuse[1]), float(o.diffuse[2]), float(o.dissolve)};
        m.metallic = float(o.metallic);
        // A roughness of its own (PBR extension), else from the specular exponent (Blinn-Phong to GGX).
        m.roughness = o.roughness > 0 ? float(o.roughness) : float(std::clamp(std::sqrt(2.0 / (double(o.shininess) + 2)), 0.05, 1.0));
        m.emissive = {float(o.emission[0]), float(o.emission[1]), float(o.emission[2])};
        m.baseTexture = texture(o.diffuse_texname, true);
        m.emissiveTexture = texture(o.emissive_texname, true);
        m.normalTexture = texture(o.normal_texname, false);
        if (o.dissolve < 1) m.alpha = ModelMaterial::Alpha::Blend;
        m.doubleSided = true;  // (OBJ says nothing of sides)
        out.materials.push_back(m);
    }
    const tinyobj::attrib_t& at = reader.GetAttrib();
    ModelMesh mesh;
    mesh.name = QFileInfo(QString::fromStdString(path)).completeBaseName().toStdString();
    // A primitive for each material, vertices shared where position, normal and uv all are.
    std::map<int, ModelPrimitive> byMaterial;
    std::map<int, std::map<std::tuple<int, int, int>, uint32_t>> seen;
    const size_t positions = at.vertices.size() / 3, normals = at.normals.size() / 3, uvs = at.texcoords.size() / 2;
    bool withNormals = true;
    for (const tinyobj::shape_t& s : reader.GetShapes()) {
        size_t offset = 0;
        for (size_t f = 0; f < s.mesh.num_face_vertices.size(); ++f) {
            const size_t fv = s.mesh.num_face_vertices[f];
            const int mat = f < s.mesh.material_ids.size() ? s.mesh.material_ids[f] : -1;
            ModelPrimitive& p = byMaterial[mat];
            p.material = mat >= 0 && size_t(mat) < out.materials.size() ? mat : -1;
            bool whole = fv == 3 && offset + 3 <= s.mesh.indices.size();  // (a face with a corner out of range: left out)
            for (size_t v = 0; v < 3 && whole; ++v) {
                const int vi = s.mesh.indices[offset + v].vertex_index;
                whole = vi >= 0 && size_t(vi) < positions;
            }
            for (size_t v = 0; v < 3 && whole; ++v) {
                const tinyobj::index_t ix = s.mesh.indices[offset + v];
                const int ni = ix.normal_index >= 0 && size_t(ix.normal_index) < normals ? ix.normal_index : -1;
                const int ti = ix.texcoord_index >= 0 && size_t(ix.texcoord_index) < uvs ? ix.texcoord_index : -1;
                if (ni < 0) withNormals = false;
                const auto key = std::make_tuple(ix.vertex_index, ni, ti);
                auto& known = seen[mat];
                auto it = known.find(key);
                if (it == known.end()) {
                    it = known.emplace(key, uint32_t(p.positions.size() / 3)).first;
                    for (int k = 0; k < 3; ++k) p.positions.push_back(at.vertices[size_t(ix.vertex_index) * 3 + size_t(k)]);
                    for (int k = 0; k < 3; ++k) p.normals.push_back(ni >= 0 ? at.normals[size_t(ni) * 3 + size_t(k)] : 0.0f);
                    p.uvs.push_back(ti >= 0 ? at.texcoords[size_t(ti) * 2] : 0.0f);
                    p.uvs.push_back(ti >= 0 ? 1 - at.texcoords[size_t(ti) * 2 + 1] : 0.0f);  // (OBJ's v counts up)
                }
                p.indices.push_back(it->second);
            }
            offset += fv;
        }
    }
    for (auto& [mat, p] : byMaterial) {
        if (p.indices.size() < 3) continue;
        p.indices.resize(p.indices.size() / 3 * 3);
        if (!withNormals) p.normals.clear();
        if (uvs == 0) p.uvs.clear();
        mesh.primitives.push_back(std::move(p));
    }
    out.meshes.push_back(std::move(mesh));
    ModelNode node;
    node.name = out.meshes.back().name;
    node.mesh = 0;
    out.nodes.push_back(node);
    out.roots = {0};
    return true;
}

}  // namespace

bool isModelPath(const std::string& path) {
    const QString s = QFileInfo(QString::fromStdString(path)).suffix().toLower();
    return s == "gltf" || s == "glb" || s == "obj";
}

bool loadModel(const std::string& path, Model& out, std::string* error) {
    out = Model{};
    out.path = path;
    const QByteArray bytes = readWhole(QString::fromStdString(path));
    if (bytes.isEmpty()) {
        if (error) *error = "Cannot read " + path;
        return false;
    }
    const QString suffix = QFileInfo(QString::fromStdString(path)).suffix().toLower();
    const bool gltf = bytes.startsWith("glTF") || suffix == "gltf" || suffix == "glb" || bytes.trimmed().startsWith("{");
    if (!(gltf ? loadGltf(path, bytes, out, error) : loadObj(path, bytes, out, error))) return false;
    finish(out);
    if (out.triangles() == 0) {
        if (error) *error = path + " has no triangles to draw";
        return false;
    }
    return true;
}

namespace {

// A file's size and time of change, as the cache compares them.
std::string fileStamp(const std::string& path) {
    const QFileInfo fi(QString::fromStdString(path));
    return std::to_string(fi.exists() ? fi.size() : -1) + "|" + std::to_string(fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0);
}

}  // namespace

std::shared_ptr<const Model> cachedModel(const std::string& path, std::string* error) {
    // Kept by path, with every file it was read from (its buffers, pictures and material libraries) as they were; a
    // file that would not load is kept too (with its error), so it is not read again each frame until it changes.
    struct Entry {
        std::string path;
        std::vector<std::pair<std::string, std::string>> stamps;
        std::shared_ptr<const Model> model;
        std::string error;
    };
    static std::mutex m;
    static std::list<Entry> recent;
    {
        std::lock_guard lock(m);
        for (auto it = recent.begin(); it != recent.end(); ++it)
            if (it->path == path) {
                const bool same = std::all_of(it->stamps.begin(), it->stamps.end(), [](const auto& s) { return fileStamp(s.first) == s.second; });
                if (!same) {
                    recent.erase(it);
                    break;
                }
                recent.splice(recent.begin(), recent, it);
                if (!it->model && error) *error = it->error;
                return it->model;
            }
    }
    Entry e;
    e.path = path;
    std::vector<std::string> files{path};
    const std::string before = fileStamp(path);
    auto model = std::make_shared<Model>();
    filesRead = &files;
    const bool ok = loadModel(path, *model, &e.error);
    filesRead = nullptr;
    if (ok) e.model = model;
    if (error && !ok) *error = e.error;
    for (const std::string& f : files) e.stamps.emplace_back(f, f == path ? before : fileStamp(f));
    std::lock_guard lock(m);
    recent.push_front(std::move(e));
    if (recent.size() > 4) recent.pop_back();
    return ok ? model : nullptr;
}

// ---- Posing --------------------------------------------------------------------------------------------------------

namespace {

std::array<double, 4> slerp(std::array<double, 4> a, std::array<double, 4> b, double t) {
    double d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0) {
        d = -d;
        for (double& v : b) v = -v;
    }
    std::array<double, 4> r;
    if (d > 0.9995) {
        for (int k = 0; k < 4; ++k) r[size_t(k)] = a[size_t(k)] + (b[size_t(k)] - a[size_t(k)]) * t;
    } else {
        const double th = std::acos(std::clamp(d, -1.0, 1.0)), s = std::sin(th);
        const double wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
        for (int k = 0; k < 4; ++k) r[size_t(k)] = a[size_t(k)] * wa + b[size_t(k)] * wb;
    }
    const double l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
    for (double& v : r) v = l > 0 ? v / l : 0;
    return r;
}

// A sampler's value at `time` (`components` values), rotations taken the short way round.
std::vector<double> sample(const ModelAnimation::Sampler& s, double time, bool rotation) {
    const size_t n = size_t(std::max(1, s.components));
    std::vector<double> out(n, 0);
    if (s.times.empty()) return out;
    const bool cubic = s.interpolation == ModelAnimation::Sampler::Interpolation::CubicSpline;
    auto value = [&](size_t key, size_t part) {  // part: 0 in-tangent, 1 value, 2 out-tangent (cubic)
        std::vector<double> v(n);
        const size_t base = cubic ? (key * 3 + part) * n : key * n;
        for (size_t k = 0; k < n; ++k) v[k] = base + k < s.values.size() ? s.values[base + k] : 0;
        return v;
    };
    if (time <= s.times.front()) return value(0, 1);
    if (time >= s.times.back()) return value(s.times.size() - 1, 1);
    const size_t hi = std::min(s.times.size() - 1, size_t(std::upper_bound(s.times.begin(), s.times.end(), time,
                                                                           [](double t, float key) { return t < double(key); }) -
                                                          s.times.begin()));
    const size_t lo = hi - 1;
    const double t0 = s.times[lo], t1 = s.times[hi], dt = t1 - t0, u = dt > 0 ? (time - t0) / dt : 0;
    if (s.interpolation == ModelAnimation::Sampler::Interpolation::Step) return value(lo, 1);
    if (cubic) {
        const std::vector<double> v0 = value(lo, 1), b0 = value(lo, 2), v1 = value(hi, 1), a1 = value(hi, 0);
        const double u2 = u * u, u3 = u2 * u;
        for (size_t k = 0; k < n; ++k)
            out[k] = (2 * u3 - 3 * u2 + 1) * v0[k] + (u3 - 2 * u2 + u) * dt * b0[k] + (-2 * u3 + 3 * u2) * v1[k] + (u3 - u2) * dt * a1[k];
        if (rotation && n == 4) {
            const double l = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3]);
            for (double& v : out) v = l > 0 ? v / l : 0;
        }
        return out;
    }
    const std::vector<double> a = value(lo, 1), b = value(hi, 1);
    if (rotation && n == 4) {
        const std::array<double, 4> r = slerp({a[0], a[1], a[2], a[3]}, {b[0], b[1], b[2], b[3]}, u);
        return {r[0], r[1], r[2], r[3]};
    }
    for (size_t k = 0; k < n; ++k) out[k] = a[k] + (b[k] - a[k]) * u;
    return out;
}

}  // namespace

ModelPose poseModel(const Model& m, int animation, double seconds) {
    std::vector<ModelNode> nodes = m.nodes;
    ModelPose pose;
    pose.weights.resize(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].mesh >= 0) pose.weights[i] = !nodes[i].weights.empty() ? nodes[i].weights : m.meshes[size_t(nodes[i].mesh)].weights;
    if (animation >= 0 && size_t(animation) < m.animations.size()) {
        const ModelAnimation& a = m.animations[size_t(animation)];
        for (const ModelAnimation::Channel& c : a.channels) {
            if (c.node < 0 || size_t(c.node) >= nodes.size() || c.sampler < 0 || size_t(c.sampler) >= a.samplers.size()) continue;
            ModelNode& n = nodes[size_t(c.node)];
            const std::vector<double> v = sample(a.samplers[size_t(c.sampler)], seconds, c.path == ModelAnimation::Channel::Path::Rotation);
            switch (c.path) {
                case ModelAnimation::Channel::Path::Translation:
                    if (v.size() >= 3) n.translation = {v[0], v[1], v[2]}, n.hasMatrix = false;
                    break;
                case ModelAnimation::Channel::Path::Rotation:
                    if (v.size() >= 4) n.rotation = {v[0], v[1], v[2], v[3]}, n.hasMatrix = false;
                    break;
                case ModelAnimation::Channel::Path::Scale:
                    if (v.size() >= 3) n.scale = {v[0], v[1], v[2]}, n.hasMatrix = false;
                    break;
                case ModelAnimation::Channel::Path::Weights:
                    pose.weights[size_t(c.node)].assign(v.begin(), v.end());
                    break;
            }
        }
    }
    pose.world.assign(nodes.size(), identity4());
    std::vector<char> done(nodes.size(), 0);
    // Parents before children (a node's parent chain, walked up then back down; a loop is broken where it closes).
    for (size_t i = 0; i < nodes.size(); ++i) {
        std::vector<size_t> chain;
        for (size_t k = i; !done[k];) {
            chain.push_back(k);
            done[k] = 2;  // (on the chain)
            const int p = nodes[k].parent;
            if (p < 0 || size_t(p) >= nodes.size() || done[size_t(p)] == 2) break;
            k = size_t(p);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            const ModelNode& n = nodes[*it];
            const Mat4 local = n.hasMatrix ? n.matrix : trsMatrix(n.translation, n.rotation, n.scale);
            const int p = n.parent;
            pose.world[*it] = p >= 0 && size_t(p) < nodes.size() && done[size_t(p)] == 1 ? pose.world[size_t(p)] * local : local;
            done[*it] = 1;
        }
    }
    return pose;
}

PosedTriangles posedTriangles(const Model& m, const ModelPose& pose) {
    PosedTriangles out;
    // Only the nodes the scene reaches.
    std::vector<int> stack(m.roots.rbegin(), m.roots.rend()), order;
    std::vector<char> seen(m.nodes.size(), 0);
    while (!stack.empty()) {
        const int n = stack.back();
        stack.pop_back();
        if (n < 0 || size_t(n) >= m.nodes.size() || seen[size_t(n)]) continue;
        seen[size_t(n)] = 1;
        order.push_back(n);
        for (auto it = m.nodes[size_t(n)].children.rbegin(); it != m.nodes[size_t(n)].children.rend(); ++it) stack.push_back(*it);
    }
    for (const int ni : order) {
        const ModelNode& node = m.nodes[size_t(ni)];
        if (node.mesh < 0 || size_t(node.mesh) >= m.meshes.size()) continue;
        const ModelMesh& mesh = m.meshes[size_t(node.mesh)];
        const std::vector<float>& weights = pose.weights[size_t(ni)];
        const ModelSkin* skin = node.skin >= 0 && size_t(node.skin) < m.skins.size() ? &m.skins[size_t(node.skin)] : nullptr;
        std::vector<Mat4> jointMats;
        if (skin)
            for (size_t j = 0; j < skin->joints.size(); ++j) {
                const int jn = skin->joints[j];
                jointMats.push_back((jn >= 0 && size_t(jn) < pose.world.size() ? pose.world[size_t(jn)] : identity4()) * skin->inverseBind[j]);
            }
        const Mat4& world = pose.world[size_t(ni)];
        for (const ModelPrimitive& p : mesh.primitives) {
            const size_t n = p.positions.size() / 3, base = out.positions.size() / 3;
            const bool skinned = skin && !p.joints.empty();
            bool flip = false;
            for (size_t v = 0; v < n; ++v) {
                std::array<double, 3> pos{p.positions[v * 3], p.positions[v * 3 + 1], p.positions[v * 3 + 2]};
                std::array<double, 3> nor{p.normals[v * 3], p.normals[v * 3 + 1], p.normals[v * 3 + 2]};
                for (size_t t = 0; t < p.targets.size() && t < weights.size(); ++t) {
                    const double w = weights[t];
                    if (w == 0) continue;
                    if (p.targets[t].positions.size() == n * 3)
                        for (int k = 0; k < 3; ++k) pos[size_t(k)] += w * p.targets[t].positions[v * 3 + size_t(k)];
                    if (p.targets[t].normals.size() == n * 3)
                        for (int k = 0; k < 3; ++k) nor[size_t(k)] += w * p.targets[t].normals[v * 3 + size_t(k)];
                }
                Mat4 M = world;
                if (skinned) {
                    M.fill(0);
                    double total = 0;
                    for (int k = 0; k < 4; ++k) {
                        const double w = p.weights[v * 4 + size_t(k)];
                        const size_t j = p.joints[v * 4 + size_t(k)];
                        if (w == 0 || j >= jointMats.size()) continue;
                        total += w;
                        for (int e = 0; e < 16; ++e) M[size_t(e)] += w * jointMats[j][size_t(e)];
                    }
                    if (total <= 0) M = world;
                    else if (std::fabs(total - 1) > 1e-6)
                        for (double& e : M) e /= total;
                }
                const Mat4 inv = inverse4(M);  // normals go through its inverse transpose
                const double det = M[0] * (M[5] * M[10] - M[9] * M[6]) - M[4] * (M[1] * M[10] - M[9] * M[2]) + M[8] * (M[1] * M[6] - M[5] * M[2]);
                const std::array<double, 3> P = transformPoint(M, pos);
                std::array<double, 3> N = {inv[0] * nor[0] + inv[1] * nor[1] + inv[2] * nor[2], inv[4] * nor[0] + inv[5] * nor[1] + inv[6] * nor[2],
                                           inv[8] * nor[0] + inv[9] * nor[1] + inv[10] * nor[2]};
                const double l = std::sqrt(N[0] * N[0] + N[1] * N[1] + N[2] * N[2]);
                for (double& c : N) c = l > 0 ? c / l : 0;
                out.positions.insert(out.positions.end(), {float(P[0]), float(P[1]), float(P[2])});
                out.normals.insert(out.normals.end(), {float(N[0]), float(N[1]), float(N[2])});
                out.uvs.insert(out.uvs.end(), {p.uvs.empty() ? 0.0f : p.uvs[v * 2], p.uvs.empty() ? 0.0f : p.uvs[v * 2 + 1]});
                if (!p.tangents.empty()) {
                    const std::array<double, 3> T = transformDirection(M, {p.tangents[v * 4], p.tangents[v * 4 + 1], p.tangents[v * 4 + 2]});
                    const double tl = std::sqrt(T[0] * T[0] + T[1] * T[1] + T[2] * T[2]);
                    out.tangents.insert(out.tangents.end(), {float(tl > 0 ? T[0] / tl : 1), float(tl > 0 ? T[1] / tl : 0), float(tl > 0 ? T[2] / tl : 0),
                                                             det < 0 ? -p.tangents[v * 4 + 3] : p.tangents[v * 4 + 3]});  // (mirrored: the bitangent too)
                } else {
                    out.tangents.insert(out.tangents.end(), {0, 0, 0, 0});  // (none)
                }
                if (!p.colors.empty()) out.colors.insert(out.colors.end(), p.colors.begin() + long(v * 4), p.colors.begin() + long(v * 4 + 4));
                else out.colors.insert(out.colors.end(), {1, 1, 1, 1});
                if (v == 0) flip = det < 0;  // (a mirrored node: its triangles' winding turns over)
            }
            for (size_t i = 0; i + 2 < p.indices.size(); i += 3) {
                const uint32_t a = uint32_t(base) + p.indices[i], b = uint32_t(base) + p.indices[i + 1], c = uint32_t(base) + p.indices[i + 2];
                if (flip) out.indices.insert(out.indices.end(), {a, c, b});
                else out.indices.insert(out.indices.end(), {a, b, c});
                out.materials.push_back(p.material);
            }
        }
    }
    return out;
}

bool modelBounds(const PosedTriangles& t, std::array<double, 3>& lo, std::array<double, 3>& hi) {
    if (t.indices.empty()) return false;
    lo = {1e300, 1e300, 1e300}, hi = {-1e300, -1e300, -1e300};
    for (uint32_t i : t.indices)
        for (int k = 0; k < 3; ++k) {
            lo[size_t(k)] = std::min(lo[size_t(k)], double(t.positions[size_t(i) * 3 + size_t(k)]));
            hi[size_t(k)] = std::max(hi[size_t(k)], double(t.positions[size_t(i) * 3 + size_t(k)]));
        }
    return true;
}

}  // namespace montage
