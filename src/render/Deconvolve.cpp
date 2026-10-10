#include "Deconvolve.h"

#include <algorithm>
#include <cmath>

namespace montage {

void gaussianPlane(std::vector<float>& plane, int w, int h, double sigma) {
    if (sigma <= 0.05 || w <= 0 || h <= 0) return;
    const int r = std::max(1, int(std::ceil(sigma * 3)));
    std::vector<float> k(size_t(2 * r + 1));
    double sum = 0;
    for (int i = -r; i <= r; ++i) sum += k[size_t(i + r)] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
    for (float& v : k) v = float(v / sum);
    std::vector<float> tmp(plane.size());
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* src = plane.data() + size_t(y) * size_t(w);
            float* dst = tmp.data() + size_t(y) * size_t(w);
            for (int x = 0; x < w; ++x) {
                float acc = 0;
                for (int i = -r; i <= r; ++i) acc += k[size_t(i + r)] * src[std::clamp(x + i, 0, w - 1)];
                dst[x] = acc;
            }
        }
    });
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* dst = plane.data() + size_t(y) * size_t(w);
            for (int x = 0; x < w; ++x) dst[x] = 0;
            for (int i = -r; i <= r; ++i) {
                const float kv = k[size_t(i + r)];
                const float* src = tmp.data() + size_t(std::clamp(y + i, 0, h - 1)) * size_t(w);
                for (int x = 0; x < w; ++x) dst[x] += kv * src[x];
            }
        }
    });
}

void focusRepair(Image& img, const FocusRepair& o) {
    if (img.empty() || o.blur < 0.2 || o.iterations <= 0 || o.strength <= 0) return;
    const int w = img.width, h = img.height;
    const size_t n = size_t(w) * size_t(h);
    // Brightness (Rec.709 weights); the small offset keeps the ratios finite in black.
    constexpr float kFloor = 1e-3f;
    std::vector<float> y(n);
    for (size_t i = 0; i < n; ++i) {
        const float* p = &img.px[i * 4];
        y[i] = std::max(0.0f, 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) + kFloor;
    }
    // Richardson–Lucy: estimate <- estimate * G(observed / G(estimate)) (G is symmetric), with
    // Biggs–Andrews acceleration: each step starts from the estimate pushed further along the
    // direction the last steps took, by how consistently they agree.
    std::vector<float> est = y, prev = y, start(n), ratio(n), step(n, 0.0f), lastStep(n, 0.0f);
    const float noise = float(std::max(0.0, o.noise));
    for (int it = 0; it < o.iterations; ++it) {
        double along = 0, norm = 0;
        for (size_t i = 0; i < n; ++i) along += double(step[i]) * lastStep[i], norm += double(lastStep[i]) * lastStep[i];
        const float a = it >= 2 && norm > 0 ? float(std::clamp(along / norm, 0.0, 0.9)) : 0.0f;
        for (size_t i = 0; i < n; ++i) start[i] = std::max(kFloor, est[i] + a * (est[i] - prev[i]));
        ratio = start;
        gaussianPlane(ratio, w, h, o.blur);
        for (size_t i = 0; i < n; ++i) {
            float r = y[i] / std::max(ratio[i], kFloor);
            // Leave differences within the noise alone (soft threshold), and keep each step bounded.
            const float d = r - 1;
            r = 1 + (std::fabs(d) <= noise ? 0.0f : d - std::copysign(noise, d));
            ratio[i] = std::clamp(r, 0.5f, 2.0f);
        }
        gaussianPlane(ratio, w, h, o.blur);
        std::swap(lastStep, step);
        prev = est;
        for (size_t i = 0; i < n; ++i) {
            const float next = start[i] * ratio[i];
            step[i] = next - start[i];
            est[i] = next;
        }
    }
    const float s = float(std::clamp(o.strength, 0.0, 1.0));
    parallelRows(h, [&](int y0, int y1) {
        for (int yy = y0; yy < y1; ++yy)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(yy) * size_t(w) + size_t(x);
                const float gain = 1 + s * (std::clamp(est[i] / y[i], 0.25f, 4.0f) - 1);
                float* p = &img.px[i * 4];
                const float cap = std::max(p[3], 1e-6f) * 4.0f;
                for (int c = 0; c < 3; ++c) p[c] = std::clamp(p[c] * gain, 0.0f, cap);
            }
    });
}

}  // namespace montage
