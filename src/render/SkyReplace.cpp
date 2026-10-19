#include "SkyReplace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

#include "ClipAnalysis.h"
#include "Processing.h"
#include "core/Effects.h"
#include "core/Provenance.h"
#include "media/DepthMap.h"
#include "media/VideoDepth.h"
#include "media/CameraSolve.h"
#include "media/ValueMap.h"

namespace montage {

namespace {

// The mean of each 3 x 3 neighbourhood (edges clamped).
std::vector<float> boxMean3(const std::vector<float>& v, int w, int h) {
    std::vector<float> across(v.size()), out(v.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t r = size_t(y) * size_t(w);
            across[r + size_t(x)] = (v[r + size_t(std::max(x - 1, 0))] + v[r + size_t(x)] + v[r + size_t(std::min(x + 1, w - 1))]) / 3;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            out[size_t(y) * size_t(w) + size_t(x)] = (across[size_t(std::max(y - 1, 0)) * size_t(w) + size_t(x)] +
                                                      across[size_t(y) * size_t(w) + size_t(x)] +
                                                      across[size_t(std::min(y + 1, h - 1)) * size_t(w) + size_t(x)]) /
                                                     3;
    return out;
}

using Mat3 = std::array<double, 9>;  // row-major

Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[size_t(i * 3 + j)] += a[size_t(i * 3 + k)] * b[size_t(k * 3 + j)];
    return r;
}

Mat3 transpose(const Mat3& a) { return {a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8]}; }

// A rotation turned `amount` of the way from none (by its axis and angle).
Mat3 partRotation(const Mat3& R, double amount) {
    if (std::fabs(amount - 1) < 1e-9) return R;
    const double c = std::clamp((R[0] + R[4] + R[8] - 1) / 2, -1.0, 1.0);
    const double theta = std::acos(c);
    if (theta < 1e-9) return {1, 0, 0, 0, 1, 0, 0, 0, 1};
    double ax = R[7] - R[5], ay = R[2] - R[6], az = R[3] - R[1];
    const double len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len < 1e-12) return amount < 0.5 ? Mat3{1, 0, 0, 0, 1, 0, 0, 0, 1} : R;  // (a half turn: no axis to scale along)
    ax /= len, ay /= len, az /= len;
    const double t = theta * amount, s = std::sin(t), k = 1 - std::cos(t);
    return {1 - k * (ay * ay + az * az), -s * az + k * ax * ay,       s * ay + k * ax * az,
            s * az + k * ax * ay,       1 - k * (ax * ax + az * az), -s * ax + k * ay * az,
            -s * ay + k * ax * az,      s * ax + k * ay * az,        1 - k * (ax * ax + ay * ay)};
}

Mat3 inverse(const Mat3& m) {
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::fabs(det) < 1e-15) return {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const double d = 1 / det;
    return {(m[4] * m[8] - m[5] * m[7]) * d, (m[2] * m[7] - m[1] * m[8]) * d, (m[1] * m[5] - m[2] * m[4]) * d,
            (m[5] * m[6] - m[3] * m[8]) * d, (m[0] * m[8] - m[2] * m[6]) * d, (m[2] * m[3] - m[0] * m[5]) * d,
            (m[3] * m[7] - m[4] * m[6]) * d, (m[1] * m[6] - m[0] * m[7]) * d, (m[0] * m[4] - m[1] * m[3]) * d};
}

Similarity compose(const Similarity& second, const Similarity& first) {  // second after first
    const double c = second.scale * std::cos(second.angle), s = second.scale * std::sin(second.angle);
    return {c * first.tx - s * first.ty + second.tx, s * first.tx + c * first.ty + second.ty, first.angle + second.angle,
            first.scale * second.scale};
}

Similarity inverse(const Similarity& m) {
    const double k = 1 / std::max(1e-9, m.scale), c = k * std::cos(-m.angle), s = k * std::sin(-m.angle);
    return {-(c * m.tx - s * m.ty), -(s * m.tx + c * m.ty), -m.angle, k};
}

// The camera path an analysis measured, as each frame's place from its first (read once a version).
struct SkyPath {
    double fps = 0, start = 0;
    std::vector<Similarity> at;
};

std::shared_ptr<const SkyPath> skyPath(const std::string& text) {
    if (text.empty()) return nullptr;
    static std::mutex m;
    static std::map<size_t, std::shared_ptr<const SkyPath>> cache;
    const size_t key = std::hash<std::string>{}(text);
    std::lock_guard lock(m);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    CameraMotion motion;
    std::shared_ptr<SkyPath> path;
    if (cameraMotionFromString(text, motion) && !motion.steps.empty() && motion.fps > 0) {
        path = std::make_shared<SkyPath>();
        path->fps = motion.fps;
        path->start = motion.start;
        path->at.resize(motion.steps.size());
        for (size_t i = 1; i < motion.steps.size(); ++i) path->at[i] = compose(motion.steps[i], path->at[i - 1]);
    }
    if (cache.size() > 32) cache.clear();
    cache[key] = path;
    return path;
}

