#include "VideoDenoise.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>

#include "media/Tracking.h"

namespace montage {

namespace {

constexpr int kFlowWidth = 480;  // flow is measured on a copy this wide (as for retiming)
constexpr int kFlowStep = 6;

// Flows between consecutive frames, by "<key>#<from frame>".
struct ScaledFlow {
    FlowField field;
    double scale = 1;  // image pixels per flow pixel
    Point2 at(double x, double y) const {
        const Point2 f = field.at((x + 0.5) / scale - 0.5, (y + 0.5) / scale - 0.5);
        return {f.x * scale, f.y * scale};
    }
};

std::shared_ptr<const ScaledFlow> measureFlow(const Image& a, const Image& b) {
    auto f = std::make_shared<ScaledFlow>();
    const GrayImage ga = toGray(a, kFlowWidth);
    f->scale = double(a.width) / ga.width;
    f->field = denseFlow(ga, toGray(b, kFlowWidth), kFlowStep);
    return f;
}

std::shared_ptr<const ScaledFlow> consecutiveFlow(const std::string& key, int64_t from,
                                                  const std::function<const Image*(int64_t)>& frame) {
    static std::mutex m;
    static std::deque<std::pair<std::string, std::shared_ptr<const ScaledFlow>>> cache;
    const std::string k = key + '#' + std::to_string(from);
    {
        std::lock_guard lock(m);
        for (const auto& [name, f] : cache)
            if (name == k) return f;
    }
    const Image *a = frame(from), *b = frame(from + 1);
    if (!a || !b || a->width != b->width || a->height != b->height || a->width < 16 || a->height < 16) return nullptr;
    auto flow = measureFlow(*a, *b);
    std::lock_guard lock(m);
    cache.emplace_front(k, flow);
    if (cache.size() > 32) cache.pop_back();
    return flow;
}

// Bilinear; false outside the picture.
bool sampleInside(const Image& img, double x, double y, float out[4]) {
    if (x < -0.5 || y < -0.5 || x > img.width - 0.5 || y > img.height - 0.5) return false;
    x = std::clamp(x, 0.0, double(img.width - 1));
    y = std::clamp(y, 0.0, double(img.height - 1));
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
    const float fx = float(x - x0), fy = float(y - y0);
    const float *a = img.at(x0, y0), *b = img.at(x1, y0), *c = img.at(x0, y1), *d = img.at(x1, y1);
    for (int k = 0; k < 4; ++k) {
        const float top = a[k] + (b[k] - a[k]) * fx, bottom = c[k] + (d[k] - c[k]) * fx;
        out[k] = top + (bottom - top) * fy;
    }
    return true;
}

// 3x3 box sum of a plane, in place.
void box3(std::vector<float>& v, int w, int h) {
    std::vector<float> tmp(v.size());
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* r = &v[size_t(y) * size_t(w)];
            float* o = &tmp[size_t(y) * size_t(w)];
            for (int x = 0; x < w; ++x) o[x] = r[std::max(0, x - 1)] + r[x] + r[std::min(w - 1, x + 1)];
        }
    });
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float *a = &tmp[size_t(std::max(0, y - 1)) * size_t(w)], *b = &tmp[size_t(y) * size_t(w)],
                        *c = &tmp[size_t(std::min(h - 1, y + 1)) * size_t(w)];
            float* o = &v[size_t(y) * size_t(w)];
            for (int x = 0; x < w; ++x) o[x] = a[x] + b[x] + c[x];
        }
    });
}

// exp(-u) for u in [0, 16), tabulated.
struct ExpTable {
    static constexpr int kSize = 4096;
    static constexpr float kMax = 16;
    float v[kSize + 1];
    ExpTable() {
        for (int i = 0; i <= kSize; ++i) v[i] = std::exp(-kMax * float(i) / kSize);
    }
    float operator()(float u) const { return u >= kMax ? 0.0f : v[int(u * (kSize / kMax))]; }
};
const ExpTable& expTable() {
    static const ExpTable t;
    return t;
}

}  // namespace

double estimateNoise(const Image& img) {
    if (img.width < 4 || img.height < 4) return 0;
    const int bw = img.width / 2, bh = img.height / 2;
    double total = 0;
    for (int c = 0; c < 3; ++c) {
        std::vector<float> d;
        d.reserve(size_t(bw) * size_t(bh));
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                const float *a = img.at(2 * bx, 2 * by), *b = img.at(2 * bx + 1, 2 * by), *e = img.at(2 * bx, 2 * by + 1),
                            *f = img.at(2 * bx + 1, 2 * by + 1);
                if (a[3] < 0.99f || b[3] < 0.99f || e[3] < 0.99f || f[3] < 0.99f) continue;  // edges of a shape
                d.push_back(std::fabs(a[c] - b[c] - e[c] + f[c]) * 0.5f);
            }
        if (d.empty()) return 0;
        auto mid = d.begin() + std::ptrdiff_t(d.size() / 2);
        std::nth_element(d.begin(), mid, d.end());
        total += *mid / 0.6745;
    }
    return total / 3;
}

