#include "Image.h"

#include "core/Expression.h"

#include <QtCore/qfloat16.h>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace montage {

namespace {
// Frame buffers are tens of megabytes. glibc serves allocations that large
// with fresh mmap()s, so every frame paid for page faults and zero-filling.
// Keeping them on the heap lets freed buffers be reused.
const bool kHeapTuned = [] {
#ifdef __GLIBC__
    mallopt(M_MMAP_THRESHOLD, 256 * 1024 * 1024);
    mallopt(M_TRIM_THRESHOLD, 512 * 1024 * 1024);
#endif
    return true;
}();
}  // namespace

void Image::fill(float r, float g, float b, float a) {
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = r * a;
        px[i + 1] = g * a;
        px[i + 2] = b * a;
        px[i + 3] = a;
    }
}

size_t YcbcrPlanes::bytes() const {
    const size_t sample = wide ? 2 : 1;
    return size_t(width) * size_t(height) * sample + size_t(chromaWidth()) * size_t(chromaHeight()) * sample * 2;
}

std::vector<float> resampleRgba(const float* src, int sw, int sh, int dw, int dh) {
    // One direction: n in, m out, each output's taps and weights (normalised).
    auto weights = [](int n, int m, std::vector<int>& first, std::vector<std::vector<float>>& w) {
        first.assign(size_t(m), 0);
        w.assign(size_t(m), {});
        const float scale = float(n) / float(m), radius = std::max(1.0f, scale);
        for (int i = 0; i < m; ++i) {
            const float c = (float(i) + 0.5f) * scale - 0.5f;
            const int j0 = int(std::ceil(c - radius)), j1 = int(std::floor(c + radius));
            first[size_t(i)] = j0;
            float sum = 0;
            for (int j = j0; j <= j1; ++j) {
                const float t = std::max(0.0f, 1.0f - std::fabs(float(j) - c) / radius);
                w[size_t(i)].push_back(t);
                sum += t;
            }
            for (float& t : w[size_t(i)]) t /= sum;
        }
    };
    std::vector<int> fx, fy;
    std::vector<std::vector<float>> wx, wy;
    weights(sw, dw, fx, wx);
    weights(sh, dh, fy, wy);
    std::vector<float> across(size_t(dw) * size_t(sh) * 4), out(size_t(dw) * size_t(dh) * 4);
    parallelRows(sh, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < dw; ++x) {
                float acc[4] = {};
                for (size_t k = 0; k < wx[size_t(x)].size(); ++k) {
                    const int j = std::clamp(fx[size_t(x)] + int(k), 0, sw - 1);
                    for (int c = 0; c < 4; ++c) acc[c] += src[(size_t(y) * size_t(sw) + size_t(j)) * 4 + size_t(c)] * wx[size_t(x)][k];
                }
                std::copy(acc, acc + 4, &across[(size_t(y) * size_t(dw) + size_t(x)) * 4]);
            }
    });
    parallelRows(dh, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < dw; ++x) {
                float acc[4] = {};
                for (size_t k = 0; k < wy[size_t(y)].size(); ++k) {
                    const int j = std::clamp(fy[size_t(y)] + int(k), 0, sh - 1);
                    for (int c = 0; c < 4; ++c) acc[c] += across[(size_t(j) * size_t(dw) + size_t(x)) * 4 + size_t(c)] * wy[size_t(y)][k];
                }
                std::copy(acc, acc + 4, &out[(size_t(y) * size_t(dw) + size_t(x)) * 4]);
            }
    });
    return out;
}

