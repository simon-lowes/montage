#include "Restoration.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>

namespace montage {

namespace {

float luma(const float* p) { return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]; }

// Bilinear, false outside the image.
bool sampleInside(const Image& img, double x, double y, float out[4]) {
    if (x < -0.5 || y < -0.5 || x > img.width - 0.5 || y > img.height - 0.5) return false;
    x = std::clamp(x, 0.0, double(img.width - 1));
    y = std::clamp(y, 0.0, double(img.height - 1));
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
    const float fx = float(x - x0), fy = float(y - y0);
    const float *a = img.at(x0, y0), *b = img.at(x1, y0), *c = img.at(x0, y1), *d = img.at(x1, y1);
    for (int k = 0; k < 4; ++k) {
        const float top = a[k] + (b[k] - a[k]) * fx, bottom = c[k] + (d[k] - c[k]) * fx;
        out[k] = top + (bottom - top) * fy;
    }
    return true;
}

// Where (x, y) is in another frame.
bool follow(const Image& other, const MotionFn& to, int x, int y, float out[4]) {
    if (other.empty()) return false;
    if (!to) {
        if (x >= other.width || y >= other.height) return false;
        std::copy_n(other.at(x, y), 4, out);
        return true;
    }
    const Point2 f = to(x, y);
    return sampleInside(other, x + f.x, y + f.y, out);
}

// The motion round about (x, y), from four points `reach` pixels away: exact for motion that changes evenly across the
// picture, and not pulled by a defect at (x, y) itself.
MotionFn around(const MotionFn& to, double reach) {
    if (!to) return {};
    return [to, reach](double x, double y) {
        const Point2 a = to(x - reach, y), b = to(x + reach, y), c = to(x, y - reach), d = to(x, y + reach);
        return Point2{(a.x + b.x + c.x + d.x) / 4, (a.y + b.y + c.y + d.y) / 4};
    };
}

// The sensitivity (0..100) as a factor on thresholds: 4 at 0, 1 at 50, 1/4 at 100.
double sensitivityFactor(double s) { return std::pow(4.0, 1.0 - std::clamp(s, 0.0, 100.0) / 50.0); }

// The neighbours, followed to each pixel, and their luma; a pixel either cannot reach has `ok` 0.
struct Followed {
    std::vector<float> p, n;  // luma
    std::vector<uint8_t> okP, okN;
};
Followed followBoth(const Image& cur, const Image& prev, const Image& next, const MotionFn& toPrev, const MotionFn& toNext) {
    const int W = cur.width, H = cur.height;
    Followed f;
    f.p.assign(size_t(W) * H, 0);
    f.n.assign(size_t(W) * H, 0);
    f.okP.assign(size_t(W) * H, 0);
    f.okN.assign(size_t(W) * H, 0);
    parallelRows(H, [&](int y0, int y1) {
        float q[4];
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                if (follow(prev, toPrev, x, y, q)) f.p[i] = luma(q), f.okP[i] = 1;
                if (follow(next, toNext, x, y, q)) f.n[i] = luma(q), f.okN[i] = 1;
            }
    });
    return f;
}

// Grows a 0/1 mask by r pixels (square), as weight `w` where it was 0.
void dilate(std::vector<float>& m, int W, int H, int r, float w) {
    const std::vector<float> was = m;
    // Rows with something set (most rows of a sparse matte have nothing near them).
    std::vector<uint8_t> set(size_t(H), 0);
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) set[size_t(y)] = std::any_of(was.begin() + long(y) * W, was.begin() + long(y + 1) * W, [](float v) { return v >= 1; });
    });
    parallelRows(H, [&](int ya, int yb) {
    for (int y = ya; y < yb; ++y) {
        bool any = false;
        for (int dy = -r; dy <= r && !any; ++dy) any = y + dy >= 0 && y + dy < H && set[size_t(y + dy)];
        if (!any) continue;
        for (int x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + x;
            if (was[i] >= 1) continue;
            bool near = false;
            for (int dy = -r; dy <= r && !near; ++dy)
                for (int dx = -r; dx <= r && !near; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    near = xx >= 0 && yy >= 0 && xx < W && yy < H && was[size_t(yy) * W + xx] >= 1;
                }
            if (near) m[i] = std::max(m[i], w);
        }
    }
    });
}

// ---- Laplace's equation (the membrane through known values) ----

