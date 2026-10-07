#include "KeyframeEdit.h"

#include <algorithm>
#include <cmath>

namespace montage {

namespace {
double amplitude(double db) { return std::pow(10.0, db / 20.0); }
}  // namespace

double gainToLevel(double db) {
    if (db <= kGainLineMinDb) return 0;
    return std::clamp(std::sqrt(amplitude(db) / amplitude(kGainLineMaxDb)), 0.0, 1.0);
}

double levelToGain(double level) {
    if (level <= 0) return kGainLineMinDb;
    const double db = 20.0 * std::log10(level * level * amplitude(kGainLineMaxDb));
    return std::clamp(db, kGainLineMinDb, kGainLineMaxDb);
}

void offsetLine(Param& p, FrameTime t, double delta, double lo, double hi) {
    auto shift = [&](double& v) { v = std::clamp(v + delta, lo, hi); };
    if (p.keys.empty()) {
        shift(p.value);
        return;
    }
    if (t <= p.keys.front().t) {
        shift(p.keys.front().v);
        return;
    }
    if (t >= p.keys.back().t) {
        shift(p.keys.back().v);
        return;
    }
    const auto next = std::upper_bound(p.keys.begin(), p.keys.end(), t, [](FrameTime tt, const Keyframe& k) { return tt < k.t; });
    shift(next->v);
    shift((next - 1)->v);
}

FrameTime moveKey(Param& p, FrameTime from, FrameTime to, double v, FrameTime last) {
    const auto it = std::find_if(p.keys.begin(), p.keys.end(), [from](const Keyframe& k) { return k.t == from; });
    if (it == p.keys.end()) return -1;
    FrameTime lo = 0, hi = std::max<FrameTime>(0, last);
    if (it != p.keys.begin()) lo = (it - 1)->t + 1;
    if (it + 1 != p.keys.end()) hi = (it + 1)->t - 1;
    it->t = std::clamp(to, lo, std::max(lo, hi));
    it->v = v;
    return it->t;
}

}  // namespace montage
