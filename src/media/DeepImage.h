// Montage — deep images: a picture whose pixels hold any number of samples, each at a depth (a point at Z, or a
// volume from Z to ZBack) with its premultiplied colour and alpha, as renderers write them into deep OpenEXR files
// (media/Exr.h readDeepExr). Deep layers are merged by depth whatever their order, held out by one another, sliced and
// graded by depth, then flattened into an ordinary picture. Samples are combined as Peter Hillman's "Interpreting
// OpenEXR Deep Pixels" (OpenEXR's documentation) describes: volumes are split where other samples begin or end (the
// alpha of a part x of a volume is 1 - (1 - alpha)^x, its colour in proportion), and samples covering the same depths
// are mixed by their absorption.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace montage {

struct DeepSample {
    float z = 0, zBack = 0;   // zBack == z: a point sample
    float r = 0, g = 0, b = 0, a = 0;  // premultiplied
};

struct DeepFrame {
    int width = 0, height = 0;
    std::vector<uint32_t> offsets;    // width * height + 1: pixel i's samples are [offsets[i], offsets[i + 1])
    std::vector<DeepSample> samples;
    bool empty() const { return width <= 0 || height <= 0; }
    size_t count(int x, int y) const { return offsets[size_t(y) * size_t(width) + size_t(x) + 1] - offsets[size_t(y) * size_t(width) + size_t(x)]; }
    const DeepSample* at(int x, int y) const { return samples.data() + offsets[size_t(y) * size_t(width) + size_t(x)]; }
    size_t bytes() const { return offsets.size() * sizeof(uint32_t) + samples.size() * sizeof(DeepSample); }
    // A frame of `width` x `height` with no samples.
    static DeepFrame blank(int width, int height);
};

// Builds a frame pixel by pixel (in order): `pixel(x, y, out)` appends that pixel's samples.
DeepFrame buildDeep(int width, int height, const std::function<void(int x, int y, std::vector<DeepSample>& out)>& pixel);

// A pixel's samples tidied: volumes split wherever another sample starts or ends within them, samples covering the
// same depths mixed into one, in order front to back (by Z, then ZBack). Samples with no alpha and no colour go.
void tidyDeepPixel(std::vector<DeepSample>& samples);
// Every pixel of a frame tidied.
void tidyDeep(DeepFrame& f);

// Deep frames of one size merged into one, tidied: the same whatever their order.
DeepFrame mergeDeep(const std::vector<const DeepFrame*>& layers);

// The frame flattened front to back ("over"), premultiplied RGBA, width * height * 4. Samples are taken in order of
// Z then ZBack (tidy first for overlapping volumes).
std::vector<float> flattenDeep(const DeepFrame& f);

// `f` held out by `holdout`: each of its samples dimmed by what of `holdout` is in front of it, and within volumes
// shared with it by its share of their mix, as if `holdout` were merged with it and then taken away (what is left,
// flattened, is `f`'s part of the merge's flattened picture). Both the same size; `f` comes back tidied.
void holdOutDeep(DeepFrame& f, const DeepFrame& holdout);

// What lies from `near` to `far` (volumes cut at them), the rest taken away.
void sliceDeep(DeepFrame& f, float near, float far);

// `grade(rgb, weight)` applied to each sample's premultiplied colour within `near` to `far`, the weight falling to 0
// over `soft` beyond them (volumes split at the range's edges first).
void gradeDeep(DeepFrame& f, float near, float far, float soft, const std::function<void(float* rgb, float weight)>& grade);

// A flat picture (premultiplied RGBA, width * height * 4) made deep: each pixel with any alpha one sample at its depth
// `depth[i]` (width * height), `thickness` deep (0: a point).
DeepFrame deepFromFlat(const float* rgba, int width, int height, const float* depth, float thickness = 0);

// Each pixel (x, y) of a `width` x `height` frame given the samples of `from`'s pixel at the source coordinates
// `source(x + 0.5, y + 0.5)` (pixels, nearest; outside it: none), scaled by `opacity`.
DeepFrame resampleDeep(const DeepFrame& from, int width, int height, const std::function<void(double x, double y, double& u, double& v)>& source,
                       float opacity = 1);

}  // namespace montage
