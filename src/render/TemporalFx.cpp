#include "TemporalFx.h"

#include <algorithm>
#include <cmath>

namespace montage {

namespace {

// Bilinear, clamped at the edges (pixel centres at +0.5).
void sampleClamped(const Image& img, double x, double y, float out[4]) {
    x = std::clamp(x - 0.5, 0.0, double(img.width - 1));
    y = std::clamp(y - 0.5, 0.0, double(img.height - 1));
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
    const float fx = float(x - x0), fy = float(y - y0);
    const float *a = img.at(x0, y0), *b = img.at(x1, y0), *c = img.at(x0, y1), *d = img.at(x1, y1);
    for (int k = 0; k < 4; ++k) {
        const float top = a[k] + (b[k] - a[k]) * fx, bottom = c[k] + (d[k] - c[k]) * fx;
        out[k] = top + (bottom - top) * fy;
    }
}

}  // namespace

Image motionBlur(const Image& src, const MotionFn& back, const MotionFn& fwd, double shutter) {
    shutter = std::clamp(shutter, 0.0, 2.0);
    if (src.empty() || shutter <= 0 || (!back && !fwd)) return src;
    Image out(src.width, src.height, Image::Uninitialized{});
    const double half = shutter / 2;
    parallelRows(src.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < src.width; ++x) {
                const double px = x + 0.5, py = y + 0.5;
                // Where this point is a frame later and a frame earlier.
                const Point2 f = fwd ? fwd(px, py) : Point2{0, 0};
                Point2 b = back ? back(px, py) : Point2{0, 0};
                if (!back) b = {-f.x, -f.y};
                const Point2 fw = fwd ? f : Point2{-b.x, -b.y};
                const double len = (std::hypot(fw.x, fw.y) + std::hypot(b.x, b.y)) * half;
                float* o = out.at(x, y);
                if (len < 0.5) {
                    std::copy_n(src.at(x, y), 4, o);
                    continue;
                }
                // Samples evenly over the time the shutter is open, along that path.
                const int n = std::clamp(int(std::ceil(len)), 2, 64);
                float acc[4] = {0, 0, 0, 0};
                for (int i = 0; i < n; ++i) {
                    const double s = -half + shutter * (i + 0.5) / n;
                    const double ox = s >= 0 ? fw.x * s : -b.x * s, oy = s >= 0 ? fw.y * s : -b.y * s;
                    float q[4];
                    sampleClamped(src, px + ox, py + oy, q);
                    for (int k = 0; k < 4; ++k) acc[k] += q[k];
                }
                for (int k = 0; k < 4; ++k) o[k] = acc[k] / float(n);
            }
    });
    return out;
}

TileStats tileStats(const Image& img, int gw, int gh) {
    TileStats t;
    t.gw = std::max(1, gw), t.gh = std::max(1, gh);
    t.mean.assign(size_t(t.gw) * size_t(t.gh), 0.0f);
    if (img.empty()) return t;
    std::vector<double> sum(t.mean.size(), 0.0);
    std::vector<int> count(t.mean.size(), 0);
    for (int y = 0; y < img.height; ++y) {
        const int ty = std::min(t.gh - 1, y * t.gh / img.height);
        for (int x = 0; x < img.width; ++x) {
            const float* p = img.at(x, y);
            const size_t i = size_t(ty) * size_t(t.gw) + size_t(std::min(t.gw - 1, x * t.gw / img.width));
            sum[i] += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
            ++count[i];
        }
    }
    for (size_t i = 0; i < sum.size(); ++i) t.mean[i] = count[i] ? float(sum[i] / count[i]) : 0.0f;
    return t;
}

void deflicker(Image& img, const TileStats& self, double selfWeight, const std::vector<TileStats>& around,
               const std::vector<double>& weights, double strength) {
    if (img.empty() || self.mean.empty() || strength <= 0) return;
    const int gw = self.gw, gh = self.gh;
    // Each area's gain: the neighbourhood's average brightness over its own.
    std::vector<float> gain(self.mean.size(), 1.0f);
    for (size_t i = 0; i < gain.size(); ++i) {
        double acc = self.mean[i] * selfWeight, wsum = selfWeight;
        for (size_t k = 0; k < around.size(); ++k)
            if (around[k].gw == gw && around[k].gh == gh) acc += around[k].mean[i] * weights[k], wsum += weights[k];
        const double target = acc / wsum, here = self.mean[i];
        const double g = here > 1e-4 ? std::clamp(target / here, 0.5, 2.0) : 1.0;
        gain[i] = float(1 + (g - 1) * std::clamp(strength, 0.0, 1.0));
    }
    // Blended between area centres, so the correction is smooth across the frame.
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const double gy = std::clamp((y + 0.5) * gh / img.height - 0.5, 0.0, double(gh - 1));
            const int a = int(gy), b = std::min(a + 1, gh - 1);
            const float fy = float(gy - a);
            for (int x = 0; x < img.width; ++x) {
                const double gx = std::clamp((x + 0.5) * gw / img.width - 0.5, 0.0, double(gw - 1));
                const int l = int(gx), r = std::min(l + 1, gw - 1);
                const float fx = float(gx - l);
                auto g = [&](int ty, int tx) { return gain[size_t(ty) * size_t(gw) + size_t(tx)]; };
                const float top = g(a, l) + (g(a, r) - g(a, l)) * fx, bottom = g(b, l) + (g(b, r) - g(b, l)) * fx;
                const float k = top + (bottom - top) * fy;
                float* p = img.at(x, y);
                p[0] *= k, p[1] *= k, p[2] *= k;
            }
        }
    });
}

}  // namespace montage
