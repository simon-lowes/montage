#include "Surround.h"

#include <algorithm>
#include <cmath>

namespace montage {

namespace {

double wrap(double a) {
    while (a > 180) a -= 360;
    while (a <= -180) a += 360;
    return a;
}

}  // namespace

const std::vector<Speaker>& layoutSpeakers(const std::string& layout) {
    static const std::vector<Speaker> stereo = {{"L", -30, false, 1.0}, {"R", 30, false, 1.0}};
    static const std::vector<Speaker> five = {{"L", -30, false, 1.0},  {"R", 30, false, 1.0},  {"C", 0, false, 1.0},
                                              {"LFE", 0, true, 0.0},   {"Ls", -110, false, 1.41}, {"Rs", 110, false, 1.41}};
    static const std::vector<Speaker> seven = {{"L", -30, false, 1.0},   {"R", 30, false, 1.0},    {"C", 0, false, 1.0},
                                               {"LFE", 0, true, 0.0},    {"Lb", -150, false, 1.41}, {"Rb", 150, false, 1.41},
                                               {"Ls", -90, false, 1.41}, {"Rs", 90, false, 1.41}};
    if (layout == "5.1") return five;
    if (layout == "7.1") return seven;
    return stereo;
}

int layoutChannels(const std::string& layout) { return int(layoutSpeakers(layout).size()); }

const std::vector<std::string>& audioLayouts() {
    static const std::vector<std::string> l = {"stereo", "5.1", "7.1"};
    return l;
}

std::vector<float> panGains(const std::string& layout, double angle, double distance) {
    const auto& sp = layoutSpeakers(layout);
    std::vector<float> g(sp.size(), 0.0f);
    // The main speakers round the circle, by angle.
    std::vector<size_t> ring;
    for (size_t i = 0; i < sp.size(); ++i)
        if (!sp[i].lfe) ring.push_back(i);
    std::sort(ring.begin(), ring.end(), [&](size_t a, size_t b) { return sp[a].angle < sp[b].angle; });
    std::vector<double> pair(sp.size(), 0.0);
    angle = wrap(angle);
    if (layout != "5.1" && layout != "7.1") {
        // Stereo: the front pair only, held at its ends.
        const double u = (std::clamp(angle, -30.0, 30.0) + 30) / 60;
        pair[ring.front()] = std::cos(u * M_PI / 2);
        pair[ring.back()] = std::sin(u * M_PI / 2);
    } else {
        // Between the two speakers either side of the angle, going round past the back.
        const size_t n = ring.size();
        for (size_t k = 0; k < n; ++k) {
            const size_t a = ring[k], b = ring[(k + 1) % n];
            double from = sp[a].angle, to = sp[b].angle;
            if (to <= from) to += 360;
            double x = angle;
            if (x < from) x += 360;
            if (x >= from && x <= to) {
                const double u = (x - from) / (to - from);
                pair[a] = std::cos(u * M_PI / 2);
                pair[b] = std::sin(u * M_PI / 2);
                break;
            }
        }
    }
    // Nearer the middle, spread over every main speaker (power kept constant).
    const double r = std::clamp(distance, 0.0, 1.0);
    for (size_t i : ring) g[i] = float(std::sqrt(r * pair[i] * pair[i] + (1 - r) / double(ring.size())));
    return g;
}

SurroundGains surroundGains(const std::string& layout, const SurroundPan& p) {
    SurroundGains out;
    const double angle = std::atan2(p.x, p.y) * 180 / M_PI;
    const double distance = std::min(1.0, std::hypot(p.x, p.y));
    const double width = std::clamp(p.width, 0.0, 1.0);
    const double half = 30 * width;
    out.left = panGains(layout, angle - half, distance);
    out.right = panGains(layout, angle + half, distance);
    // Drawn together, the two channels add up: a sound in both stays as loud as it was on two speakers.
    const float k = float(std::sqrt(0.5 + 0.5 * width));
    for (float& g : out.left) g *= k;
    for (float& g : out.right) g *= k;
    out.lfe = p.lfeDb <= -99 ? 0.0f : float(std::pow(10.0, p.lfeDb / 20));
    return out;
}

void downmixToStereo(const std::string& layout, const float* in, int frames, float* out) {
    const auto& sp = layoutSpeakers(layout);
    const int n = int(sp.size());
    if (n == 2) {
        std::copy(in, in + size_t(frames) * 2, out);
        return;
    }
    std::vector<std::pair<float, float>> w(static_cast<size_t>(n));
    for (int c = 0; c < n; ++c) {
        const std::string name = sp[size_t(c)].name;
        if (sp[size_t(c)].lfe) w[size_t(c)] = {0, 0};
        else if (name == "L") w[size_t(c)] = {1, 0};
        else if (name == "R") w[size_t(c)] = {0, 1};
        else if (name == "C") w[size_t(c)] = {0.7071f, 0.7071f};
        else if (name[0] == 'L') w[size_t(c)] = {0.7071f, 0};
        else w[size_t(c)] = {0, 0.7071f};
    }
    for (int i = 0; i < frames; ++i) {
        float l = 0, r = 0;
        for (int c = 0; c < n; ++c) {
            const float v = in[size_t(i) * size_t(n) + size_t(c)];
            l += v * w[size_t(c)].first;
            r += v * w[size_t(c)].second;
        }
        out[size_t(i) * 2] = l;
        out[size_t(i) * 2 + 1] = r;
    }
}

}  // namespace montage
