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

Effect* paramOwner(Clip& c, const ParamAddress& a) {
    switch (a.slot) {
        case ParamSlot::Motion: return &c.motion;
        case ParamSlot::Audio: return &c.audio;
        case ParamSlot::Timing: return &c.timing;
        case ParamSlot::Generator: return &c.generator;
        case ParamSlot::Effect:
            for (Effect& e : c.effects)
                if (e.id == a.effect) return &e;
    }
    return nullptr;
}

const Effect* paramOwner(const Clip& c, const ParamAddress& a) { return paramOwner(const_cast<Clip&>(c), a); }

Param* findParam(Clip& c, const ParamAddress& a) {
    Effect* e = paramOwner(c, a);
    if (!e) return nullptr;
    const auto it = e->params.find(a.param);
    return it == e->params.end() ? nullptr : &it->second;
}

const Param* findParam(const Clip& c, const ParamAddress& a) { return findParam(const_cast<Clip&>(c), a); }

std::pair<FrameTime, FrameTime> shiftRange(const Param& p, const std::vector<FrameTime>& keys, FrameTime last) {
    FrameTime down = -(FrameTime(1) << 40), up = FrameTime(1) << 40;
    auto moving = [&](FrameTime t) { return std::find(keys.begin(), keys.end(), t) != keys.end(); };
    for (size_t i = 0; i < p.keys.size(); ++i) {
        if (!moving(p.keys[i].t)) continue;
        const FrameTime t = p.keys[i].t;
        down = std::max(down, -t);
        up = std::min(up, last - t);
        // The nearest keys that stay put, either side.
        for (size_t j = i; j-- > 0;)
            if (!moving(p.keys[j].t)) {
                down = std::max(down, p.keys[j].t + 1 - t);
                break;
            }
        for (size_t j = i + 1; j < p.keys.size(); ++j)
            if (!moving(p.keys[j].t)) {
                up = std::min(up, p.keys[j].t - 1 - t);
                break;
            }
    }
    if (down > 0) down = 0;
    if (up < 0) up = 0;
    return {down, up};
}

void shiftKeys(Param& p, const std::vector<FrameTime>& keys, FrameTime delta) {
    if (delta == 0) return;
    for (Keyframe& k : p.keys)
        if (std::find(keys.begin(), keys.end(), k.t) != keys.end()) k.t += delta;
    std::stable_sort(p.keys.begin(), p.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
}

}  // namespace montage
