#include "FocusCheck.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <thread>

#include "ColorSpace.h"
#include "Compositor.h"
#include "core/ColorGroups.h"
#include "core/History.h"
#include "media/MediaPool.h"

namespace montage {

namespace {

constexpr double kSigmaFine = 0.6, kSigmaMid = 1.5, kSigmaBroad = 4.0;  // the sharpness's bands
constexpr double kSigmaSmear[3] = {1.0, 2.5, 6.0};                        // the motion blur's
constexpr double kFlatRms = 0.003;  // a tile whose broad band is quieter than this has nothing to judge (under a code value)

int threadCount(int threads) {
    return threads > 0 ? threads : int(std::max(1u, std::thread::hardware_concurrency()));
}

// fn(first, last) over [0, n) in up to `threads` bands of rows. Each row's result depends on its inputs alone, so the
// split never changes what is computed.
template <class Fn>
void inBands(int n, int threads, Fn&& fn) {
    const int bands = std::clamp(threadCount(threads), 1, std::max(1, n / 8));
    if (bands <= 1) {
        fn(0, n);
        return;
    }
    std::vector<std::thread> pool;
    pool.reserve(size_t(bands));
    for (int b = 0; b < bands; ++b) pool.emplace_back([&, b] { fn(int(int64_t(n) * b / bands), int(int64_t(n) * (b + 1) / bands)); });
    for (std::thread& t : pool) t.join();
}

std::vector<float> kernel(double sigma) {
    const int r = int(std::ceil(3 * sigma));
    std::vector<float> k(size_t(2 * r + 1));
    double sum = 0;
    for (int i = -r; i <= r; ++i) sum += k[size_t(i + r)] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
    for (float& v : k) v = float(v / sum);
    return k;
}

// Each plane of `out` is `luma` through the Gaussian of the same index (edges repeated).
void blurs(const float* luma, int w, int h, const std::vector<std::vector<float>>& kernels, std::vector<std::vector<float>>& out,
           int threads) {
    const size_t n = size_t(w) * size_t(h);
    std::vector<std::vector<float>> across(kernels.size(), std::vector<float>(n));
    out.assign(kernels.size(), std::vector<float>(n));
    int widest = 0;
    for (const std::vector<float>& kk : kernels) widest = std::max(widest, int(kk.size() / 2));
    inBands(h, threads, [&](int y0, int y1) {
        // Each row with its ends repeated, so the taps need no bounds.
        std::vector<float> padded(size_t(w + 2 * widest));
        for (int y = y0; y < y1; ++y) {
            const float* src = luma + size_t(y) * size_t(w);
            for (int x = -widest; x < w + widest; ++x) padded[size_t(x + widest)] = src[std::clamp(x, 0, w - 1)];
            for (size_t k = 0; k < kernels.size(); ++k) {
                const std::vector<float>& kk = kernels[k];
                const int r = int(kk.size() / 2);
                const float* row = padded.data() + widest - r;
                float* dst = across[k].data() + size_t(y) * size_t(w);
                for (int x = 0; x < w; ++x) {
                    float acc = 0;
                    for (int i = 0; i <= 2 * r; ++i) acc += kk[size_t(i)] * row[x + i];
                    dst[x] = acc;
                }
            }
        }
    });
    inBands(h, threads, [&](int y0, int y1) {
        for (size_t k = 0; k < kernels.size(); ++k) {
            const std::vector<float>& kk = kernels[k];
            const int r = int(kk.size() / 2);
            for (int y = y0; y < y1; ++y) {
                float* dst = out[k].data() + size_t(y) * size_t(w);
                for (int x = 0; x < w; ++x) dst[x] = 0;
                for (int i = -r; i <= r; ++i) {
                    const float c = kk[size_t(i + r)];
                    const float* src = across[k].data() + size_t(std::clamp(y + i, 0, h - 1)) * size_t(w);
                    for (int x = 0; x < w; ++x) dst[x] += c * src[x];
                }
            }
        }
    });
}

struct TileSums {
    double fine = 0, broad = 0;           // energies of the bands
    double f[3] = {0, 0, 0}, b[3] = {0, 0, 0};  // structure tensors (xx, xy, yy) of the smear's two bands' gradients
    int64_t count = 0;
    double noise = 0;                     // its noise's standard deviation, as estimated
};

// What white noise of unit variance leaves in a band (the difference of two Gaussians, each separable), per pixel.
double noiseGain(const std::vector<float>& a, const std::vector<float>& b) {
    double aa = 0, bb = 0, ab = 0;
    const int ra = int(a.size() / 2), rb = int(b.size() / 2);
    for (float v : a) aa += double(v) * v;
    for (float v : b) bb += double(v) * v;
    for (int i = -std::min(ra, rb); i <= std::min(ra, rb); ++i) ab += double(a[size_t(i + ra)]) * b[size_t(i + rb)];
    return aa * aa - 2 * ab * ab + bb * bb;
}

std::string percent(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f %%", v * 100);
    return b;
}

std::string number(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

std::string direction(double angle) {
    const double a = std::fmod(std::fmod(angle, 180.0) + 180.0, 180.0);
    if (a < 22.5 || a >= 157.5) return "across the picture";
    if (a >= 67.5 && a < 112.5) return "up and down";
    return "diagonally (" + number("%.0f°", a) + ")";
}

// The value at `share` (0..1) through the sorted values.
double quantile(std::vector<double> v, double share) {
    if (v.empty()) return 0;
    const size_t at = size_t(std::floor(std::clamp(share, 0.0, 1.0) * double(v.size() - 1) + 1e-9));
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(at), v.end());
    return v[at];
}

// Runs of `on` at least `minLength` long, gaps of one frame bridged: [a, b) indexes.
std::vector<std::pair<int, int>> runs(const std::vector<char>& on, int minLength) {
    std::vector<std::pair<int, int>> out;
    const int n = int(on.size());
    for (int i = 0; i < n;) {
        if (!on[size_t(i)]) {
            ++i;
            continue;
        }
        int j = i + 1;
        while (j < n && (on[size_t(j)] || (j + 1 < n && on[size_t(j + 1)]))) ++j;
        while (j > i && !on[size_t(j - 1)]) --j;
        if (j - i >= minLength) out.push_back({i, j});
        i = j;
    }
    return out;
}

}  // namespace

std::vector<float> lumaOf(const Image& img) {
    std::vector<float> y(size_t(std::max(0, img.width)) * size_t(std::max(0, img.height)));
    for (size_t k = 0; k < y.size(); ++k) {
        const float* px = &img.px[k * 4];
        y[k] = std::clamp(0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2], 0.0f, 1.0f);
    }
    return y;
}

