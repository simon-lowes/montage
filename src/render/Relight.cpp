#include "Relight.h"

#include <algorithm>
#include <cmath>

#include "Processing.h"
#include "media/DepthMap.h"

namespace montage {

std::vector<float> depthNormals(const DepthMap& depth, int w, int h, double relief, double smoothness) {
    std::vector<float> n(size_t(std::max(0, w)) * size_t(std::max(0, h)) * 3, 0.0f);
    if (depth.empty() || w < 2 || h < 2) return n;
    // The depth at this size, smoothed so texture does not read as shape.
    const std::vector<float> d = depth.resized(w, h);
    Image z(w, h, Image::Uninitialized{});
    for (size_t i = 0; i < d.size(); ++i) std::fill_n(&z.px[i * 4], 4, d[i]);
    gaussianBlur(z, smoothness);
    // Slopes per fraction of the picture's short side, so the look does not depend on its size.
    const double scale = relief * std::min(w, h) / 2.0;
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const float l = z.at(std::max(0, x - 1), y)[0], r = z.at(std::min(w - 1, x + 1), y)[0];
                const float u = z.at(x, std::max(0, y - 1))[0], dn = z.at(x, std::min(h - 1, y + 1))[0];
                const double gx = (r - l) / 2 * scale, gy = (dn - u) / 2 * scale;
                // Nearer is larger depth, so a surface rising to the right faces left.
                const double nx = -gx, ny = -gy, nz = 1, len = std::sqrt(nx * nx + ny * ny + nz * nz);
                float* o = &n[(size_t(y) * size_t(w) + size_t(x)) * 3];
                o[0] = float(nx / len), o[1] = float(ny / len), o[2] = float(nz / len);
            }
    });
    return n;
}

void relight(Image& img, const DepthMap& depth, const RelightSettings& s) {
    const int W = img.width, H = img.height;
    if (img.empty() || depth.empty()) return;
    const std::vector<float> n = depthNormals(depth, W, H, s.relief, s.smoothness);
    const std::vector<float> d = depth.resized(W, H);
    const double az = s.azimuth * M_PI / 180, el = std::clamp(s.elevation, 0.0, 90.0) * M_PI / 180;
    // Towards the light: right is +x, up the picture is -y.
    const double lx = std::cos(el) * std::cos(az), ly = -std::cos(el) * std::sin(az), lz = std::sin(el);
    const double flat = lz;  // how a surface facing the viewer is lit
    const double intensity = std::max(0.0, s.intensity), shadows = std::clamp(s.shadows, 0.0, 1.0), reach = std::clamp(s.reach, 0.0, 1.0);
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * size_t(W) + size_t(x);
                float* p = img.at(x, y);
                const float* q = &n[i * 3];
                if (s.showNormals) {
                    for (int c = 0; c < 3; ++c) p[c] = (q[c] * 0.5f + 0.5f) * p[3];
                    continue;
                }
                // Against a surface facing the camera, which keeps the light it had: turned towards the
                // light it brightens, turned away it darkens.
                const double rel = q[0] * lx + q[1] * ly + q[2] * lz - flat;
                // Nearer parts only, when the light's reach is short.
                const double near = reach >= 1 ? 1.0 : std::clamp((d[i] - (1 - reach)) / std::max(1e-3, reach), 0.0, 1.0);
                if (rel >= 0) {
                    for (int c = 0; c < 3; ++c) p[c] = float(p[c] * (1 + intensity * s.color[c] * rel * near));
                } else {
                    const double k = 1 - shadows * std::min(1.0, -rel) * near;
                    for (int c = 0; c < 3; ++c) p[c] = float(p[c] * k);
                }
            }
    });
}

}  // namespace montage