// Solves for the unknown cells of v (4 values a cell: RGBA) so each is the mean of its neighbours, the known cells held:
// first on a grid half the size (an unknown coarse cell where any of its four is), which starts the iterations here.
void membrane(std::vector<float>& v, const std::vector<uint8_t>& unknown, int w, int h) {
    size_t unknowns = 0;
    for (uint8_t u : unknown) unknowns += u;
    if (!unknowns) return;
    const bool coarse = w > 12 && h > 12;
    if (coarse) {
        const int cw = (w + 1) / 2, ch = (h + 1) / 2;
        std::vector<float> cv(size_t(cw) * ch * 4, 0);
        std::vector<uint8_t> cu(size_t(cw) * ch, 0);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                const size_t c = size_t(y) * cw + x;
                int n = 0;
                float sum[4] = {0, 0, 0, 0};
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const int xx = 2 * x + dx, yy = 2 * y + dy;
                        if (xx >= w || yy >= h) continue;
                        const size_t i = size_t(yy) * w + xx;
                        if (unknown[i]) cu[c] = 1;
                        for (int k = 0; k < 4; ++k) sum[k] += v[i * 4 + k];
                        ++n;
                    }
                if (!cu[c])
                    for (int k = 0; k < 4; ++k) cv[c * 4 + k] = sum[k] / float(n);
            }
        membrane(cv, cu, cw, ch);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                if (!unknown[i]) continue;
                const size_t c = size_t(y / 2) * cw + x / 2;
                for (int k = 0; k < 4; ++k) v[i * 4 + k] = cv[c * 4 + k];
            }
    } else {
        // A small grid: start from the known values' mean.
        double sum[4] = {0, 0, 0, 0};
        size_t n = 0;
        for (size_t i = 0; i < unknown.size(); ++i)
            if (!unknown[i]) {
                for (int k = 0; k < 4; ++k) sum[k] += v[i * 4 + k];
                ++n;
            }
        for (size_t i = 0; i < unknown.size(); ++i)
            if (unknown[i])
                for (int k = 0; k < 4; ++k) v[i * 4 + k] = n ? float(sum[k] / double(n)) : 0.0f;
    }
    // Over-relaxed Gauss-Seidel, red-black (each colour's cells depend only on the other's, so a large area is swept
    // in parallel); few sweeps when the coarse solution started it.
    const int sweeps = coarse ? 60 : 300;
    const float omega = 1.85f;
    std::vector<size_t> cells[2];
    for (size_t i = 0; i < unknown.size(); ++i)
        if (unknown[i]) cells[(i % size_t(w) + i / size_t(w)) % 2].push_back(i);
    std::mutex lock;
    for (int it = 0; it < sweeps; ++it) {
        float change = 0;
        for (const std::vector<size_t>& colour : cells) {
        auto sweep = [&](int c0, int c1) {
        float local = 0;
        for (int ci = c0; ci < c1; ++ci) {
            const size_t i = colour[size_t(ci)];
            const int x = int(i % size_t(w)), y = int(i / size_t(w));
            float sum[4] = {0, 0, 0, 0};
            int n = 0;
            auto add = [&](size_t j) {
                for (int k = 0; k < 4; ++k) sum[k] += v[j * 4 + k];
                ++n;
            };
            if (x > 0) add(i - 1);
            if (x + 1 < w) add(i + 1);
            if (y > 0) add(i - size_t(w));
            if (y + 1 < h) add(i + size_t(w));
            if (!n) continue;
            for (int k = 0; k < 4; ++k) {
                const float target = sum[k] / float(n), d = (target - v[i * 4 + k]) * omega;
                v[i * 4 + k] += d;
                local = std::max(local, std::fabs(d));
            }
        }
        const std::lock_guard<std::mutex> hold(lock);
        change = std::max(change, local);
        };
        if (colour.size() > 20000) parallelRows(int(colour.size()), sweep);
        else sweep(0, int(colour.size()));
        }
        if (change < 1e-5f) break;
    }
}

// A matte's bounding box (empty when nothing is set).
bool matteBox(const std::vector<float>& matte, int W, int H, int& x0, int& y0, int& x1, int& y1) {
    x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; ++y) {
        const float* row = matte.data() + size_t(y) * W;
        for (int x = 0; x < W; ++x)
            if (row[x] > 0) x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    return x1 >= 0;
}

// The area (x0, y0) w x h of img, its matte `m` (w x h), replaced by `source` (w x h x 4, RGBA; null for black): copied, or by
// its gradients levelled to the known pixels round it (Poisson). The area should hold a pixel round the matte.
void pasteArea(Image& img, int x0, int y0, int w, int h, const std::vector<float>& m, const float* source, bool seamless) {
    auto src = [&](size_t i, int k) { return source ? source[i * 4 + size_t(k)] : 0.0f; };
    std::vector<float> v;
    std::vector<uint8_t> unknown;
    if (seamless) {
        v.assign(size_t(w) * h * 4, 0);
        unknown.assign(size_t(w) * h, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = size_t(y) * w + x;
                if (m[i] > 0) {
                    unknown[i] = 1;
                    continue;
                }
                const float* a = img.at(x + x0, y + y0);
                for (int k = 0; k < 4; ++k) v[i * 4 + size_t(k)] = a[k] - src(i, k);
            }
        // An area that touches the frame's edge has no known values there: those sides are left free.
        membrane(v, unknown, w, h);
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            const float a = std::min(1.0f, m[i]);
            if (a <= 0) continue;
            float* q = img.at(x + x0, y + y0);
            for (int k = 0; k < 4; ++k) q[k] += (src(i, k) + (seamless ? v[i * 4 + size_t(k)] : 0.0f) - q[k]) * a;
        }
}

