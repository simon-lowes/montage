// Montage — Gaussian splats (media/Splats.h). The SPZ reader and writer are written from Niantic's spz library (MIT,
// Copyright (c) 2024 Niantic Labs; its loader also Copyright (c) 2025 Adobe Inc.): the header, each attribute's
// packing, the smallest-three rotations and the harmonics' quantisation are as it reads and writes them.
#include "Splats.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>

#include "core/Zip.h"
#include "media/Image.h"
#include "media/ImageSequence.h"

#ifdef MONTAGE_WITH_OPENEXR
#include <zstd.h>  // (the copy OpenEXR builds in)
#endif

namespace montage {

namespace {

constexpr size_t kMaxSplats = size_t(1) << 24;
constexpr size_t kMaxUnpacked = size_t(1) << 31;  // the most an SPZ file's gzip may inflate to
constexpr uint32_t kSpzMagic = 0x5053474e;     // "NGSP"
constexpr float kSpzColorScale = 0.15f;

bool fail(std::string* error, const std::string& why) {
    if (error) *error = why;
    return false;
}

float sigmoid(double x) { return float(1 / (1 + std::exp(-std::clamp(x, -80.0, 80.0)))); }

// The real spherical harmonics of degrees 1 to 3 at unit direction (x, y, z), in the order and with the signs the
// splatting tools use.
void shBasis(double x, double y, double z, double* b) {
    constexpr double C1 = 0.4886025119029199;
    constexpr double C2[5] = {1.0925484305920792, -1.0925484305920792, 0.31539156525252005, -1.0925484305920792, 0.5462742152960396};
    constexpr double C3[7] = {-0.5900435899266435, 2.890611442640554, -0.4570457994644658, 0.3731763325901154,
                              -0.4570457994644658, 1.445305721320277, -0.5900435899266435};
    const double xx = x * x, yy = y * y, zz = z * z;
    b[0] = -C1 * y, b[1] = C1 * z, b[2] = -C1 * x;
    b[3] = C2[0] * x * y, b[4] = C2[1] * y * z, b[5] = C2[2] * (2 * zz - xx - yy), b[6] = C2[3] * x * z, b[7] = C2[4] * (xx - yy);
    b[8] = C3[0] * y * (3 * xx - yy), b[9] = C3[1] * x * y * z, b[10] = C3[2] * y * (4 * zz - xx - yy),
    b[11] = C3[3] * z * (2 * zz - 3 * xx - 3 * yy), b[12] = C3[4] * x * (4 * zz - xx - yy), b[13] = C3[5] * z * (xx - yy),
    b[14] = C3[6] * x * (xx - 3 * yy);
}

// Each coefficient's sign when the cloud turns 180° about x (y and z change sign): +1 or -1 as its function is even or
// odd under that turn.
const std::array<float, 15>& turnSigns() {
    static const std::array<float, 15> s = [] {
        std::array<float, 15> r{};
        double a[15], b[15];
        shBasis(0.36, 0.48, 0.8, a);
        shBasis(0.36, -0.48, -0.8, b);
        for (int k = 0; k < 15; ++k) r[size_t(k)] = a[k] * b[k] < 0 ? -1.0f : 1.0f;
        return r;
    }();
    return s;
}

// Turns the cloud 180° about x: y down, z ahead to y up, z back (and back again).
void turnAboutX(SplatCloud& c) {
    const std::array<float, 15>& sign = turnSigns();
    const size_t k = size_t(c.shCoefficients());
    for (size_t i = 0; i < c.count; ++i) {
        c.positions[i * 3 + 1] = -c.positions[i * 3 + 1], c.positions[i * 3 + 2] = -c.positions[i * 3 + 2];
        c.rotations[i * 4 + 1] = -c.rotations[i * 4 + 1], c.rotations[i * 4 + 2] = -c.rotations[i * 4 + 2];
        for (size_t j = 0; j < k; ++j)
            for (size_t ch = 0; ch < 3; ++ch) c.sh[(i * k + j) * 3 + ch] *= sign[j];
    }
}

// Makes each splat sound (a unit rotation; nothing that is not a number, hidden), and finds the bounds.
void finish(SplatCloud& c) {
    for (size_t i = 0; i < c.count; ++i) {
        float* q = &c.rotations[i * 4];
        const double l = std::sqrt(double(q[0]) * q[0] + double(q[1]) * q[1] + double(q[2]) * q[2] + double(q[3]) * q[3]);
        bool sound = l > 1e-12 && std::isfinite(l);
        for (int a = 0; a < 3; ++a)
            sound = sound && std::isfinite(c.positions[i * 3 + size_t(a)]) && std::isfinite(c.scales[i * 3 + size_t(a)]) &&
                    std::isfinite(c.colors[i * 3 + size_t(a)]);
        const size_t k = size_t(c.shCoefficients()) * 3;
        for (size_t j = 0; j < k && sound; ++j) sound = std::isfinite(c.sh[i * k + j]);
        if (!sound) {
            std::fill(c.colors.begin() + std::ptrdiff_t(i * 3), c.colors.begin() + std::ptrdiff_t(i * 3 + 3), 0.0f);
            std::fill(c.sh.begin() + std::ptrdiff_t(i * k), c.sh.begin() + std::ptrdiff_t(i * k + k), 0.0f);
            std::fill(q, q + 4, 0.0f), q[3] = 1;
            for (int a = 0; a < 3; ++a) c.positions[i * 3 + size_t(a)] = 0, c.scales[i * 3 + size_t(a)] = 0;
            c.opacities[i] = 0;
            continue;
        }
        for (int a = 0; a < 4; ++a) q[a] = float(q[a] / l);
        if (!std::isfinite(c.opacities[i])) c.opacities[i] = 0;
    }
    if (c.count == 0) return;
    // The middle 90% of the centres (captured scenes often stand inside a far dome of background splats), widened by
    // three times a typical splat's largest scale (so a few splats have a size too).
    std::vector<float> v(c.count);
    for (size_t i = 0; i < c.count; ++i) v[i] = std::max({c.scales[i * 3], c.scales[i * 3 + 1], c.scales[i * 3 + 2]});
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(c.count / 2), v.end());
    const float margin = 3 * v[c.count / 2];
    for (size_t a = 0; a < 3; ++a) {
        for (size_t i = 0; i < c.count; ++i) v[i] = c.positions[i * 3 + a];
        const size_t lo = c.count / 20, hi = c.count - 1 - c.count / 20;
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(lo), v.end());
        c.lo[a] = v[lo] - margin;
        std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(hi), v.end());
        c.hi[a] = v[hi] + margin;
    }
}

