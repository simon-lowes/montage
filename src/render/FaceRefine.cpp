#include "FaceRefine.h"

#include <algorithm>
#include <cmath>

#include "Processing.h"

namespace montage {

namespace {

double smoothstep(double e0, double e1, double x) {
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// 1 inside [lo, hi], falling to 0 over `ramp` outside it.
double band(double v, double lo, double hi, double ramp) {
    if (v >= lo && v <= hi) return 1.0;
    const double out = v < lo ? lo - v : v - hi;
    return std::max(0.0, 1.0 - out / ramp);
}

// How skin-coloured a pixel is, from its chroma (the classic YCbCr skin box, softened).
double skinness(double r, double g, double b) {
    const double cb = 0.5 - 0.168736 * r - 0.331264 * g + 0.5 * b, cr = 0.5 + 0.5 * r - 0.418688 * g - 0.081312 * b;
    return band(cb, 0.30, 0.50, 0.05) * band(cr, 0.52, 0.68, 0.05);
}

}  // namespace

std::vector<float> faceSkinMask(const FaceBox& f, int w, int h) {
    std::vector<float> m(size_t(std::max(0, w)) * size_t(std::max(0, h)), 0.0f);
    const double fw = f.w * w, fh = f.h * h;
    if (fw < 2 || fh < 2) return m;
    const double cx = (f.x + f.w / 2) * w, cy = (f.y + f.h * 0.48) * h, a = fw * 0.52, b = fh * 0.6;
    const double eyeR = 0.11 * fw;
    const double ex[2] = {f.landmarks[0] * w, f.landmarks[2] * w}, ey[2] = {f.landmarks[1] * h, f.landmarks[3] * h};
    const double mx = (f.landmarks[6] + f.landmarks[8]) / 2 * w, my = (f.landmarks[7] + f.landmarks[9]) / 2 * h;
    const double mw = std::max<double>(2.0, std::hypot((f.landmarks[8] - f.landmarks[6]) * w, (f.landmarks[9] - f.landmarks[7]) * h));
    const int x0 = std::max(0, int(cx - a - 2)), x1 = std::min(w, int(cx + a + 2)), y0 = std::max(0, int(cy - b - 2)), y1 = std::min(h, int(cy + b + 2));
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            const double r = std::hypot((px - cx) / a, (py - cy) / b);
            double v = 1 - smoothstep(0.8, 1.0, r);
            if (v <= 0) continue;
            for (int e = 0; e < 2; ++e) v *= smoothstep(eyeR * 0.8, eyeR * 1.3, std::hypot(px - ex[e], py - ey[e]));
            v *= smoothstep(0.8, 1.2, std::hypot((px - mx) / (0.65 * mw), (py - my) / (0.3 * mw)));
            m[size_t(y) * size_t(w) + size_t(x)] = float(v);
        }
    return m;
}

void refineFaces(Image& img, const std::vector<FaceBox>& faces, const FaceRefineSettings& s) {
    const int W = img.width, H = img.height;
    for (const FaceBox& f : faces) {
        const double fw = f.w * W;
        if (fw < 8) continue;
        // Work on the face and a margin round it.
        const int rx0 = std::max(0, int((f.x - f.w * 0.3) * W)), ry0 = std::max(0, int((f.y - f.h * 0.3) * H));
        const int rx1 = std::min(W, int((f.x + f.w * 1.3) * W) + 1), ry1 = std::min(H, int((f.y + f.h * 1.4) * H) + 1);
        const int rw = rx1 - rx0, rh = ry1 - ry0;
        if (rw < 4 || rh < 4) continue;
        Image region(rw, rh, Image::Uninitialized{});
        for (int y = 0; y < rh; ++y) std::copy_n(img.at(rx0, ry0 + y), size_t(rw) * 4, region.at(0, y));
        FaceBox local = f;
        local.x = float((f.x * W - rx0) / rw), local.y = float((f.y * H - ry0) / rh), local.w = float(f.w * W / rw), local.h = float(f.h * H / rh);
        for (int k = 0; k < 5; ++k) {
            local.landmarks[2 * k] = float((f.landmarks[2 * k] * W - rx0) / rw);
            local.landmarks[2 * k + 1] = float((f.landmarks[2 * k + 1] * H - ry0) / rh);
        }
        const std::vector<float> mask = faceSkinMask(local, rw, rh);
        Image soft = region, fine = region;
        gaussianBlur(soft, std::max(1.5, 0.025 * fw));
        gaussianBlur(fine, std::max(0.8, 0.006 * fw));
        const double eyeR = 0.075 * fw;
        const double ex[2] = {f.landmarks[0] * W - rx0, f.landmarks[2] * W - rx0}, ey[2] = {f.landmarks[1] * H - ry0, f.landmarks[3] * H - ry0};
        for (int y = 0; y < rh; ++y)
            for (int x = 0; x < rw; ++x) {
                const size_t i = size_t(y) * size_t(rw) + size_t(x);
                float* p = region.at(x, y);
                const float a = p[3];
                if (a <= 1e-6f) continue;
                const float* q = soft.at(x, y);
                const double skin = mask[i] * skinness(p[0] / a, p[1] / a, p[2] / a);
                if (s.showMask) {
                    for (int c = 0; c < 3; ++c) p[c] = float(skin * a);
                    continue;
                }
                // Smoothing: small differences from the blurred face (texture, blemishes) go; edges stay.
                const double dl = 0.2126 * (p[0] - q[0]) + 0.7152 * (p[1] - q[1]) + 0.0722 * (p[2] - q[2]);
                const double keep = std::exp(-(dl / (0.08 * a)) * (dl / (0.08 * a)));
                const double k = std::clamp(s.smooth, 0.0, 1.0) * skin * keep;
                for (int c = 0; c < 3; ++c) p[c] -= float(k * (p[c] - q[c]));
                if (s.lighten > 0) {
                    const double l = 1 + 0.5 * std::clamp(s.lighten, 0.0, 1.0) * mask[i];
                    for (int c = 0; c < 3; ++c) p[c] = float(std::min<double>(p[c] * l, a));
                }
                // Eyes: brighter and crisper within a soft circle round each, on the eye rather than the
                // skin round it (the whites and iris are not skin-coloured).
                const double eyeSkin = 1 - 0.5 * skinness(p[0] / a, p[1] / a, p[2] / a);
                for (int e = 0; e < 2; ++e) {
                    const double w = (1 - smoothstep(eyeR * 0.5, eyeR, std::hypot(x + 0.5 - ex[e], y + 0.5 - ey[e]))) * eyeSkin;
                    if (w <= 0) continue;
                    const float* r = fine.at(x, y);
                    for (int c = 0; c < 3; ++c) {
                        double v = p[c] + 1.5 * std::clamp(s.eyesSharp, 0.0, 1.0) * w * (p[c] - r[c]);
                        v *= 1 + 0.35 * std::clamp(s.eyesBright, 0.0, 1.0) * w;
                        p[c] = float(std::clamp(v, 0.0, double(a)));
                    }
                }
            }
        for (int y = 0; y < rh; ++y) std::copy_n(region.at(0, y), size_t(rw) * 4, img.at(rx0, ry0 + y));
    }
}

}  // namespace montage