// img's own picture (dx, dy) away over the area, the edges held, read before anything is written.
std::vector<float> shiftedArea(const Image& img, int x0, int y0, int w, int h, double dx, double dy) {
    std::vector<float> out(size_t(w) * h * 4);
    const int W = img.width, H = img.height;
    float q[4];
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double sx = std::clamp(x + x0 + dx, 0.0, double(W - 1)), sy = std::clamp(y + y0 + dy, 0.0, double(H - 1));
            sampleInside(img, sx, sy, q);
            std::copy_n(q, 4, &out[(size_t(y) * w + x) * 4]);
        }
    return out;
}

// A full-frame matte's box, a pixel round it, and the matte cut to it.
bool cutMatte(const std::vector<float>& matte, int W, int H, int& x0, int& y0, int& w, int& h, std::vector<float>& local) {
    int x1, y1;
    if (matte.size() != size_t(W) * H || !matteBox(matte, W, H, x0, y0, x1, y1)) return false;
    x0 = std::max(0, x0 - 1), y0 = std::max(0, y0 - 1), x1 = std::min(W - 1, x1 + 1), y1 = std::min(H - 1, y1 + 1);
    w = x1 - x0 + 1, h = y1 - y0 + 1;
    local.resize(size_t(w) * h);
    for (int y = 0; y < h; ++y) std::copy_n(matte.data() + size_t(y + y0) * W + x0, w, local.data() + size_t(y) * w);
    return true;
}

}  // namespace

// ---- Dirt ----------------------------------------------------------------------------------------------------------------

std::vector<float> detectDirt(const Image& cur, const Image& prev, const Image& next, const MotionFn& toPrev,
                              const MotionFn& toNext, const DirtSettings& s, int* count) {
    if (count) *count = 0;
    const int W = cur.width, H = cur.height;
    if (cur.empty() || prev.empty() || next.empty()) return {};
    const double sigma = s.noise >= 0 ? s.noise : estimateNoise(cur);
    const float T = float(std::max(0.03, 5 * sigma) * sensitivityFactor(s.sensitivity));
    const int maxSize = std::max(1, int(std::lround(s.maxSize)));
    const double reach = maxSize / 2.0 + 4;
    const Followed f = followBoth(cur, prev, next, around(toPrev, reach), around(toNext, reach));
    // Each pixel: how far it stands out the same way from both neighbours where they agree (signed), and how far it is
    // from the nearer of them either way (how well the motion was followed).
    std::vector<float> spike(size_t(W) * H, 0), miss(size_t(W) * H, 1);
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const size_t i = size_t(y) * W + x;
                if (!f.okP[i] || !f.okN[i]) continue;
                const float c = luma(cur.at(x, y)), dp = c - f.p[i], dn = c - f.n[i];
                const float m = std::min(std::fabs(dp), std::fabs(dn));
                miss[i] = m;
                if (dp * dn > 0 && std::fabs(f.p[i] - f.n[i]) < 0.5f * m + 0.5f * T) spike[i] = dp > 0 ? m : -m;
            }
    });
    std::vector<float> matte(size_t(W) * H, 0);
    std::vector<int> label(size_t(W) * H, 0);
    int found = 0, next_label = 0;
    std::vector<size_t> comp;
    std::deque<size_t> queue;
    for (size_t start = 0; start < spike.size(); ++start) {
        if (label[start] || std::fabs(spike[start]) <= T) continue;
        // The speck: grown to its fainter edge (half the threshold, the same way).
        const float sign = spike[start] > 0 ? 1.0f : -1.0f;
        label[start] = ++next_label;
        comp.clear();
        queue.assign(1, start);
        int x0 = W, y0 = H, x1 = -1, y1 = -1;
        bool tooBig = false;
        float peak = 0;
        while (!queue.empty()) {
            const size_t i = queue.front();
            queue.pop_front();
            comp.push_back(i);
            peak = std::max(peak, std::fabs(spike[i]));
            const int x = int(i % size_t(W)), y = int(i / size_t(W));
            x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
            if (x1 - x0 + 1 > maxSize || y1 - y0 + 1 > maxSize) tooBig = true;
            if (tooBig && comp.size() > size_t(maxSize) * maxSize * 4) break;
            const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
            for (int k = 0; k < 4; ++k) {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= W || ny[k] >= H) continue;
                const size_t j = size_t(ny[k]) * W + nx[k];
                if (label[j] || spike[j] * sign <= 0.5f * T) continue;
                label[j] = next_label;
                queue.push_back(j);
            }
        }
        // Mark the rest of a large one so it is not started again.
        while (!queue.empty()) {
            label[queue.front()] = next_label;
            queue.pop_front();
        }
        if (tooBig) continue;
        // The motion has to have been followed around it: the ring two pixels out matches a neighbour closely, and far
        // more closely than the speck stands out (where the motion was missed, texture can look like specks).
        double ringMiss = 0;
        int ring = 0;
        for (int y = std::max(0, y0 - 3); y <= std::min(H - 1, y1 + 3); ++y)
            for (int x = std::max(0, x0 - 3); x <= std::min(W - 1, x1 + 3); ++x) {
                if (x >= x0 - 1 && x <= x1 + 1 && y >= y0 - 1 && y <= y1 + 1) continue;
                ringMiss += miss[size_t(y) * W + x];
                ++ring;
            }
        if (!ring || ringMiss / ring > std::min(0.5 * T, 0.25 * peak)) continue;
        for (size_t i : comp) matte[i] = 1;
        ++found;
    }
    if (found) {
        dilate(matte, W, H, 2, 1);  // a speck's soft edge (and, in compressed footage, its ringing)
        dilate(matte, W, H, 1, 0.5f);
    }
    if (count) *count = found;
    return matte;
}

