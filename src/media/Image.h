// Montage — image buffers used by the renderer.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
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

// An IEEE half float's value (one that is not a number reads 0).
inline float halfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    uint32_t bits;
    if (e == 0) {
        if (m == 0) {
            bits = sign;
        } else {  // subnormal: 2^-14 * m/1024
            const float v = float(m) * (1.0f / 16777216.0f);
            return sign ? -v : v;
        }
    } else if (e == 31) {
        if (m) return 0.0f;
        bits = sign | 0x7f800000u;
    } else {
        bits = sign | ((e + 112) << 23) | (m << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// A decoded picture's Y′CbCr planes as the decoder made them, for render/GpuCompositor to convert on the GPU instead of
// the decoder converting them to RGBA: 8 to 16 bits; 4:2:0, 4:2:2 or 4:4:4; three planes, or Cb and Cr alternating in
// a second one (NV12, P010). ycbcrToRgba is the same conversion on the CPU.
struct YcbcrPlanes {
    int width = 0, height = 0;  // the luma plane's
    int bits = 8;               // significant bits a sample
    bool wide = false;          // samples in little-endian 16-bit words rather than bytes ...
    int shift = 0;              // ... with their bits this far up from the bottom (P010: 6)
    int log2ChromaW = 1, log2ChromaH = 1;
    bool interleaved = false;   // Cb and Cr alternating in plane 1
    bool fullRange = false;
    float kr = 0.2126f, kb = 0.0722f;  // the matrix's red and blue weights
    float chromaX = 0, chromaY = 0.5f;  // where the first chroma sample sits, in luma pixels from the first luma sample
    const uint8_t* data[3] = {};
    int stride[3] = {};                // bytes a row
    std::shared_ptr<const void> owner;  // what keeps the samples
    int chromaWidth() const { return (width + (1 << log2ChromaW) - 1) >> log2ChromaW; }
    int chromaHeight() const { return (height + (1 << log2ChromaH) - 1) >> log2ChromaH; }
    size_t bytes() const;
};

// 16-bit RGBA straight alpha, as produced by the decoder (cache-friendly): code values 0..65535 for 0..1, or, for a
// floating-point source (OpenEXR, float DPX/TIFF), half floats kept as they are, values past 0..1 included. Or, from a
// decoder asked for them, the Y′CbCr planes as decoded (`planes`, px empty), shown at width x height.
struct Frame16 {
    int width = 0;
    int height = 0;
    double pts = 0;  // seconds
    bool half = false;         // px holds IEEE half floats rather than 0..65535
    bool damaged = false;      // the decoder reported errors in it (render/FrameRepair.h)
    PixelVector<uint16_t> px;  // written in full by the decoder
    std::shared_ptr<const YcbcrPlanes> planes;
    size_t bytes() const { return px.size() * sizeof(uint16_t) + (planes ? planes->bytes() : 0); }
    // Sample i as 0..1 (a half float's value clamped there): for analysis that works on the visible range.
    float unit(size_t i) const { return half ? std::min(1.0f, std::max(0.0f, halfToFloat(px[i]))) : float(px[i]) * (1.0f / 65535.0f); }
    // The same as a 16-bit code value.
    uint16_t code(size_t i) const { return half ? uint16_t(unit(i) * 65535.0f + 0.5f) : px[i]; }
};
using Frame16Ptr = std::shared_ptr<const Frame16>;

Image toImage(const Frame16& f);
// Y′CbCr planes as straight RGBA (alpha 1, colour clamped to 0..1) at their own size, as the GPU converts them: chroma
// interpolated bilinearly from where its samples sit (edges repeated), the planes' matrix and range. Then, when the
// size differs, resampled to w x h as the GPU resamples (resampleRgba).
std::vector<float> ycbcrToRgba(const YcbcrPlanes& p, int w = 0, int h = 0);
// The same at the planes' own size into 16-bit RGBA (as Frame16's px; alpha 65535).
void ycbcrToRgba16(const YcbcrPlanes& p, uint16_t* dst);
// Straight RGBA resampled from sw x sh to dw x dh, rows then columns, each output pixel a tent of the source's pixels
// around where it falls (radius one source pixel, or the scale when shrinking: bilinear enlarging, an area-like
// average shrinking), edges repeated.
std::vector<float> resampleRgba(const float* src, int sw, int sh, int dw, int dh);
// Float RGBA (straight alpha, values unclamped) into a half-float Frame16, scaled to `w` x `h` (an area average when
// smaller, bilinear when larger) when that differs.
std::shared_ptr<Frame16> halfFrame(const float* rgba, int srcW, int srcH, int w, int h, double pts);
// Converts to 8-bit RGBA (straight alpha) e.g. for display / PNG export.
std::vector<uint8_t> toRgba8(const Image& img);
// The same into caller memory (e.g. a QImage's bits), `stride` bytes per row.
void toRgba8(const Image& img, uint8_t* dst, size_t stride);
// Clamped, unpremultiplied 8-bit RGB(A) helpers for encoders.
void toRgba16(const Image& img, std::vector<uint16_t>& out);

// Runs fn(y0, y1) over row bands on a shared worker pool.
void parallelRows(int height, const std::function<void(int, int)>& fn);

}  // namespace montage
