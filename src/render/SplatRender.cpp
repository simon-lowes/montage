#include "SplatRender.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <thread>

namespace montage {

namespace {

constexpr int kTile = 16;
constexpr double kBlur = 0.3;  // px², added to each splat's covariance on the screen
constexpr size_t kMaxEntries = size_t(1) << 27;  // splats in all the tiles together (512 MB of indices)

using M3 = std::array<double, 9>;  // row-major

M3 mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[size_t(i * 3 + j)] = a[size_t(i * 3)] * b[size_t(j)] + a[size_t(i * 3 + 1)] * b[size_t(3 + j)] + a[size_t(i * 3 + 2)] * b[size_t(6 + j)];
    return r;
}

// What every splat is projected with: the cloud's space to the camera's (q = T p + o), the cloud-space eye, the
// screen's matrix, and how far off the picture the Jacobian is taken (as the original does, so splats far to the side
// do not stretch without end).
struct View {
    M3 T{};
    std::array<double, 3> o{}, eye{};
    double a[4] = {1, 0, 0, 1}, sx = 0, sy = 0;
    double nearZ = 1e-3;
    double limLo[2] = {-1e300, -1e300}, limHi[2] = {1e300, 1e300};

    View(const Mat4& place, const ModelCamera& cam) {
        const M3 M{place[0], place[4], place[8], place[1], place[5], place[9], place[2], place[6], place[10]};
        const M3 R{cam.R[0], cam.R[1], cam.R[2], cam.R[3], cam.R[4], cam.R[5], cam.R[6], cam.R[7], cam.R[8]};
        T = mul(R, M);
        const double d[3] = {place[12] - cam.C[0], place[13] - cam.C[1], place[14] - cam.C[2]};
        for (int i = 0; i < 3; ++i) o[size_t(i)] = R[size_t(i * 3)] * d[0] + R[size_t(i * 3 + 1)] * d[1] + R[size_t(i * 3 + 2)] * d[2];
        eye = transformPoint(inverse4(place), cam.C);
        a[0] = cam.screen[0], a[1] = cam.screen[1], a[2] = cam.screen[3], a[3] = cam.screen[4];
        sx = cam.screen[2], sy = cam.screen[5];
        nearZ = cam.nearZ;
    }

    // The picture's own extent in x/z and y/z, widened by 30% each way.
    void limitTo(int w, int h) {
        const double det = a[0] * a[3] - a[1] * a[2];
        if (std::abs(det) < 1e-300) return;
        double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
        for (int corner = 0; corner < 4; ++corner) {
            const double px = (corner & 1 ? w : 0) - sx, py = (corner & 2 ? h : 0) - sy;
            const double u = (a[3] * px - a[1] * py) / det, v = (-a[2] * px + a[0] * py) / det;
            lo[0] = std::min(lo[0], u), hi[0] = std::max(hi[0], u), lo[1] = std::min(lo[1], v), hi[1] = std::max(hi[1], v);
        }
        for (int k = 0; k < 2; ++k) {
            const double mid = (lo[k] + hi[k]) / 2, half = (hi[k] - lo[k]) / 2 * 1.3;
            limLo[k] = mid - half, limHi[k] = mid + half;
        }
    }

