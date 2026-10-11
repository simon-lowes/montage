#include "Retime.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>

#include "media/Tracking.h"

namespace montage {

namespace {

constexpr int kFlowWidth = 480;  // flow is measured on a copy this wide
constexpr int kFlowStep = 6;     // grid spacing at that size

void sampleBilinear(const Image& img, double x, double y, float out[4]) {
    x = std::clamp(x, 0.0, double(img.width - 1));
    y = std::clamp(y, 0.0, double(img.height - 1));
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
    const float fx = float(x - x0), fy = float(y - y0);
    const float *a = img.at(x0, y0), *b = img.at(x1, y0), *c = img.at(x0, y1), *d = img.at(x1, y1);
    for (int k = 0; k < 4; ++k) {
        const float top = a[k] + (b[k] - a[k]) * fx, bottom = c[k] + (d[k] - c[k]) * fx;
        out[k] = top + (bottom - top) * fy;
    }
}

std::shared_ptr<const FlowField> flowBetween(const Image& a, const Image& b, const std::string& key, double& scale) {
    static std::mutex m;
    static std::deque<std::pair<std::string, std::shared_ptr<const FlowField>>> cache;
    const GrayImage ga = toGray(a, kFlowWidth);
    scale = double(a.width) / ga.width;
    if (!key.empty()) {
        std::lock_guard lock(m);
        for (const auto& [k, f] : cache)
            if (k == key) return f;
    }
    const GrayImage gb = toGray(b, kFlowWidth);
    auto flow = std::make_shared<const FlowField>(denseFlow(ga, gb, kFlowStep));
    if (!key.empty()) {
        std::lock_guard lock(m);
        cache.emplace_front(key, flow);
        if (cache.size() > 6) cache.pop_back();
    }
    return flow;
}

}  // namespace

Image blendFrames(const Image& a, const Image& b, double t) {
    if (a.width != b.width || a.height != b.height) return t < 0.5 ? a : b;
    Image out(a.width, a.height, Image::Uninitialized{});
    const float u = float(std::clamp(t, 0.0, 1.0));
    parallelRows(a.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float *p = a.row(y), *q = b.row(y);
            float* o = out.row(y);
            for (int i = 0; i < a.width * 4; ++i) o[i] = p[i] + (q[i] - p[i]) * u;
        }
    });
    return out;
}

Image interpolateFrames(const Image& a, const Image& b, double t, const std::string& cacheKey) {
    if (a.width != b.width || a.height != b.height || a.width < 16 || a.height < 16) return blendFrames(a, b, t);
    double scale = 1;
    const auto flow = flowBetween(a, b, cacheKey, scale);
    Image out(a.width, a.height, Image::Uninitialized{});
    const double u = std::clamp(t, 0.0, 1.0);
    parallelRows(a.height, [&](int y0, int y1) {
        float pa[4], pb[4];
        for (int y = y0; y < y1; ++y) {
            float* o = out.row(y);
            for (int x = 0; x < a.width; ++x, o += 4) {
                // The motion through this pixel, then each frame sampled where it was / will be.
                const Point2 f = flow->at((x + 0.5) / scale - 0.5, (y + 0.5) / scale - 0.5);
                const double fx = f.x * scale, fy = f.y * scale;
                sampleBilinear(a, x - u * fx, y - u * fy, pa);
                sampleBilinear(b, x + (1 - u) * fx, y + (1 - u) * fy, pb);
                for (int k = 0; k < 4; ++k) o[k] = float(pa[k] * (1 - u) + pb[k] * u);
            }
        }
    });
    return out;
}

}  // namespace montage