// The clip's 3D camera solve (its first enabled 3D Camera Tracker holding one), read once a version.
std::shared_ptr<const CameraSolve> clipSolve(const Clip& c) {
    const std::string* text = nullptr;
    for (const Effect& e : c.effects)
        if (e.enabled && e.type == "camera_track")
            if (auto it = e.strings.find("solve"); it != e.strings.end() && !it->second.empty()) {
                text = &it->second;
                break;
            }
    if (!text) return nullptr;
    static std::mutex m;
    static std::map<size_t, std::shared_ptr<const CameraSolve>> cache;
    const size_t key = std::hash<std::string>{}(*text);
    std::lock_guard lock(m);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    auto solve = std::make_shared<CameraSolve>();
    std::shared_ptr<const CameraSolve> out;
    if (cameraSolveFromString(*text, *solve) && solve->focal > 0 && solve->width > 0) out = solve;
    if (cache.size() > 8) cache.clear();
    cache[key] = out;
    return out;
}

// Sky Replacement's use of depth is only told of when the model is there to use.
[[maybe_unused]] const bool depthCheck = (setOptionalModelCheck([](const std::string& model) {
                                              return model != "depth" || (depthAvailable() && depthModel().installed());
                                          }),
                                          true);
[[maybe_unused]] const bool analysedCheck = (setAnalysedDepthCheck([](const std::string& path) {
                                                 double from, to;
                                                 return analysedDepthRange(path, from, to);
                                             }),
                                             true);

double localSeconds(const Sequence& seq, const Clip& c, FrameTime local) { return c.sourceFrameAt(c.start + local) / seq.fpsValue(); }

}  // namespace