void resize(SplatCloud& c, size_t n, int degree) {
    c.count = n;
    c.shDegree = degree;
    c.positions.assign(n * 3, 0);
    c.scales.assign(n * 3, 0);
    c.rotations.assign(n * 4, 0);
    c.opacities.assign(n, 0);
    c.colors.assign(n * 3, 0);
    c.sh.assign(n * size_t(c.shCoefficients()) * 3, 0);
}

// The number of harmonic coefficients (degree 1 up) of a degree, and the degree a count of them gives.
int coefficientsOf(int degree) { return (degree + 1) * (degree + 1) - 1; }
int degreeOf(int coefficients) { return coefficients < 3 ? 0 : coefficients < 8 ? 1 : coefficients < 15 ? 2 : coefficients < 24 ? 3 : 4; }

// ---- PLY -----------------------------------------------------------------------------------------------------------

struct PlyProperty {
    std::string name;
    int size = 0;
    bool real = false, sign = false, list = false;
};

struct PlyElement {
    std::string name;
    uint64_t count = 0;
    std::vector<PlyProperty> properties;
    size_t stride() const {
        size_t s = 0;
        for (const PlyProperty& p : properties) s += size_t(p.size);
        return s;
    }
};

bool plyType(const std::string& t, PlyProperty& p) {
    struct Kind {
        const char* name;
        int size;
        bool real, sign;
    };
    static const Kind kinds[] = {{"char", 1, false, true},    {"int8", 1, false, true},    {"uchar", 1, false, false},
                                 {"uint8", 1, false, false},  {"short", 2, false, true},   {"int16", 2, false, true},
                                 {"ushort", 2, false, false}, {"uint16", 2, false, false}, {"int", 4, false, true},
                                 {"int32", 4, false, true},   {"uint", 4, false, false},   {"uint32", 4, false, false},
                                 {"float", 4, true, true},    {"float32", 4, true, true},  {"double", 8, true, true},
                                 {"float64", 8, true, true}};
    for (const Kind& k : kinds)
        if (t == k.name) {
            p.size = k.size, p.real = k.real, p.sign = k.sign;
            return true;
        }
    return false;
}

double plyValue(const uint8_t* at, const PlyProperty& p, bool big) {
    uint8_t b[8];
    for (int i = 0; i < p.size; ++i) b[i] = at[big ? p.size - 1 - i : i];
    switch (p.size) {
    case 1: return p.sign ? double(int8_t(b[0])) : double(b[0]);
    case 2: {
        uint16_t v;
        std::memcpy(&v, b, 2);
        return p.sign ? double(int16_t(v)) : double(v);
    }
    case 4: {
        uint32_t v;
        std::memcpy(&v, b, 4);
        if (p.real) {
            float f;
            std::memcpy(&f, &v, 4);
            return f;
        }
        return p.sign ? double(int32_t(v)) : double(v);
    }
    default: {
        double d;
        std::memcpy(&d, b, 8);
        return d;
    }
    }
}

