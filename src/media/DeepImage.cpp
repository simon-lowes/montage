#include "DeepImage.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Image.h"

namespace montage {

namespace {

bool before(const DeepSample& a, const DeepSample& b) { return a.z < b.z || (a.z == b.z && a.zBack < b.zBack); }
bool sameSpan(const DeepSample& a, const DeepSample& b) { return a.z == b.z && a.zBack == b.zBack; }

// The part of volume `s` from z0 to z1 (within it).
DeepSample part(const DeepSample& s, float z0, float z1) {
    DeepSample o = s;
    o.z = z0, o.zBack = z1;
    const double x = double(z1 - z0) / double(s.zBack - s.z);
    double a = s.a, k;
    if (a >= 1) {
        k = 1;  // opaque all through: each part opaque, its colour the same
    } else if (a <= 0) {
        a = 0, k = x;  // emits without absorbing: colour by thickness
    } else {
        const double ax = -std::expm1(x * std::log1p(-a));
        k = ax / a;
        a = ax;
    }
    o.a = float(a);
    o.r = float(s.r * k), o.g = float(s.g * k), o.b = float(s.b * k);
    return o;
}

// The pieces of `in` cut at every depth in `cuts` (sorted) that falls inside a volume.
void split(const std::vector<DeepSample>& in, const std::vector<float>& cuts, std::vector<DeepSample>& out) {
    out.clear();
    for (const DeepSample& s : in) {
        if (!(s.zBack > s.z)) {
            out.push_back(s);
            continue;
        }
        float from = s.z;
        for (auto it = std::upper_bound(cuts.begin(), cuts.end(), s.z); it != cuts.end() && *it < s.zBack; ++it) {
            out.push_back(part(s, from, *it));
            from = *it;
        }
        out.push_back(from == s.z ? s : part(s, from, s.zBack));
    }
}

void cutsOf(const std::vector<DeepSample>& s, std::vector<float>& cuts) {
    cuts.clear();
    for (const DeepSample& d : s) {
        cuts.push_back(d.z);
        if (d.zBack > d.z) cuts.push_back(d.zBack);
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
}

// Samples over the same depths mixed: by absorption (u = -ln(1 - alpha)), each giving colour in proportion; opaque
// ones (if any) alone, averaged. `share[j]`, when given, gets sample j's part of the mix.
DeepSample mix(const DeepSample* s, size_t n, std::vector<DeepSample>* share = nullptr) {
    DeepSample o = s[0];
    o.r = o.g = o.b = o.a = 0;
    if (share) share->assign(n, o);
    size_t opaque = 0;
    for (size_t j = 0; j < n; ++j) opaque += s[j].a >= 1;
    if (opaque) {
        for (size_t j = 0; j < n; ++j)
            if (s[j].a >= 1) {
                const float k = 1.0f / float(opaque);
                o.r += s[j].r * k, o.g += s[j].g * k, o.b += s[j].b * k;
                if (share) (*share)[j].r = s[j].r * k, (*share)[j].g = s[j].g * k, (*share)[j].b = s[j].b * k, (*share)[j].a = k;
            }
        o.a = 1;
        return o;
    }
    std::vector<double> u(n);
    double sum = 0;
    for (size_t j = 0; j < n; ++j) sum += u[j] = s[j].a > 0 ? -std::log1p(-double(s[j].a)) : 0;
    const double a = -std::expm1(-sum);
    const double w = sum > 1e-12 ? a / sum : 1 - sum / 2;
    for (size_t j = 0; j < n; ++j) {
        const double v = s[j].a > 1e-7 ? u[j] / s[j].a : 1 + s[j].a / 2;  // (u / alpha -> 1 as alpha -> 0)
        const double k = v * w;
        o.r += float(s[j].r * k), o.g += float(s[j].g * k), o.b += float(s[j].b * k);
        if (share) {
            DeepSample& p = (*share)[j];
            p.r = float(s[j].r * k), p.g = float(s[j].g * k), p.b = float(s[j].b * k);
            p.a = float(sum > 1e-12 ? a * u[j] / sum : s[j].a);
        }
    }
    o.a = float(a);
    return o;
}

bool visible(const DeepSample& s) { return s.a > 0 || s.r != 0 || s.g != 0 || s.b != 0; }

}  // namespace

DeepFrame DeepFrame::blank(int width, int height) {
    DeepFrame f;
    f.width = std::max(0, width), f.height = std::max(0, height);
    f.offsets.assign(size_t(f.width) * size_t(f.height) + 1, 0);
    return f;
}

DeepFrame buildDeep(int width, int height, const std::function<void(int, int, std::vector<DeepSample>&)>& pixel) {
    DeepFrame f = DeepFrame::blank(width, height);
    if (f.empty()) return f;
    // Rows made at once, each into its own list, then joined.
    std::vector<std::vector<DeepSample>> rows(size_t(f.height));
    std::vector<std::vector<uint32_t>> counts(size_t(f.height));
    parallelRows(f.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            std::vector<DeepSample>& row = rows[size_t(y)];
            std::vector<uint32_t>& c = counts[size_t(y)];
            c.resize(size_t(f.width));
            for (int x = 0; x < f.width; ++x) {
                const size_t was = row.size();
                pixel(x, y, row);
                c[size_t(x)] = uint32_t(row.size() - was);
            }
        }
    });
    size_t total = 0;
    for (const auto& r : rows) total += r.size();
    f.samples.reserve(total);
    size_t i = 0;
    for (int y = 0; y < f.height; ++y) {
        for (int x = 0; x < f.width; ++x, ++i) f.offsets[i + 1] = f.offsets[i] + counts[size_t(y)][size_t(x)];
        f.samples.insert(f.samples.end(), rows[size_t(y)].begin(), rows[size_t(y)].end());
        std::vector<DeepSample>().swap(rows[size_t(y)]);
    }
    return f;
}

