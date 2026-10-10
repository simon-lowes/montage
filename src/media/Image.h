// Montage — image buffers used by the renderer.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace montage {

// A vector allocator that leaves elements uninitialised when sized without a
// value: frame buffers are tens of megabytes and are written in full, so
// zero-filling them first only costs memory bandwidth.
template <class T>
struct NoInitAllocator : std::allocator<T> {
    using std::allocator<T>::allocator;
    template <class U>
    struct rebind {
        using other = NoInitAllocator<U>;
    };
    template <class U>
    void construct(U* p) noexcept {
        ::new (static_cast<void*>(p)) U;
    }
    template <class U, class... A>
    void construct(U* p, A&&... a) {
        ::new (static_cast<void*>(p)) U(std::forward<A>(a)...);
    }
};
template <class T>
using PixelVector = std::vector<T, NoInitAllocator<T>>;

// Float RGBA, premultiplied alpha, display-referred (Rec.709 / sRGB gamma).
struct Image {
    int width = 0;
    int height = 0;
    PixelVector<float> px;  // width * height * 4

    struct Uninitialized {};
    Image() = default;
    // Transparent black.
    Image(int w, int h) : width(w), height(h), px(size_t(w) * size_t(h) * 4, 0.0f) {}
    // Contents undefined: for producers that write every pixel.
    Image(int w, int h, Uninitialized) : width(w), height(h), px(size_t(w) * size_t(h) * 4) {}
    bool empty() const { return width <= 0 || height <= 0; }
    float* row(int y) { return px.data() + size_t(y) * size_t(width) * 4; }
    const float* row(int y) const { return px.data() + size_t(y) * size_t(width) * 4; }
    float* at(int x, int y) { return px.data() + (size_t(y) * size_t(width) + size_t(x)) * 4; }
    const float* at(int x, int y) const { return px.data() + (size_t(y) * size_t(width) + size_t(x)) * 4; }
    void fill(float r, float g, float b, float a);
};

// 16-bit RGBA straight alpha, as produced by the decoder (cache-friendly).
struct Frame16 {
    int width = 0;
    int height = 0;
    double pts = 0;  // seconds
    PixelVector<uint16_t> px;  // written in full by the decoder
    size_t bytes() const { return px.size() * sizeof(uint16_t); }
};
using Frame16Ptr = std::shared_ptr<const Frame16>;

Image toImage(const Frame16& f);
// Converts to 8-bit RGBA (straight alpha) e.g. for display / PNG export.
std::vector<uint8_t> toRgba8(const Image& img);
// The same into caller memory (e.g. a QImage's bits), `stride` bytes per row.
void toRgba8(const Image& img, uint8_t* dst, size_t stride);
// Clamped, unpremultiplied 8-bit RGB(A) helpers for encoders.
void toRgba16(const Image& img, std::vector<uint16_t>& out);

// Runs fn(y0, y1) over row bands on a shared worker pool.
void parallelRows(int height, const std::function<void(int, int)>& fn);

}  // namespace montage