namespace {

// The sky found on a smaller copy (the sky is large and smooth): 1 where it is on a grid of f x f pixel cells.
struct CoarseSky {
    int w = 0, h = 0, f = 1;
    std::vector<float> grid;
};
CoarseSky coarseSky(const Image& img, const ValueMap* depth, double sensitivity, double lowest) {
    const int W = img.width, H = img.height;
    CoarseSky out;
    const int f = out.f = std::max(1, int(std::ceil(std::max(W, H) / 320.0)));
    if (W <= 0 || H <= 0) return out;
    const double sens = std::clamp(sensitivity, 0.0, 1.0);
    const int w = out.w = (W + f - 1) / f, h = out.h = (H + f - 1) / f;
    const size_t m = size_t(w) * size_t(h);
    out.grid.assign(m, 0.f);
    std::vector<float> col(m * 3, 0.f), alpha(m, 0.f);
    parallelRows(h, [&](int y0, int y1) {
        for (int sy = y0; sy < y1; ++sy)
            for (int sx = 0; sx < w; ++sx) {
                double sum[4] = {0, 0, 0, 0};
                int count = 0;
                for (int y = sy * f; y < std::min(H, sy * f + f); ++y)
                    for (int x = sx * f; x < std::min(W, sx * f + f); ++x, ++count) {
                        const float* p = img.at(x, y);
                        for (int k = 0; k < 4; ++k) sum[k] += p[k];
                    }
                const size_t i = size_t(sy) * size_t(w) + size_t(sx);
                alpha[i] = float(sum[3] / std::max(1, count));
                for (int k = 0; k < 3; ++k) col[i * 3 + size_t(k)] = sum[3] > 1e-6 ? float(std::max(0.0, sum[k] / sum[3])) : 0.f;
            }
    });
    auto lum = [&](size_t i) { return 0.2126f * col[i * 3] + 0.7152f * col[i * 3 + 1] + 0.0722f * col[i * 3 + 2]; };
    auto distance = [&](size_t i, size_t j) {
        return std::max({std::fabs(col[i * 3] - col[j * 3]), std::fabs(col[i * 3 + 1] - col[j * 3 + 1]),
                         std::fabs(col[i * 3 + 2] - col[j * 3 + 2])});
    };
    // Texture: how much the colour changes from pixel to pixel, over each pixel's neighbourhood.
    std::vector<float> grad(m, 0.f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const size_t r = size_t(y) * size_t(w) + size_t(std::min(x + 1, w - 1)), d = size_t(std::min(y + 1, h - 1)) * size_t(w) + size_t(x);
            grad[i] = distance(i, r) + distance(i, d);
        }
    const std::vector<float> texture = boxMean3(grad, w, h);
    // The sky is among the brightest things in the upper part of the picture.
    std::vector<float> upper;
    for (int y = 0; y < std::max(1, h / 2); ++y)
        for (int x = 0; x < w; ++x)
            if (const size_t i = size_t(y) * size_t(w) + size_t(x); alpha[i] > 0.5f) upper.push_back(lum(i));
    if (upper.empty()) return out;
    std::nth_element(upper.begin(), upper.begin() + std::ptrdiff_t(upper.size() * 95 / 100), upper.end());
    const float bright = upper[upper.size() * 95 / 100];
    std::vector<float> far;
    float nearest = 0;
    if (depth && !depth->empty()) {
        far = depth->resized(w, h);
        for (float v : far) nearest = std::max(nearest, v);
    }
    const float smooth = float(0.03 + 0.09 * sens), dim = float(0.8 - 0.45 * sens), reach = float(0.04 + 0.06 * sens);
    const float farEnough = float(0.12 + 0.3 * sens) * std::max(nearest, 1e-6f);
    const int lowestRow = int(std::ceil(std::clamp(lowest, 0.0, 1.0) * h));
    std::vector<uint8_t> candidate(m, 0);
    for (int y = 0; y < std::min(h, lowestRow); ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            candidate[i] = alpha[i] > 0.5f && texture[i] < smooth && lum(i) >= std::max(0.12f, dim * bright) && (far.empty() || far[i] <= farEnough);
        }
    // Grown from the candidates open to the top of the picture (the first opaque pixel down each column).
    std::vector<uint8_t> sky(m, 0);
    std::deque<int> queue;
    for (int x = 0; x < w; ++x)
        for (int y = 0; y < h; ++y) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            if (alpha[i] <= 0.5f) continue;
            if (candidate[i]) {
                sky[i] = 1;
                queue.push_back(int(i));
            }
            break;
        }
    while (!queue.empty()) {
        const int i = queue.front();
        queue.pop_front();
        const int x = i % w, y = i / w;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if ((!dx && !dy) || nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                const size_t j = size_t(ny) * size_t(w) + size_t(nx);
                if (sky[j] || !candidate[j] || distance(size_t(i), j) > reach) continue;
                // (nor a step at a time down a soft horizon: little change from the sky a few rows up)
                if (const size_t up = j - std::min(size_t(ny), size_t(6)) * size_t(w); sky[up] && distance(up, j) > 1.6f * reach) continue;
                sky[j] = 1;
                queue.push_back(int(j));
            }
    }
    // Scraps are not sky: a smooth bright patch or two in a picture of the ground.
    {
        size_t opaque = 0;
        for (size_t i = 0; i < m; ++i) opaque += alpha[i] > 0.5f;
        std::vector<uint8_t> seen(m, 0);
        for (size_t s = 0; s < m; ++s) {
            if (sky[s] != 1 || seen[s]) continue;
            std::vector<int> part{int(s)};
            seen[s] = 1;
            for (size_t q = 0; q < part.size(); ++q) {
                const int x = part[q] % w, y = part[q] / w;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        const size_t j = size_t(ny) * size_t(w) + size_t(nx);
                        if (sky[j] == 1 && !seen[j]) {
                            seen[j] = 1;
                            part.push_back(int(j));
                        }
                    }
            }
            if (part.size() * 200 < opaque)
                for (int i : part) sky[size_t(i)] = 0;
        }
    }
    // Sky seen through leaves and gaps, cut off from the rest: the sky's own colour at that height, near it.
    std::vector<double> rowColour(size_t(h) * 3, 0.0);
    std::vector<int> rowCount(size_t(h), 0);
    std::vector<uint8_t> main(m, 0);
    for (size_t i = 0; i < m; ++i)
        if (sky[i]) {
            const size_t y = i / size_t(w);
            for (int k = 0; k < 3; ++k) rowColour[y * 3 + size_t(k)] += col[i * 3 + size_t(k)];
            ++rowCount[y];
            main[i] = 1;
        }
    if (std::all_of(rowCount.begin(), rowCount.end(), [](int n) { return n == 0; })) return out;  // no sky
    const std::vector<float> toSky = distanceTransform(main, w, h);
    const float close = float(0.05 + 0.07 * sens), nearby = 0.08f * float(std::max(w, h));
    for (size_t i = 0; i < m; ++i) {
        const size_t y = i / size_t(w);
        if (!candidate[i] || sky[i] || rowCount[y] == 0 || toSky[i] > nearby) continue;
        float d = 0;
        for (int k = 0; k < 3; ++k) d = std::max(d, std::fabs(col[i * 3 + size_t(k)] - float(rowColour[y * 3 + size_t(k)] / rowCount[y])));
        if (d <= close) sky[i] = 2;
    }
    // Clouds: holes in the sky (not reaching the bottom or the sides) with nothing dark in them are sky; a bird or a kite
    // stays.
    {
        std::vector<int> label(m, -1);
        size_t opaque = 0;
        for (size_t i = 0; i < m; ++i) opaque += alpha[i] > 0.5f;
        for (size_t s = 0; s < m; ++s) {
            if (sky[s] || label[s] >= 0) continue;
            std::vector<int> part{int(s)};
            label[s] = int(s);
            bool edge = false;
            size_t dark = 0;
            for (size_t q = 0; q < part.size(); ++q) {
                const int i = part[q], x = i % w, y = i / w;
                edge = edge || x == 0 || x == w - 1 || y == h - 1 || alpha[size_t(i)] <= 0.5f;
                dark += lum(size_t(i)) < 0.6f * dim * bright;
                const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; ++k) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
                    const size_t j = size_t(ny[k]) * size_t(w) + size_t(nx[k]);
                    if (sky[j] || label[j] >= 0) continue;
                    label[j] = int(s);
                    part.push_back(int(j));
                }
            }
            if (!edge && part.size() * 33 < opaque && dark * 20 < part.size())
                for (int i : part) sky[size_t(i)] = 3;
        }
    }
    for (size_t i = 0; i < m; ++i) out.grid[i] = sky[i] ? 1.f : 0.f;
    return out;
}