std::vector<float> detectLastingDirt(const Image& cur, const Image& prev, const Image& next, const Image& prev2, const Image& next2,
                                     const MotionFn& toPrev, const MotionFn& toNext, const MotionFn& toPrev2, const MotionFn& toNext2,
                                     const DirtSettings& s, int* count) {
    if (count) *count = 0;
    std::vector<float> m = detectDirt(cur, prev2, next2, toPrev2, toNext2, s);
    if (m.empty() || prev.empty() || next.empty()) return {};
    const int W = cur.width, H = cur.height;
    const double sigma = s.noise >= 0 ? s.noise : estimateNoise(cur);
    const float T = float(std::max(0.03, 5 * sigma) * sensitivityFactor(s.sensitivity));
    const double reach = std::max(1.0, s.maxSize) / 2 + 4;
    const Followed f = followBoth(cur, prev, next, around(toPrev, reach), around(toNext, reach));
    const Followed f2 = followBoth(cur, prev2, next2, around(toPrev2, reach), around(toNext2, reach));
    // Each speck kept when one neighbour has it and the other does not: most of its core (what stands out from the frames
    // two away) matches the previous frame and not the next, or the next and not the previous. (What stands out from both
    // neighbours is dirt detectDirt finds; what both have lasts three frames and is taken for the picture.)
    std::vector<char> seen(m.size(), 0);
    std::vector<size_t> stack, piece;
    int kept = 0;
    for (size_t i = 0; i < m.size(); ++i) {
        if (m[i] <= 0 || seen[i]) continue;
        seen[i] = 1;
        stack.assign(1, i);
        piece.clear();
        int core = 0, one = 0;
        while (!stack.empty()) {
            const size_t j = stack.back();
            stack.pop_back();
            piece.push_back(j);
            const int x = int(j % size_t(W)), y = int(j / size_t(W));
            const float c = luma(cur.at(x, y));
            if (f2.okP[j] && f2.okN[j] && std::fabs(c - (f2.p[j] + f2.n[j]) / 2) > T) {
                ++core;
                const bool inP = f.okP[j] && std::fabs(c - f.p[j]) < T, inN = f.okN[j] && std::fabs(c - f.n[j]) < T;
                const bool outP = f.okP[j] && std::fabs(c - f.p[j]) > T, outN = f.okN[j] && std::fabs(c - f.n[j]) > T;
                one += (inP && outN) || (inN && outP);
            }
            const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
            for (int k = 0; k < 4; ++k)
                if (nx[k] >= 0 && ny[k] >= 0 && nx[k] < W && ny[k] < H) {
                    const size_t n = size_t(ny[k]) * W + nx[k];
                    if (m[n] > 0 && !seen[n]) seen[n] = 1, stack.push_back(n);
                }
        }
        if (core > 0 && 2 * one >= core) ++kept;
        else
            for (size_t j : piece) m[j] = 0;
    }
    if (count) *count = kept;
    return m;
}

void fillFromNeighbours(Image& cur, const std::vector<float>& matte, const Image& prev, const Image& next,
                        const MotionFn& toPrev, const MotionFn& toNext, double reach) {
    const int W = cur.width, H = cur.height;
    if (matte.size() != size_t(W) * H) return;
    const MotionFn mp = around(toPrev, reach), mn = around(toNext, reach);
    parallelRows(H, [&](int y0, int y1) {
        float a[4], b[4];
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const float m = matte[size_t(y) * W + x];
                if (m <= 0) continue;
                const bool ha = follow(prev, mp, x, y, a), hb = follow(next, mn, x, y, b);
                if (!ha && !hb) continue;
                float* q = cur.at(x, y);
                for (int k = 0; k < 4; ++k) {
                    const float v = ha && hb ? 0.5f * (a[k] + b[k]) : ha ? a[k] : b[k];
                    q[k] += (v - q[k]) * m;
                }
            }
    });
}

std::vector<DustSpot> dustSpotsFromString(const std::string& s) {
    std::vector<DustSpot> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream l(line);
        DustSpot d;
        if (l >> d.frame >> d.x >> d.y >> d.r && d.r > 0 && std::isfinite(d.x) && std::isfinite(d.y) && std::isfinite(d.r))
            out.push_back(d);
    }
    return out;
}

bool hasDustSpotAt(const std::string& s, int64_t frame) {
    // Each line's first number, without reading the rest (asked for every frame of a clip).
    const char* p = s.c_str();
    while (*p) {
        char* end = nullptr;
        const long long f = std::strtoll(p, &end, 10);
        if (end != p && f == frame) return true;
        p = std::strchr(p, '\n');
        if (!p) break;
        ++p;
    }
    return false;
}