// The header's lines up to end_header, and where the data starts; false if it is not a PLY header.
bool plyHeader(const uint8_t* data, size_t size, std::vector<std::string>& lines, size_t& start) {
    const size_t limit = std::min<size_t>(size, size_t(1) << 20);
    size_t at = 0;
    while (at < limit) {
        size_t end = at;
        while (end < limit && data[end] != '\n') ++end;
        if (end >= limit) return false;
        std::string line(reinterpret_cast<const char*>(data + at), end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        at = end + 1;
        if (lines.empty() && line != "ply") return false;
        if (line == "end_header") {
            start = at;
            return true;
        }
        lines.push_back(line);
    }
    return false;
}

bool loadPly(const std::string& path, const uint8_t* data, size_t size, SplatCloud& out, std::string* error) {
    std::vector<std::string> lines;
    size_t start = 0;
    if (!plyHeader(data, size, lines, start)) return fail(error, path + " is not a PLY file");
    bool big = false;
    std::vector<PlyElement> elements;
    for (size_t li = 1; li < lines.size(); ++li) {
        std::vector<std::string> w;
        for (const QString& s : QString::fromStdString(lines[li]).split(QLatin1Char(' '), Qt::SkipEmptyParts)) w.push_back(s.toStdString());
        if (w.empty() || w[0] == "comment" || w[0] == "obj_info") continue;
        if (w[0] == "format") {
            if (w.size() < 2) return fail(error, path + " has no format");
            if (w[1] == "ascii") return fail(error, path + " is a text PLY file: save it as binary to use its splats");
            if (w[1] != "binary_little_endian" && w[1] != "binary_big_endian") return fail(error, path + " is in a PLY format not known here");
            big = w[1] == "binary_big_endian";
        } else if (w[0] == "element" && w.size() >= 3) {
            PlyElement e;
            e.name = w[1];
            bool okCount = false;
            e.count = QString::fromStdString(w[2]).toULongLong(&okCount);
            if (!okCount) return fail(error, path + " has an element without a count");
            elements.push_back(e);
        } else if (w[0] == "property" && !elements.empty()) {
            PlyProperty p;
            if (w.size() >= 5 && w[1] == "list") {
                p.list = true;
                p.name = w[4];
            } else if (w.size() >= 3 && plyType(w[1], p)) {
                p.name = w[2];
            } else {
                return fail(error, path + " has a property of a type not known here");
            }
            if (elements.back().properties.size() >= 4096) return fail(error, path + " has too many properties");
            elements.back().properties.push_back(p);
        }
    }
    // The vertices, after any elements of a fixed size.
    size_t at = start;
    const PlyElement* vertex = nullptr;
    for (const PlyElement& e : elements) {
        if (e.name == "vertex") {
            vertex = &e;
            break;
        }
        for (const PlyProperty& p : e.properties)
            if (p.list) return fail(error, path + " has lists before its vertices");
        if (e.stride() && e.count > (size - at) / e.stride()) return fail(error, path + " is cut short");
        at += size_t(e.count) * e.stride();
    }
    if (!vertex) return fail(error, path + " has no vertices");
    for (const PlyProperty& p : vertex->properties)
        if (p.list) return fail(error, path + " is a PLY mesh, not Gaussian splats");
    auto find = [&](const std::string& name) {
        for (size_t i = 0; i < vertex->properties.size(); ++i)
            if (vertex->properties[i].name == name) return int(i);
        return -1;
    };
    const int xyz[3] = {find("x"), find("y"), find("z")};
    const int scale[3] = {find("scale_0"), find("scale_1"), find("scale_2")};
    const int rot[4] = {find("rot_1"), find("rot_2"), find("rot_3"), find("rot_0")};  // (x, y, z, w)
    const int opacity = find("opacity");
    int dc[3] = {find("f_dc_0"), find("f_dc_1"), find("f_dc_2")};
    const bool rgb = dc[0] < 0 || dc[1] < 0 || dc[2] < 0;
    if (rgb) dc[0] = find("red"), dc[1] = find("green"), dc[2] = find("blue");
    const bool splats = opacity >= 0 && std::all_of(std::begin(scale), std::end(scale), [](int i) { return i >= 0; }) &&
                        std::all_of(std::begin(rot), std::end(rot), [](int i) { return i >= 0; });
    if (!splats) return fail(error, path + " is a PLY point cloud or mesh, not Gaussian splats (it has no opacity, scales and rotations)");
    if (std::any_of(std::begin(xyz), std::end(xyz), [](int i) { return i < 0; }) || dc[0] < 0 || dc[1] < 0 || dc[2] < 0)
        return fail(error, path + " has splats without positions or colours");
    std::vector<int> rest;
    for (int i = 0;; ++i) {
        const int f = find("f_rest_" + std::to_string(i));
        if (f < 0) break;
        rest.push_back(f);
    }
    const int perChannel = int(rest.size() / 3), degree = degreeOf(perChannel);
    if (vertex->count == 0) return fail(error, path + " has no splats");
    if (vertex->count > kMaxSplats) return fail(error, path + " has more than 16,777,216 splats");
    const size_t stride = vertex->stride();
    if (stride == 0 || vertex->count > (size - at) / stride) return fail(error, path + " is cut short");
    const int kept = std::min(degree, 3);
    resize(out, size_t(vertex->count), kept);
    if (degree > 3) out.notes.push_back("The colour's fourth-degree harmonics are left out");
    std::vector<size_t> offset(vertex->properties.size());
    for (size_t i = 1; i < offset.size(); ++i) offset[i] = offset[i - 1] + size_t(vertex->properties[i - 1].size);
    const size_t K = size_t(coefficientsOf(kept));
    auto value = [&](const uint8_t* v, int i) { return plyValue(v + offset[size_t(i)], vertex->properties[size_t(i)], big); };
    for (size_t s = 0; s < out.count; ++s) {
        const uint8_t* v = data + at + s * stride;
        for (int a = 0; a < 3; ++a) {
            out.positions[s * 3 + size_t(a)] = float(value(v, xyz[a]));
            out.scales[s * 3 + size_t(a)] = float(std::exp(std::clamp(value(v, scale[a]), -40.0, 40.0)));
            const PlyProperty& cp = vertex->properties[size_t(dc[a])];
            const double c = value(v, dc[a]);
            out.colors[s * 3 + size_t(a)] = float(rgb ? ((cp.real ? c : c / 255) - 0.5) / kSplatSh0 : c);
        }
        for (int a = 0; a < 4; ++a) out.rotations[s * 4 + size_t(a)] = float(value(v, rot[a]));
        out.opacities[s] = sigmoid(value(v, opacity));
        for (size_t j = 0; j < K; ++j)
            for (size_t ch = 0; ch < 3; ++ch) out.sh[(s * K + j) * 3 + ch] = float(value(v, rest[ch * size_t(perChannel) + j]));
    }
    turnAboutX(out);
    return true;
}

// ---- SPZ -----------------------------------------------------------------------------------------------------------

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | uint64_t(le32(p + 4)) << 32; }

