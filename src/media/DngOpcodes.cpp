#include "DngOpcodes.h"
#include "Image.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace montage {

namespace {

// Whole-frame passes one opcode list may cost (see the appliers).
constexpr double kFramePassBudget = 16;

// Big-endian reads from an opcode's parameters (opcode lists are always big-endian).
struct Reader {
    const uint8_t* p;
    size_t size, at = 0;
    bool ok = true;
    bool need(size_t n) {
        if (!ok || at + n > size) ok = false;
        return ok;
    }
    uint32_t u32() {
        if (!need(4)) return 0;
        const uint32_t v = uint32_t(p[at]) << 24 | uint32_t(p[at + 1]) << 16 | uint32_t(p[at + 2]) << 8 | p[at + 3];
        at += 4;
        return v;
    }
    double f64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = v << 8 | p[at + size_t(i)];
        at += 8;
        double d;
        std::memcpy(&d, &v, 8);
        return d;
    }
    float f32() {
        const uint32_t v = u32();
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
};

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}

// ---- Warps ------------------------------------------------------------------------------------------------------

struct WarpParams {
    bool fisheye = false;
    std::vector<std::array<double, 6>> sets;  // kr0..kr3, kt0, kt1 (fisheye: no tangential terms)
    double cx = 0.5, cy = 0.5;
};
bool readWarp(const DngOpcode& op, WarpParams& w) {
    if (op.id != kWarpRectilinear && op.id != kWarpFisheye) return false;
    Reader r{op.params.data(), op.params.size()};
    const uint32_t n = r.u32();
    if (n < 1 || n > 4) return false;
    w.fisheye = op.id == kWarpFisheye;
    for (uint32_t i = 0; i < n; ++i) {
        std::array<double, 6> k{};
        for (int j = 0; j < (w.fisheye ? 4 : 6); ++j) k[size_t(j)] = r.f64();
        w.sets.push_back(k);
    }
    w.cx = r.f64();
    w.cy = r.f64();
    for (const auto& k : w.sets)
        for (double v : k)
            if (!std::isfinite(v) || std::fabs(v) > 1e6) return false;
    return r.ok && std::isfinite(w.cx) && std::isfinite(w.cy) && std::fabs(w.cx) <= 1e3 && std::fabs(w.cy) <= 1e3;
}

bool warpSource(const WarpParams& w, int plane, double x, double y, int width, int height, double& sx, double& sy) {
    if (w.sets.empty()) return false;
    const auto& k = w.sets[w.sets.size() == 1 ? 0 : size_t(std::min(plane, int(w.sets.size()) - 1))];
    const double x1 = width - 1, y1 = height - 1;
    const double cx = w.cx * x1, cy = w.cy * y1;
    const double mx = std::max(std::fabs(cx), std::fabs(x1 - cx)), my = std::max(std::fabs(cy), std::fabs(y1 - cy));
    const double m = std::sqrt(mx * mx + my * my);
    if (m <= 0) return false;
    const double dx = (x - cx) / m, dy = (y - cy) / m;
    const double r2 = dx * dx + dy * dy;
    if (w.fisheye) {
        const double r = std::sqrt(r2);
        double f = k[0];  // (the limit as r goes to 0)
        if (r > 1e-12) {
            const double t = std::atan(r), t2 = t * t;
            f = (k[0] * t + k[1] * t * t2 + k[2] * t * t2 * t2 + k[3] * t * t2 * t2 * t2) / r;
        }
        sx = cx + m * f * dx;
        sy = cy + m * f * dy;
        return true;
    }
    const double f = k[0] + k[1] * r2 + k[2] * r2 * r2 + k[3] * r2 * r2 * r2;
    const double xt = k[4] * (2 * dx * dy) + k[5] * (r2 + 2 * dx * dx);
    const double yt = k[5] * (2 * dx * dy) + k[4] * (r2 + 2 * dy * dy);
    sx = cx + m * (f * dx + xt);
    sy = cy + m * (f * dy + yt);
    return true;
}