FocusMeasure measureFocus(const float* luma, int w, int h, int threads) {
    FocusMeasure m;
    if (!luma || w < 8 || h < 8) return m;
    static const std::vector<std::vector<float>> kernels{kernel(kSigmaFine),     kernel(kSigmaMid),      kernel(kSigmaBroad),
                                                         kernel(kSigmaSmear[0]), kernel(kSigmaSmear[1]), kernel(kSigmaSmear[2])};
    std::vector<std::vector<float>> g;
    blurs(luma, w, h, kernels, g, threads);
    const float *g1 = g[0].data(), *g2 = g[1].data(), *g3 = g[2].data(), *s1 = g[3].data(), *s2 = g[4].data(), *s3 = g[5].data();
    // Tiles over the picture less a pixel's margin (for the gradients), each summed by one thread in a set order.
    const int margin = 1;
    const int tile = std::clamp(std::min(w, h) / 10, 16, 64);
    const int iw = w - 2 * margin, ih = h - 2 * margin;
    const int nx = std::max(1, iw / tile), ny = std::max(1, ih / tile);
    std::vector<TileSums> tiles(size_t(nx) * size_t(ny));
    inBands(ny, threads, [&](int t0, int t1) {
        for (int ty = t0; ty < t1; ++ty)
            for (int tx = 0; tx < nx; ++tx) {
                TileSums& s = tiles[size_t(ty) * size_t(nx) + size_t(tx)];
                const int x0 = margin + int(int64_t(iw) * tx / nx), x1 = margin + int(int64_t(iw) * (tx + 1) / nx);
                const int y0 = margin + int(int64_t(ih) * ty / ny), y1 = margin + int(int64_t(ih) * (ty + 1) / ny);
                for (int y = y0; y < y1; ++y) {
                    const size_t row = size_t(y) * size_t(w);
                    for (int x = x0; x < x1; ++x) {
                        const size_t i = row + size_t(x);
                        const double fine = double(g1[i]) - g2[i], broad = double(g1[i]) - g3[i];
                        s.fine += fine * fine;
                        s.broad += broad * broad;
                        // Central differences of each band.
                        const size_t l = i - 1, r = i + 1, u = i - size_t(w), d = i + size_t(w);
                        const double fx = 0.5 * ((double(s1[r]) - s2[r]) - (double(s1[l]) - s2[l]));
                        const double fy = 0.5 * ((double(s1[d]) - s2[d]) - (double(s1[u]) - s2[u]));
                        const double bx = 0.5 * ((double(s2[r]) - s3[r]) - (double(s2[l]) - s3[l]));
                        const double by = 0.5 * ((double(s2[d]) - s3[d]) - (double(s2[u]) - s3[u]));
                        s.f[0] += fx * fx, s.f[1] += fx * fy, s.f[2] += fy * fy;
                        s.b[0] += bx * bx, s.b[1] += bx * by, s.b[2] += by * by;
                    }
                }
                s.count = int64_t(x1 - x0) * int64_t(y1 - y0);
                // The noise (grain, a sensor's at high gain): the median of a mask that leaves smooth shading and most
                // edges out (Immerkær's), robust to what edges remain.
                std::vector<float> r;
                r.reserve(size_t(s.count));
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x) {
                        const float* c = luma + size_t(y) * size_t(w) + size_t(x);
                        const float* up = c - w;
                        const float* dn = c + w;
                        r.push_back(std::fabs(up[-1] - 2 * up[0] + up[1] - 2 * (c[-1] - 2 * c[0] + c[1]) + dn[-1] - 2 * dn[0] + dn[1]));
                    }
                if (!r.empty()) {
                    std::nth_element(r.begin(), r.begin() + std::ptrdiff_t(r.size() / 2), r.end());
                    s.noise = 1.4826 * double(r[r.size() / 2]) / 6;
                }
            }
    });
    // The picture's noise (the median tile's), and what it leaves in each band: taken off, so grain that is there
    // whatever the focus does not read as detail (a dark, grainy shot's soft frame scored over its sharp one).
    {
        std::vector<double> noise;
        for (const TileSums& s : tiles)
            if (s.count > 0) noise.push_back(s.noise);
        double sigma = 0;
        if (!noise.empty()) {
            std::nth_element(noise.begin(), noise.begin() + std::ptrdiff_t(noise.size() / 2), noise.end());
            sigma = noise[noise.size() / 2];
        }
        static const double fineGain = noiseGain(kernels[0], kernels[1]), broadGain = noiseGain(kernels[0], kernels[2]);
        const double v = sigma * sigma;
        for (TileSums& s : tiles) {
            s.fine = std::max(0.0, s.fine - double(s.count) * v * fineGain);
            s.broad = std::max(0.0, s.broad - double(s.count) * v * broadGain);
        }
    }
    // The busiest tiles: a quarter of them (at least four), of those with anything to judge.
    std::vector<size_t> order;
    for (size_t i = 0; i < tiles.size(); ++i)
        if (tiles[i].count > 0 && std::sqrt(tiles[i].broad / double(tiles[i].count)) >= kFlatRms) order.push_back(i);
    if (order.size() < std::min<size_t>(2, tiles.size())) return m;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return tiles[a].broad / double(tiles[a].count) > tiles[b].broad / double(tiles[b].count); });
    const size_t take = std::min(order.size(), std::max<size_t>(4, (tiles.size() + 3) / 4));
    TileSums sum;
    for (size_t k = 0; k < take; ++k) {
        const TileSums& t = tiles[order[k]];
        sum.fine += t.fine, sum.broad += t.broad, sum.count += t.count;
        for (int c = 0; c < 3; ++c) sum.f[c] += t.f[c], sum.b[c] += t.b[c];
    }
    if (sum.broad <= 0) return m;
    m.textured = true;
    m.sharpness = sum.fine / sum.broad;
    m.contrast = std::sqrt(sum.broad / double(sum.count));
    // The generalised eigenvalues of the finer smear band's tensor F against the coarser one's B (F v = λ B v): how much
    // of the detail each way survives, whatever way the picture's own edges run. Where the coarser band itself runs
    // nearly one way only (a picture of stripes), there is no telling.
    const double *F = sum.f, *B = sum.b;
    const double trB = B[0] + B[2], detB = B[0] * B[2] - B[1] * B[1];
    const double discB = std::sqrt(std::max(0.0, 0.25 * trB * trB - detB));
    const double bMin = 0.5 * trB - discB, bMax = 0.5 * trB + discB;
    if (bMax <= 0 || bMin < 0.05 * bMax) return m;
    const double qa = detB, qb = F[0] * B[2] + F[2] * B[0] - 2 * F[1] * B[1], qc = F[0] * F[2] - F[1] * F[1];
    if (qb <= 0) return m;
    const double disc = std::sqrt(std::max(0.0, qb * qb - 4 * qa * qc));
    m.anisotropy = std::clamp(disc / qb, 0.0, 1.0);
    // The way the most was lost: the eigenvector of the smaller eigenvalue (at right angles to either row of F - λB,
    // the longer one taken), a gradient's way, so a smear across the picture loses the gradients across it.
    const double lMin = (qb - disc) / (2 * qa);
    const double r1x = F[1] - lMin * B[1], r1y = -(F[0] - lMin * B[0]);
    const double r2x = F[2] - lMin * B[2], r2y = -(F[1] - lMin * B[1]);
    const bool first = r1x * r1x + r1y * r1y >= r2x * r2x + r2y * r2y;
    const double vx = first ? r1x : r2x, vy = first ? r1y : r2y;
    const double angle = std::atan2(-vy, vx) * 180 / M_PI;  // (y runs down the picture)
    m.blurAngle = std::fmod(std::fmod(angle, 180.0) + 180.0, 180.0);
    return m;
}

