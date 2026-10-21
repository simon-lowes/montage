#include "FilmScan.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>

namespace montage {

namespace {

// Printing density's conversion (10-bit log scans): 0.002 density a code value, reference white 685 and black 95 (1.18
// density between them); a scan holds up to 1023 codes of density.
constexpr double kDensityPerCode = 0.002, kWhiteDensity = (685 - 95) * kDensityPerCode, kMostDensity = 1023 * kDensityPerCode;

// The light through the film from a code value, and back.
double transmittance(double v, ScanEncoding e, Transfer t) {
    switch (e) {
        case ScanEncoding::Linear: return v;
        case ScanEncoding::PrintingDensity: return std::pow(10.0, -v * 1023 * kDensityPerCode);
        default: return toLinear(t, v);
    }
}
double codeValue(double light, ScanEncoding e, Transfer t) {
    switch (e) {
        case ScanEncoding::Linear: return light;
        case ScanEncoding::PrintingDensity: return -std::log10(std::max(light, 1e-12)) / (1023 * kDensityPerCode);
        default: return fromLinear(t, light);
    }
}

// Each pixel's colour unpremultiplied, changed by `fn`, premultiplied again.
template <class Fn>
void eachColour(Image& img, Fn fn) {
    parallelRows(img.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            float* p = img.row(y);
            for (int x = 0; x < img.width; ++x, p += 4) {
                const float a = p[3];
                if (a <= 0) continue;
                double c[3] = {p[0] / a, p[1] / a, p[2] / a};
                fn(c);
                for (int i = 0; i < 3; ++i) p[i] = float(c[i] * a);
            }
        }
    });
}

// The value `q` of the way up `v` (0..1).
float percentile(std::vector<float>& v, double q) {
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + long(k), v.end());
    return v[k];
}

// A step over rows and columns that reads about `n` pixels of the picture.
int strideFor(const Image& img, double n) { return std::max(1, int(std::sqrt(double(img.width) * img.height / n))); }

}  // namespace

// ---- Negatives -----------------------------------------------------------------------------------------------------------

bool filmBase(const Image& img, ScanEncoding encoding, Transfer transfer, double base[3]) {
    if (img.empty()) return false;
    const int step = strideFor(img, 250000), gw = (img.width + step - 1) / step, gh = (img.height + step - 1) / step;
    std::vector<std::array<float, 3>> light(size_t(gw) * gh);
    std::vector<char> valid(light.size(), 0);
    float brightest = 0;
    for (int gy = 0; gy < gh; ++gy)
        for (int gx = 0; gx < gw; ++gx) {
            const float* p = img.at(gx * step, gy * step);
            if (p[3] <= 0) continue;
            std::array<float, 3>& t = light[size_t(gy) * gw + gx];
            for (int i = 0; i < 3; ++i) t[size_t(i)] = float(transmittance(p[i] / p[3], encoding, transfer));
            brightest = std::max(brightest, *std::max_element(t.begin(), t.end()));
            valid[size_t(gy) * gw + gx] = 1;
        }
    // Not the holes (as clear as the scan gets, the same in every channel), nor 4 pixels round them, where their edges
    // and a video file's softer colour mix them with the base.
    std::vector<char> hole(light.size(), 0), nearHole(light.size(), 0);
    for (size_t i = 0; i < light.size(); ++i) {
        const auto& t = light[i];
        const float lo = *std::min_element(t.begin(), t.end()), hi = *std::max_element(t.begin(), t.end());
        hole[i] = valid[i] && hi > 0.9f * brightest && lo > 0.85f * hi;
    }
    const int r = std::max(1, (4 + step - 1) / step);
    for (int gy = 0; gy < gh; ++gy)
        for (int gx = 0; gx < gw; ++gx)
            if (hole[size_t(gy) * gw + gx])
                for (int y = std::max(0, gy - r); y <= std::min(gh - 1, gy + r); ++y)
                    for (int x = std::max(0, gx - r); x <= std::min(gw - 1, gx + r); ++x) nearHole[size_t(y) * gw + x] = 1;
    // The clearest film: the clearest 2 % of what is left, each channel's median there (the rebate is most of it when it is
    // in the frame; the median, unlike the very clearest, is not lifted by the scan's noise).
    auto clearest = [&](bool holes, double out[3]) {
        std::vector<float> sums;
        for (size_t i = 0; i < light.size(); ++i)
            if (valid[i] && !(holes && nearHole[i])) sums.push_back(light[i][0] + light[i][1] + light[i][2]);
        if (sums.empty()) return false;
        const float cut = percentile(sums, 0.98);
        std::vector<float> channel[3];
        for (size_t i = 0; i < light.size(); ++i)
            if (valid[i] && !(holes && nearHole[i]) && light[i][0] + light[i][1] + light[i][2] >= cut)
                for (int c = 0; c < 3; ++c) channel[c].push_back(light[i][size_t(c)]);
        for (int c = 0; c < 3; ++c) out[c] = percentile(channel[c], 0.5);
        return true;
    };
    // The bright neutral film is holes when perforations are found, or when the film left is coloured (a colour negative's
    // mask); a black-and-white negative with no holes in the frame has a clear base as neutral as they would be.
    double clear[3];
    if (!clearest(true, clear) && !clearest(false, clear)) return false;
    const double lo = std::min({clear[0], clear[1], clear[2]}), hi = std::max({clear[0], clear[1], clear[2]});
    PerfSettings left, right;
    right.side = 1;
    if (lo > 0.85 * hi && findPerforations(img, left).empty() && findPerforations(img, right).empty()) clearest(false, clear);
    for (int i = 0; i < 3; ++i) base[i] = codeValue(clear[i], encoding, transfer);
    return true;
}