// Keys' cubic (a = -0.5): the cubic spline the specification recommends.
inline double cubicWeight(double t) {
    t = std::fabs(t);
    if (t < 1) return (1.5 * t - 2.5) * t * t + 1;
    if (t < 2) return ((-0.5 * t + 2.5) * t - 4) * t + 2;
    return 0;
}
float sampleCubic(const float* src, int width, int height, int planes, int plane, double x, double y) {
    // (a far-off position, as a crafted warp gives, reads the edge: kept where the conversion to int is defined)
    x = std::clamp(x, -4.0, double(width) + 4), y = std::clamp(y, -4.0, double(height) + 4);
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const double fx = x - ix, fy = y - iy;
    double sum = 0;
    for (int j = -1; j <= 2; ++j) {
        const double wy = cubicWeight(fy - j);
        const int yy = std::clamp(iy + j, 0, height - 1);
        const float* row = src + size_t(yy) * size_t(width) * size_t(planes);
        for (int i = -1; i <= 2; ++i) {
            const int xx = std::clamp(ix + i, 0, width - 1);
            sum += wy * cubicWeight(fx - i) * row[size_t(xx) * size_t(planes) + size_t(plane)];
        }
    }
    return float(sum);
}

void applyWarp(const WarpParams& w, float* pixels, int width, int height, int planes) {
    const std::vector<float> src(pixels, pixels + size_t(width) * size_t(height) * size_t(planes));
    parallelRows(height, [&](int y0, int y1) {  // (rows on every core: a camera's lens warp on each CinemaDNG frame)
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < width; ++x)
                for (int p = 0; p < planes; ++p) {
                    double sx, sy;
                    if (!warpSource(w, p, x, y, width, height, sx, sy)) continue;
                    pixels[(size_t(y) * size_t(width) + size_t(x)) * size_t(planes) + size_t(p)] =
                        std::clamp(sampleCubic(src.data(), width, height, planes, p, sx, sy), 0.0f, 1.0f);
                }
    });
}

// ---- Vignette ---------------------------------------------------------------------------------------------------

struct Vignette {
    double k[5]{}, cx = 0.5, cy = 0.5;
};
bool readVignette(const DngOpcode& op, Vignette& v) {
    Reader r{op.params.data(), op.params.size()};
    for (double& k : v.k) k = r.f64();
    v.cx = r.f64();
    v.cy = r.f64();
    for (double k : v.k)
        if (!std::isfinite(k) || std::fabs(k) > 1e6) return false;
    return r.ok && std::isfinite(v.cx) && std::isfinite(v.cy) && std::fabs(v.cx) <= 1e3 && std::fabs(v.cy) <= 1e3;
}
double vignetteGain(const Vignette& v, double x, double y, int width, int height) {
    const double x1 = width - 1, y1 = height - 1;
    const double cx = v.cx * x1, cy = v.cy * y1;
    const double mx = std::max(std::fabs(cx), std::fabs(x1 - cx)), my = std::max(std::fabs(cy), std::fabs(y1 - cy));
    const double m = std::sqrt(mx * mx + my * my);
    if (m <= 0) return 1;
    const double r2 = ((x - cx) * (x - cx) + (y - cy) * (y - cy)) / (m * m);
    return 1 + r2 * (v.k[0] + r2 * (v.k[1] + r2 * (v.k[2] + r2 * (v.k[3] + r2 * v.k[4]))));
}

// ---- Gain map ---------------------------------------------------------------------------------------------------