std::vector<uint8_t> focusPeaking(const float* luma, int w, int h, double threshold, int threads) {
    std::vector<uint8_t> mask(size_t(std::max(0, w)) * size_t(std::max(0, h)), 0);
    if (!luma || w < 2 || h < 2) return mask;
    static const std::vector<std::vector<float>> kernels{kernel(kSigmaFine), kernel(kSigmaMid)};
    std::vector<std::vector<float>> g;
    blurs(luma, w, h, kernels, g, threads);
    const float t = float(threshold);
    for (size_t i = 0; i < mask.size(); ++i) mask[i] = std::fabs(g[0][i] - g[1][i]) >= t;
    return mask;
}

double peakingThreshold(int sensitivity) {
    switch (std::clamp(sensitivity, 0, 2)) {
        case 0: return 0.07;
        case 1: return 0.05;
        default: return 0.035;  // (an edge of half the range blurred by σ 3 px reaches about 0.02)
    }
}

std::vector<FocusIssue> judgeFocus(std::vector<FocusShot>& shots, const FocusOptions& o, double fps) {
    std::vector<FocusIssue> issues;
    const int minFrames = std::max(1, int(std::ceil(o.minSeconds * std::max(1.0, fps) - 1e-9)));
    std::vector<std::vector<char>> motion(shots.size());
    for (size_t si = 0; si < shots.size(); ++si) {
        FocusShot& shot = shots[si];
        const size_t n = shot.frames.size();
        motion[si].assign(n, 0);
        std::vector<double> sharp;
        for (size_t i = 0; i < n; ++i) {
            const FocusMeasure& f = shot.frames[i];
            if (!f.textured) continue;
            if (f.anisotropy >= o.motionAnisotropy) motion[si][i] = 1;
            else sharp.push_back(f.sharpness);
        }
        shot.best = quantile(sharp, 0.9);
        shot.typical = quantile(sharp, 0.5);
        // Stretches well under the shot's own best.
        std::vector<char> soft(n, 0);
        for (size_t i = 0; i < n; ++i) {
            const FocusMeasure& f = shot.frames[i];
            soft[i] = f.textured && !motion[si][i] && shot.best > 0 && f.sharpness < o.softShare * shot.best;
        }
        for (const auto& [a, b] : runs(soft, minFrames)) {
            std::vector<double> v;
            for (int i = a; i < b; ++i)
                if (soft[size_t(i)]) v.push_back(shot.frames[size_t(i)].sharpness);
            FocusIssue is;
            is.kind = FocusFinding::Soft;
            is.start = shot.start + a, is.end = shot.start + b, is.shot = si;
            is.share = quantile(v, 0.5) / shot.best;
            is.text = "Soft focus: " + percent(is.share) + " as sharp as the shot's best";
            issues.push_back(is);
        }
        for (const auto& [a, b] : runs(motion[si], minFrames)) {
            std::vector<double> an;
            double c = 0, s = 0;
            for (int i = a; i < b; ++i) {
                if (!motion[si][size_t(i)]) continue;
                const FocusMeasure& f = shot.frames[size_t(i)];
                an.push_back(f.anisotropy);
                c += std::cos(f.blurAngle * M_PI / 90), s += std::sin(f.blurAngle * M_PI / 90);
            }
            FocusIssue is;
            is.kind = FocusFinding::MotionBlur;
            is.start = shot.start + a, is.end = shot.start + b, is.shot = si;
            is.anisotropy = quantile(an, 0.5);
            is.angle = std::fmod(std::atan2(s, c) * 90 / M_PI + 180, 180);
            is.text = "Motion blur " + direction(is.angle) + " (detail lost one way: anisotropy " + number("%.2f", is.anisotropy) +
                      "), not focus";
            issues.push_back(is);
        }
    }
    // Shots well under the other angles of their scene: each angle at its best shot, set against the median of the others.
    std::map<std::string, std::map<std::string, double>> scenes;
    for (size_t si = 0; si < shots.size(); ++si) {
        const FocusShot& shot = shots[si];
        if (shot.scene.empty() || shot.typical <= 0) continue;
        double& best = scenes[shot.scene][shot.angle];
        best = std::max(best, shot.typical);
    }
    for (size_t si = 0; si < shots.size(); ++si) {
        const FocusShot& shot = shots[si];
        if (shot.scene.empty() || shot.typical <= 0) continue;
        const size_t moving = size_t(std::count(motion[si].begin(), motion[si].end(), 1));
        if (2 * moving > shot.frames.size()) continue;  // (mostly motion blur, reported as that)
        std::vector<double> others;
        for (const auto& [angle, typical] : scenes[shot.scene])
            if (angle != shot.angle) others.push_back(typical);
        if (others.empty()) continue;
        const double other = quantile(others, 0.5);
        if (other <= 0 || shot.typical >= o.angleShare * other) continue;
        FocusIssue is;
        is.kind = FocusFinding::SoftAngle;
        is.start = shot.start, is.end = shot.end, is.shot = si;
        is.share = shot.typical / other;
        is.text = "Soft focus: the shot is " + percent(is.share) + " as sharp as the other angles of " +
                  (shot.sceneName.empty() ? std::string("its scene") : "scene " + shot.sceneName) +
                  (shot.angleName.empty() ? std::string() : " (it is " + shot.angleName + ")");
        issues.push_back(is);
    }
    std::stable_sort(issues.begin(), issues.end(), [](const FocusIssue& a, const FocusIssue& b) {
        return a.start != b.start ? a.start < b.start : int(a.kind) < int(b.kind);
    });
    return issues;
}