// Back at full size, smoothly (before its edges are put on the picture's own).
std::vector<float> fullSize(const CoarseSky& c, int W, int H) {
    std::vector<float> out(size_t(std::max(0, W)) * size_t(std::max(0, H)), 0.f);
    if (c.grid.empty()) return out;
    const int f = c.f, w = c.w, h = c.h;
    const std::vector<float>& coarse = c.grid;
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float gy = std::clamp((y + 0.5f) / float(f) - 0.5f, 0.f, float(h - 1));
            const int a = int(gy), b = std::min(a + 1, h - 1);
            const float fy = gy - float(a);
            for (int x = 0; x < W; ++x) {
                const float gx = std::clamp((x + 0.5f) / float(f) - 0.5f, 0.f, float(w - 1));
                const int l = int(gx), r = std::min(l + 1, w - 1);
                const float fx = gx - float(l);
                const float top = coarse[size_t(a) * size_t(w) + size_t(l)] * (1 - fx) + coarse[size_t(a) * size_t(w) + size_t(r)] * fx;
                const float bottom = coarse[size_t(b) * size_t(w) + size_t(l)] * (1 - fx) + coarse[size_t(b) * size_t(w) + size_t(r)] * fx;
                out[size_t(y) * size_t(W) + size_t(x)] = top * (1 - fy) + bottom * fy;
            }
        }
    });
    return out;
}

// Its edges on the picture's own, and nothing where the picture is transparent.
void finishSky(std::vector<float>& matte, const Image& img, double radius, double refine) {
    if (refine > 0) refineMatteEdges(matte, img, radius, refine);
    for (size_t i = 0; i < matte.size(); ++i) matte[i] = img.px[i * 4 + 3] > 1e-3f ? std::clamp(matte[i], 0.f, 1.f) : 0.f;
}

}  // namespace

std::vector<float> skyMatte(const Image& img, const ValueMap* depth, double sensitivity, double lowest, double refine) {
    const CoarseSky c = coarseSky(img, depth, sensitivity, lowest);
    std::vector<float> out = fullSize(c, img.width, img.height);
    finishSky(out, img, 1.5 * c.f + 2, refine);
    return out;
}

namespace {

// What a frame's sky is carried from: its picture in grey (at most 320 wide) and its coarse sky, kept for the frames
// around it, by what decides the frame.
struct SkyLook {
    GrayImage gray;
    CoarseSky sky;
};

std::mutex gSkyMutex;
std::deque<std::pair<std::string, std::shared_ptr<const SkyLook>>> gSkyLooks;
std::deque<std::pair<std::string, std::shared_ptr<const FlowField>>> gSkyFlows;

template <typename T>
std::shared_ptr<const T> keptLook(std::deque<std::pair<std::string, std::shared_ptr<const T>>>& kept, const std::string& key) {
    if (key.empty()) return nullptr;
    std::lock_guard lock(gSkyMutex);
    for (const auto& [k, v] : kept)
        if (k == key) return v;
    return nullptr;
}
template <typename T>
void keepLook(std::deque<std::pair<std::string, std::shared_ptr<const T>>>& kept, const std::string& key, std::shared_ptr<const T> v,
              size_t most) {
    if (key.empty()) return;
    std::lock_guard lock(gSkyMutex);
    kept.emplace_front(key, std::move(v));
    if (kept.size() > most) kept.pop_back();
}

}  // namespace