// The attribute arrays of an SPZ file, packed.
struct Packed {
    size_t count = 0;
    int degree = 0, fractionalBits = 12, version = 0;
    bool antialiased = false;
    std::vector<uint8_t> positions, alphas, colors, scales, rotations, sh;
    size_t positionBytes() const { return version == 1 ? 6 : 9; }
    size_t rotationBytes() const { return version >= 3 ? 4 : 3; }
    size_t shBytes() const { return size_t(coefficientsOf(degree)) * 3; }
    void allocate() {
        positions.resize(count * positionBytes());
        alphas.resize(count);
        colors.resize(count * 3);
        scales.resize(count * 3);
        rotations.resize(count * rotationBytes());
        sh.resize(count * shBytes());
    }
    std::vector<uint8_t>* stream(int i) {
        std::vector<uint8_t>* s[6] = {&positions, &alphas, &colors, &scales, &rotations, &sh};
        return s[i];
    }
    size_t streamBytes(int i) const {
        const size_t each[6] = {positionBytes(), 1, 3, 3, rotationBytes(), shBytes()};
        return count * each[i];
    }
    size_t bytes() const { return count * (positionBytes() + 1 + 3 + 3 + rotationBytes() + shBytes()); }
};

// The most a packed cloud of `fileSize` bytes is let unpack to: real clouds pack a few times smaller (16 leaves room),
// so a small file claiming millions of splats (or a compression bomb) is refused before anything is allocated.
uint64_t unpackLimit(size_t fileSize) { return std::min<uint64_t>(kMaxUnpacked, (uint64_t(64) << 20) + uint64_t(fileSize) * 16); }

bool unpack(const Packed& p, const std::string& path, SplatCloud& out, std::string* error) {
    if (p.fractionalBits > 30) return fail(error, path + " stores its positions in a way not known here");
    const int kept = std::min(p.degree, 3);
    resize(out, p.count, kept);
    out.antialiased = p.antialiased;
    if (p.degree > 3) out.notes.push_back("The colour's fourth-degree harmonics are left out");
    const double unit = 1.0 / double(int64_t(1) << p.fractionalBits);
    const size_t K = size_t(coefficientsOf(kept)), stored = size_t(coefficientsOf(p.degree));
    for (size_t i = 0; i < p.count; ++i) {
        for (size_t a = 0; a < 3; ++a) {
            if (p.version == 1) {
                const uint8_t* h = &p.positions[(i * 3 + a) * 2];
                out.positions[i * 3 + a] = halfToFloat(uint16_t(h[0] | h[1] << 8));
            } else {
                const uint8_t* b = &p.positions[(i * 3 + a) * 3];
                int32_t v = int32_t(b[0]) | int32_t(b[1]) << 8 | int32_t(b[2]) << 16;
                if (v & 0x800000) v -= 0x1000000;
                out.positions[i * 3 + a] = float(v * unit);
            }
            out.scales[i * 3 + a] = float(std::exp(p.scales[i * 3 + a] / 16.0 - 10));
            out.colors[i * 3 + a] = float((p.colors[i * 3 + a] / 255.0 - 0.5) / kSpzColorScale);
        }
        out.opacities[i] = float(p.alphas[i] / 255.0);
        float* q = &out.rotations[i * 4];
        if (p.version >= 3) {  // the smallest three, 10 bits each (a sign and 9 bits), and which was largest
            uint32_t comp = le32(&p.rotations[i * 4]);
            const int largest = int(comp >> 30);
            double sum = 0;
            for (int k = 3; k >= 0; --k) {
                if (k == largest) continue;
                const uint32_t mag = comp & 511u, neg = (comp >> 9) & 1u;
                comp >>= 10;
                const double v = 0.70710678118654752 * mag / 511.0;
                q[k] = float(neg ? -v : v);
                sum += v * v;
            }
            q[largest] = float(std::sqrt(std::max(0.0, 1 - sum)));
        } else {  // x, y and z, w made up as positive
            double sum = 0;
            for (int k = 0; k < 3; ++k) {
                const double v = p.rotations[i * 3 + size_t(k)] / 127.5 - 1;
                q[k] = float(v);
                sum += v * v;
            }
            q[3] = float(std::sqrt(std::max(0.0, 1 - sum)));
        }
        for (size_t j = 0; j < K; ++j)
            for (size_t ch = 0; ch < 3; ++ch) out.sh[(i * K + j) * 3 + ch] = float((p.sh[(i * stored + j) * 3 + ch] - 128.0) / 128.0);
    }
    return true;
}