struct GainMap {
    uint32_t top = 0, left = 0, bottom = 0, right = 0, plane = 0, planes = 1, rowPitch = 1, colPitch = 1;
    uint32_t pointsV = 0, pointsH = 0, mapPlanes = 1;
    double spacingV = 0, spacingH = 0, originV = 0, originH = 0;
    std::vector<float> gains;  // pointsV x pointsH x mapPlanes
};
bool readGainMap(const DngOpcode& op, GainMap& g) {
    Reader r{op.params.data(), op.params.size()};
    g.top = r.u32(), g.left = r.u32(), g.bottom = r.u32(), g.right = r.u32();
    g.plane = r.u32(), g.planes = r.u32(), g.rowPitch = r.u32(), g.colPitch = r.u32();
    g.pointsV = r.u32(), g.pointsH = r.u32();
    g.spacingV = r.f64(), g.spacingH = r.f64(), g.originV = r.f64(), g.originH = r.f64();
    g.mapPlanes = r.u32();
    for (double d : {g.spacingV, g.spacingH, g.originV, g.originH})
        if (!std::isfinite(d) || std::fabs(d) > 1e6) return false;
    if (!r.ok || g.pointsV == 0 || g.pointsH == 0 || g.mapPlanes == 0 || g.pointsV > 65536 || g.pointsH > 65536 || g.mapPlanes > 4 ||
        g.rowPitch == 0 || g.colPitch == 0 || g.rowPitch > (1u << 20) || g.colPitch > (1u << 20) || uint64_t(g.pointsV) * g.pointsH * g.mapPlanes * 4 > op.params.size())
        return false;
    g.gains.resize(size_t(g.pointsV) * g.pointsH * g.mapPlanes);
    for (float& v : g.gains) {
        v = r.f32();
        if (!std::isfinite(v)) v = 1;
    }
    return r.ok;
}
// The map's gain for one of its planes at a pixel's centre (relative to the picture's size), bilinear inside the map and
// its edge values carried on outside.
double gainAt(const GainMap& g, int mapPlane, double row, double col, int width, int height) {
    const double v = ((row + 0.5) / height - g.originV) / (g.spacingV > 0 ? g.spacingV : 1);
    const double h = ((col + 0.5) / width - g.originH) / (g.spacingH > 0 ? g.spacingH : 1);
    const double cv = std::clamp(v, 0.0, double(g.pointsV - 1)), ch = std::clamp(h, 0.0, double(g.pointsH - 1));
    const int v0 = std::min(int(cv), int(g.pointsV) - 1), h0 = std::min(int(ch), int(g.pointsH) - 1);
    const int v1 = std::min(v0 + 1, int(g.pointsV) - 1), h1 = std::min(h0 + 1, int(g.pointsH) - 1);
    const double fv = cv - v0, fh = ch - h0;
    auto at = [&](int a, int b) { return double(g.gains[(size_t(a) * g.pointsH + size_t(b)) * g.mapPlanes + size_t(mapPlane)]); };
    return (1 - fv) * ((1 - fh) * at(v0, h0) + fh * at(v0, h1)) + fv * ((1 - fh) * at(v1, h0) + fh * at(v1, h1));
}

// ---- Bad pixels -------------------------------------------------------------------------------------------------

// Whether a mosaic pixel is green, for the opcode's Bayer phase (0: red top-left, 1: green on a red row, 2: green on
// a blue row, 3: blue).
bool greenAt(uint32_t phase, int row, int col) { return ((row + col) & 1) == ((phase == 1 || phase == 2) ? 0 : 1); }

// Each marked pixel replaced by the mean of its unmarked neighbours of its own colour (greens: the four diagonal ones
// and the four two away; reds and blues: the eight two away).
void patchBadPixels(uint16_t* raw, int width, int height, int stride, uint32_t phase, const std::vector<uint8_t>& bad) {
    static const int kGreen[8][2] = {{-1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-2, 0}, {2, 0}, {0, -2}, {0, 2}};
    static const int kOther[8][2] = {{-2, -2}, {-2, 0}, {-2, 2}, {0, -2}, {0, 2}, {2, -2}, {2, 0}, {2, 2}};
    std::vector<std::pair<size_t, uint16_t>> fixes;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            if (!bad[size_t(y) * size_t(width) + size_t(x)]) continue;
            const auto& offs = greenAt(phase, y, x) ? kGreen : kOther;
            double sum = 0;
            int n = 0;
            for (const auto& o : offs) {
                const int yy = y + o[0], xx = x + o[1];
                if (yy < 0 || xx < 0 || yy >= height || xx >= width || bad[size_t(yy) * size_t(width) + size_t(xx)]) continue;
                sum += raw[size_t(yy) * size_t(stride) + size_t(xx)];
                ++n;
            }
            if (n > 0) fixes.push_back({size_t(y) * size_t(stride) + size_t(x), uint16_t(std::lround(sum / n))});
        }
    for (const auto& [i, v] : fixes) raw[i] = v;
}

// How many whole-frame passes a gain map's area at its pitch comes to (at least a little: each opcode costs).
double gainMapPasses(const GainMap& g, int width, int height) {
    const double rows = (std::min<double>(g.bottom, height) - std::min<double>(g.top, height)) / std::max(1u, g.rowPitch);
    const double cols = (std::min<double>(g.right, width) - std::min<double>(g.left, width)) / std::max(1u, g.colPitch);
    return std::max(0.05, std::max(0.0, rows) * std::max(0.0, cols) * std::max(1u, g.planes) / std::max(1.0, double(width) * height));
}

}  // namespace

