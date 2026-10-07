// Montage — image buffers used by the renderer.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace montage {

// Float RGBA, premultiplied alpha, display-referred (Rec.709 / sRGB gamma).
struct Image {
    int width = 0;
    int height = 0;
    std::vector<float> px;  // width * height * 4

    Image() = default;
    Image(int w, int h) : width(w), height(h), px(size_t(w) * size_t(h) * 4, 0.0f) {}
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
    std::vector<uint16_t> px;
    size_t bytes() const { return px.size() * sizeof(uint16_t); }
};
using Frame16Ptr = std::shared_ptr<const Frame16>;

Image toImage(const Frame16& f);
// Converts to 8-bit RGBA (straight alpha) e.g. for display / PNG export.
std::vector<uint8_t> toRgba8(const Image& img);
// Clamped, unpremultiplied 8-bit RGB(A) helpers for encoders.
void toRgba16(const Image& img, std::vector<uint16_t>& out);

// Runs fn(y0, y1) over row bands on a shared worker pool.
void parallelRows(int height, const std::function<void(int, int)>& fn);

}  // namespace montage