bool loadSpz(const std::string& path, const std::string& bytes, SplatCloud& out, std::string* error) {
    const auto* d = reinterpret_cast<const uint8_t*>(bytes.data());
    const size_t size = bytes.size();
    Packed p;
    if (size >= 32 && le32(d) == kSpzMagic) {
        p.version = int(le32(d + 4));
        if (p.version != 4) return fail(error, path + " is an SPZ file of version " + std::to_string(p.version) + ", not read here");
        const uint32_t n = le32(d + 8);
        p.degree = d[12], p.fractionalBits = d[13], p.antialiased = d[14] & 1;
        const int streams = d[15];
        const size_t toc = le32(d + 16);
        if (n == 0 || n > kMaxSplats || uint64_t(n) > uint64_t(size) * 1024 / 9) return fail(error, path + " claims more splats than it can hold");
        if (p.degree > 4) return fail(error, path + " has harmonics of a degree not known here");
        if (d[14] & 2) out.notes.push_back("Its extensions are not read");
        p.count = n;
        if (toc < 32 || toc > size || size_t(streams) * 16 > size - toc) return fail(error, path + " is cut short");
        const int expected = p.degree > 0 ? 6 : 5;
        if (streams != expected) return fail(error, path + " has " + std::to_string(streams) + " streams, not " + std::to_string(expected));
        if (p.bytes() > unpackLimit(size)) return fail(error, path + " claims more splats than it can hold");
        for (int i = 0; i < streams; ++i)
            if (le64(d + toc + size_t(i) * 16 + 8) != p.streamBytes(i)) return fail(error, path + " is cut short or damaged");
#ifdef MONTAGE_WITH_OPENEXR
        p.allocate();
        size_t at = toc + size_t(streams) * 16;
        for (int i = 0; i < streams; ++i) {
            const uint64_t packed = le64(d + toc + size_t(i) * 16), unpacked = le64(d + toc + size_t(i) * 16 + 8);
            std::vector<uint8_t>& s = *p.stream(i);
            if (packed > size - at || unpacked != s.size()) return fail(error, path + " is cut short or damaged");
            const size_t r = ZSTD_decompress(s.data(), s.size(), d + at, size_t(packed));
            if (ZSTD_isError(r) || r != s.size()) return fail(error, path + " is damaged");
            at += size_t(packed);
        }
#else
        return fail(error, path + " is an SPZ version 4 file (zstd), which this build cannot read");
#endif
    } else if (size >= 2 && d[0] == 0x1f && d[1] == 0x8b) {
        std::string raw, why;
        if (!gunzip(bytes, raw, &why, size_t(unpackLimit(size)))) return fail(error, path + ": " + why);
        const auto* r = reinterpret_cast<const uint8_t*>(raw.data());
        if (raw.size() < 16 || le32(r) != kSpzMagic) return fail(error, path + " is not an SPZ file");
        p.version = int(le32(r + 4));
        if (p.version < 1 || p.version > 3) return fail(error, path + " is an SPZ file of version " + std::to_string(p.version) + ", not read here");
        const uint32_t n = le32(r + 8);
        p.degree = r[12], p.fractionalBits = r[13], p.antialiased = r[14] & 1;
        if (r[14] & 2) out.notes.push_back("Its extensions are not read");
        if (p.degree > 4) return fail(error, path + " has harmonics of a degree not known here");
        if (n == 0 || n > kMaxSplats) return fail(error, path + " has no splats or more than 16,777,216");
        p.count = n;
        const size_t each = p.positionBytes() + 1 + 3 + 3 + p.rotationBytes() + p.shBytes();
        if (raw.size() - 16 < p.count * each) return fail(error, path + " is cut short");
        p.allocate();
        size_t at = 16;
        for (int i = 0; i < 6; ++i) {
            std::vector<uint8_t>& s = *p.stream(i);
            if (!s.empty()) std::memcpy(s.data(), r + at, s.size());
            at += s.size();
        }
    } else {
        return fail(error, path + " is not an SPZ file");
    }
    return unpack(p, path, out, error);
}