std::string dustSpotsToString(const std::vector<DustSpot>& spots) {
    std::ostringstream o;
    o.precision(6);
    for (const DustSpot& d : spots) o << d.frame << ' ' << d.x << ' ' << d.y << ' ' << d.r << '\n';
    return o.str();
}

LocalMatte dustArea(const Image& cur, const Image* prev, const Image* next, const MotionFn& toPrev, const MotionFn& toNext,
                    const DustSpot& spot, double sensitivity) {
    const int W = cur.width, H = cur.height;
    LocalMatte out;
    const double cx = spot.x * W - 0.5, cy = spot.y * H - 0.5, r = std::max(1.0, spot.r * W);
    // The circle and a few pixels round it (the grown edge, and known pixels for a fill from the surround).
    const int x0 = std::max(0, int(std::floor(cx - r - 3))), x1 = std::min(W - 1, int(std::ceil(cx + r + 3)));
    const int y0 = std::max(0, int(std::floor(cy - r - 3))), y1 = std::min(H - 1, int(std::ceil(cy + r + 3)));
    if (x0 > x1 || y0 > y1) return out;
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    out.x0 = x0, out.y0 = y0, out.w = w, out.h = h;
    out.m.assign(size_t(w) * h, 0);
    // The circle, its outer quarter soft.
    auto circle = [&](int x, int y) {
        const double d = std::hypot(x - cx, y - cy);
        return float(std::clamp((r - d) / (0.25 * r + 0.5), 0.0, 1.0));
    };
    const bool neighbours = prev && next && !prev->empty() && !next->empty();
    if (!neighbours) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) out.m[size_t(y) * w + x] = circle(x + x0, y + y0);
        return out;
    }
    // Inside: what differs from both neighbours (the same way or not; the brush says it is dirt).
    const float T = float(0.02 * sensitivityFactor(sensitivity));
    std::vector<float> core(size_t(w) * h, 0);
    float a[4], b[4];
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (circle(x + x0, y + y0) <= 0) continue;
            const bool ha = follow(*prev, toPrev, x + x0, y + y0, a), hb = follow(*next, toNext, x + x0, y + y0, b);
            if (!ha || !hb) continue;
            const float c = luma(cur.at(x + x0, y + y0)), dp = std::fabs(c - luma(a)), dn = std::fabs(c - luma(b));
            if (std::min(dp, dn) > T) core[size_t(y) * w + x] = 1;
        }
    dilate(core, w, h, 1, 1);
    dilate(core, w, h, 1, 0.5f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * w + x;
            out.m[i] = core[i] * circle(x + x0, y + y0);
        }
    return out;
}

