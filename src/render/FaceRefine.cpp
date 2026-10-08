#include "FaceRefine.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Deconvolve.h"
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

// Grey-level dilation (max) or erosion (min) of a plane over a (2r+1)-square, separably.
void rankFilter(std::vector<float>& plane, int w, int h, int r, bool takeMax) {
    std::vector<float> tmp(plane.size());
    auto pick = [takeMax](float a, float b) { return takeMax ? std::max(a, b) : std::min(a, b); };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float v = plane[size_t(y) * size_t(w) + size_t(x)];
            for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k) v = pick(v, plane[size_t(y) * size_t(w) + size_t(k)]);
            tmp[size_t(y) * size_t(w) + size_t(x)] = v;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float v = tmp[size_t(y) * size_t(w) + size_t(x)];
            for (int k = std::max(0, y - r); k <= std::min(h - 1, y + r); ++k) v = pick(v, tmp[size_t(k) * size_t(w) + size_t(x)]);
            plane[size_t(y) * size_t(w) + size_t(x)] = v;
        }
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

int removeBlemishes(Image& img, const std::vector<FaceBox>& faces, const BlemishSettings& s) {
    const int W = img.width, H = img.height;
    int healed = 0;
    for (const FaceBox& f : faces) {
        const double fw = f.w * W;
        if (fw < 24) continue;  // too small to have spots worth finding
        const int rx0 = std::max(0, int((f.x - f.w * 0.1) * W)), ry0 = std::max(0, int((f.y - f.h * 0.1) * H));
        const int rx1 = std::min(W, int((f.x + f.w * 1.1) * W) + 1), ry1 = std::min(H, int((f.y + f.h * 1.15) * H) + 1);
        const int rw = rx1 - rx0, rh = ry1 - ry0;
        if (rw < 8 || rh < 8) continue;
        const size_t n = size_t(rw) * size_t(rh);
        FaceBox local = f;
        local.x = float((f.x * W - rx0) / rw), local.y = float((f.y * H - ry0) / rh), local.w = float(f.w * W / rw), local.h = float(f.h * H / rh);
        for (int k = 0; k < 5; ++k) {
            local.landmarks[2 * k] = float((f.landmarks[2 * k] * W - rx0) / rw);
            local.landmarks[2 * k + 1] = float((f.landmarks[2 * k + 1] * H - ry0) / rh);
        }
        const std::vector<float> mask = faceSkinMask(local, rw, rh);
        // Brightness and redness, each against the skin round it: a spot is darker or redder than its surroundings.
        std::vector<float> lum(n), red(n);
        for (int y = 0; y < rh; ++y)
            for (int x = 0; x < rw; ++x) {
                const float* p = img.at(rx0 + x, ry0 + y);
                const float a = std::max(p[3], 1e-6f);
                const size_t i = size_t(y) * size_t(rw) + size_t(x);
                lum[i] = (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / a;
                red[i] = (p[0] - 0.5f * (p[1] + p[2])) / a;
            }
        // Blobs: at the scale of a spot, the brightness curves up in every direction round a dark mark
        // (and redness down round a red one), so both curvatures (the Hessian's eigenvalues) are large.
        // A crease or the edge of a shadow curves one way only, and pores are too faint.
        const double maxSide = std::max(2.0, std::clamp(s.maxSize, 0.005, 0.3) * fw);
        const double sigma = std::max(1.0, maxSide / 3.5);
        gaussianPlane(lum, rw, rh, sigma);
        gaussianPlane(red, rw, rh, sigma);
        // The smaller curvature (scale-normalised), the larger one, and whether it is round enough.
        auto blob = [&](const std::vector<float>& v, int x, int y, double sign, double& weak, double& strong) {
            auto at = [&](int xx, int yy) { return double(v[size_t(std::clamp(yy, 0, rh - 1)) * size_t(rw) + size_t(std::clamp(xx, 0, rw - 1))]); };
            const double c = at(x, y);
            const double hxx = sign * (at(x - 1, y) - 2 * c + at(x + 1, y)), hyy = sign * (at(x, y - 1) - 2 * c + at(x, y + 1));
            const double hxy = sign * (at(x + 1, y + 1) - at(x - 1, y + 1) - at(x + 1, y - 1) + at(x - 1, y - 1)) / 4;
            const double tr = hxx + hyy, disc = std::sqrt(std::max(0.0, tr * tr / 4 - (hxx * hyy - hxy * hxy)));
            weak = (tr / 2 - disc) * sigma * sigma, strong = (tr / 2 + disc) * sigma * sigma;
        };
        // The colour round each pixel must be skin (not the hairline or a brow), and the eyes' corners
        // and the nostrils are features, not marks.
        std::vector<float> around[3];
        for (int c = 0; c < 3; ++c) {
            around[c].resize(n);
            for (int y = 0; y < rh; ++y)
                for (int x = 0; x < rw; ++x) {
                    const float* p = img.at(rx0 + x, ry0 + y);
                    around[c][size_t(y) * size_t(rw) + size_t(x)] = p[c] / std::max(p[3], 1e-6f);
                }
            gaussianPlane(around[c], rw, rh, 2.5 * sigma);
        }
        const double ex[2] = {local.landmarks[0] * rw, local.landmarks[2] * rw}, ey[2] = {local.landmarks[1] * rh, local.landmarks[3] * rh};
        const double nx = local.landmarks[4] * rw, ny = local.landmarks[5] * rh;
        auto feature = [&](int x, int y) {
            for (int e = 0; e < 2; ++e)
                if (std::hypot(x + 0.5 - ex[e], y + 0.5 - ey[e]) < 0.18 * fw) return true;
            // The nostrils sit beside and just below the tip of the nose; the hairline along the top.
            const double dx = (x + 0.5 - nx) / (0.15 * fw), dy = (y + 0.5 - ny - 0.035 * fw) / (0.065 * fw);
            return dx * dx + dy * dy < 1 || y + 0.5 < (local.y + 0.08 * local.h) * rh;
        };
        const double sens = std::clamp(s.sensitivity, 0.0, 1.0);
        const double darkMin = 0.05 - 0.03 * sens, redMin = 0.025 - 0.015 * sens;
        std::vector<uint8_t> spot(n, 0);
        for (int y = 0; y < rh; ++y)
            for (int x = 0; x < rw; ++x) {
                const size_t i = size_t(y) * size_t(rw) + size_t(x);
                if (mask[i] < 0.5f || feature(x, y) || skinness(around[0][i], around[1][i], around[2][i]) < 0.75) continue;
                double weak, strong;
                blob(lum, x, y, 1.0, weak, strong);
                const bool dark = weak / std::max(0.05f, lum[i]) > darkMin && weak > 0.4 * strong;
                blob(red, x, y, -1.0, weak, strong);
                const bool redder = weak > redMin && weak > 0.4 * strong;
                spot[i] = dark || redder;
            }
        // Only small, roughly round marks: eyebrows, hairlines and shadows are larger or long.
        std::vector<int> label(n, 0);
        std::vector<uint8_t> keep(n, 0);
        std::vector<int> stack;
        int next = 0;
        for (size_t start = 0; start < n; ++start) {
            if (!spot[start] || label[start]) continue;
            ++next;
            std::vector<size_t> members;
            int bx0 = rw, by0 = rh, bx1 = -1, by1 = -1;
            stack.assign(1, int(start));
            label[start] = next;
            while (!stack.empty()) {
                const size_t i = size_t(stack.back());
                stack.pop_back();
                members.push_back(i);
                const int x = int(i % size_t(rw)), y = int(i / size_t(rw));
                bx0 = std::min(bx0, x), bx1 = std::max(bx1, x), by0 = std::min(by0, y), by1 = std::max(by1, y);
                const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& q : nb) {
                    if (q[0] < 0 || q[1] < 0 || q[0] >= rw || q[1] >= rh) continue;
                    const size_t j = size_t(q[1]) * size_t(rw) + size_t(q[0]);
                    if (spot[j] && !label[j]) label[j] = next, stack.push_back(int(j));
                }
            }
            const int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1;
            if (bw > maxSide || bh > maxSide || std::max(bw, bh) > 3 * std::min(bw, bh) + 2) continue;
            for (size_t i : members) keep[i] = 1;
            ++healed;
        }
        if (!std::any_of(keep.begin(), keep.end(), [](uint8_t v) { return v != 0; })) continue;
        // The healed area: each core grown over the spot round it (its fainter edge), with a feathered border.
        std::vector<float> area(n);
        for (size_t i = 0; i < n; ++i) area[i] = keep[i] ? 1.0f : 0.0f;
        gaussianPlane(area, rw, rh, std::max(1.0, 0.012 * fw));
        std::vector<float> hard(n), soft(n);
        for (size_t i = 0; i < n; ++i) {
            hard[i] = area[i] > 0.02f ? 1.0f : 0.0f;
            soft[i] = std::min(1.0f, area[i] * 6.0f);
        }
        // Each healed pixel filled from the skin round it (a blur that only counts pixels outside the spots).
        const double reach = std::max(2.0, 0.025 * fw);
        std::vector<float> weight(n), chan[4];
        for (size_t i = 0; i < n; ++i) weight[i] = 1 - hard[i];
        for (int c = 0; c < 4; ++c) {
            chan[c].resize(n);
            for (int y = 0; y < rh; ++y)
                for (int x = 0; x < rw; ++x) {
                    const size_t i = size_t(y) * size_t(rw) + size_t(x);
                    chan[c][i] = img.at(rx0 + x, ry0 + y)[c] * weight[i];
                }
            gaussianPlane(chan[c], rw, rh, reach);
        }
        gaussianPlane(weight, rw, rh, reach);
        const float amount = float(std::clamp(s.amount, 0.0, 1.0));
        for (int y = 0; y < rh; ++y)
            for (int x = 0; x < rw; ++x) {
                const size_t i = size_t(y) * size_t(rw) + size_t(x);
                if (soft[i] <= 0) continue;
                float* p = img.at(rx0 + x, ry0 + y);
                if (s.showSpots) {
                    p[0] = std::max(p[0], soft[i] * p[3]);
                    continue;
                }
                if (weight[i] < 1e-3f) continue;
                const float k = amount * soft[i];
                for (int c = 0; c < 3; ++c) p[c] += k * (chan[c][i] / weight[i] - p[c]);
            }
    }
    return healed;
}

}  // namespace montage