uint8_t toByte(double v) { return uint8_t(std::clamp(std::round(v), 0.0, 255.0)); }

// The harmonics' coefficient to 8 bits, on a coarser grid (`bucket` codes a step) that keeps 0 exact.
uint8_t shByte(double v, int bucket) {
    int q = int(std::round(v * 128) + 128);
    q = (q + bucket / 2) / bucket * bucket;
    return uint8_t(std::clamp(q, 0, 255));
}

// A gzip file of stored (uncompressed) blocks: no smaller, but any reader takes it.
std::string gzipStored(const std::string& data) {
    std::string g("\x1f\x8b\x08\0\0\0\0\0\0\xff", 10);
    size_t at = 0;
    do {
        const size_t n = std::min<size_t>(65535, data.size() - at);
        const bool last = at + n == data.size();
        g += char(last ? 1 : 0);
        g += char(n & 0xff), g += char(n >> 8), g += char(~n & 0xff), g += char((~n >> 8) & 0xff);
        g.append(data, at, n);
        at += n;
    } while (at < data.size());
    const uint32_t crc = crc32(data.data(), data.size()), len = uint32_t(data.size());
    for (int i = 0; i < 4; ++i) g += char((crc >> (8 * i)) & 0xff);
    for (int i = 0; i < 4; ++i) g += char((len >> (8 * i)) & 0xff);
    return g;
}

bool saveSpz(const SplatCloud& c, const std::string& path, int version, std::string* error) {
#ifndef MONTAGE_WITH_OPENEXR
    version = std::min(version, 3);
#endif
    version = std::clamp(version, 2, 4);
    Packed p;
    p.count = c.count, p.degree = c.shDegree, p.version = version, p.antialiased = c.antialiased;
    p.allocate();
    const double unit = double(1 << p.fractionalBits), most = double((1 << 23) - 1);
    const size_t K = size_t(c.shCoefficients());
    for (size_t i = 0; i < c.count; ++i) {
        for (size_t a = 0; a < 3; ++a) {
            const auto v = int32_t(std::clamp(std::round(double(c.positions[i * 3 + a]) * unit), -most, most));
            for (int b = 0; b < 3; ++b) p.positions[(i * 3 + a) * 3 + size_t(b)] = uint8_t((v >> (8 * b)) & 0xff);
            p.scales[i * 3 + a] = toByte((std::log(std::max(1e-30, double(c.scales[i * 3 + a]))) + 10) * 16);
            p.colors[i * 3 + a] = toByte(c.colors[i * 3 + a] * (kSpzColorScale * 255.0) + 127.5);
        }
        p.alphas[i] = toByte(c.opacities[i] * 255.0);
        std::array<double, 4> q{c.rotations[i * 4], c.rotations[i * 4 + 1], c.rotations[i * 4 + 2], c.rotations[i * 4 + 3]};
        if (version >= 3) {
            int largest = 0;
            for (int k = 1; k < 4; ++k)
                if (std::abs(q[size_t(k)]) > std::abs(q[size_t(largest)])) largest = k;
            const bool negate = q[size_t(largest)] < 0;
            uint32_t comp = uint32_t(largest);
            for (int k = 0; k < 4; ++k) {
                if (k == largest) continue;
                const uint32_t neg = uint32_t((q[size_t(k)] < 0) != negate);
                const auto mag = uint32_t(511.0 * std::min(1.0, std::abs(q[size_t(k)]) / 0.70710678118654752) + 0.5);
                comp = comp << 10 | neg << 9 | mag;
            }
            for (int b = 0; b < 4; ++b) p.rotations[i * 4 + size_t(b)] = uint8_t((comp >> (8 * b)) & 0xff);
        } else {
            const double s = q[3] < 0 ? -127.5 : 127.5;
            for (int k = 0; k < 3; ++k) p.rotations[i * 3 + size_t(k)] = toByte(q[size_t(k)] * s + 127.5);
        }
        for (size_t j = 0; j < K; ++j)
            for (size_t ch = 0; ch < 3; ++ch)
                p.sh[(i * K + j) * 3 + ch] = shByte(c.sh[(i * K + j) * 3 + ch], j < 3 ? 1 << 3 : 1 << 4);  // (5 and 4 bits)
    }
    std::string file;
    auto put32 = [](std::string& s, uint32_t v) {
        for (int i = 0; i < 4; ++i) s += char((v >> (8 * i)) & 0xff);
    };
    const uint8_t flags = c.antialiased ? 1 : 0;
    if (version >= 4) {
#ifdef MONTAGE_WITH_OPENEXR
        std::vector<std::string> chunks;
        for (int i = 0; i < 6; ++i) {
            const std::vector<uint8_t>& s = *p.stream(i);
            if (s.empty()) continue;
            std::string z(ZSTD_compressBound(s.size()), '\0');
            const size_t n = ZSTD_compress(z.data(), z.size(), s.data(), s.size(), 9);
            if (ZSTD_isError(n)) return fail(error, "Cannot compress the splats");
            z.resize(n);
            chunks.push_back(std::move(z));
        }
        put32(file, kSpzMagic), put32(file, 4), put32(file, uint32_t(c.count));
        file += char(p.degree), file += char(p.fractionalBits), file += char(flags), file += char(chunks.size());
        put32(file, 32);
        file.append(12, '\0');
        int i = 0;
        for (int k = 0; k < 6; ++k) {
            const std::vector<uint8_t>& s = *p.stream(k);
            if (s.empty()) continue;
            put32(file, uint32_t(chunks[size_t(i)].size() & 0xffffffffu)), put32(file, uint32_t(uint64_t(chunks[size_t(i)].size()) >> 32));
            put32(file, uint32_t(s.size() & 0xffffffffu)), put32(file, uint32_t(uint64_t(s.size()) >> 32));
            ++i;
        }
        for (const std::string& z : chunks) file += z;
#endif
    } else {
        std::string raw;
        put32(raw, kSpzMagic), put32(raw, uint32_t(version)), put32(raw, uint32_t(c.count));
        raw += char(p.degree), raw += char(p.fractionalBits), raw += char(flags), raw += '\0';
        for (int i = 0; i < 6; ++i) raw.append(reinterpret_cast<const char*>(p.stream(i)->data()), p.stream(i)->size());
        file = gzipStored(raw);
    }
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::WriteOnly) || f.write(file.data(), qint64(file.size())) != qint64(file.size())) return fail(error, "Cannot write " + path);
    return true;
}