std::vector<float> LocalMatte::full(int W, int H) const {
    std::vector<float> f(size_t(std::max(0, W)) * std::max(0, H), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (x + x0 < W && y + y0 < H) f[size_t(y + y0) * W + x + x0] = m[size_t(y) * w + x];
    return f;
}

std::vector<float> dustMatte(const Image& cur, const Image* prev, const Image* next, const MotionFn& toPrev,
                             const MotionFn& toNext, const DustSpot& spot, double sensitivity) {
    return dustArea(cur, prev, next, toPrev, toNext, spot, sensitivity).full(cur.width, cur.height);
}

void fillFromNeighbours(Image& cur, const LocalMatte& matte, const Image& prev, const Image& next, const MotionFn& toPrev,
                        const MotionFn& toNext, double reach) {
    const MotionFn mp = around(toPrev, reach), mn = around(toNext, reach);
    float a[4], b[4];
    for (int y = 0; y < matte.h; ++y)
        for (int x = 0; x < matte.w; ++x) {
            const float m = matte.m[size_t(y) * matte.w + x];
            if (m <= 0) continue;
            const int X = x + matte.x0, Y = y + matte.y0;
            const bool ha = follow(prev, mp, X, Y, a), hb = follow(next, mn, X, Y, b);
            if (!ha && !hb) continue;
            float* q = cur.at(X, Y);
            for (int k = 0; k < 4; ++k) {
                const float v = ha && hb ? 0.5f * (a[k] + b[k]) : ha ? a[k] : b[k];
                q[k] += (v - q[k]) * m;
            }
        }
}

void fillFromSurround(Image& img, const LocalMatte& matte) {
    if (matte.w > 0 && matte.h > 0) pasteArea(img, matte.x0, matte.y0, matte.w, matte.h, matte.m, nullptr, true);
}

// ---- Scratches -----------------------------------------------------------------------------------------------------------

namespace {

// Slanting and wandering scratches in the thin-line response `line` (W x H, `o` the distance to each side): each followed
// down row by row from where it stands out most (seeds every eighth row), a pixel either way a row at most, kept when it
// runs over a quarter of the frame within about 22 degrees of upright, standing out the same way well above the row's own texture most of the way and thin
// (no wider than `width`) nearly all of it. Added to `matte` (where it is not there already); the number found.
int followScratches(const std::vector<float>& line, int W, int H, int o, int width, double sigma, double factor, int polarity,
                    std::vector<float>& matte) {
    const int minRows = std::max(24, H / 4);
    if (H < minRows || W <= 2 * o + 2) return 0;
    // What each row's texture gives the response (its 90th percentile across the row: scratches are few).
    std::vector<float> bar(static_cast<size_t>(H));
    parallelRows(H, [&](int y0, int y1) {
        std::vector<float> a;
        for (int y = y0; y < y1; ++y) {
            a.assign(line.begin() + long(size_t(y) * W + o), line.begin() + long(size_t(y) * W + W - o));
            for (float& v : a) v = std::fabs(v);
            std::nth_element(a.begin(), a.begin() + long(a.size() * 9 / 10), a.end());
            bar[size_t(y)] = float(std::max({3.0 * a[a.size() * 9 / 10], 3 * sigma, 0.02}) * factor);
        }
    });
    auto at = [&](int x, int y) { return line[size_t(y) * W + x]; };
    // The run of pixels standing out with the line on row y about x (half as much as it does there), as [l, r].
    auto run = [&](int x, int y, float sign, int& l, int& r) {
        const float v = sign * at(x, y);
        l = r = x;
        while (l - 1 >= o && sign * at(l - 1, y) > 0.3f * v) --l;
        while (r + 1 < W - o && sign * at(r + 1, y) > 0.3f * v) ++r;
    };
    int found = 0;
    for (int sy = 0; sy + minRows <= H; sy += 8)
        for (int x = o + 1; x < W - o - 1; ++x) {
            const float v = at(x, sy);
            const float sign = v > 0 ? 1.0f : -1.0f;
            if (std::fabs(v) <= bar[size_t(sy)] || std::fabs(v) < std::fabs(at(x - 1, sy)) || std::fabs(v) < std::fabs(at(x + 1, sy)) ||
                matte[size_t(sy) * W + x] > 0)
                continue;
            if ((polarity == 1 && sign > 0) || (polarity == 2 && sign < 0)) continue;
            std::vector<int> xs{x};
            int cx = x, misses = 0, agree = 1, thin = 0;
            double sum = std::fabs(v), level = bar[size_t(sy)];
            for (int y = sy + 1; y < H && misses <= 6; ++y) {
                int bx = cx;
                float bv = -1e30f;
                for (int dx = -1; dx <= 1; ++dx)
                    if (const int nx = cx + dx; nx >= o && nx < W - o && sign * at(nx, y) > bv) bv = sign * at(nx, y), bx = nx;
                if (bv > 0.4f * bar[size_t(y)]) ++agree, misses = 0;
                else ++misses;
                cx = bx;
                xs.push_back(cx);
                sum += bv, level += bar[size_t(y)];
            }
            xs.resize(xs.size() - size_t(misses));
            const int rows = int(xs.size());
            if (rows < minRows || agree < 0.6 * rows || sum / rows < 0.5 * level / rows) continue;
            // (A scratch runs down the film: within about 22 degrees of upright, so a wire or a mast is left alone.)
            if (std::abs(xs.back() - xs.front()) > 0.4 * rows) continue;
            std::vector<std::pair<int, int>> runs(static_cast<size_t>(rows));
            for (int k = 0; k < rows; ++k) {
                run(xs[size_t(k)], sy + k, sign, runs[size_t(k)].first, runs[size_t(k)].second);
                thin += runs[size_t(k)].second - runs[size_t(k)].first + 1 <= width;
            }
            if (thin < 0.8 * rows) continue;
            ++found;
            for (int k = 0; k < rows; ++k) {
                const int y = sy + k;
                int l = runs[size_t(k)].first, r = runs[size_t(k)].second;
                if (r - l + 1 > width) l = r = xs[size_t(k)];
                for (int c = l; c <= r; ++c) matte[size_t(y) * W + c] = 1;
                if (l > 0) matte[size_t(y) * W + l - 1] = std::max(matte[size_t(y) * W + l - 1], 0.5f);
                if (r + 1 < W) matte[size_t(y) * W + r + 1] = std::max(matte[size_t(y) * W + r + 1], 0.5f);
            }
        }
    return found;
}

}  // namespace

std::vector<float> detectScratches(const Image& img, const ScratchSettings& s, int* count) {
    if (count) *count = 0;
    const int W = img.width, H = img.height;
    std::vector<float> matte(size_t(W) * H, 0);
    if (img.empty() || W < 8 || H < 8) return matte;
    const int width = std::clamp(int(std::lround(s.maxWidth)), 1, 32), o = width + 1;
    if (W <= 2 * o + 1) return matte;  // (too narrow to tell a line from its sides)
    // Each pixel: how far it stands out from both sides the same way (a thin line, not an edge).
    std::vector<float> Y(size_t(W) * H), line(size_t(W) * H, 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) Y[size_t(y) * W + x] = luma(img.at(x, y));
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = o; x < W - o; ++x) {
                const size_t i = size_t(y) * W + x;
                const float l = Y[i] - Y[i - o], r = Y[i] - Y[i + o];
                if (l * r > 0) line[i] = l > 0 ? std::min(l, r) : std::max(l, r);
            }
    });
    const double sigma = estimateNoise(img), factor = sensitivityFactor(s.sensitivity);
    const int band = std::max(16, H / 8);
    std::vector<float> prof(static_cast<size_t>(W)), cons(static_cast<size_t>(W));
    int found = 0;
    for (int b0 = 0; b0 < H; b0 += band) {
        const int b1 = std::min(H, b0 + band), rows = b1 - b0;
        if (rows < 8) break;
        for (int x = 0; x < W; ++x) {
            double sum = 0;
            for (int y = b0; y < b1; ++y) sum += line[size_t(y) * W + x];
            prof[size_t(x)] = float(sum / rows);
        }
        // What the picture's own texture gives the profile (its 90th percentile across the columns: scratches are few),
        // so the bar rises with it.
        std::vector<float> abs(prof.begin() + o, prof.end() - o);
        for (float& v : abs) v = std::fabs(v);
        if (abs.empty()) break;
        std::nth_element(abs.begin(), abs.begin() + long(abs.size() * 9 / 10), abs.end());
        const double texture = abs[abs.size() * 9 / 10];
        const double bar = std::max({3 * texture, 4 * sigma / std::sqrt(double(rows)), 0.02}) * factor;
        for (int x = 0; x < W; ++x) {
            const float p = prof[size_t(x)];
            int same = 0;
            if (std::fabs(p) > bar)
                for (int y = b0; y < b1; ++y) same += line[size_t(y) * W + x] * (p > 0 ? 1.0f : -1.0f) > 0.25f * std::fabs(p);
            const bool polar = s.polarity == 0 || (s.polarity == 1 && p < 0) || (s.polarity == 2 && p > 0);
            cons[size_t(x)] = std::fabs(p) > bar && polar && same > rows * 0.6 ? 1.0f : 0.0f;
        }
        // Runs of marked columns no wider than the setting are scratches (a wider one is something in the picture).
        for (int x = 0; x < W;) {
            if (!cons[size_t(x)]) {
                ++x;
                continue;
            }
            int e = x;
            while (e + 1 < W && cons[size_t(e) + 1]) ++e;
            if (e - x + 1 <= width) {
                ++found;
                for (int y = b0; y < b1; ++y) {
                    for (int c = x; c <= e; ++c) matte[size_t(y) * W + c] = 1;
                    if (x > 0) matte[size_t(y) * W + x - 1] = std::max(matte[size_t(y) * W + x - 1], 0.5f);
                    if (e + 1 < W) matte[size_t(y) * W + e + 1] = std::max(matte[size_t(y) * W + e + 1], 0.5f);
                }
            }
            x = e + 1;
        }
    }
    if (s.follow) found += followScratches(line, W, H, o, width, sigma, factor, s.polarity, matte);
    if (count) *count = found;
    return matte;
}

