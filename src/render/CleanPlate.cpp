#include "CleanPlate.h"

#include <algorithm>
#include <cmath>

#include "render/ColorSpace.h"
#include "render/Ofx.h"
#include "render/Processing.h"

namespace montage {

namespace {

thread_local CleanPlateContext tPlate;

// The screen colour's lead over the other two (Screen Balance weighing the larger against the smaller), as the
// Keyer measures it.
struct Dominance {
    int dom = 1, o1 = 2, o2 = 0;
    float balance = 0.5f;
    explicit Dominance(const float key[3], float b) : balance(b) {
        dom = (key[1] >= key[0] && key[1] >= key[2]) ? 1 : (key[2] >= key[0] ? 2 : 0);
        o1 = (dom + 1) % 3, o2 = (dom + 2) % 3;
    }
    float operator()(const float* c) const {
        return c[dom] - (balance * std::max(c[o1], c[o2]) + (1 - balance) * std::min(c[o1], c[o2]));
    }
};

// `img` (in `from`) into the colours the effect running now works in (currentWorkingSpace: a scene-referred
// sequence's display values, for the Keyer).
void intoEffectColours(Image& img, const ColorSpace* from) {
    const ColorSpace& here = currentWorkingSpace();
    if (!from || from == &here || img.empty()) return;
    const SceneGrading* sg = currentSceneGrading();
    if (sg && &here == sg->display) outputTransform(img, *from, here, sg->peakNits, sg->kind, true);
    else convertExactly(img, *from, here);
}

// `src` resampled (bilinear, pixel centres matched) to w x h.
Image resized(const Image& src, int w, int h) {
    if (src.width == w && src.height == h) return src;
    Image out(w, h);
    const double sx = double(src.width) / w, sy = double(src.height) / h;
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const double fy = std::clamp((y + 0.5) * sy - 0.5, 0.0, double(src.height - 1));
            const int j0 = int(fy), j1 = std::min(j0 + 1, src.height - 1);
            const float b = float(fy - j0);
            for (int x = 0; x < w; ++x) {
                const double fx = std::clamp((x + 0.5) * sx - 0.5, 0.0, double(src.width - 1));
                const int i0 = int(fx), i1 = std::min(i0 + 1, src.width - 1);
                const float a = float(fx - i0);
                const float *p00 = src.at(i0, j0), *p10 = src.at(i1, j0), *p01 = src.at(i0, j1), *p11 = src.at(i1, j1);
                float* q = out.at(x, y);
                for (int k = 0; k < 4; ++k)
                    q[k] = (p00[k] * (1 - a) + p10[k] * a) * (1 - b) + (p01[k] * (1 - a) + p11[k] * a) * b;
            }
        }
    });
    return out;
}

// The pixels of a mask shrunk by r px (a pixel stays only when every pixel within r of it is set).
void shrinkMask(std::vector<uint8_t>& mask, int w, int h, double r) {
    if (r <= 0) return;
    std::vector<uint8_t> outside(mask.size());
    for (size_t i = 0; i < mask.size(); ++i) outside[i] = mask[i] ? 0 : 1;
    const std::vector<float> d = distanceTransform(outside, w, h);
    for (size_t i = 0; i < mask.size(); ++i) mask[i] = mask[i] && d[i] > r ? 1 : 0;
}

}  // namespace

const CleanPlateContext& currentCleanPlate() { return tPlate; }
CleanPlateScope::CleanPlateScope(CleanPlateContext context) : previous_(std::move(tPlate)) { tPlate = std::move(context); }
CleanPlateScope::~CleanPlateScope() { tPlate = std::move(previous_); }