bool savePly(const SplatCloud& c, const std::string& path, std::string* error) {
    SplatCloud t = c;  // (back to y down, z ahead)
    turnAboutX(t);
    const size_t K = size_t(c.shCoefficients());
    std::string h = "ply\nformat binary_little_endian 1.0\nelement vertex " + std::to_string(c.count) + "\n";
    std::vector<std::string> names{"x", "y", "z", "nx", "ny", "nz", "f_dc_0", "f_dc_1", "f_dc_2"};
    for (size_t i = 0; i < K * 3; ++i) names.push_back("f_rest_" + std::to_string(i));
    for (const char* n : {"opacity", "scale_0", "scale_1", "scale_2", "rot_0", "rot_1", "rot_2", "rot_3"}) names.push_back(n);
    for (const std::string& n : names) h += "property float " + n + "\n";
    h += "end_header\n";
    std::vector<float> v;
    v.reserve(c.count * names.size());
    for (size_t i = 0; i < c.count; ++i) {
        for (size_t a = 0; a < 3; ++a) v.push_back(t.positions[i * 3 + a]);
        v.insert(v.end(), {0.0f, 0.0f, 0.0f});
        for (size_t a = 0; a < 3; ++a) v.push_back(t.colors[i * 3 + a]);
        for (size_t ch = 0; ch < 3; ++ch)
            for (size_t j = 0; j < K; ++j) v.push_back(t.sh[(i * K + j) * 3 + ch]);
        const double o = std::clamp(double(t.opacities[i]), 1e-7, 1 - 1e-7);
        v.push_back(float(std::log(o / (1 - o))));
        for (size_t a = 0; a < 3; ++a) v.push_back(float(std::log(std::max(1e-30, double(t.scales[i * 3 + a])))));
        v.push_back(t.rotations[i * 4 + 3]);
        for (size_t a = 0; a < 3; ++a) v.push_back(t.rotations[i * 4 + a]);
    }
    QFile f(QString::fromStdString(path));
    const qint64 bytes = qint64(v.size() * sizeof(float));
    if (!f.open(QIODevice::WriteOnly) || f.write(h.data(), qint64(h.size())) != qint64(h.size()) ||
        f.write(reinterpret_cast<const char*>(v.data()), bytes) != bytes)  // (floats little-endian on every platform built for)
        return fail(error, "Cannot write " + path);
    return true;
}

std::string suffixOf(const std::string& path) { return QFileInfo(QString::fromStdString(path)).suffix().toLower().toStdString(); }

std::string fileStamp(const std::string& path) {
    const QFileInfo fi(QString::fromStdString(path));
    return std::to_string(fi.exists() ? fi.size() : -1) + "|" + std::to_string(fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : 0);
}

}  // namespace

bool isSplatFile(const std::string& path) {
    const std::string s = suffixOf(path);
    return s == "ply" || s == "spz";
}

bool loadSplats(const std::string& path, SplatCloud& out, std::string* error) {
    out = SplatCloud{};
    out.path = path;
    QFile f(QString::fromStdString(path));
    if (!f.open(QIODevice::ReadOnly)) return fail(error, "Cannot read " + path);
    bool ok;
    if (suffixOf(path) == "spz") {
        if (f.size() > qint64(kMaxUnpacked)) return fail(error, path + " is too large");
        const QByteArray b = f.readAll();
        ok = loadSpz(path, b.toStdString(), out, error);
    } else {
        const qint64 size = f.size();
        const uchar* map = size > 0 ? f.map(0, size) : nullptr;
        QByteArray b;
        if (!map && size > 0) b = f.readAll();
        const auto* d = map ? reinterpret_cast<const uint8_t*>(map) : reinterpret_cast<const uint8_t*>(b.constData());
        ok = loadPly(path, d, map ? size_t(size) : size_t(b.size()), out, error);
    }
    if (!ok) {
        const std::string p = path;
        out = SplatCloud{};
        out.path = p;
        return false;
    }
    finish(out);
    return true;
}