void fillAcross(Image& img, const std::vector<float>& matte) {
    const int W = img.width, H = img.height;
    if (matte.size() != size_t(W) * H) return;
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float* m = &matte[size_t(y) * W];
            for (int x = 0; x < W;) {
                if (m[x] <= 0) {
                    ++x;
                    continue;
                }
                int e = x;
                while (e + 1 < W && m[e + 1] > 0) ++e;
                const int l = x - 1, r = e + 1;
                if (l < 0 && r >= W) break;
                float a[4], b[4];
                std::copy_n(img.at(l >= 0 ? l : r, y), 4, a);
                std::copy_n(img.at(r < W ? r : l, y), 4, b);
                for (int c = x; c <= e; ++c) {
                    const float t = float(c - l) / float(r - l);
                    float* q = img.at(c, y);
                    for (int k = 0; k < 4; ++k) q[k] += (a[k] + (b[k] - a[k]) * t - q[k]) * m[c];
                }
                x = e + 1;
            }
        }
    });
}

// ---- Patches, clone and heal -------------------------------------------------------------------------------------------------

Image shiftedImage(const Image& img, double dx, double dy) {
    Image out(img.width, img.height, Image::Uninitialized{});
    const int W = img.width, H = img.height;
    parallelRows(H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < W; ++x) {
                const double sx = std::clamp(x + dx, 0.0, double(W - 1)), sy = std::clamp(y + dy, 0.0, double(H - 1));
                sampleInside(img, sx, sy, out.at(x, y));
            }
    });
    return out;
}

void seamlessPaste(Image& img, const Image& source, const std::vector<float>& matte) {
    const int W = img.width, H = img.height;
    int x0, y0, w, h;
    std::vector<float> m;
    if (source.width != W || source.height != H || !cutMatte(matte, W, H, x0, y0, w, h, m)) return;
    std::vector<float> s(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) std::copy_n(source.at(x + x0, y + y0), 4, &s[(size_t(y) * w + x) * 4]);
    pasteArea(img, x0, y0, w, h, m, s.data(), true);
}

void fillFromSurround(Image& img, const std::vector<float>& matte) {
    int x0, y0, w, h;
    std::vector<float> m;
    if (cutMatte(matte, img.width, img.height, x0, y0, w, h, m)) pasteArea(img, x0, y0, w, h, m, nullptr, true);
}