namespace {

// The picture at t: the topmost enabled clip with media on a track that is on; a title, matte or other generator only
// where no media is under it; adjustment layers never (as a reconform reads a cut).
const Clip* topClip(const Sequence& s, FrameTime t) {
    const Clip* graphic = nullptr;
    for (size_t i = s.videoTracks.size(); i-- > 0;) {
        const Track& tr = s.videoTracks[i];
        if (tr.muted) continue;
        for (const Clip& c : tr.clips) {
            if (!c.enabled || !c.contains(t)) continue;
            if (!c.isGenerator()) return &c;
            if (c.generator.type != "adjustment" && !graphic) graphic = &c;
        }
    }
    return graphic;
}

// A media item's scene and angle from its metadata: its scene, and its camera, else its shot, else itself.
void mediaSceneAndAngle(const MediaItem& m, FocusShot& shot) {
    auto meta = [&](const char* key) {
        const auto it = m.metadata.find(key);
        return it == m.metadata.end() ? std::string() : it->second;
    };
    shot.scene = meta("scene");
    if (shot.scene.empty()) return;
    shot.sceneName = shot.scene;
    const std::string camera = meta("camera"), angle = meta("shot");
    shot.angle = !camera.empty() ? "camera:" + camera : !angle.empty() ? "shot:" + angle : "media:" + std::to_string(m.id);
    shot.angleName = !camera.empty() ? "camera " + camera : !angle.empty() ? "shot " + angle : m.name;
}

void sceneAndAngle(const Project& p, const Sequence& s, const Clip& c, FocusShot& shot) {
    for (const ColorGroup* g : colorGroupsOf(s, c)) {
        if (g->kind == "scene") shot.scene = "group:" + std::to_string(g->id), shot.sceneName = g->name;
        else if (g->kind == "angle") shot.angle = "group:" + std::to_string(g->id), shot.angleName = g->name;
    }
    if (!shot.scene.empty() && !shot.angle.empty()) return;
    shot.scene.clear(), shot.angle.clear(), shot.sceneName.clear(), shot.angleName.clear();
    if (const MediaItem* m = c.mediaId ? p.findMedia(c.mediaId) : nullptr) mediaSceneAndAngle(*m, shot);
}

}  // namespace