    bool project(const SplatCloud& c, size_t i, ProjectedSplat& out) const {
        const float* p = &c.positions[i * 3];
        double q[3];
        for (int r = 0; r < 3; ++r) q[r] = T[size_t(r * 3)] * p[0] + T[size_t(r * 3 + 1)] * p[1] + T[size_t(r * 3 + 2)] * p[2] + o[size_t(r)];
        if (!(q[2] > nearZ)) return false;
        const double u = q[0] / q[2], v = q[1] / q[2];
        out.x = a[0] * u + a[1] * v + sx;
        out.y = a[2] * u + a[3] * v + sy;
        out.depth = q[2];
        // Its covariance, R S² Rᵀ, into the camera.
        const float* rq = &c.rotations[i * 4];
        const double x = rq[0], y = rq[1], z = rq[2], w = rq[3];
        const M3 Rq{1 - 2 * (y * y + z * z), 2 * (x * y - w * z),     2 * (x * z + w * y),
                    2 * (x * y + w * z),     1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                    2 * (x * z - w * y),     2 * (y * z + w * x),     1 - 2 * (x * x + y * y)};
        const M3 L = mul(T, Rq);  // its axes in the camera
        const double s2[3] = {double(c.scales[i * 3]) * c.scales[i * 3], double(c.scales[i * 3 + 1]) * c.scales[i * 3 + 1],
                              double(c.scales[i * 3 + 2]) * c.scales[i * 3 + 2]};
        // The Jacobian of the projection there: J = A [1/z 0 -u/z; 0 1/z -v/z].
        const double uc = std::clamp(u, limLo[0], limHi[0]), vc = std::clamp(v, limLo[1], limHi[1]);
        const double J0[2][3] = {{1 / q[2], 0, -uc / q[2]}, {0, 1 / q[2], -vc / q[2]}};
        double J[2][3];
        for (int r = 0; r < 2; ++r)
            for (int k = 0; k < 3; ++k) J[r][k] = a[r * 2] * J0[0][k] + a[r * 2 + 1] * J0[1][k];
        // Σ' = (J L) S² (J L)ᵀ.
        double G[2][3];
        for (int r = 0; r < 2; ++r)
            for (int k = 0; k < 3; ++k) G[r][k] = J[r][0] * L[size_t(k)] + J[r][1] * L[size_t(3 + k)] + J[r][2] * L[size_t(6 + k)];
        out.xx = G[0][0] * G[0][0] * s2[0] + G[0][1] * G[0][1] * s2[1] + G[0][2] * G[0][2] * s2[2];
        out.xy = G[0][0] * G[1][0] * s2[0] + G[0][1] * G[1][1] * s2[1] + G[0][2] * G[1][2] * s2[2];
        out.yy = G[1][0] * G[1][0] * s2[0] + G[1][1] * G[1][1] * s2[1] + G[1][2] * G[1][2] * s2[2];
        return true;
    }
};

// Runs fn(task) for tasks [0, n): on the shared workers, on `threads` threads, or here.
void eachTask(int n, int threads, const std::function<void(int)>& fn) {
    if (n <= 0) return;
    if (threads == 1) {
        for (int i = 0; i < n; ++i) fn(i);
    } else if (threads <= 0) {
        // (As 16 rows a task, since the shared workers split no finer than 16 rows a band.)
        parallelRows(n * 16, [&](int a, int b) {
            for (int i = (a + 15) / 16; i < (b + 15) / 16; ++i) fn(i);
        });
    } else {
        std::atomic<int> next{0};
        std::vector<std::thread> pool;
        for (int t = 0; t < std::min(threads, n); ++t)
            pool.emplace_back([&] {
                for (int i; (i = next.fetch_add(1)) < n;) fn(i);
            });
        for (std::thread& t : pool) t.join();
    }
}

// A splat ready to blend: its centre, the inverse of its blurred covariance, opacity, colour and depth, and the
// tiles it may touch.
struct Drawn {
    float x, y, ia, ib, ic, opacity, r, g, b, depth;
    int tx0, ty0, tx1, ty1;
    bool shown;
};

}  // namespace

bool projectSplat(const SplatCloud& cloud, size_t i, const Mat4& place, const ModelCamera& camera, ProjectedSplat& out) {
    if (i >= cloud.count) return false;
    return View(place, camera).project(cloud, i, out);
}