std::vector<float> steadySkyMatte(const Image& img, const ValueMap* depth, const SkyShot& shot, double sensitivity, double lowest,
                                  double refine) {
    const int W = img.width, H = img.height;
    auto look = std::make_shared<SkyLook>();
    look->gray = toGray(img, 320);
    look->sky = coarseSky(img, depth, sensitivity, lowest);
    const CoarseSky& own = look->sky;
    std::string settings;
    {
        char buf[96];
        std::snprintf(buf, sizeof buf, "|%dx%d|%.4f|%.4f", W, H, sensitivity, lowest);
        settings = buf;
    }
    // (This frame's own look is of the picture as it is here, with what is below and its matte, so it is not kept for
    // its neighbours, which see the clip's layer alone: the flows from it are its own.)
    const std::string ownKey = shot.key ? shot.key(0) : std::string();
    std::vector<float> out = fullSize(own, W, H);
    if (own.grid.empty() || !shot.frame) {
        finishSky(out, img, 1.5 * own.f + 2, refine);
        return out;
    }
    const double kg = double(look->gray.width) / W;  // full-size pixels to grey ones
    // The grid cells' centres in grey pixels, followed frame by frame along the flow.
    std::vector<float> sum = own.grid, weight(own.grid.size(), 1.0f);
    for (int dir : {-1, 1}) {
        std::shared_ptr<const SkyLook> from = look;
        std::string fromKey = ownKey.empty() ? std::string() : ownKey + "|self";
        std::vector<Point2> at(own.grid.size());
        std::vector<uint8_t> alive(own.grid.size(), 1);
        for (int gy = 0; gy < own.h; ++gy)
            for (int gx = 0; gx < own.w; ++gx)
                at[size_t(gy) * size_t(own.w) + size_t(gx)] = {((gx + 0.5) * own.f - 0.5) * kg, ((gy + 0.5) * own.f - 0.5) * kg};
        for (int k = 1; k <= 3; ++k) {
            const int offset = dir * k;
            const std::string key = ownKey.empty() ? std::string() : shot.key(offset);
            std::shared_ptr<const SkyLook> next = key.empty() ? nullptr : keptLook(gSkyLooks, key + settings);
            if (!next) {
                Image picture;
                std::shared_ptr<const ValueMap> theirDepth;
                if (!shot.frame(offset, picture, theirDepth) || picture.width != W || picture.height != H) break;
                auto made = std::make_shared<SkyLook>();
                made->gray = toGray(picture, 320);
                made->sky = coarseSky(picture, theirDepth.get(), sensitivity, lowest);
                next = made;
                if (!key.empty()) keepLook(gSkyLooks, key + settings, next, 24);
            }
            if (next->sky.grid.size() != own.grid.size() || next->gray.width != look->gray.width) break;
            // The flow from the last frame to this one (kept too).
            const std::string flowKey = fromKey.empty() || key.empty() ? std::string() : fromKey + ">" + key + settings;
            std::shared_ptr<const FlowField> flow = keptLook(gSkyFlows, flowKey);
            if (!flow) {
                flow = std::make_shared<const FlowField>(denseFlow(from->gray, next->gray, 4));
                keepLook(gSkyFlows, flowKey, flow, 24);
            }
            const float wt = float(std::exp(-k * k / 8.0));  // (nearer frames count for more)
            const GrayImage& g0 = look->gray;
            const GrayImage& gk = next->gray;
            for (size_t i = 0; i < at.size(); ++i) {
                if (!alive[i]) continue;
                const Point2 d = flow->at(at[i].x, at[i].y);
                at[i].x += d.x, at[i].y += d.y;
                if (at[i].x < 0 || at[i].y < 0 || at[i].x > gk.width - 1 || at[i].y > gk.height - 1) {
                    alive[i] = 0;
                    continue;
                }
                // Only where the two pictures agree once carried (not something that moved in front).
                const int gx = int(i % size_t(own.w)), gy = int(i / size_t(own.w));
                const double hx = ((gx + 0.5) * own.f - 0.5) * kg, hy = ((gy + 0.5) * own.f - 0.5) * kg;
                const float diff = std::fabs(g0.sample(hx, hy) - gk.sample(at[i].x, at[i].y));
                const float agree = std::exp(-(diff / 0.06f) * (diff / 0.06f));
                // Their sky there (on their grid).
                const double cx = std::clamp((at[i].x / kg + 0.5) / own.f - 0.5, 0.0, double(own.w - 1));
                const double cy = std::clamp((at[i].y / kg + 0.5) / own.f - 0.5, 0.0, double(own.h - 1));
                const int x0 = int(cx), y0 = int(cy), x1 = std::min(x0 + 1, own.w - 1), y1 = std::min(y0 + 1, own.h - 1);
                const float fx = float(cx - x0), fy = float(cy - y0);
                auto g = [&](int x, int y) { return next->sky.grid[size_t(y) * size_t(own.w) + size_t(x)]; };
                const float m = (g(x0, y0) * (1 - fx) + g(x1, y0) * fx) * (1 - fy) + (g(x0, y1) * (1 - fx) + g(x1, y1) * fx) * fy;
                sum[i] += m * wt * agree;
                weight[i] += wt * agree;
            }
            from = next;
            fromKey = key;
        }
    }
    CoarseSky held = own;
    for (size_t i = 0; i < held.grid.size(); ++i) held.grid[i] = sum[i] / weight[i];
    out = fullSize(held, W, H);
    finishSky(out, img, 1.5 * own.f + 2, refine);
    return out;
}

double skyHorizonRow(const std::vector<float>& matte, int w, int h) {
    if (w <= 0 || h <= 0 || matte.size() != size_t(w) * size_t(h)) return 0;
    std::vector<double> rows(size_t(h), 0.0);
    double total = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) rows[size_t(y)] += matte[size_t(y) * size_t(w) + size_t(x)];
        total += rows[size_t(y)];
    }
    if (total <= 0) return h * 0.5;
    double seen = 0;
    for (int y = 0; y < h; ++y)
        if ((seen += rows[size_t(y)]) >= 0.98 * total) return y + 1;
    return h;
}