std::vector<FocusIssue> checkSequenceFocus(const Project& p, const Sequence& s, FrameTime in, FrameTime out, const FocusOptions& o,
                                           const std::function<void(double)>& progress, const std::atomic<bool>* cancel,
                                           std::vector<FocusShot>* shotsOut) {
    if (out < 0) out = s.duration();
    in = std::max<FrameTime>(0, in);
    if (out <= in || s.width <= 0 || s.height <= 0) return {};
    RenderOptions ro;
    ro.scale = std::min(1.0, double(std::max(16, o.width)) / s.width);
    const ColorSpace& space = sequenceColorSpace(s);
    const bool hdr = space.hdr() || space.sceneReferred;
    std::vector<FocusShot> shots;
    const Clip* last = nullptr;
    for (FrameTime t = in; t < out; ++t) {
        if (cancel && cancel->load()) return {};
        const Clip* c = topClip(s, t);
        // A new shot where the topmost clip changes, unless it carries on its media's time (a through edit).
        bool same = !shots.empty() && c == last;
        if (!same && c && last && c->mediaId && c->mediaId == last->mediaId && last->end() == c->start && c->speed == last->speed &&
            std::fabs(last->sourceIn + double(last->duration) * last->speed - c->sourceIn) < 0.5)
            same = true;
        if (!same) {
            FocusShot shot;
            shot.start = t;
            if (c) sceneAndAngle(p, s, *c, shot);
            shots.push_back(shot);
        }
        last = c;
        Image img = renderProgramFrame(p, s, t, ro);
        if (hdr && !img.empty()) convertColor(img, space, rec709Space(), std::clamp(s.hdrPeakNits, 100.0, 10000.0));
        const std::vector<float> luma = lumaOf(img);
        shots.back().frames.push_back(measureFocus(luma.data(), img.width, img.height, o.threads));
        shots.back().end = t + 1;
        if (progress) progress(double(t - in + 1) / double(out - in));
    }
    std::vector<FocusIssue> issues = judgeFocus(shots, o, s.fpsValue());
    if (shotsOut) *shotsOut = std::move(shots);
    return issues;
}

