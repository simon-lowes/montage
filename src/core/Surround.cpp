#include "Surround.h"

#include <algorithm>
#include <cmath>

#include "Ambisonics.h"

namespace montage {

namespace {

double wrap(double a) {
    while (a > 180) a -= 360;
    while (a <= -180) a += 360;
    return a;
}

}  // namespace

const std::vector<Speaker>& layoutSpeakers(const std::string& layout) {
    static const std::vector<Speaker> stereo = {{"L", -30, false, 1.0, 0, "M+030"}, {"R", 30, false, 1.0, 0, "M-030"}};
    static const std::vector<Speaker> five = {{"L", -30, false, 1.0, 0, "M+030"},   {"R", 30, false, 1.0, 0, "M-030"},
                                              {"C", 0, false, 1.0, 0, "M+000"},     {"LFE", 0, true, 0.0, 0, "LFE1"},
                                              {"Ls", -110, false, 1.41, 0, "M+110"}, {"Rs", 110, false, 1.41, 0, "M-110"}};
    static const std::vector<Speaker> seven = {{"L", -30, false, 1.0, 0, "M+030"},   {"R", 30, false, 1.0, 0, "M-030"},
                                               {"C", 0, false, 1.0, 0, "M+000"},     {"LFE", 0, true, 0.0, 0, "LFE1"},
                                               {"Lb", -150, false, 1.41, 0, "M+135"}, {"Rb", 150, false, 1.41, 0, "M-135"},
                                               {"Ls", -90, false, 1.41, 0, "M+090"},  {"Rs", 90, false, 1.41, 0, "M-090"}};
    // Immersive: BS.2051's positions (2+5+0, 4+5+0, 4+7+0; 7.1.2 as 0+7+0 with the pair above at the sides).
    auto with = [](std::vector<Speaker> base, std::initializer_list<Speaker> extra) {
        base.insert(base.end(), extra);
        return base;
    };
    static const std::vector<Speaker> sevenBs = {{"L", -30, false, 1.0, 0, "M+030"},   {"R", 30, false, 1.0, 0, "M-030"},
                                                 {"C", 0, false, 1.0, 0, "M+000"},     {"LFE", 0, true, 0.0, 0, "LFE1"},
                                                 {"Lb", -135, false, 1.41, 0, "M+135"}, {"Rb", 135, false, 1.41, 0, "M-135"},
                                                 {"Ls", -90, false, 1.41, 0, "M+090"},  {"Rs", 90, false, 1.41, 0, "M-090"}};
    static const std::vector<Speaker> fiveTwo =
        with(five, {{"Ltf", -30, false, 1.0, 30, "U+030"}, {"Rtf", 30, false, 1.0, 30, "U-030"}});
    static const std::vector<Speaker> fiveFour = with(five, {{"Ltf", -30, false, 1.0, 30, "U+030"}, {"Rtf", 30, false, 1.0, 30, "U-030"},
                                                             {"Ltr", -110, false, 1.0, 30, "U+110"}, {"Rtr", 110, false, 1.0, 30, "U-110"}});
    static const std::vector<Speaker> sevenTwo =  // overhead at the sides ("top middle"), as Dolby's 7.1.2 bed
        with(sevenBs, {{"Lts", -90, false, 1.0, 30, "U+090"}, {"Rts", 90, false, 1.0, 30, "U-090"}});
    static const std::vector<Speaker> sevenFour = with(sevenBs, {{"Ltf", -45, false, 1.0, 30, "U+045"}, {"Rtf", 45, false, 1.0, 30, "U-045"},
                                                                 {"Ltr", -135, false, 1.0, 30, "U+135"}, {"Rtr", 135, false, 1.0, 30, "U-135"}});
    if (layout == "5.1") return five;
    if (layout == "7.1") return seven;
    if (layout == "5.1.2") return fiveTwo;
    if (layout == "5.1.4") return fiveFour;
    if (layout == "7.1.2") return sevenTwo;
    if (layout == "7.1.4") return sevenFour;
    // Ambisonics: the field's components, measured for loudness by the omnidirectional W alone.
    static const std::vector<Speaker> ambix = {{"W", 0, false, 1.0, 0, ""}, {"Y", 0, false, 0.0, 0, ""},
                                               {"Z", 0, false, 0.0, 0, ""}, {"X", 0, false, 0.0, 0, ""}};
    if (layout == "ambix") return ambix;
    return stereo;
}

int layoutChannels(const std::string& layout) { return int(layoutSpeakers(layout).size()); }

const std::vector<std::string>& audioLayouts() {
    static const std::vector<std::string> l = {"stereo", "5.1", "7.1", "5.1.2", "5.1.4", "7.1.2", "7.1.4", "ambix"};
    return l;
}

bool ambisonicLayout(const std::string& layout) { return layout == "ambix"; }

bool immersiveLayout(const std::string& layout) {
    for (const Speaker& sp : layoutSpeakers(layout))
        if (sp.elevation > 0) return true;
    return false;
}

std::string earLevelLayout(const std::string& layout) {
    if (layout == "5.1.2" || layout == "5.1.4") return "5.1";
    if (layout == "7.1.2" || layout == "7.1.4") return "7.1";
    return layout;
}

namespace {

// Gains round one ring of speakers: between the two either side of the angle (going round past the back), or for a
// stereo pair between the two and held at their ends; nearer the middle, spread over all of them (power constant).
void ringGains(const std::vector<Speaker>& sp, std::vector<size_t> ring, double angle, double distance, bool pairOnly, double scale,
               std::vector<float>& g) {
    if (ring.empty()) return;
    std::sort(ring.begin(), ring.end(), [&](size_t a, size_t b) { return sp[a].angle < sp[b].angle; });
    std::vector<double> pair(sp.size(), 0.0);
    angle = wrap(angle);
    if (ring.size() == 1) {
        pair[ring.front()] = 1;
    } else if (pairOnly) {
        const double from = sp[ring.front()].angle, to = sp[ring.back()].angle;
        const double u = (std::clamp(angle, from, to) - from) / (to - from);
        pair[ring.front()] = std::cos(u * M_PI / 2);
        pair[ring.back()] = std::sin(u * M_PI / 2);
    } else {
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
    const double r = std::clamp(distance, 0.0, 1.0);
    for (size_t i : ring) g[i] = float(scale * std::sqrt(r * pair[i] * pair[i] + (1 - r) / double(ring.size())));
}

}  // namespace

std::vector<float> panGains(const std::string& layout, double angle, double distance, double height) {
    if (ambisonicLayout(layout)) {
        // From the direction, overhead at height 1; the nearer the middle, the more it is heard all round.
        const auto g = foaEncode(angle, std::clamp(height, 0.0, 1.0) * 90, std::clamp(distance, 0.0, 1.0));
        return {g.begin(), g.end()};
    }
    const auto& sp = layoutSpeakers(layout);
    std::vector<float> g(sp.size(), 0.0f);
    std::vector<size_t> ear, top;
    for (size_t i = 0; i < sp.size(); ++i)
        if (!sp[i].lfe) (sp[i].elevation > 0 ? top : ear).push_back(i);
    // Stereo keeps to the front pair; a surround layout pans all the way round. Overhead, the same round the
    // speakers above, crossfaded with the ear-level ring at constant power.
    const double h = top.empty() ? 0.0 : std::clamp(height, 0.0, 1.0);
    ringGains(sp, ear, angle, distance, sp.size() == 2, std::cos(h * M_PI / 2), g);
    if (h > 0) ringGains(sp, top, angle, distance, false, std::sin(h * M_PI / 2), g);
    return g;
}

SurroundGains surroundGains(const std::string& layout, const SurroundPan& p) {
    SurroundGains out;
    const double angle = std::atan2(p.x, p.y) * 180 / M_PI;
    const double distance = std::min(1.0, std::hypot(p.x, p.y));
    const double width = std::clamp(p.width, 0.0, 1.0);
    const double half = 30 * width;
    out.left = panGains(layout, angle - half, distance, p.z);
    out.right = panGains(layout, angle + half, distance, p.z);
    // Drawn together, the two channels add up: a sound in both stays as loud as it was on two speakers. In an
    // ambisonic field both always add in W (as in phase as the sound is), so each goes in 3 dB down at any width: a
    // sound in both is then as loud as on two speakers, and W keeps the headroom a front stereo pair would take.
    const float k = float(ambisonicLayout(layout) ? std::sqrt(0.5) : std::sqrt(0.5 + 0.5 * width));
    for (float& g : out.left) g *= k;
    for (float& g : out.right) g *= k;
    out.lfe = p.lfeDb <= -99 ? 0.0f : float(std::pow(10.0, p.lfeDb / 20));
    return out;
}

void downmixToStereo(const std::string& layout, const float* in, int frames, float* out) {
    if (ambisonicLayout(layout)) {
        for (int i = 0; i < frames; ++i) foaDecodeStereo(in + size_t(i) * 4, out[size_t(i) * 2], out[size_t(i) * 2 + 1]);
        return;
    }
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