void tidyDeepPixel(std::vector<DeepSample>& samples) {
    samples.erase(std::remove_if(samples.begin(), samples.end(), [](const DeepSample& s) { return !visible(s); }), samples.end());
    if (samples.size() < 2) return;
    std::vector<float> cuts;
    cutsOf(samples, cuts);
    std::vector<DeepSample> pieces;
    split(samples, cuts, pieces);
    std::stable_sort(pieces.begin(), pieces.end(), before);
    samples.clear();
    for (size_t i = 0; i < pieces.size();) {
        size_t j = i + 1;
        while (j < pieces.size() && sameSpan(pieces[i], pieces[j])) ++j;
        samples.push_back(j - i == 1 ? pieces[i] : mix(&pieces[i], j - i));
        i = j;
    }
}

namespace {
// Each pixel's samples through `fn` (from several threads at once).
void eachPixel(DeepFrame& f, const std::function<void(int, int, std::vector<DeepSample>&)>& fn) {
    const DeepFrame& in = f;
    f = buildDeep(f.width, f.height, [&](int x, int y, std::vector<DeepSample>& out) {
        std::vector<DeepSample> s(in.at(x, y), in.at(x, y) + in.count(x, y));
        fn(x, y, s);
        out.insert(out.end(), s.begin(), s.end());
    });
}
}  // namespace

void tidyDeep(DeepFrame& f) {
    DeepFrame in = std::move(f);
    f = buildDeep(in.width, in.height, [&](int x, int y, std::vector<DeepSample>& out) {
        std::vector<DeepSample> s(in.at(x, y), in.at(x, y) + in.count(x, y));
        tidyDeepPixel(s);
        out.insert(out.end(), s.begin(), s.end());
    });
}

DeepFrame mergeDeep(const std::vector<const DeepFrame*>& layers) {
    std::vector<const DeepFrame*> use;
    for (const DeepFrame* l : layers)
        if (l && !l->empty()) use.push_back(l);
    if (use.empty()) return {};
    const int w = use[0]->width, h = use[0]->height;
    return buildDeep(w, h, [&](int x, int y, std::vector<DeepSample>& out) {
        std::vector<DeepSample> s;
        for (const DeepFrame* l : use)
            if (x < l->width && y < l->height) s.insert(s.end(), l->at(x, y), l->at(x, y) + l->count(x, y));
        tidyDeepPixel(s);
        out.insert(out.end(), s.begin(), s.end());
    });
}

std::vector<float> flattenDeep(const DeepFrame& f) {
    std::vector<float> rgba(size_t(std::max(0, f.width)) * size_t(std::max(0, f.height)) * 4, 0.0f);
    parallelRows(f.height, [&](int y0, int y1) {
        std::vector<DeepSample> s;
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < f.width; ++x) {
                s.assign(f.at(x, y), f.at(x, y) + f.count(x, y));
                if (!std::is_sorted(s.begin(), s.end(), before)) std::stable_sort(s.begin(), s.end(), before);
                double r = 0, g = 0, b = 0, a = 0;
                for (const DeepSample& d : s) {
                    const double t = 1 - a;
                    r += t * d.r, g += t * d.g, b += t * d.b, a += t * d.a;
                    if (a >= 1) break;
                }
                float* o = &rgba[(size_t(y) * size_t(f.width) + size_t(x)) * 4];
                o[0] = float(r), o[1] = float(g), o[2] = float(b), o[3] = float(std::min(a, 1.0));
            }
    });
    return rgba;
}