std::vector<MediaFocus> checkMediaFocus(const Project& p, const std::vector<Id>& media, const FocusOptions& o,
                                        const std::function<void(double)>& progress, const std::atomic<bool>* cancel) {
    std::vector<MediaFocus> out;
    std::vector<FocusShot> shots;
    std::vector<const MediaItem*> items;
    double total = 0, done = 0;
    for (Id id : media)
        if (const MediaItem* m = p.findMedia(id); m && m->kind == MediaKind::Video && m->hasVideo && m->width > 0 && m->height > 0) {
            items.push_back(m);
            total += std::max(1.0, std::round(m->duration * (m->fps.valid() ? m->fps.toDouble() : 25.0)));
        }
    // Every angle of a scene is measured at one width (the narrowest of them, up to the options'), so that one is not
    // set against another for being scaled down further.
    std::map<std::string, int> sceneWidth;
    for (const MediaItem* m : items) {
        FocusShot probe;
        mediaSceneAndAngle(*m, probe);
        if (probe.scene.empty()) continue;
        const auto [it, fresh] = sceneWidth.try_emplace(probe.scene, m->width);
        if (!fresh) it->second = std::min(it->second, m->width);
    }
    for (const MediaItem* m : items) {
        MediaFocus r;
        r.media = m->id;
        const double fps = m->fps.valid() ? m->fps.toDouble() : 25.0;
        const FrameTime frames = std::max<FrameTime>(1, FrameTime(std::llround(m->duration * fps)));
        FocusShot shot;
        shot.start = 0, shot.end = frames;
        mediaSceneAndAngle(*m, shot);
        const int own = shot.scene.empty() ? m->width : sceneWidth[shot.scene];
        const int w = std::min(std::max(16, o.width), own), h = std::max(1, int(std::lround(double(w) * m->height / m->width)));
        // A subclip is its parent's file from its in point.
        const double from = m->subclipOf ? m->subclipIn : 0.0;
        // (A picture that cannot be read renders black: it is not judged.)
        r.checked = MediaPool::instance().videoFrame(m->path, from + 0.5 / fps, 16, 16) != nullptr;
        for (FrameTime f = 0; r.checked && f < frames; ++f) {
            if (cancel && cancel->load()) return {};
            const Image img = renderMediaFrame(p, *m, from + (double(f) + 0.5) / fps, w, h);
            const std::vector<float> luma = lumaOf(img);
            shot.frames.push_back(measureFocus(luma.data(), img.width, img.height, o.threads));
            if (progress) progress(++done / std::max(1.0, total));
        }
        if (!r.checked) done += double(frames);
        out.push_back(r);
        shots.push_back(std::move(shot));
    }
    // Judged together, so each is set against the other angles of its scene. (Frame rates may differ between media:
    // the shortest stretch is judged at the first one's.)
    const std::vector<FocusIssue> issues = judgeFocus(shots, o, items.empty() ? 25.0 : (items[0]->fps.valid() ? items[0]->fps.toDouble() : 25.0));
    for (const FocusIssue& i : issues) out[i.shot].issues.push_back(i);
    for (size_t k = 0; k < out.size(); ++k) {
        MediaFocus& r = out[k];
        const MediaItem* m = items[k];
        r.typical = shots[k].typical;
        if (!r.checked) {
            r.summary = "Not read";
            continue;
        }
        if (std::none_of(shots[k].frames.begin(), shots[k].frames.end(), [](const FocusMeasure& f) { return f.textured; })) {
            r.summary = "Too little detail to judge";
            continue;
        }
        const Rational fps = m->fps.valid() ? m->fps : Rational{25, 1};
        const FrameTime origin = m->timecode > 0 ? FrameTime(std::llround(m->timecode * fps.toDouble())) : 0;
        // Drop-frame only when its own timecode is (as its log's Start and End are).
        const auto df = m->metadata.find("timecode_drop");
        const bool drop = isDropFrameRate(fps) && df != m->metadata.end() && df->second == "1";
        auto tc = [&](FrameTime f) { return formatTimecode(origin + f, fps, drop); };
        std::string text;
        for (const FocusIssue& i : r.issues) {
            if (!text.empty()) text += "; ";
            if (i.kind == FocusFinding::SoftAngle) text += "Soft against the other angles of its scene (" + percent(i.share) + ")";
            else
                text += std::string(i.kind == FocusFinding::Soft ? "Soft " : "Motion blur ") + tc(i.start) + "–" + tc(i.end);
        }
        r.summary = text.empty() ? std::string("Sharp") : text;
    }
    return out;
}

}  // namespace montage