Image skyGradient(int w, int h, const float zenith[3], const float horizon[3], double horizonRow) {
    Image out(w, h, Image::Uninitialized{});
    const double span = std::max(1.0, horizonRow);
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float u = float(std::clamp((y + 0.5) / span, 0.0, 1.0));
            float c[3];
            for (int k = 0; k < 3; ++k) c[k] = zenith[k] + (horizon[k] - zenith[k]) * u;
            float* row = out.row(y);
            for (int x = 0; x < w; ++x) {
                for (int k = 0; k < 3; ++k) row[x * 4 + k] = c[k];
                row[x * 4 + 3] = 1;
            }
        }
    });
    return out;
}

namespace {

// A position folded back into 0..n-1 (the picture mirrored at its edges, again and again).
double mirrored(double v, int n) {
    if (n <= 1 || !std::isfinite(v)) return 0;
    const double period = 2.0 * (n - 1);
    v = std::fmod(std::fabs(v), period);
    return v > n - 1 ? period - v : v;
}

}  // namespace

Image warpSky(const Image& sky, const Homography& hm) {
    const int W = sky.width, H = sky.height;
    Image out(W, H, Image::Uninitialized{});
    Mat3 m;
    std::copy(std::begin(hm.h), std::end(hm.h), m.begin());
    const Mat3 back = inverse(m);
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const double px = x + 0.5, py = y + 0.5;
                const double d = back[6] * px + back[7] * py + back[8];
                float* o = out.at(x, y);
                if (std::fabs(d) < 1e-12) {
                    std::fill(o, o + 4, 0.f);
                    continue;
                }
                const double sx = mirrored((back[0] * px + back[1] * py + back[2]) / d - 0.5, W);
                const double sy = mirrored((back[3] * px + back[4] * py + back[5]) / d - 0.5, H);
                const int l = int(sx), t = int(sy), r = std::min(l + 1, W - 1), b = std::min(t + 1, H - 1);
                const float fx = float(sx - l), fy = float(sy - t);
                const float *p00 = sky.at(l, t), *p10 = sky.at(r, t), *p01 = sky.at(l, b), *p11 = sky.at(r, b);
                for (int k = 0; k < 4; ++k)
                    o[k] = (p00[k] * (1 - fx) + p10[k] * fx) * (1 - fy) + (p01[k] * (1 - fx) + p11[k] * fx) * fy;
            }
    });
    return out;
}

void composeSky(Image& layer, const Image& sky, const std::vector<float>& matte, double match, double wrap, double wrapWidth) {
    const int W = layer.width, H = layer.height;
    const size_t n = size_t(W) * size_t(H);
    if (W <= 0 || H <= 0 || sky.width != W || sky.height != H || matte.size() != n) return;
    // The foreground lit by the new sky: its colour moved the way the sky's own changed, `match` of the way.
    if (match > 0) {
        double was[3] = {0, 0, 0}, now[3] = {0, 0, 0}, weight = 0, cover = 0;
        for (size_t i = 0; i < n; ++i) {
            const double m = matte[i];
            if (m <= 0) continue;
            const float* p = &layer.px[i * 4];
            const float* s = &sky.px[i * 4];
            for (int k = 0; k < 3; ++k) {
                was[k] += m * p[k];
                now[k] += m * p[3] * s[k];
            }
            weight += m * p[3];
            cover += m * p[3] * s[3];
        }
        if (weight > 1e-6 && cover > 1e-6) {
            float gain[3];
            for (int k = 0; k < 3; ++k)
                gain[k] = float(std::pow(std::clamp((now[k] / cover + 0.02) / (was[k] / weight + 0.02), 0.25, 4.0), std::min(1.0, match)));
            parallelRows(H, [&](int y0, int y1) {
                for (size_t i = size_t(y0) * size_t(W); i < size_t(y1) * size_t(W); ++i) {
                    const float keep = 1 - std::clamp(matte[i], 0.f, 1.f);
                    for (int k = 0; k < 3; ++k) layer.px[i * 4 + size_t(k)] *= 1 + (gain[k] - 1) * keep;
                }
            });
        }
    }
    // Light wrap: the sky's light (blurred) laid over the foreground near it.
    if (wrap > 0 && wrapWidth >= 0.5) {
        Image glow(W, H, Image::Uninitialized{});
        for (size_t i = 0; i < n; ++i) {
            const float m = std::clamp(matte[i], 0.f, 1.f) * layer.px[i * 4 + 3];
            for (int k = 0; k < 4; ++k) glow.px[i * 4 + size_t(k)] = sky.px[i * 4 + size_t(k)] * m;
        }
        gaussianBlur(glow, wrapWidth);
        const float amount = float(std::min(1.0, wrap));
        parallelRows(H, [&](int y0, int y1) {
            for (size_t i = size_t(y0) * size_t(W); i < size_t(y1) * size_t(W); ++i) {
                const float g = glow.px[i * 4 + 3];
                if (g <= 1e-4f) continue;
                float* p = &layer.px[i * 4];
                const float w = amount * std::min(1.f, g) * (1 - std::clamp(matte[i], 0.f, 1.f));
                for (int k = 0; k < 3; ++k) p[k] += (glow.px[i * 4 + size_t(k)] / g * p[3] - p[k]) * w;
            }
        });
    }
    parallelRows(H, [&](int y0, int y1) {
        for (size_t i = size_t(y0) * size_t(W); i < size_t(y1) * size_t(W); ++i) {
            const float m = std::clamp(matte[i], 0.f, 1.f);
            if (m <= 0) continue;
            float* p = &layer.px[i * 4];
            const float* s = &sky.px[i * 4];
            const float a = p[3];
            for (int k = 0; k < 4; ++k) p[k] = p[k] * (1 - m) + s[k] * m * a;
        }
    });
}