bool parseDngOpcodeList(const uint8_t* data, size_t size, DngOpcodeList& out) {
    out.clear();
    Reader r{data, size};
    const uint32_t count = r.u32();
    if (!r.ok || count > 256) return false;  // (cameras write a handful; this bounds the work a crafted file asks for)
    for (uint32_t i = 0; i < count; ++i) {
        DngOpcode op;
        op.id = r.u32();
        op.version = r.u32();
        op.flags = r.u32();
        const uint32_t bytes = r.u32();
        if (!r.need(bytes)) return false;
        op.params.assign(data + r.at, data + r.at + bytes);
        r.at += bytes;
        out.push_back(std::move(op));
    }
    return r.ok;
}

std::vector<uint8_t> dngOpcodeListBytes(const DngOpcodeList& list) {
    std::vector<uint8_t> out;
    putU32(out, uint32_t(list.size()));
    for (const DngOpcode& op : list) {
        putU32(out, op.id);
        putU32(out, op.version);
        putU32(out, op.flags);
        putU32(out, uint32_t(op.params.size()));
        out.insert(out.end(), op.params.begin(), op.params.end());
    }
    return out;
}

bool readDngOpcodeLists(const std::string& path, DngOpcodeLists& out) {
    out = {};
    std::unique_ptr<FILE, int (*)(FILE*)> f(std::fopen(path.c_str(), "rb"), std::fclose);
    if (!f) return false;
    std::fseek(f.get(), 0, SEEK_END);
    const long fileSize = std::ftell(f.get());
    uint8_t head[8];
    std::fseek(f.get(), 0, SEEK_SET);
    if (fileSize < 8 || std::fread(head, 1, 8, f.get()) != 8) return false;
    const bool le = head[0] == 'I' && head[1] == 'I';
    if (!le && !(head[0] == 'M' && head[1] == 'M')) return false;
    auto u16 = [&](const uint8_t* p) { return uint16_t(le ? p[0] | p[1] << 8 : p[0] << 8 | p[1]); };
    auto u32 = [&](const uint8_t* p) {
        return le ? uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24
                  : uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
    };
    if (u16(head + 2) != 42) return false;
    auto readAt = [&](uint32_t at, size_t n, std::vector<uint8_t>& buf) {
        if (uint64_t(at) + n > uint64_t(fileSize)) return false;
        buf.resize(n);
        std::fseek(f.get(), long(at), SEEK_SET);
        return std::fread(buf.data(), 1, n, f.get()) == n;
    };
    // IFD 0 and its chain, and their SubIFDs (one level down, as DNGs put the raw picture there behind a preview).
    std::vector<uint32_t> todo{u32(head + 4)};
    std::vector<uint32_t> seen;
    bool found = false;
    while (!todo.empty() && seen.size() < 64 && !found) {
        const uint32_t ifd = todo.front();
        todo.erase(todo.begin());
        if (ifd == 0 || std::find(seen.begin(), seen.end(), ifd) != seen.end()) continue;
        seen.push_back(ifd);
        std::vector<uint8_t> buf;
        if (!readAt(ifd, 2, buf)) continue;
        const uint16_t n = u16(buf.data());
        std::vector<uint8_t> entries;
        if (!readAt(ifd + 2, size_t(n) * 12 + 4, entries)) continue;
        uint32_t subfileType = 0;
        std::pair<uint32_t, uint32_t> lists[3] = {};  // (offset, size)
        for (int i = 0; i < n; ++i) {
            const uint8_t* e = &entries[size_t(i) * 12];
            const uint16_t tag = u16(e), type = u16(e + 2);
            const uint32_t count = u32(e + 4);
            if (tag == 254) subfileType = type == 3 ? u16(e + 8) : u32(e + 8);
            else if (tag == 330 && (type == 4 || type == 13)) {
                if (count == 1) todo.push_back(u32(e + 8));
                else if (std::vector<uint8_t> subs; count <= 64 && readAt(u32(e + 8), size_t(count) * 4, subs))
                    for (uint32_t k = 0; k < count; ++k) todo.push_back(u32(&subs[size_t(k) * 4]));
            } else if (tag == 51008 || tag == 51009 || tag == 51022) {
                const int which = tag == 51008 ? 0 : tag == 51009 ? 1 : 2;
                lists[which] = {count <= 4 ? ifd + 2 + uint32_t(i) * 12 + 8 : u32(e + 8), count};  // (UNDEFINED: a byte each)
            }
        }
        todo.push_back(u32(&entries[size_t(n) * 12]));  // (the next IFD)
        if (subfileType != 0) continue;
        found = true;
        for (int k = 0; k < 3; ++k) {
            if (lists[k].second == 0 || lists[k].second > (64u << 20)) continue;
            std::vector<uint8_t> bytes;
            if (readAt(lists[k].first, lists[k].second, bytes)) parseDngOpcodeList(bytes.data(), bytes.size(), out.list[k]);
        }
    }
    return found;
}

