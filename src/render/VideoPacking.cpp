#include "VideoPacking.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace montage::video {

namespace {

uint16_t code16(double v) { return uint16_t(std::clamp(std::lround(v * 256), 0L, 65535L)); }

// A 16-bit sample as 10 bits, within SDI's legal codes.
uint32_t ten(uint16_t v) { return uint32_t(std::clamp((int(v) + 32) >> 6, 4, 1019)); }
uint8_t eight(uint16_t v) { return uint8_t(std::clamp((int(v) + 128) >> 8, 1, 254)); }

void put32(uint8_t*& d, uint32_t v) {
    d[0] = uint8_t(v), d[1] = uint8_t(v >> 8), d[2] = uint8_t(v >> 16), d[3] = uint8_t(v >> 24);
    d += 4;
}

void put16(uint8_t* d, uint16_t v) {
    d[0] = uint8_t(v), d[1] = uint8_t(v >> 8);
}

}  // namespace

Ycbcr422::Ycbcr422(int w, int h)
    : width(w), height(h), y(size_t(w) * size_t(h)), cb(size_t((w + 1) / 2) * size_t(h)), cr(cb.size()) {}

Ycbcr422 toYcbcr422(const Image& rgb, YcbcrMatrix matrix) {
    Ycbcr422 out(rgb.width, rgb.height);
    const double kr = matrix == YcbcrMatrix::Bt2020 ? 0.2627 : 0.2126;
    const double kb = matrix == YcbcrMatrix::Bt2020 ? 0.0593 : 0.0722;
    const double kg = 1 - kr - kb;
    const int w = rgb.width, cw = out.chromaWidth();
    parallelRows(rgb.height, [&](int y0, int y1) {
        std::vector<double> cb(static_cast<size_t>(w)), cr(static_cast<size_t>(w));
        for (int y = y0; y < y1; ++y) {
            const float* p = rgb.row(y);
            uint16_t* yo = out.y.data() + size_t(y) * size_t(w);
            for (int x = 0; x < w; ++x, p += 4) {
                const double r = p[0], g = p[1], b = p[2];
                const double luma = kr * r + kg * g + kb * b;
                yo[x] = code16(16 + 219 * luma);
                cb[size_t(x)] = (b - luma) / (2 * (1 - kb));
                cr[size_t(x)] = (r - luma) / (2 * (1 - kr));
            }
            uint16_t* cbo = out.cb.data() + size_t(y) * size_t(cw);
            uint16_t* cro = out.cr.data() + size_t(y) * size_t(cw);
            for (int i = 0; i < cw; ++i) {
                const size_t x = size_t(2 * i), l = x > 0 ? x - 1 : x, r = std::min(x + 1, size_t(w - 1));
                cbo[i] = code16(128 + 224 * (cb[l] + 2 * cb[x] + cb[r]) / 4);
                cro[i] = code16(128 + 224 * (cr[l] + 2 * cr[x] + cr[r]) / 4);
            }
        }
    });
    return out;
}

size_t v210RowBytes(int width) { return size_t((width + 47) / 48) * 128; }

void packV210(const Ycbcr422& in, uint8_t* dst, size_t stride) {
    const int w = in.width, cw = in.chromaWidth();
    const size_t used = size_t((w * 8 + 11) / 12) * 4;
    parallelRows(in.height, [&](int y0, int y1) {
        for (int row = y0; row < y1; ++row) {
            const uint16_t* y = in.y.data() + size_t(row) * size_t(w);
            const uint16_t* u = in.cb.data() + size_t(row) * size_t(cw);
            const uint16_t* v = in.cr.data() + size_t(row) * size_t(cw);
            uint8_t* d = dst + size_t(row) * stride;
            int x = 0;
            for (; x + 6 <= w; x += 6, y += 6, u += 3, v += 3) {
                put32(d, ten(u[0]) | ten(y[0]) << 10 | ten(v[0]) << 20);
                put32(d, ten(y[1]) | ten(u[1]) << 10 | ten(y[2]) << 20);
                put32(d, ten(v[1]) | ten(y[3]) << 10 | ten(u[2]) << 20);
                put32(d, ten(y[4]) | ten(v[2]) << 10 | ten(y[5]) << 20);
            }
            // The last two or four pixels, as FFmpeg's encoder leaves them.
            if (x < w - 1) {
                put32(d, ten(u[0]) | ten(y[0]) << 10 | ten(v[0]) << 20);
                if (x == w - 2) put32(d, ten(y[1]));
            }
            if (x < w - 3) {
                put32(d, ten(y[1]) | ten(u[1]) << 10 | ten(y[2]) << 20);
                put32(d, ten(v[1]) | ten(y[3]) << 10);
            }
            std::memset(dst + size_t(row) * stride + used, 0, stride - used);
        }
    });
}

void packUyvy(const Ycbcr422& in, uint8_t* dst, size_t stride) {
    const int w = in.width, cw = in.chromaWidth();
    parallelRows(in.height, [&](int y0, int y1) {
        for (int row = y0; row < y1; ++row) {
            const uint16_t* y = in.y.data() + size_t(row) * size_t(w);
            const uint16_t* u = in.cb.data() + size_t(row) * size_t(cw);
            const uint16_t* v = in.cr.data() + size_t(row) * size_t(cw);
            uint8_t* d = dst + size_t(row) * stride;
            for (int x = 0; x < w; x += 2, d += 4) {
                // (an odd width's last pixel twice)
                d[0] = eight(u[x / 2]), d[1] = eight(y[x]), d[2] = eight(v[x / 2]), d[3] = eight(y[std::min(x + 1, w - 1)]);
            }
        }
    });
}

void packP216(const Ycbcr422& in, uint8_t* dst, size_t stride) {
    const int w = in.width, cw = in.chromaWidth();
    uint8_t* uv = dst + stride * size_t(in.height);
    parallelRows(in.height, [&](int y0, int y1) {
        for (int row = y0; row < y1; ++row) {
            const uint16_t* y = in.y.data() + size_t(row) * size_t(w);
            const uint16_t* u = in.cb.data() + size_t(row) * size_t(cw);
            const uint16_t* v = in.cr.data() + size_t(row) * size_t(cw);
            uint8_t* dy = dst + size_t(row) * stride;
            uint8_t* duv = uv + size_t(row) * stride;
            for (int x = 0; x < w; ++x) put16(dy + 2 * x, y[x]);
            for (int i = 0; i < cw; ++i) {
                put16(duv + 4 * i, u[i]);
                put16(duv + 4 * i + 2, v[i]);
            }
        }
    });
}

}  // namespace montage::video