bool skyMotion(const Sequence& seq, const Clip& c, const Effect& e, FrameTime local, int w, int h, Homography& out) {
    const int follow = int(std::lround(e.p("follow", local, 1)));
    const double amount = std::clamp(e.p("follow_amount", local, 100) / 100, 0.0, 2.0);
    if (follow <= 0 || amount <= 0 || w <= 0 || h <= 0) return false;
    Mat3 move;
    if (follow == 1) {
        const std::shared_ptr<const SkyPath> path = skyPath(e.s("motion"));
        if (!path) return false;
        const long long last = (long long)path->at.size() - 1;
        auto frame = [&](FrameTime t) {
            return size_t(std::clamp(std::llround((localSeconds(seq, c, t) - path->start) * path->fps), 0LL, last));
        };
        Similarity rel = compose(path->at[frame(local)], inverse(path->at[frame(c.duration / 2)]));
        // (part of the way: the pan, turn and zoom each scaled)
        rel = {rel.tx * amount, rel.ty * amount, rel.angle * amount, std::pow(std::max(1e-6, rel.scale), amount)};
        // Its steps are about the frame's centre in fractions of the width.
        const double cs = rel.scale * std::cos(rel.angle), sn = rel.scale * std::sin(rel.angle);
        const double ox = w / 2.0, oy = h / 2.0;
        move = {cs, -sn, ox + rel.tx * w - cs * ox + sn * oy, sn, cs, oy + rel.ty * w - sn * ox - cs * oy, 0, 0, 1};
    } else {
        const std::shared_ptr<const CameraSolve> solve = clipSolve(c);
        if (!solve || solve->cameras.empty()) return false;
        int at = cameraSolveFrame(seq, c, *solve, local), from = cameraSolveFrame(seq, c, *solve, c.duration / 2);
        if (from < 0 || !solve->cameras[size_t(from)].valid)  // (the first frame the solve reaches)
            for (from = 0; from < int(solve->cameras.size()) && !solve->cameras[size_t(from)].valid; ++from) {
            }
        if (at < 0 || from >= int(solve->cameras.size()) || !solve->cameras[size_t(at)].valid) return false;
        // A sky at infinity only turns with the camera: x' = K R_at R_from^T K^-1 x.
        const Mat3 turn = partRotation(mul(solve->cameras[size_t(at)].R, transpose(solve->cameras[size_t(from)].R)), amount);
        const double s = double(w) / solve->width;
        const Mat3 K = {solve->focal * s, 0, w / 2.0, 0, solve->focal * s, h / 2.0, 0, 0, 1};
        move = mul(mul(K, turn), inverse(K));
    }
    std::copy(move.begin(), move.end(), std::begin(out.h));
    return true;
}