bool dngWarpSource(const DngOpcode& op, int plane, double x, double y, int width, int height, double& sx, double& sy) {
    WarpParams w;
    return readWarp(op, w) && warpSource(w, plane, x, y, width, height, sx, sy);
}

int applyDngMosaicOpcodes(const DngOpcodeList& list, int stage, uint16_t* raw, int width, int height, int stride, double black,
                          double white, const double* blackPattern, int patternRows, int patternCols) {
    if (!raw || width <= 0 || height <= 0) return 0;
    const double top = stage == 1 ? 65535.0 : white;  // (the logical range: list 1's the stored values', list 2's 0..1)
    const bool pattern = stage != 1 && blackPattern && patternRows > 0 && patternCols > 0;
    const double flat = stage == 1 ? 0.0 : black;
    // A gain scales the light above black: v = b + (v - b) * g, kept between black and white.
    auto scaled = [&](uint16_t& v, int y, int x, double gain) {
        const double b = pattern ? flat + blackPattern[size_t(y % patternRows) * size_t(patternCols) + size_t(x % patternCols)] : flat;
        const double span = std::max(1.0, top - b);
        const double l = (v - b) / span * gain;
        v = uint16_t(std::clamp(std::lround(b + std::clamp(l, 0.0, 1.0) * span), 0L, 65535L));
    };
    int applied = 0;
    // The work a list may ask for, in whole-frame passes: a camera's needs a few; a crafted one of hundreds of
    // full-frame opcodes would stall every frame of a sequence, so those past the budget are left out.
    double budget = kFramePassBudget;
    for (const DngOpcode& op : list) {
        if (budget <= 0) break;
        switch (op.id) {
            case kGainMap: {
                GainMap g;
                if (!readGainMap(op, g) || g.plane != 0) break;  // (a mosaic has one plane)
                budget -= gainMapPasses(g, width, height);
                const int y0 = int(std::min<uint32_t>(g.top, uint32_t(height))), y1 = int(std::min<uint32_t>(g.bottom, uint32_t(height)));
                const int x0 = int(std::min<uint32_t>(g.left, uint32_t(width))), x1 = int(std::min<uint32_t>(g.right, uint32_t(width)));
                for (int y = y0; y < y1; y += int(g.rowPitch))
                    for (int x = x0; x < x1; x += int(g.colPitch)) {
                        scaled(raw[size_t(y) * size_t(stride) + size_t(x)], y, x, gainAt(g, 0, y, x, width, height));
                    }
                ++applied;
                break;
            }
            case kFixVignetteRadial: {
                Vignette vg;
                if (!readVignette(op, vg)) break;
                budget -= 1;
                parallelRows(height, [&](int y0, int y1) {
                    for (int y = y0; y < y1; ++y)
                        for (int x = 0; x < width; ++x) scaled(raw[size_t(y) * size_t(stride) + size_t(x)], y, x, vignetteGain(vg, x, y, width, height));
                });
                ++applied;
                break;
            }
            case kFixBadPixelsConstant: {
                Reader r{op.params.data(), op.params.size()};
                const uint32_t constant = r.u32(), phase = r.u32();
                if (!r.ok) break;
                budget -= 2;
                std::vector<uint8_t> bad(size_t(width) * size_t(height));
                for (int y = 0; y < height; ++y)
                    for (int x = 0; x < width; ++x) bad[size_t(y) * size_t(width) + size_t(x)] = raw[size_t(y) * size_t(stride) + size_t(x)] == constant;
                patchBadPixels(raw, width, height, stride, phase & 3, bad);
                ++applied;
                break;
            }
            case kFixBadPixelsList: {
                Reader r{op.params.data(), op.params.size()};
                const uint32_t phase = r.u32(), points = r.u32(), rects = r.u32();
                if (!r.ok || uint64_t(points) * 8 + uint64_t(rects) * 16 > op.params.size() || rects > 4096) break;
                budget -= 2;
                std::vector<uint8_t> bad(size_t(width) * size_t(height));
                for (uint32_t i = 0; i < points; ++i) {
                    const uint32_t row = r.u32(), col = r.u32();
                    if (row < uint32_t(height) && col < uint32_t(width)) bad[size_t(row) * size_t(width) + col] = 1;
                }
                uint64_t area = 0;  // (bounded: a crafted list of whole-frame rectangles would take hours)
                for (uint32_t i = 0; i < rects; ++i) {
                    const uint32_t t = r.u32(), l = r.u32(), b = std::min<uint32_t>(r.u32(), uint32_t(height)), rt = std::min<uint32_t>(r.u32(), uint32_t(width));
                    if (t < b && l < rt) area += uint64_t(b - t) * (rt - l);
                    if (area > 4 * uint64_t(width) * uint64_t(height)) break;
                    for (uint32_t y = t; y < b; ++y)
                        for (uint32_t x = l; x < rt; ++x) bad[size_t(y) * size_t(width) + x] = 1;
                }
                if (!r.ok) break;
                patchBadPixels(raw, width, height, stride, phase & 3, bad);
                ++applied;
                break;
            }
            default: break;  // (warps belong to list 3; the rest Montage does not apply)
        }
    }
    return applied;
}