bool fillHoles(std::vector<float>& v, int channels, const std::vector<uint8_t>& known, int w, int h, int sweeps) {
    const int C = channels;
    if (w <= 0 || h <= 0 || C <= 0 || C > 4 || v.size() != size_t(w) * size_t(h) * size_t(C) || known.size() != size_t(w) * size_t(h)) return false;
    struct Level {
        int w = 0, h = 0;
        std::vector<float> v, wt;  // values, and how much of each cell is known (0..1)
    };
    std::vector<Level> levels(1);
    levels[0].w = w, levels[0].h = h, levels[0].v = v;
    levels[0].wt.resize(known.size());
    bool any = false, all = true;
    for (size_t i = 0; i < known.size(); ++i) {
        levels[0].wt[i] = known[i] ? 1.0f : 0.0f;
        any = any || known[i], all = all && known[i];
    }
    if (!any) return false;
    if (all) return true;
    // Pull: each cell of the next level the mean of the known values in its 2 x 2 below.
    while (levels.back().w > 1 || levels.back().h > 1) {
        const Level& f = levels.back();
        Level c;
        c.w = (f.w + 1) / 2, c.h = (f.h + 1) / 2;
        c.v.assign(size_t(c.w) * size_t(c.h) * size_t(C), 0.0f);
        c.wt.assign(size_t(c.w) * size_t(c.h), 0.0f);
        parallelRows(c.h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < c.w; ++x) {
                    double sw = 0, acc[4] = {0, 0, 0, 0};
                    int n = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            const int fx = 2 * x + dx, fy = 2 * y + dy;
                            if (fx >= f.w || fy >= f.h) continue;
                            ++n;
                            const size_t i = size_t(fy) * size_t(f.w) + size_t(fx);
                            const float k = f.wt[i];
                            if (k <= 0) continue;
                            sw += k;
                            for (int ch = 0; ch < C; ++ch) acc[ch] += double(k) * f.v[i * size_t(C) + size_t(ch)];
                        }
                    const size_t o = size_t(y) * size_t(c.w) + size_t(x);
                    c.wt[o] = float(sw / std::max(1, n));
                    if (sw > 0)
                        for (int ch = 0; ch < C; ++ch) c.v[o * size_t(C) + size_t(ch)] = float(acc[ch] / sw);
                }
        });
        levels.push_back(std::move(c));
    }
    // Push: each level's cells that are not wholly known take the level above (bilinear) for what they lack, then are
    // relaxed towards their neighbours' mean (red then black, so the result does not depend on the threads).
    for (int l = int(levels.size()) - 2; l >= 0; --l) {
        Level& f = levels[size_t(l)];
        const Level& c = levels[size_t(l) + 1];
        parallelRows(f.h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                const double cy = std::clamp((y + 0.5) / 2 - 0.5, 0.0, double(c.h - 1));
                const int j0 = int(cy), j1 = std::min(j0 + 1, c.h - 1);
                const double b = cy - j0;
                for (int x = 0; x < f.w; ++x) {
                    const size_t i = size_t(y) * size_t(f.w) + size_t(x);
                    const float k = f.wt[i];
                    if (k >= 1) continue;
                    const double cx = std::clamp((x + 0.5) / 2 - 0.5, 0.0, double(c.w - 1));
                    const int i0 = int(cx), i1 = std::min(i0 + 1, c.w - 1);
                    const double a = cx - i0;
                    for (int ch = 0; ch < C; ++ch) {
                        auto at = [&](int cx2, int cy2) { return double(c.v[(size_t(cy2) * size_t(c.w) + size_t(cx2)) * size_t(C) + size_t(ch)]); };
                        const double up = (at(i0, j0) * (1 - a) + at(i1, j0) * a) * (1 - b) + (at(i0, j1) * (1 - a) + at(i1, j1) * a) * b;
                        float& out = f.v[i * size_t(C) + size_t(ch)];
                        out = float(k * out + (1 - k) * up);
                    }
                }
            }
        });
        // (fewer on the two finest levels, where what is left to settle is local and a sweep costs the most)
        const int levelSweeps = sweeps <= 0 ? 0 : l == 0 ? std::max(1, sweeps / 8) : l == 1 ? std::max(1, sweeps / 4) : sweeps;
        for (int s = 0; s < levelSweeps; ++s)
            for (int colour = 0; colour < 2; ++colour)
                parallelRows(f.h, [&](int y0, int y1) {
                    for (int y = y0; y < y1; ++y)
                        for (int x = (y + colour) & 1; x < f.w; x += 2) {
                            const size_t i = size_t(y) * size_t(f.w) + size_t(x);
                            if (f.wt[i] >= 1) continue;
                            size_t nb[4];
                            int n = 0;
                            if (x > 0) nb[n++] = i - 1;
                            if (x + 1 < f.w) nb[n++] = i + 1;
                            if (y > 0) nb[n++] = i - size_t(f.w);
                            if (y + 1 < f.h) nb[n++] = i + size_t(f.w);
                            if (n == 0) continue;
                            for (int ch = 0; ch < C; ++ch) {
                                double sum = 0;
                                for (int k = 0; k < n; ++k) sum += f.v[nb[k] * size_t(C) + size_t(ch)];
                                f.v[i * size_t(C) + size_t(ch)] = float(sum / n);
                            }
                        }
                });
    }
    v = std::move(levels[0].v);
    return true;
}