void spatialDenoise(Image& img, double sigma, double luma, double chroma, const std::vector<float>& noise) {
    luma = std::clamp(luma, 0.0, 1.0);
    chroma = std::clamp(chroma, 0.0, 1.0);
    if (img.empty() || sigma < 1e-4 || (luma <= 0 && chroma <= 0)) return;
    const int w = img.width, h = img.height;
    const size_t n = size_t(w) * size_t(h);
    // Rec.709 Y'CbCr of the (premultiplied) pixels.
    std::vector<float> Y(n), Cb(n), Cr(n);
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                const float* p = &img.px[i * 4];
                const float l = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
                Y[i] = l;
                Cb[i] = (p[2] - l) / 1.8556f;
                Cr[i] = (p[0] - l) / 1.5748f;
            }
    });
    std::vector<float> Yo = Y, Cbo = Cb, Cro = Cr;
    const ExpTable& ex = expTable();
    auto noiseAt = [&](size_t i) { return std::max(1e-4f, noise.empty() ? float(sigma) : noise[i]); };
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                const float s = noiseAt(i);
                if (luma > 0) {
                    // Luma: 5x5, the range a little over the noise at low settings and three times it at full.
                    const float hy = float(0.5 + 2.5 * luma) * s, inv = 1.0f / (2 * hy * hy);
                    float acc = 0, wsum = 0;
                    for (int dy = -2; dy <= 2; ++dy) {
                        const int yy = std::clamp(y + dy, 0, h - 1);
                        for (int dx = -2; dx <= 2; ++dx) {
                            const int xx = std::clamp(x + dx, 0, w - 1);
                            const float v = Y[size_t(yy) * size_t(w) + size_t(xx)], d = v - Y[i];
                            const float wt = ex(float(dx * dx + dy * dy) * (1.0f / (2 * 1.2f * 1.2f)) + d * d * inv);
                            acc += v * wt;
                            wsum += wt;
                        }
                    }
                    Yo[i] = acc / wsum;
                }
                if (chroma > 0) {
                    // Chroma: 7x7, kept to the same side of luma edges.
                    const float hc = float(0.5 + 3.0 * chroma) * s, invC = 1.0f / (2 * hc * hc);
                    const float hg = 3 * s, invG = 1.0f / (2 * hg * hg);
                    float ab = 0, ar = 0, wsum = 0;
                    for (int dy = -3; dy <= 3; ++dy) {
                        const int yy = std::clamp(y + dy, 0, h - 1);
                        for (int dx = -3; dx <= 3; ++dx) {
                            const int xx = std::clamp(x + dx, 0, w - 1);
                            const size_t j = size_t(yy) * size_t(w) + size_t(xx);
                            const float dl = Y[j] - Y[i], db = Cb[j] - Cb[i], dr = Cr[j] - Cr[i];
                            const float wt = ex(float(dx * dx + dy * dy) * (1.0f / (2 * 2.0f * 2.0f)) + dl * dl * invG +
                                                (db * db + dr * dr) * invC);
                            ab += Cb[j] * wt;
                            ar += Cr[j] * wt;
                            wsum += wt;
                        }
                    }
                    Cbo[i] = ab / wsum;
                    Cro[i] = ar / wsum;
                }
            }
    });
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                float* p = &img.px[i * 4];
                const float r = Yo[i] + 1.5748f * Cro[i], b = Yo[i] + 1.8556f * Cbo[i];
                const float g = (Yo[i] - 0.2126f * r - 0.0722f * b) / 0.7152f;
                p[0] = std::max(0.0f, r);
                p[1] = std::max(0.0f, g);
                p[2] = std::max(0.0f, b);
            }
    });
}