void invertNegative(Image& img, const NegativeSettings& s, Transfer transfer) {
    const double gamma = std::max(0.05, s.gamma), black = std::pow(10.0, -kWhiteDensity / gamma), gain = std::exp2(s.exposure);
    double baseLight[3];
    for (int i = 0; i < 3; ++i) baseLight[i] = std::max(1e-9, transmittance(s.base[i], s.encoding, transfer));
    eachColour(img, [&](double c[3]) {
        for (int i = 0; i < 3; ++i) {
            // (Up to the most density a scan holds: an opaque speck or the scanner's black stays a bright white.)
            const double d = std::clamp(std::log10(baseLight[i] / std::max(1e-9, transmittance(c[i], s.encoding, transfer))), 0.0, kMostDensity);
            const double light = (std::pow(10.0, (d - kWhiteDensity) / gamma) - black) / (1 - black) * gain;
            c[i] = fromLinear(transfer, light);
        }
    });
}

void makeNegative(Image& img, const NegativeSettings& s, Transfer transfer) {
    const double gamma = std::max(0.05, s.gamma), black = std::pow(10.0, -kWhiteDensity / gamma), gain = std::exp2(s.exposure);
    double baseLight[3];
    for (int i = 0; i < 3; ++i) baseLight[i] = transmittance(s.base[i], s.encoding, transfer);
    eachColour(img, [&](double c[3]) {
        for (int i = 0; i < 3; ++i) {
            const double light = std::max(0.0, toLinear(transfer, c[i]) / gain);
            const double d = gamma * std::log10(light * (1 - black) + black) + kWhiteDensity;
            c[i] = codeValue(baseLight[i] * std::pow(10.0, -d), s.encoding, transfer);
        }
    });
}

// ---- Fading --------------------------------------------------------------------------------------------------------------

FadeLevels fadeLevels(const Image& img) {
    FadeLevels f;
    if (img.empty()) return f;
    const int step = strideFor(img, 250000);
    std::vector<float> v[3];
    for (int y = 0; y < img.height; y += step)
        for (int x = 0; x < img.width; x += step) {
            const float* p = img.at(x, y);
            if (p[3] <= 0) continue;
            for (int i = 0; i < 3; ++i) v[i].push_back(p[i] / p[3]);
        }
    if (v[0].empty()) return f;
    for (int i = 0; i < 3; ++i) {
        f.lo[i] = percentile(v[i], 0.005);
        f.mid[i] = percentile(v[i], 0.5);
        f.hi[i] = percentile(v[i], 0.995);
    }
    return f;
}

FadeLevels meanFadeLevels(const std::vector<FadeLevels>& levels) {
    FadeLevels m;
    if (levels.empty()) return m;
    for (int i = 0; i < 3; ++i) {
        double lo = 0, mid = 0, hi = 0;
        for (const FadeLevels& f : levels) lo += f.lo[i], mid += f.mid[i], hi += f.hi[i];
        const double n = double(levels.size());
        m.lo[i] = float(lo / n), m.mid[i] = float(mid / n), m.hi[i] = float(hi / n);
    }
    return m;
}