int applyDngPictureOpcodes(const DngOpcodeList& list, float* pixels, int width, int height, int planes, int fullWidth, int fullHeight) {
    if (!pixels || width <= 0 || height <= 0 || planes <= 0) return 0;
    // A gain map's area and pitch are in the full picture's pixels; a smaller one (half size) takes them scaled.
    const double scaleY = fullHeight > 0 ? double(height) / fullHeight : 1.0, scaleX = fullWidth > 0 ? double(width) / fullWidth : 1.0;
    auto scaled = [](uint32_t v, double s, int limit) { return int(std::min<double>(std::ceil(v * s - 1e-9), limit)); };
    int applied = 0, warps = 0;
    bool skipWarp = false;
    double budget = kFramePassBudget;  // (as for the mosaic's lists)
    for (const DngOpcode& op : list) {
        if (budget <= 0) break;
        const bool skip = skipWarp;
        skipWarp = false;
        switch (op.id) {
            case kWarpRectilinear:
            case kWarpFisheye: {
                WarpParams w;
                if (skip || ++warps > 4 || !readWarp(op, w)) break;  // (a camera writes one; each resamples the whole picture)
                budget -= 4;  // (a cubic resample of every plane: dearer than a gain)
                applyWarp(w, pixels, width, height, planes);
                ++applied;
                break;
            }
            case kFixVignetteRadial: {
                Vignette vg;
                if (!readVignette(op, vg)) break;
                budget -= 1;
                parallelRows(height, [&](int y0, int y1) {
                    for (int y = y0; y < y1; ++y)
                        for (int x = 0; x < width; ++x) {
                            const float g = float(vignetteGain(vg, x, y, width, height));
                            float* px = pixels + (size_t(y) * size_t(width) + size_t(x)) * size_t(planes);
                            for (int p = 0; p < planes; ++p) px[p] = std::clamp(px[p] * g, 0.0f, 1.0f);
                        }
                });
                ++applied;
                break;
            }
            case kGainMap: {
                GainMap g;
                if (!readGainMap(op, g)) break;
                budget -= gainMapPasses(g, int(std::lround(width / scaleX)), int(std::lround(height / scaleY)));
                const int y0 = scaled(g.top, scaleY, height), y1 = scaled(g.bottom, scaleY, height);
                const int x0 = scaled(g.left, scaleX, width), x1 = scaled(g.right, scaleX, width);
                const int rowStep = std::max(1, int(std::lround(g.rowPitch * scaleY))), colStep = std::max(1, int(std::lround(g.colPitch * scaleX)));
                for (int y = y0; y < y1; y += rowStep)
                    for (int x = x0; x < x1; x += colStep)
                        for (uint32_t k = 0; k < g.planes; ++k) {
                            const uint32_t p = g.plane + k;
                            if (p >= uint32_t(planes)) break;
                            const int mp = int(std::min(k, g.mapPlanes - 1));
                            float& v = pixels[(size_t(y) * size_t(width) + size_t(x)) * size_t(planes) + p];
                            v = std::clamp(float(v * gainAt(g, mp, y, x, width, height)), 0.0f, 1.0f);
                        }
                ++applied;
                break;
            }
            case kWarpRectilinear2:
                // Not applied; the skip rule is for readers that do apply it, so a warp after it is applied here.
                break;
            default: break;
        }
    }
    return applied;
}

}  // namespace montage