namespace {

// Each pixel of `p` as straight RGB clamped to 0..1, given to put(x, y, r, g, b) row band by row band in parallel.
template <class Put>
void eachYcbcrPixel(const YcbcrPlanes& p, const Put& put) {
    const int W = p.width, H = p.height, cw = p.chromaWidth(), ch = p.chromaHeight();
    const float top = float((1 << p.bits) - 1), unit = float(1 << (p.bits - 8));
    const float yOff = p.fullRange ? 0.0f : 16.0f * unit, yRange = p.fullRange ? top : 219.0f * unit;
    const float cOff = p.fullRange ? float(1 << (p.bits - 1)) : 128.0f * unit, cRange = p.fullRange ? top : 224.0f * unit;
    auto sample = [&](int plane, int x, int y) {
        const uint8_t* row = p.data[plane] + size_t(y) * size_t(p.stride[plane]);
        if (!p.wide) return float(row[x]);
        uint16_t v;
        std::memcpy(&v, row + size_t(x) * 2, 2);
        return float(v >> p.shift);
    };
    auto chroma = [&](int which, int x, int y) {  // (Cb 0, Cr 1)
        return p.interleaved ? sample(1, x * 2 + which, y) : sample(1 + which, x, y);
    };
    const float sx = float(1 << p.log2ChromaW), sy = float(1 << p.log2ChromaH);
    const float kr = p.kr, kb = p.kb, kg = 1.0f - kr - kb;
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            // Where the luma sample falls among the chroma samples, interpolated between the two either side.
            const float cy = (float(y) - p.chromaY) / sy;
            const int cy0 = int(std::floor(cy));
            const float fy = cy - float(cy0);
            const int ya = std::clamp(cy0, 0, ch - 1), yb = std::clamp(cy0 + 1, 0, ch - 1);
            for (int x = 0; x < W; ++x) {
                const float cx = (float(x) - p.chromaX) / sx;
                const int cx0 = int(std::floor(cx));
                const float fx = cx - float(cx0);
                const int xa = std::clamp(cx0, 0, cw - 1), xb = std::clamp(cx0 + 1, 0, cw - 1);
                float c[2];
                for (int k = 0; k < 2; ++k) {
                    const float a = chroma(k, xa, ya) + (chroma(k, xb, ya) - chroma(k, xa, ya)) * fx;
                    const float b = chroma(k, xa, yb) + (chroma(k, xb, yb) - chroma(k, xa, yb)) * fx;
                    c[k] = (a + (b - a) * fy - cOff) / cRange;
                }
                const float Y = (sample(0, x, y) - yOff) / yRange;
                const float r = Y + 2.0f * (1.0f - kr) * c[1], b = Y + 2.0f * (1.0f - kb) * c[0];
                const float g = (Y - kr * r - kb * b) / kg;
                put(x, y, std::clamp(r, 0.0f, 1.0f), std::clamp(g, 0.0f, 1.0f), std::clamp(b, 0.0f, 1.0f));
            }
        }
    });
}

}  // namespace

std::vector<float> ycbcrToRgba(const YcbcrPlanes& p, int w, int h) {
    const int W = p.width, H = p.height;
    std::vector<float> rgba(size_t(W) * size_t(H) * 4);
    eachYcbcrPixel(p, [&](int x, int y, float r, float g, float b) {
        float* o = &rgba[(size_t(y) * size_t(W) + size_t(x)) * 4];
        o[0] = r, o[1] = g, o[2] = b, o[3] = 1.0f;
    });
    if (w <= 0) w = W;
    if (h <= 0) h = H;
    return w == W && h == H ? rgba : resampleRgba(rgba.data(), W, H, w, h);
}

void ycbcrToRgba16(const YcbcrPlanes& p, uint16_t* dst) {
    eachYcbcrPixel(p, [&](int x, int y, float r, float g, float b) {
        uint16_t* o = dst + (size_t(y) * size_t(p.width) + size_t(x)) * 4;
        o[0] = uint16_t(r * 65535.0f + 0.5f), o[1] = uint16_t(g * 65535.0f + 0.5f), o[2] = uint16_t(b * 65535.0f + 0.5f);
        o[3] = 65535;
    });
}

Image toImage(const Frame16& f) {
    if (f.planes && f.px.empty()) {
        const std::vector<float> rgba = ycbcrToRgba(*f.planes, f.width, f.height);
        Image img(f.width, f.height, Image::Uninitialized{});
        std::copy(rgba.begin(), rgba.end(), img.px.begin());  // (alpha 1: premultiplied as it is)
        return img;
    }
    Image img(f.width, f.height, Image::Uninitialized{});
    if (f.half) {
        // Half floats as they are (values past 0..1 kept); alpha clamped to 0..1 before premultiplying.
        parallelRows(f.height, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float* d = img.row(y);
                qFloatFromFloat16(d, reinterpret_cast<const qfloat16*>(f.px.data() + size_t(y) * size_t(f.width) * 4), qsizetype(f.width) * 4);
                for (int x = 0; x < f.width; ++x, d += 4) {
                    float a = d[3] == d[3] ? std::clamp(d[3], 0.0f, 1.0f) : 0.0f;
                    for (int c = 0; c < 3; ++c) d[c] = std::isfinite(d[c]) ? d[c] * a : 0.0f;
                    d[3] = a;
                }
            }
        });
        return img;
    }
    const float k = 1.0f / 65535.0f;
    parallelRows(f.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint16_t* s = f.px.data() + size_t(y) * size_t(f.width) * 4;
            float* d = img.row(y);
            for (int x = 0; x < f.width; ++x, s += 4, d += 4) {
                float a = s[3] * k;
                d[0] = s[0] * k * a;
                d[1] = s[1] * k * a;
                d[2] = s[2] * k * a;
                d[3] = a;
            }
        }
    });
    return img;
}