void restoreFade(Image& img, const FadeLevels& f, double strength) {
    strength = std::clamp(strength, 0.0, 1.0);
    if (strength <= 0) return;
    // The channel that kept most of its range is the one the others are brought to.
    int ref = 0;
    for (int i = 1; i < 3; ++i)
        if (f.hi[i] - f.lo[i] > f.hi[ref] - f.lo[ref]) ref = i;
    auto middle = [&](int i) { return std::clamp((f.mid[i] - f.lo[i]) / std::max(1e-4f, f.hi[i] - f.lo[i]), 0.02f, 0.98f); };
    double gamma[3];
    for (int i = 0; i < 3; ++i) gamma[i] = std::clamp(double(std::log(middle(ref)) / std::log(middle(i))), 0.25, 4.0);
    const double lo = f.lo[ref], range = f.hi[ref] - f.lo[ref];
    if (range <= 1e-4) return;
    eachColour(img, [&](double c[3]) {
        for (int i = 0; i < 3; ++i) {
            const double n = (c[i] - f.lo[i]) / std::max(1e-4f, f.hi[i] - f.lo[i]);
            // (Straight on beyond the levels, with the slope the curve has at its ends.)
            const double m = n <= 0 ? n : n >= 1 ? 1 + (n - 1) * gamma[i] : std::pow(n, gamma[i]);
            c[i] += strength * (lo + range * m - c[i]);
        }
    });
}

// ---- Perforations --------------------------------------------------------------------------------------------------------

std::vector<Point2> findPerforations(const Image& img, const PerfSettings& s, std::vector<float>* found) {
    std::vector<Point2> out;
    const int W = img.width, H = img.height;
    if (found) found->assign(size_t(std::max(0, W)) * std::max(0, H), 0.0f);
    const int sw = std::clamp(int(std::lround(s.rebate * W)), 0, W);
    if (img.empty() || sw < 4 || H < 8) return out;
    const int x0 = s.side == 1 ? W - sw : 0;
    std::vector<float> Y(size_t(sw) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < sw; ++x) {
            const float* p = img.at(x0 + x, y);
            Y[size_t(y) * sw + x] = (p[0] + p[1] + p[2]) / 3;
        }
    std::vector<float> sorted = Y;
    const float floor = percentile(sorted, 0.5), top = *std::max_element(Y.begin(), Y.end());
    if (top - floor < 0.05f) return out;
    const float cut = (floor + top) / 2;
    // The holes: what is brighter than halfway from the rebate to the brightest, in pieces (4-connected).
    std::vector<int> label(Y.size(), -1);
    struct Piece {
        int x0, y0, x1, y1, area;
        bool edge;
        std::vector<float> levels;
    };
    std::vector<Piece> pieces;
    std::vector<int> stack;
    for (size_t i = 0; i < Y.size(); ++i) {
        if (Y[i] <= cut || label[i] >= 0) continue;
        Piece pc{sw, H, -1, -1, 0, false, {}};
        const int id = int(pieces.size());
        label[i] = id;
        stack.assign(1, int(i));
        while (!stack.empty()) {
            const int j = stack.back();
            stack.pop_back();
            const int x = j % sw, y = j / sw;
            pc.x0 = std::min(pc.x0, x), pc.y0 = std::min(pc.y0, y), pc.x1 = std::max(pc.x1, x), pc.y1 = std::max(pc.y1, y);
            ++pc.area;
            pc.levels.push_back(Y[size_t(j)]);
            pc.edge = pc.edge || x == 0 || y == 0 || x == sw - 1 || y == H - 1;
            const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (const auto& n : nb)
                if (n[0] >= 0 && n[1] >= 0 && n[0] < sw && n[1] < H) {
                    const size_t k = size_t(n[1]) * sw + size_t(n[0]);
                    if (Y[k] > cut && label[k] < 0) label[k] = id, stack.push_back(int(k));
                }
        }
        pieces.push_back(std::move(pc));
    }
    // Whole holes of about one size, filling most of their box (bright picture in the rebate is neither).
    std::vector<int> areas;
    for (const Piece& p : pieces)
        if (!p.edge && p.area >= 12 && p.area >= 0.6 * (p.x1 - p.x0 + 1) * (p.y1 - p.y0 + 1)) areas.push_back(p.area);
    if (areas.empty()) return out;
    std::nth_element(areas.begin(), areas.begin() + long(areas.size() / 2), areas.end());
    const int typical = areas[areas.size() / 2];
    for (Piece& p : pieces) {
        if (p.edge || p.area < 12 || p.area < 0.6 * (p.x1 - p.x0 + 1) * (p.y1 - p.y0 + 1) || p.area < typical / 2 || p.area > typical * 2)
            continue;
        // Its centre to a fraction of a pixel: each pixel round it weighed by how far it is from the rebate's level
        // to the hole's (unclamped, so the noise on the rebate weighs nothing on the whole), so the soft edges count as
        // much as they cover.
        // (The rebate's level round this hole: the median of its box's border, so picture in the strip does not count.)
        const int bx0 = std::max(0, p.x0 - 3), bx1 = std::min(sw - 1, p.x1 + 3), by0 = std::max(0, p.y0 - 3), by1 = std::min(H - 1, p.y1 + 3);
        std::vector<float> border;
        for (int y = by0; y <= by1; ++y)
            for (int x = bx0; x <= bx1; ++x)
                if (y == by0 || y == by1 || x == bx0 || x == bx1) border.push_back(Y[size_t(y) * sw + x]);
        const float hole = percentile(p.levels, 0.9), low = std::min(percentile(border, 0.5), cut);
        if (hole - low <= 0) continue;
        double sw0 = 0, sx = 0, sy = 0;
        for (int y = by0; y <= by1; ++y)
            for (int x = bx0; x <= bx1; ++x) {
                const double w = (Y[size_t(y) * sw + x] - low) / (hole - low);
                sw0 += w, sx += w * x, sy += w * y;
                if (found && w > 0) (*found)[size_t(y) * W + x0 + x] = std::max((*found)[size_t(y) * W + x0 + x], float(std::min(w, 1.0)));
            }
        if (sw0 > 0) out.push_back({x0 + sx / sw0, sy / sw0});
    }
    std::sort(out.begin(), out.end(), [](const Point2& a, const Point2& b) { return a.y < b.y; });
    return out;
}