PlateSettings plateSettings(const Effect& e, FrameTime t) {
    PlateSettings s;
    s.key[0] = float(e.p("key.r", t, 0.1)), s.key[1] = float(e.p("key.g", t, 0.75)), s.key[2] = float(e.p("key.b", t, 0.2));
    s.balance = float(std::clamp(e.p("balance", t, 0.5), 0.0, 1.0));
    s.threshold = float(std::clamp(e.p("plate_threshold", t, 50), 1.0, 100.0) / 100);
    s.erode = std::max(0.0, e.p("plate_erode", t, 3));
    s.frames = std::clamp(int(std::lround(e.p("plate_frames", t, 1))), 1, 9);
    s.spacing = std::max(1, int(std::lround(e.p("plate_spacing", t, 10))));
    s.blur = std::max(0.0, e.p("plate_blur", t, 0));
    return s;
}

std::vector<uint8_t> screenPixels(const Image& img, const PlateSettings& s) {
    const Dominance dominance(s.key, s.balance);
    const float screen = std::max(1e-3f, dominance(s.key));
    std::vector<uint8_t> mask(size_t(img.width) * size_t(img.height), 0);
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                if (p[3] <= 0) continue;
                const float c[3] = {p[0] / p[3], p[1] / p[3], p[2] / p[3]};
                mask[size_t(y) * size_t(img.width) + size_t(x)] = dominance(c) >= s.threshold * screen ? 1 : 0;
            }
        }
    });
    return mask;
}

Image makeCleanPlate(const PlateSettings& s, FrameTime t, const Image& img, double pixelScale, std::vector<uint8_t>* found) {
    const int w = img.width, h = img.height;
    if (img.empty()) return {};
    const double erode = s.erode * pixelScale;
    // This frame, and the others either side of it the clip has (in the colours the effect runs in).
    std::vector<Image> others;
    if (s.frames > 1)
        if (const ofx::FrameFetch& fetch = ofx::currentFetch()) {
            const int half = (s.frames - 1) / 2, extra = s.frames - 1 - 2 * half;
            for (int k = -half; k <= half + extra; ++k) {
                if (k == 0) continue;
                Image o;
                if (!fetch(double(t + FrameTime(k) * s.spacing), o) || o.empty()) continue;
                intoEffectColours(o, currentCleanPlate().space);
                others.push_back(resized(o, w, h));
            }
        }
    std::vector<std::vector<uint8_t>> masks;
    std::vector<const Image*> frames{&img};
    for (const Image& o : others) frames.push_back(&o);
    for (const Image* f : frames) {
        masks.push_back(screenPixels(*f, s));
        shrinkMask(masks.back(), w, h, erode);
    }
    if (found) *found = masks.front();
    std::vector<float> v(size_t(w) * size_t(h) * 3, 0.0f);
    std::vector<uint8_t> known(size_t(w) * size_t(h), 0);
    parallelRows(h, [&](int y0, int y1) {
        std::vector<float> samples;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                for (int ch = 0; ch < 3; ++ch) {
                    samples.clear();
                    for (size_t f = 0; f < frames.size(); ++f)
                        if (masks[f][i]) {
                            const float* p = frames[f]->at(x, y);
                            samples.push_back(p[ch] / p[3]);
                        }
                    if (samples.empty()) break;
                    // (the median; of an even count, the mean of the middle two)
                    const size_t m = samples.size() / 2;
                    std::nth_element(samples.begin(), samples.begin() + std::ptrdiff_t(m), samples.end());
                    float value = samples[m];
                    if (samples.size() % 2 == 0) value = 0.5f * (value + *std::max_element(samples.begin(), samples.begin() + std::ptrdiff_t(m)));
                    v[i * 3 + size_t(ch)] = value;
                    known[i] = 1;
                }
            }
    });
    if (!fillHoles(v, 3, known, w, h, 40)) return {};
    Image plate(w, h);
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        for (int ch = 0; ch < 3; ++ch) plate.px[i * 4 + size_t(ch)] = v[i * 3 + size_t(ch)];
        plate.px[i * 4 + 3] = 1;
    }
    if (s.blur * pixelScale > 0) gaussianBlur(plate, s.blur * pixelScale);
    return plate;
}