Image renderSplats(const SplatCloud& cloud, const Mat4& place, const ModelCamera& camera, int w, int h,
                   std::vector<float>* depthOut, const SplatRenderOptions& options) {
    w = std::max(1, w), h = std::max(1, h);
    Image img(w, h);
    if (depthOut) depthOut->assign(size_t(w) * size_t(h), 0.0f);
    if (cloud.count == 0) return img;
    View view(place, camera);
    view.limitTo(w, h);
    const int tilesX = (w + kTile - 1) / kTile, tilesY = (h + kTile - 1) / kTile;

    // Each splat projected, coloured, and given the tiles it reaches.
    std::vector<Drawn> drawn(cloud.count);
    constexpr size_t kChunk = 4096;
    eachTask(int((cloud.count + kChunk - 1) / kChunk), options.threads, [&](int task) {
        const size_t end = std::min(cloud.count, (size_t(task) + 1) * kChunk);
        for (size_t i = size_t(task) * kChunk; i < end; ++i) {
            Drawn& d = drawn[i];
            d.shown = false;
            const float* p = &cloud.positions[i * 3];
            if (options.crop && (p[0] < options.cropLo[0] || p[0] > options.cropHi[0] || p[1] < options.cropLo[1] ||
                                 p[1] > options.cropHi[1] || p[2] < options.cropLo[2] || p[2] > options.cropHi[2]))
                continue;
            ProjectedSplat s;
            if (cloud.opacities[i] < 1.0f / 255 || !view.project(cloud, i, s)) continue;
            const double a = s.xx + kBlur, b = s.xy, c = s.yy + kBlur, det = a * c - b * b;
            if (!(det > 1e-12) || !std::isfinite(det)) continue;
            double opacity = cloud.opacities[i];
            if (cloud.antialiased) opacity *= std::sqrt(std::max(0.0, (s.xx * s.yy - s.xy * s.xy) / det));
            if (opacity < 1.0 / 255) continue;
            // Out to where it fades below 1/255: Mahalanobis distance sqrt(2 ln(255 opacity)) along its widest axis.
            const double mid = (a + c) / 2, widest = mid + std::sqrt(std::max(0.0, mid * mid - det));
            const double r = std::sqrt(2 * std::log(255 * opacity) * widest);
            if (!(s.x + r >= 0 && s.x - r <= w && s.y + r >= 0 && s.y - r <= h)) continue;
            // (Clamped to the picture before they are whole numbers: a huge splat's reach may be past what an int holds.)
            auto tileOf = [](double v, double size, int tiles) { return std::clamp(int(std::floor(std::clamp(v, 0.0, size) / kTile)), 0, tiles - 1); };
            d.tx0 = tileOf(s.x - r, w, tilesX), d.tx1 = tileOf(s.x + r, w, tilesX);
            d.ty0 = tileOf(s.y - r, h, tilesY), d.ty1 = tileOf(s.y + r, h, tilesY);
            const double dir[3] = {p[0] - view.eye[0], p[1] - view.eye[1], p[2] - view.eye[2]};
            const double dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            const std::array<float, 3> col = splatColor(cloud, i, dl > 0 ? std::array<double, 3>{dir[0] / dl, dir[1] / dl, dir[2] / dl}
                                                                        : std::array<double, 3>{0, 0, 1});
            d.x = float(s.x), d.y = float(s.y);
            d.ia = float(c / det), d.ib = float(-b / det), d.ic = float(a / det);
            d.opacity = float(opacity);
            d.r = col[0], d.g = col[1], d.b = col[2];
            d.depth = float(s.depth);
            d.shown = true;
        }
    });

    // At most kMaxEntries splats in all the tiles together (a few huge splats in a small file could otherwise ask for
    // more memory than there is): past that the farthest are left out.
    size_t entries = 0;
    for (const Drawn& d : drawn)
        if (d.shown) entries += size_t(d.tx1 - d.tx0 + 1) * size_t(d.ty1 - d.ty0 + 1);
    if (entries > kMaxEntries) {
        std::vector<uint32_t> order;
        for (size_t i = 0; i < drawn.size(); ++i)
            if (drawn[i].shown) order.push_back(uint32_t(i));
        std::sort(order.begin(), order.end(), [&](uint32_t p, uint32_t q) { return drawn[p].depth < drawn[q].depth || (drawn[p].depth == drawn[q].depth && p < q); });
        entries = 0;
        for (const uint32_t i : order) {
            Drawn& d = drawn[i];
            const size_t n = size_t(d.tx1 - d.tx0 + 1) * size_t(d.ty1 - d.ty0 + 1);
            if (entries + n > kMaxEntries) d.shown = false;
            else entries += n;
        }
    }

    // The splats each tile holds, nearest first (equal depths in the file's order).
    std::vector<size_t> start(size_t(tilesX) * size_t(tilesY) + 1, 0);
    for (const Drawn& d : drawn)
        if (d.shown)
            for (int ty = d.ty0; ty <= d.ty1; ++ty)
                for (int tx = d.tx0; tx <= d.tx1; ++tx) ++start[size_t(ty) * size_t(tilesX) + size_t(tx) + 1];
    for (size_t t = 1; t < start.size(); ++t) start[t] += start[t - 1];
    std::vector<uint32_t> list(start.back());
    {
        std::vector<size_t> fill(start.begin(), start.end() - 1);
        for (size_t i = 0; i < drawn.size(); ++i) {
            const Drawn& d = drawn[i];
            if (!d.shown) continue;
            for (int ty = d.ty0; ty <= d.ty1; ++ty)
                for (int tx = d.tx0; tx <= d.tx1; ++tx) list[fill[size_t(ty) * size_t(tilesX) + size_t(tx)]++] = uint32_t(i);
        }
    }

    const float layer = std::clamp(options.opacity, 0.0f, 1.0f);
    eachTask(tilesX * tilesY, options.threads, [&](int tile) {
        uint32_t* first = list.data() + start[size_t(tile)];
        uint32_t* last = list.data() + start[size_t(tile) + 1];
        std::sort(first, last, [&](uint32_t p, uint32_t q) { return drawn[p].depth < drawn[q].depth || (drawn[p].depth == drawn[q].depth && p < q); });
        const int x0 = (tile % tilesX) * kTile, y0 = (tile / tilesX) * kTile;
        for (int y = y0; y < std::min(h, y0 + kTile); ++y)
            for (int x = x0; x < std::min(w, x0 + kTile); ++x) {
                const float cx = float(x) + 0.5f, cy = float(y) + 0.5f;
                float T = 1, rgb[3] = {0, 0, 0}, weight = 0, depthSum = 0, median = -1;
                for (const uint32_t* it = first; it != last; ++it) {
                    const Drawn& d = drawn[*it];
                    const float dx = d.x - cx, dy = d.y - cy;
                    const float power = -0.5f * (d.ia * dx * dx + d.ic * dy * dy) - d.ib * dx * dy;
                    if (power > 0) continue;
                    const float alpha = std::min(0.99f, d.opacity * std::exp(power));
                    if (alpha < 1.0f / 255) continue;
                    const float next = T * (1 - alpha);
                    if (next < 1e-4f) break;
                    const float wgt = alpha * T;
                    rgb[0] += wgt * d.r, rgb[1] += wgt * d.g, rgb[2] += wgt * d.b;
                    weight += wgt, depthSum += wgt * d.depth;
                    if (median < 0 && next < 0.5f) median = d.depth;
                    T = next;
                }
                float* q = img.at(x, y);
                q[0] = rgb[0] * layer, q[1] = rgb[1] * layer, q[2] = rgb[2] * layer, q[3] = (1 - T) * layer;
                if (depthOut && weight > 0) (*depthOut)[size_t(y) * size_t(w) + size_t(x)] = median >= 0 ? median : depthSum / weight;
            }
    });
    return img;
}

}  // namespace montage