Image denoiseFrame(const Image& current, const std::vector<const Image*>& neighbours, const DenoiseSettings& s,
                   const std::vector<MotionFn>& motion, double* sigmaOut) {
    if (current.empty()) return current;
    const double sigma = s.noise > 0 ? s.noise : estimateNoise(current);
    if (sigmaOut) *sigmaOut = sigma;
    if (sigma < 1e-4) return current;
    const int w = current.width, h = current.height;
    const size_t n = size_t(w) * size_t(h);
    Image out = current;
    std::vector<float> noise;  // what is left of it at each pixel after the temporal pass

    // Temporal: each neighbour warped onto this frame and weighted by how well it matches here.
    std::vector<const Image*> usable;
    for (const Image* nb : neighbours)
        if (nb && nb->width == w && nb->height == h) usable.push_back(nb);
    if (s.temporal > 0 && !usable.empty()) {
        std::vector<float> acc(current.px.begin(), current.px.end());
        std::vector<float> wsum(n, 1.0f), w2sum(n, 1.0f);
        const bool flowOk = s.motion && w >= 16 && h >= 16;
        const float twoSigma2 = float(2 * sigma * sigma), invH = 1.0f / float(2 * sigma * sigma * s.temporal * s.temporal);
        const ExpTable& ex = expTable();
        Image warped(w, h, Image::Uninitialized{});
        std::vector<float> dist(n), valid(n);
        for (size_t k = 0; k < usable.size(); ++k) {
            const Image& nb = *usable[k];
            MotionFn move = flowOk && k < motion.size() ? motion[k] : MotionFn();
            if (flowOk && !move) {
                auto flow = measureFlow(current, nb);
                move = [flow](double x, double y) { return flow->at(x, y); };
            }
            parallelRows(h, [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y)
                    for (int x = 0; x < w; ++x) {
                        const size_t i = size_t(y) * size_t(w) + size_t(x);
                        float* q = warped.at(x, y);
                        bool in = true;
                        if (move) {
                            const Point2 f = move(x, y);
                            in = sampleInside(nb, x + f.x, y + f.y, q);
                        } else {
                            std::copy_n(nb.at(x, y), 4, q);
                        }
                        const float* c = current.at(x, y);
                        float e = 0;
                        for (int ch = 0; ch < 3; ++ch) e += (c[ch] - q[ch]) * (c[ch] - q[ch]);
                        dist[i] = in ? e * (1.0f / 3) : 1e3f;
                        valid[i] = in ? 1.0f : 0.0f;
                    }
            });
            box3(dist, w, h);  // patch distance: a 3x3 neighbourhood, so one noisy pixel does not decide
            parallelRows(h, [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y)
                    for (int x = 0; x < w; ++x) {
                        const size_t i = size_t(y) * size_t(w) + size_t(x);
                        if (valid[i] <= 0) continue;
                        const float d = dist[i] * (1.0f / 9);
                        const float wt = ex(std::max(0.0f, d - twoSigma2) * invH);
                        if (wt <= 0) continue;
                        const float* q = warped.at(x, y);
                        for (int ch = 0; ch < 4; ++ch) acc[i * 4 + size_t(ch)] += q[ch] * wt;
                        wsum[i] += wt;
                        w2sum[i] += wt * wt;
                    }
            });
        }
        noise.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const float inv = 1.0f / wsum[i];
            for (int ch = 0; ch < 4; ++ch) out.px[i * 4 + size_t(ch)] = acc[i * 4 + size_t(ch)] * inv;
            noise[i] = float(sigma) * std::sqrt(w2sum[i]) * inv;
        }
    }

    // Spatial, as strong as the noise that is left.
    spatialDenoise(out, sigma, s.spatialLuma, s.spatialChroma, noise);

    if (s.blend > 0) {
        const float b = float(std::clamp(s.blend, 0.0, 1.0));
        for (size_t i = 0; i < out.px.size(); ++i) out.px[i] += (current.px[i] - out.px[i]) * b;
    }
    return out;
}

std::vector<MotionFn> chainedMotion(const std::string& key, int64_t base, const std::vector<int>& offsets,
                                    const std::function<const Image*(int64_t)>& frame) {
    std::vector<MotionFn> out(offsets.size());
    for (size_t i = 0; i < offsets.size(); ++i) {
        const int k = offsets[i];
        if (k == 0) continue;
        // The flows along the way: forwards base -> base + k, or backwards through base - 1 -> base and so on.
        std::vector<std::shared_ptr<const ScaledFlow>> steps;
        bool ok = true;
        for (int j = 0; j < std::abs(k) && ok; ++j) {
            const int64_t from = k > 0 ? base + j : base - 1 - j;
            auto f = consecutiveFlow(key, from, frame);
            ok = f != nullptr;
            steps.push_back(std::move(f));
        }
        if (!ok) continue;
        if (k > 0) {
            out[i] = [steps](double x, double y) {
                double px = x, py = y;
                for (const auto& f : steps) {
                    const Point2 d = f->at(px, py);
                    px += d.x, py += d.y;
                }
                return Point2{px - x, py - y};
            };
        } else {
            // Each step backwards inverts a forward flow: q with q + F(q) = p, by two fixed-point steps.
            out[i] = [steps](double x, double y) {
                double px = x, py = y;
                for (const auto& f : steps) {
                    Point2 d = f->at(px, py);
                    d = f->at(px - d.x, py - d.y);
                    px -= d.x, py -= d.y;
                }
                return Point2{px - x, py - y};
            };
        }
    }
    return out;
}

}  // namespace montage