bool saveSplats(const SplatCloud& cloud, const std::string& path, std::string* error, int spzVersion) {
    if (cloud.positions.size() != cloud.count * 3 || cloud.sh.size() != cloud.count * size_t(cloud.shCoefficients()) * 3)
        return fail(error, "The splats are not whole");
    return suffixOf(path) == "spz" ? saveSpz(cloud, path, spzVersion, error) : savePly(cloud, path, error);
}

std::shared_ptr<const SplatCloud> cachedSplats(const std::string& path, std::string* error) {
    struct Entry {
        std::string path, stamp, error;
        std::shared_ptr<const SplatCloud> cloud;
    };
    static std::mutex m;
    static std::list<Entry> recent;
    const std::string stamp = fileStamp(path);
    {
        std::lock_guard lock(m);
        for (auto it = recent.begin(); it != recent.end(); ++it)
            if (it->path == path) {
                if (it->stamp != stamp) {
                    recent.erase(it);
                    break;
                }
                recent.splice(recent.begin(), recent, it);
                if (!it->cloud && error) *error = it->error;
                return it->cloud;
            }
    }
    Entry e;
    e.path = path, e.stamp = stamp;
    auto cloud = std::make_shared<SplatCloud>();
    if (loadSplats(path, *cloud, &e.error)) e.cloud = cloud;
    else if (error) *error = e.error;
    std::lock_guard lock(m);
    recent.push_front(e);
    if (recent.size() > 3) recent.pop_back();
    return e.cloud;
}

std::array<float, 3> splatColor(const SplatCloud& c, size_t i, const std::array<double, 3>& dir) {
    std::array<double, 3> v{0.5 + kSplatSh0 * c.colors[i * 3], 0.5 + kSplatSh0 * c.colors[i * 3 + 1], 0.5 + kSplatSh0 * c.colors[i * 3 + 2]};
    const size_t K = size_t(c.shCoefficients());
    if (K) {
        double b[15];
        shBasis(dir[0], dir[1], dir[2], b);
        const float* s = &c.sh[i * K * 3];
        for (size_t j = 0; j < K; ++j)
            for (size_t ch = 0; ch < 3; ++ch) v[ch] += b[j] * s[j * 3 + ch];
    }
    return {float(std::max(0.0, v[0])), float(std::max(0.0, v[1])), float(std::max(0.0, v[2]))};
}

std::string splatFrameFile(const std::string& file, double seconds, double fps, bool loop, int* frames) {
    if (frames) *frames = 1;
    // The run, found once while its folder is unchanged.
    struct Run {
        std::string file, stamp;
        bool numbered = false;
        ImageSequence seq;
    };
    static std::mutex m;
    static std::list<Run> recent;
    const QFileInfo fi(QString::fromStdString(file));
    const QFileInfo dir(fi.absolutePath());
    const std::string stamp = std::to_string(dir.lastModified().toMSecsSinceEpoch());
    Run run;
    bool found = false;
    {
        std::lock_guard lock(m);
        for (auto it = recent.begin(); it != recent.end(); ++it)
            if (it->file == file && it->stamp == stamp) {
                recent.splice(recent.begin(), recent, it);
                run = *it;
                found = true;
                break;
            }
    }
    if (!found) {
        run.file = file, run.stamp = stamp;
        run.numbered = detectImageSequence(file, run.seq);
        std::lock_guard lock(m);
        recent.push_front(run);
        if (recent.size() > 8) recent.pop_back();
    }
    if (!run.numbered) return file;
    // Its files are named as `file` is, the number written the same way.
    static const QRegularExpression re(QStringLiteral("^(.*?)(\\d+)(\\.[A-Za-z0-9]+)$"));
    const auto match = re.match(fi.fileName());
    if (!match.hasMatch()) return file;
    const QString digits = match.captured(2);
    const bool padded = digits.size() > 1 && digits.startsWith(QLatin1Char('0'));
    auto name = [&](int n) {
        const QString number = padded ? QStringLiteral("%1").arg(n, int(digits.size()), 10, QLatin1Char('0')) : QString::number(n);
        return QDir(fi.absolutePath()).filePath(match.captured(1) + number + match.captured(3)).toStdString();
    };
    // From the file chosen to the run's end (a file chosen part way in starts there, and an unrelated capture numbered
    // just before it is not played).
    const int from = std::clamp(digits.toInt(), run.seq.first, run.seq.last);
    const int count = run.seq.last - from + 1;
    if (frames) *frames = count;
    auto n = int64_t(std::floor(std::clamp(seconds * fps, -1e15, 1e15) + 1e-6));
    n = loop ? ((n % count) + count) % count : std::clamp<int64_t>(n, 0, count - 1);
    return name(from + int(n));
}

}  // namespace montage