void cleanPlate(const Effect& e, FrameTime t, Image& img, double pixelScale) {
    std::vector<uint8_t> found;
    Image plate = makeCleanPlate(plateSettings(e, t), t, img, pixelScale, &found);
    if (std::lround(e.p("view", t, 0)) == 1) {  // the screen found, white
        for (size_t i = 0; i < size_t(img.width) * size_t(img.height); ++i) {
            const float v = i < found.size() && found[i] ? 1.0f : 0.0f;
            img.px[i * 4] = img.px[i * 4 + 1] = img.px[i * 4 + 2] = v;
            img.px[i * 4 + 3] = 1;
        }
        return;
    }
    if (!plate.empty()) img = std::move(plate);  // (no screen: the picture as it was)
}

Image keyerPlate(const Effect& e, FrameTime t, const Image& img, double pixelScale) {
    const int mode = int(std::lround(e.p("clean_plate", t, 0)));
    if (mode == 1) return makeCleanPlate(plateSettings(e, t), t, img, pixelScale);
    if (mode != 2) return {};
    const CleanPlateContext& ctx = currentCleanPlate();
    if (!ctx.plate || ctx.plate->empty()) return {};
    Image plate = resized(*ctx.plate, img.width, img.height);
    intoEffectColours(plate, ctx.space);
    const double blur = std::max(0.0, e.p("plate_blur", t, 0)) * pixelScale;
    if (blur > 0) gaussianBlur(plate, blur);
    return plate;
}

void edgeExtend(const Effect& e, FrameTime t, Image& img) {
    const float amount = float(std::clamp(e.p("amount", t, 100), 0.0, 100.0) / 100);
    const float solid = float(std::clamp(e.p("solid", t, 95), 1.0, 100.0) / 100);
    const int w = img.width, h = img.height;
    if (amount <= 0 || img.empty()) return;
    // The solid foreground's colours, pushed out over everything else.
    std::vector<float> v(size_t(w) * size_t(h) * 3, 0.0f);
    std::vector<uint8_t> known(size_t(w) * size_t(h), 0);
    bool edges = false;
    for (size_t i = 0; i < known.size(); ++i) {
        const float a = img.px[i * 4 + 3];
        if (a >= solid) {
            known[i] = 1;
            for (int ch = 0; ch < 3; ++ch) v[i * 3 + size_t(ch)] = img.px[i * 4 + size_t(ch)] / a;
        } else if (a > 0) {
            edges = true;
        }
    }
    if (!edges || !fillHoles(v, 3, known, w, h, 2)) return;
    parallelRows(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                float* p = &img.px[i * 4];
                if (known[i] || p[3] <= 0) continue;
                for (int ch = 0; ch < 3; ++ch) p[ch] += (v[i * 3 + size_t(ch)] * p[3] - p[ch]) * amount;
            }
    });
}

}  // namespace montage