std::shared_ptr<Frame16> halfFrame(const float* rgba, int srcW, int srcH, int w, int h, double pts) {
    auto out = std::make_shared<Frame16>();
    if (w <= 0) w = srcW;
    if (h <= 0) h = srcH;
    out->width = w, out->height = h, out->pts = pts, out->half = true;
    out->px.resize(size_t(w) * size_t(h) * 4);
    if (srcW <= 0 || srcH <= 0) return out;
    // Each output pixel from the source: the average of the source pixels it covers when smaller (weighted by
    // coverage, colour weighted by alpha so edges keep their colour), bilinear when larger.
    const double sx = double(srcW) / w, sy = double(srcH) / h;
    parallelRows(h, [&](int y0, int y1) {
        std::vector<float> row(size_t(w) * 4);
        for (int y = y0; y < y1; ++y) {
            for (int x = 0; x < w; ++x) {
                float* d = &row[size_t(x) * 4];
                double acc[4] = {0, 0, 0, 0}, wsum = 0;
                if (sx > 1 || sy > 1) {
                    const double fx0 = x * sx, fx1 = (x + 1) * sx, fy0 = y * sy, fy1 = (y + 1) * sy;
                    for (int j = int(fy0); j < std::min(srcH, int(std::ceil(fy1))); ++j) {
                        const double wy = std::min(fy1, j + 1.0) - std::max(fy0, double(j));
                        for (int i = int(fx0); i < std::min(srcW, int(std::ceil(fx1))); ++i) {
                            const double wt = wy * (std::min(fx1, i + 1.0) - std::max(fx0, double(i)));
                            const float* p = rgba + (size_t(j) * size_t(srcW) + size_t(i)) * 4;
                            const double a = std::isfinite(p[3]) ? std::clamp(double(p[3]), 0.0, 1.0) : 0.0;
                            for (int c = 0; c < 3; ++c) acc[c] += std::isfinite(p[c]) ? wt * a * p[c] : 0.0;
                            acc[3] += wt * a;
                            wsum += wt;
                        }
                    }
                } else {
                    const double fx = std::clamp((x + 0.5) * sx - 0.5, 0.0, srcW - 1.0), fy = std::clamp((y + 0.5) * sy - 0.5, 0.0, srcH - 1.0);
                    const int x0 = int(fx), y0i = int(fy), x1 = std::min(srcW - 1, x0 + 1), y1i = std::min(srcH - 1, y0i + 1);
                    const double tx = fx - x0, ty = fy - y0i;
                    for (const auto& [i, j, wt] : {std::tuple{x0, y0i, (1 - tx) * (1 - ty)}, std::tuple{x1, y0i, tx * (1 - ty)},
                                                   std::tuple{x0, y1i, (1 - tx) * ty}, std::tuple{x1, y1i, tx * ty}}) {
                        const float* p = rgba + (size_t(j) * size_t(srcW) + size_t(i)) * 4;
                        const double a = std::isfinite(p[3]) ? std::clamp(double(p[3]), 0.0, 1.0) : 0.0;
                        for (int c = 0; c < 3; ++c) acc[c] += std::isfinite(p[c]) ? wt * a * p[c] : 0.0;
                        acc[3] += wt * a;
                        wsum += wt;
                    }
                }
                const double a = wsum > 0 ? acc[3] / wsum : 0;
                for (int c = 0; c < 3; ++c) d[c] = acc[3] > 0 ? float(std::clamp(acc[c] / acc[3], -65504.0, 65504.0)) : 0.0f;  // (half's range)
                d[3] = float(a);
            }
            qFloatToFloat16(reinterpret_cast<qfloat16*>(out->px.data() + size_t(y) * size_t(w) * 4), row.data(), qsizetype(w) * 4);
        }
    });
    return out;
}