void holdOutDeep(DeepFrame& f, const DeepFrame& holdout) {
    DeepFrame in = std::move(f);
    f = buildDeep(in.width, in.height, [&](int x, int y, std::vector<DeepSample>& out) {
        std::vector<DeepSample> mine(in.at(x, y), in.at(x, y) + in.count(x, y));
        std::vector<DeepSample> theirs;
        if (x < holdout.width && y < holdout.height) theirs.assign(holdout.at(x, y), holdout.at(x, y) + holdout.count(x, y));
        if (theirs.empty()) {
            tidyDeepPixel(mine);
            out.insert(out.end(), mine.begin(), mine.end());
            return;
        }
        // Both cut at each other's depths, then taken front to back by span: what of this one shows there (its share
        // where both are), dimmed by what the holdout has in front.
        std::vector<DeepSample> all = mine;
        all.insert(all.end(), theirs.begin(), theirs.end());
        std::vector<float> cuts;
        cutsOf(all, cuts);
        std::vector<DeepSample> a, b;
        split(mine, cuts, a);
        split(theirs, cuts, b);
        struct Piece {
            DeepSample s;
            bool held;  // the holdout's
        };
        std::vector<Piece> pieces;
        for (const DeepSample& s : a)
            if (visible(s)) pieces.push_back({s, false});
        for (const DeepSample& s : b)
            if (visible(s)) pieces.push_back({s, true});
        std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& p, const Piece& q) { return before(p.s, q.s); });
        double through = 1;  // the holdout's transmission so far
        std::vector<DeepSample> span, share;
        for (size_t i = 0; i < pieces.size();) {
            size_t j = i + 1;
            while (j < pieces.size() && sameSpan(pieces[i].s, pieces[j].s)) ++j;
            span.clear();
            bool any = false, held = false;
            for (size_t k = i; k < j; ++k) span.push_back(pieces[k].s), any = any || !pieces[k].held, held = held || pieces[k].held;
            if (any) {
                DeepSample o = span[0];
                o.r = o.g = o.b = o.a = 0;
                if (held) {
                    mix(span.data(), span.size(), &share);
                } else {
                    share = span;
                }
                for (size_t k = i; k < j; ++k)
                    if (!pieces[k].held) {
                        const DeepSample& p = share[k - i];
                        o.r += p.r, o.g += p.g, o.b += p.b, o.a += p.a;
                    }
                // (several of this one's samples over one span: their mix, as tidying would make it)
                if (!held && j - i > 1) o = mix(span.data(), span.size());
                const float t = float(through);
                o.r *= t, o.g *= t, o.b *= t, o.a *= t;
                if (visible(o)) out.push_back(o);
            }
            if (held) {
                double keep = 1;
                for (size_t k = i; k < j; ++k)
                    if (pieces[k].held) keep *= 1 - std::min(1.0, double(pieces[k].s.a));
                through *= keep;
            }
            i = j;
        }
    });
}

void sliceDeep(DeepFrame& f, float near, float far) {
    if (far < near) std::swap(near, far);
    eachPixel(f, [&](int, int, std::vector<DeepSample>& s) {
        std::vector<DeepSample> pieces;
        split(s, {near, far}, pieces);
        s.clear();
        for (const DeepSample& d : pieces)
            if (d.z >= near && d.zBack <= far) s.push_back(d);
    });
}

void gradeDeep(DeepFrame& f, float near, float far, float soft, const std::function<void(float*, float)>& grade) {
    if (far < near) std::swap(near, far);
    soft = std::max(0.0f, soft);
    std::vector<float> cuts = {near - soft, near, far, far + soft};
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    eachPixel(f, [&](int, int, std::vector<DeepSample>& s) {
        std::vector<DeepSample> pieces;
        split(s, cuts, pieces);
        for (DeepSample& d : pieces) {
            const float z = (d.z + d.zBack) / 2;
            float w = z >= near && z <= far ? 1.0f : 0.0f;
            if (soft > 0 && z < near) w = std::max(0.0f, 1 - (near - z) / soft);
            if (soft > 0 && z > far) w = std::max(0.0f, 1 - (z - far) / soft);
            if (w <= 0) continue;
            float rgb[3] = {d.r, d.g, d.b};
            grade(rgb, w);
            d.r = rgb[0], d.g = rgb[1], d.b = rgb[2];
        }
        s = std::move(pieces);
    });
}

DeepFrame deepFromFlat(const float* rgba, int width, int height, const float* depth, float thickness) {
    return buildDeep(width, height, [&](int x, int y, std::vector<DeepSample>& out) {
        const size_t i = size_t(y) * size_t(width) + size_t(x);
        const float* p = rgba + i * 4;
        DeepSample s;
        s.r = p[0], s.g = p[1], s.b = p[2], s.a = std::clamp(p[3], 0.0f, 1.0f);
        if (!visible(s) || !std::isfinite(depth[i])) return;
        s.z = depth[i];
        s.zBack = depth[i] + std::max(0.0f, thickness);
        out.push_back(s);
    });
}

DeepFrame resampleDeep(const DeepFrame& from, int width, int height, const std::function<void(double, double, double&, double&)>& source,
                       float opacity) {
    return buildDeep(width, height, [&](int x, int y, std::vector<DeepSample>& out) {
        double u = -1, v = -1;
        source(x + 0.5, y + 0.5, u, v);
        const int sx = int(std::floor(u)), sy = int(std::floor(v));
        if (sx < 0 || sy < 0 || sx >= from.width || sy >= from.height) return;
        const DeepSample* s = from.at(sx, sy);
        for (size_t k = 0, n = from.count(sx, sy); k < n; ++k) {
            DeepSample d = s[k];
            if (opacity != 1) d.r *= opacity, d.g *= opacity, d.b *= opacity, d.a *= opacity;
            out.push_back(d);
        }
    });
}

}  // namespace montage
