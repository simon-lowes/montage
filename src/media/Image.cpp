#include "Image.h"

#include <algorithm>
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

Image toImage(const Frame16& f) {
    Image img(f.width, f.height, Image::Uninitialized{});
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
    for (int b = 0; b < bands; ++b) {
        int y0 = int(int64_t(height) * b / bands);
        int y1 = int(int64_t(height) * (b + 1) / bands);
        p.submit([&, y0, y1] {
            fn(y0, y1);
            std::lock_guard lock(m);
            if (--remaining == 0) cv.notify_one();
        });
    }
    std::unique_lock lock(m);
    cv.wait(lock, [&] { return remaining == 0; });
}

}  // namespace montage