void applySkyReplacement(const Sequence& seq, const Clip& c, const Effect& e, FrameTime local, Image& full, const Image* skyLayer,
                         double scale, double sourceSeconds, const std::vector<float>* mask, const ValueMap* fullDepth,
                         const SkyShot& shot) {
    if (full.empty()) return;
    const bool gradient = std::lround(e.p("source", local, 0)) == 1, showMatte = std::lround(e.p("view", local, 0)) == 1;
    const bool hasSky = gradient || (skyLayer && skyLayer->width == full.width && skyLayer->height == full.height);
    if (!hasSky && !showMatte) return;  // nothing on the sky track now
    // Within the clip's picture on the canvas (its opaque bounds), so a moved or smaller picture is taken as a whole.
    int x0 = full.width, y0 = full.height, x1 = -1, y1 = -1;
    for (int y = 0; y < full.height; ++y) {
        const float* row = full.row(y);
        for (int x = 0; x < full.width; ++x)
            if (row[x * 4 + 3] > 1e-3f) {
                x0 = std::min(x0, x), x1 = std::max(x1, x);
                y0 = std::min(y0, y), y1 = std::max(y1, y);
            }
    }
    if (x1 < x0) return;
    const bool whole = x0 == 0 && y0 == 0 && x1 == full.width - 1 && y1 == full.height - 1;
    const int W = x1 - x0 + 1, H = y1 - y0 + 1;
    auto crop = [&](const Image& from) {
        Image out(W, H, Image::Uninitialized{});
        for (int y = 0; y < H; ++y) std::copy(from.at(x0, y0 + y), from.at(x0, y0 + y) + size_t(W) * 4, out.row(y));
        return out;
    };
    Image part = whole ? Image() : crop(full);
    Image& layer = whole ? full : part;
    const bool useDepth = e.p("use_depth", local, 1) > 0.5;
    // The depth over the clip's picture: as given (cropped), or found in it.
    auto depthOf = [&](const Image& picture, const ValueMap* over) -> std::shared_ptr<const ValueMap> {
        if (!useDepth) return nullptr;
        if (over && over->width == full.width && over->height == full.height) {
            if (whole) return std::make_shared<ValueMap>(*over);
            auto d = std::make_shared<ValueMap>();
            d->width = W, d->height = H;
            d->values.resize(size_t(W) * size_t(H));
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) d->values[size_t(y) * size_t(W) + size_t(x)] = over->values[size_t(y0 + y) * size_t(full.width) + size_t(x0 + x)];
            return d;
        }
        if (depthAvailable() && depthModel().installed()) return cachedDepth(picture);
        return nullptr;
    };
    const std::shared_ptr<const ValueMap> depth = depthOf(layer, fullDepth);
    const double sensitivity = e.p("sensitivity", local, 50) / 100, lowest = e.p("lowest", local, 80) / 100, refine = e.p("refine", local, 70) / 100;
    std::vector<float> matte;
    if (shot.frame && e.p("steady", local, 1) > 0.5) {
        // The frames around it, cut to the same part of the canvas.
        SkyShot cut;
        cut.frame = [&](int offset, Image& picture, std::shared_ptr<const ValueMap>& theirDepth) {
            Image other;
            std::shared_ptr<const ValueMap> otherDepth;
            if (!shot.frame(offset, other, otherDepth) || other.width != full.width || other.height != full.height) return false;
            picture = whole ? std::move(other) : crop(other);
            theirDepth = depthOf(picture, otherDepth.get());
            return true;
        };
        if (shot.key)
            cut.key = [&](int offset) {
                const std::string k = shot.key(offset);
                return k.empty() ? k : k + "|" + std::to_string(x0) + "," + std::to_string(y0) + "," + std::to_string(W) + "x" + std::to_string(H) +
                                           (useDepth ? "|d" : "");
            };
        matte = steadySkyMatte(layer, depth.get(), cut, sensitivity, lowest, refine);
    } else {
        matte = skyMatte(layer, depth.get(), sensitivity, lowest, refine);
    }
    const double shift = e.p("shift", local, 0) * scale, soften = e.p("soften", local, 1) * scale;
    if (std::fabs(shift) >= 0.25 || soften >= 0.25) refineMatte(matte, W, H, shift, soften, soften);
    // A mask on the effect says where the sky may be: over the whole layer (where the picture was moved, turned or
    // keyed its opaque bounds are not its frame), the bounds taken from it.
    if (hasMask(e, local)) {
        const std::vector<float> limit = mask ? *mask : effectMatte(e, local, full, scale, sourceSeconds);
        if (limit.size() == size_t(full.width) * size_t(full.height))
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x) matte[size_t(y) * size_t(W) + size_t(x)] *= limit[size_t(y0 + y) * size_t(full.width) + size_t(x0 + x)];
    }
    if (showMatte) {
        for (size_t i = 0; i < matte.size(); ++i) {
            float* p = &layer.px[i * 4];
            p[0] = p[1] = p[2] = matte[i] * p[3];
        }
    } else {
        Image sky;
        if (gradient) {
            const float zenith[3] = {float(e.p("zenith.r", local, 0.16)), float(e.p("zenith.g", local, 0.36)),
                                     float(e.p("zenith.b", local, 0.72))};
            const float horizon[3] = {float(e.p("horizon.r", local, 0.72)), float(e.p("horizon.g", local, 0.82)),
                                      float(e.p("horizon.b", local, 0.92))};
            sky = skyGradient(W, H, zenith, horizon, skyHorizonRow(matte, W, H));
        } else {
            sky = whole ? *skyLayer : crop(*skyLayer);
        }
        if (const double stops = e.p("sky_exposure", local, 0); std::fabs(stops) > 1e-4) {
            const float g = float(std::exp2(stops));
            for (size_t i = 0; i < sky.px.size(); i += 4)
                for (int k = 0; k < 3; ++k) sky.px[i + size_t(k)] *= g;
        }
        if (const double blur = e.p("sky_blur", local, 0) * scale; blur >= 0.5) gaussianBlur(sky, blur);
        if (Homography move; skyMotion(seq, c, e, local, W, H, move)) sky = warpSky(sky, move);
        composeSky(layer, sky, matte, e.p("match", local, 30) / 100, e.p("wrap", local, 20) / 100, e.p("wrap_width", local, 20) * scale);
    }
    if (!whole)
        for (int y = 0; y < H; ++y) std::copy(part.row(y), part.row(y) + size_t(W) * 4, full.at(x0, y0 + y));
}

}  // namespace montage