bool perforationOffset(const std::vector<Point2>& now, const std::vector<Point2>& reference, double& dx, double& dy) {
    dx = dy = 0;
    if (now.empty() || reference.empty()) return false;
    // (Half the spacing of the reference's holes: a hole moved further than that is taken for its neighbour.)
    double reach = 1e9;
    for (size_t i = 1; i < reference.size(); ++i) reach = std::min(reach, std::hypot(reference[i].x - reference[i - 1].x, reference[i].y - reference[i - 1].y) / 2);
    // One hole to go by (a format with a hole a frame) gives no spacing: only the hole nearest it is taken for it, not
    // every hole in the frame.
    if (reference.size() == 1) {
        const Point2& r = reference[0];
        const auto nearest = std::min_element(now.begin(), now.end(), [&](const Point2& a, const Point2& b) {
            return std::hypot(a.x - r.x, a.y - r.y) < std::hypot(b.x - r.x, b.y - r.y);
        });
        dx = nearest->x - r.x, dy = nearest->y - r.y;
        return true;
    }
    double sx = 0, sy = 0;
    int n = 0;
    for (const Point2& p : now) {
        const Point2* best = nullptr;
        double d = reach;
        for (const Point2& r : reference)
            if (const double e = std::hypot(p.x - r.x, p.y - r.y); e < d) d = e, best = &r;
        if (best) sx += p.x - best->x, sy += p.y - best->y, ++n;
    }
    if (!n) return false;
    dx = sx / n, dy = sy / n;
    return true;
}

std::string perforationsToString(const std::vector<Point2>& perfs, int W, int H) {
    std::ostringstream o;
    o.precision(7);
    // (Pixel centres at half a pixel, so the fractions hold at any size the frame is read at.)
    for (const Point2& p : perfs) o << (p.x + 0.5) / std::max(1, W) << ' ' << (p.y + 0.5) / std::max(1, H) << '\n';
    return o.str();
}

std::vector<Point2> perforationsFromString(const std::string& s, int W, int H) {
    std::vector<Point2> out;
    std::istringstream in(s);
    double x = 0, y = 0;
    while (out.size() < 64 && in >> x >> y)
        if (std::isfinite(x) && std::isfinite(y)) out.push_back({x * W - 0.5, y * H - 0.5});
    return out;
}

}  // namespace montage