void toRgba8(const Image& img, uint8_t* dst, size_t stride) {
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* s = img.row(y);
            uint8_t* d = dst + size_t(y) * stride;
            for (int x = 0; x < img.width; ++x, s += 4, d += 4) {
                if (s[3] >= 1.0f) {  // opaque (every program frame): nothing to unpremultiply
                    for (int c = 0; c < 3; ++c) d[c] = uint8_t(std::clamp(s[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                    d[3] = 255;
                    continue;
                }
                float a = std::max(s[3], 0.0f);
                float inv = a > 1e-6f ? 1.0f / a : 0.0f;
                for (int c = 0; c < 3; ++c) d[c] = uint8_t(std::clamp(s[c] * inv, 0.0f, 1.0f) * 255.0f + 0.5f);
                d[3] = uint8_t(a * 255.0f + 0.5f);
            }
        }
    });
}

std::vector<uint8_t> toRgba8(const Image& img) {
    std::vector<uint8_t> out(size_t(img.width) * size_t(img.height) * 4);
    toRgba8(img, out.data(), size_t(img.width) * 4);
    return out;
}

void toRgba16(const Image& img, std::vector<uint16_t>& out) {
    out.resize(size_t(img.width) * size_t(img.height) * 4);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* s = img.row(y);
            uint16_t* d = out.data() + size_t(y) * size_t(img.width) * 4;
            for (int x = 0; x < img.width; ++x, s += 4, d += 4) {
                float a = std::clamp(s[3], 0.0f, 1.0f);
                float inv = a > 1e-6f ? 1.0f / a : 0.0f;
                for (int c = 0; c < 3; ++c) d[c] = uint16_t(std::clamp(s[c] * inv, 0.0f, 1.0f) * 65535.0f + 0.5f);
                d[3] = uint16_t(a * 65535.0f + 0.5f);
            }
        }
    });
}

// ---------------------------------------------------------------------------
// Worker pool

namespace {

thread_local bool tInsideWorker = false;

class Pool {
public:
    Pool() {
        unsigned n = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned i = 0; i < n; ++i)
            threads_.emplace_back([this] {
                tInsideWorker = true;
                for (;;) {
                    std::function<void()> job;
                    {
                        std::unique_lock lock(m_);
                        cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                        if (stop_ && jobs_.empty()) return;
                        job = std::move(jobs_.front());
                        jobs_.pop_front();
                    }
                    job();
                }
            });
    }
    ~Pool() {
        {
            std::lock_guard lock(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) t.join();
    }
    size_t size() const { return threads_.size(); }
    void submit(std::function<void()> f) {
        {
            std::lock_guard lock(m_);
            jobs_.push_back(std::move(f));
        }
        cv_.notify_one();
    }

private:
    std::vector<std::thread> threads_;
    std::deque<std::function<void()>> jobs_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
};

Pool& pool() {
    static Pool p;
    return p;
}

}  // namespace

void parallelRows(int height, const std::function<void(int, int)>& fn) {
    if (height <= 0) return;
    Pool& p = pool();
    int bands = int(std::min<size_t>(p.size(), size_t(std::max(1, height / 16))));
    if (bands <= 1 || tInsideWorker) {
        fn(0, height);
        return;
    }
    // The counter is only touched under the mutex, so the last worker has
    // released the mutex (and touches nothing else) before the waiting caller
    // can observe zero and destroy these locals.
    int remaining = bands;
    std::mutex m;
    std::condition_variable cv;
    // Expressions evaluated in a band belong where the caller's do (core/Expression.h).
    const ExpressionContext where = currentExpressionContext();
    for (int b = 0; b < bands; ++b) {
        int y0 = int(int64_t(height) * b / bands);
        int y1 = int(int64_t(height) * (b + 1) / bands);
        p.submit([&, y0, y1] {
            {
                ExpressionScope scope(where.project, where.sequence, where.clip, where.cancel);
                fn(y0, y1);
            }
            std::lock_guard lock(m);
            if (--remaining == 0) cv.notify_one();
        });
    }
    std::unique_lock lock(m);
    cv.wait(lock, [&] { return remaining == 0; });
}

}  // namespace montage