void pasteShifted(Image& img, const std::vector<float>& matte, double dx, double dy, bool seamless) {
    int x0, y0, w, h;
    std::vector<float> m;
    if (!cutMatte(matte, img.width, img.height, x0, y0, w, h, m)) return;
    const std::vector<float> s = shiftedArea(img, x0, y0, w, h, dx, dy);
    pasteArea(img, x0, y0, w, h, m, s.data(), seamless);
}

void paintStroke(Image& img, const std::vector<std::array<double, 2>>& points, double radius, double softness, double dx,
                 double dy, bool heal) {
    const int W = img.width, H = img.height;
    if (points.empty() || radius <= 0 || W <= 0 || H <= 0) return;
    // Only the stroke's box is worked on: as far as its soft edge reaches, and a ring of pixels it leaves alone round
    // that (heal's known boundary).
    const double rr = std::max(0.5, radius), r = rr + (std::clamp(softness, 0.0, 1.0) * rr + 0.5) / 2 + 2;
    double lx = 1e18, ly = 1e18, hx = -1e18, hy = -1e18;
    for (const auto& q : points) lx = std::min(lx, q[0]), ly = std::min(ly, q[1]), hx = std::max(hx, q[0]), hy = std::max(hy, q[1]);
    const int x0 = std::max(0, int(std::floor(lx - r))), y0 = std::max(0, int(std::floor(ly - r)));
    const int x1 = std::min(W - 1, int(std::ceil(hx + r))), y1 = std::min(H - 1, int(std::ceil(hy + r)));
    if (x0 > x1 || y0 > y1) return;
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    std::vector<std::array<double, 2>> local(points);
    for (auto& q : local) q[0] -= x0, q[1] -= y0;
    const std::vector<float> m = strokeMatte(local, radius, softness, w, h);
    const std::vector<float> s = shiftedArea(img, x0, y0, w, h, dx, dy);
    pasteArea(img, x0, y0, w, h, m, s.data(), heal);
}

std::vector<PaintStroke> paintStrokesFromString(const std::string& s) {
    // A line a stroke: "clone"|"heal", frame, hold, track, dx, dy, radius, softness, then x y pairs.
    std::vector<PaintStroke> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream l(line);
        std::string kind;
        PaintStroke p;
        int hold = 0, track = 0;
        if (!(l >> kind >> p.frame >> hold >> track >> p.dx >> p.dy >> p.radius >> p.softness) || (kind != "clone" && kind != "heal"))
            continue;
        p.heal = kind == "heal";
        p.hold = hold != 0;
        p.track = track != 0;
        double x, y;
        while (l >> x >> y)
            if (std::isfinite(x) && std::isfinite(y)) p.points.push_back({x, y});
        if (!p.points.empty() && p.radius > 0 && std::isfinite(p.dx) && std::isfinite(p.dy)) out.push_back(std::move(p));
    }
    return out;
}

std::string paintStrokesToString(const std::vector<PaintStroke>& strokes) {
    std::ostringstream o;
    o.precision(6);
    for (const PaintStroke& p : strokes) {
        o << (p.heal ? "heal" : "clone") << ' ' << p.frame << ' ' << int(p.hold) << ' ' << int(p.track) << ' ' << p.dx << ' ' << p.dy
          << ' ' << p.radius << ' ' << p.softness;
        for (const auto& q : p.points) o << ' ' << q[0] << ' ' << q[1];
        o << '\n';
    }
    return o.str();
}

std::vector<float> strokeMatte(const std::vector<std::array<double, 2>>& pts, double radius, double softness, int w, int h) {
    std::vector<float> m(size_t(w) * h, 0);
    if (pts.empty() || radius <= 0) return m;
    const double r = std::max(0.5, radius), soft = std::clamp(softness, 0.0, 1.0) * r + 0.5;
    // Each segment (or the one point) as a capsule, the coverage the most of any.
    for (size_t i = 0; i == 0 || i + 1 < pts.size(); ++i) {
        const auto& a = pts[i];
        const auto& b = i + 1 < pts.size() ? pts[i + 1] : pts[i];
        const double reach = r + soft / 2 + 1;  // (where the soft edge reaches nothing)
        const int x0 = std::max(0, int(std::floor(std::min(a[0], b[0]) - reach))), x1 = std::min(w - 1, int(std::ceil(std::max(a[0], b[0]) + reach)));
        const int y0 = std::max(0, int(std::floor(std::min(a[1], b[1]) - reach))), y1 = std::min(h - 1, int(std::ceil(std::max(a[1], b[1]) + reach)));
        const double vx = b[0] - a[0], vy = b[1] - a[1], len = vx * vx + vy * vy;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double t = len > 0 ? std::clamp(((x - a[0]) * vx + (y - a[1]) * vy) / len, 0.0, 1.0) : 0.0;
                const double d = std::hypot(x - a[0] - t * vx, y - a[1] - t * vy);
                const double v = std::clamp((r - d) / soft + 0.5, 0.0, 1.0);
                float& q = m[size_t(y) * w + x];
                q = std::max(q, float(v * v * (3 - 2 * v)));
            }
    }
    return m;
}

}  // namespace montage
