#include "KeyframeEdit.h"

#include <algorithm>
#include <limits>
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

bool easeKey(Param& p, FrameTime t, bool in, bool out) {
    const auto it = std::find_if(p.keys.begin(), p.keys.end(), [t](const Keyframe& k) { return k.t == t; });
    if (it == p.keys.end()) return false;
    const size_t i = size_t(it - p.keys.begin());
    if (in && i > 0) {
        Keyframe& prev = p.keys[i - 1];
        Keyframe& k = p.keys[i];
        const double seg = double(k.t - prev.t);
        if (prev.interp != Interp::Hold) {
            if (prev.interp != Interp::Bezier && prev.outDt == 0 && prev.outDv == 0) {
                // Keep how the segment left the key before: straight, or flat if it was smooth.
                prev.outDt = seg / 3;
                prev.outDv = prev.interp == Interp::Linear ? (k.v - prev.v) / 3 : 0;
            }
            prev.interp = Interp::Bezier;
            k.inDt = -seg / 3;
            k.inDv = 0;
        }
    }
    if (out && i + 1 < p.keys.size()) {
        Keyframe& k = p.keys[i];
        Keyframe& next = p.keys[i + 1];
        const double seg = double(next.t - k.t);
        if (k.interp != Interp::Bezier && next.inDt == 0 && next.inDv == 0) {
            next.inDt = -seg / 3;
            next.inDv = k.interp == Interp::Linear ? -(next.v - k.v) / 3 : 0;
        }
        k.interp = Interp::Bezier;
        k.outDt = seg / 3;
        k.outDv = 0;
    }
    return true;
}

bool setKeyHandle(Param& p, FrameTime t, bool out, double dt, double dv, bool linked) {
    const auto it = std::find_if(p.keys.begin(), p.keys.end(), [t](const Keyframe& k) { return k.t == t; });
    if (it == p.keys.end()) return false;
    const size_t i = size_t(it - p.keys.begin());
    double inDt, inDv, outDt, outDv;
    keyHandles(p.keys, i, inDt, inDv, outDt, outDv);
    Keyframe& k = p.keys[i];
    if (out) {
        if (i + 1 >= p.keys.size()) return false;
        k.outDt = std::clamp(dt, 0.0, double(p.keys[i + 1].t - k.t));
        k.outDv = dv;
        k.interp = Interp::Bezier;
        if (linked && i > 0 && k.outDt > 1e-9) {
            k.inDt = inDt;
            k.inDv = k.outDv / k.outDt * inDt;
        }
    } else {
        if (i == 0) return false;
        k.inDt = std::clamp(dt, -double(k.t - p.keys[i - 1].t), 0.0);
        k.inDv = dv;
        p.keys[i - 1].interp = Interp::Bezier;
        if (linked && i + 1 < p.keys.size() && k.inDt < -1e-9) {
            k.outDt = outDt;
            k.outDv = k.inDv / k.inDt * outDt;
        }
    }
    return true;
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

CopiedKeys copyKeys(const Clip& c, const std::vector<std::pair<ParamAddress, FrameTime>>& keys) {
    CopiedKeys out;
    FrameTime first = std::numeric_limits<FrameTime>::max();
    for (const auto& [a, t] : keys)
        if (const Param* p = findParam(c, a); p && p->keyAt(t)) first = std::min(first, t);
    for (const auto& [a, t] : keys) {
        const Param* p = findParam(c, a);
        const Keyframe* k = p ? p->keyAt(t) : nullptr;
        if (!k) continue;
        auto lane = std::find_if(out.lanes.begin(), out.lanes.end(), [&](const CopiedKeys::Lane& l) { return l.address == a; });
        if (lane == out.lanes.end()) {
            CopiedKeys::Lane l;
            l.address = a;
            if (a.slot == ParamSlot::Effect)
                if (const Effect* e = paramOwner(c, a)) l.effectType = e->type;
            out.lanes.push_back(std::move(l));
            lane = out.lanes.end() - 1;
        }
        Keyframe copy = *k;
        copy.t -= first;
        lane->keys.push_back(copy);
    }
    for (auto& l : out.lanes)
        std::sort(l.keys.begin(), l.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
    return out;
}

Effect* pasteOwner(Clip& target, const CopiedKeys::Lane& lane) {
    if (lane.address.slot != ParamSlot::Effect) {
        Effect* e = paramOwner(target, lane.address);
        return e && (lane.address.slot != ParamSlot::Generator || !e->empty()) ? e : nullptr;
    }
    if (Effect* same = paramOwner(target, lane.address); same && same->type == lane.effectType) return same;
    for (Effect& e : target.effects)
        if (e.type == lane.effectType) return &e;
    return nullptr;
}

int pasteKeys(Clip& target, const CopiedKeys& keys, FrameTime at) {
    int pasted = 0;
    for (const CopiedKeys::Lane& lane : keys.lanes) {
        Effect* owner = pasteOwner(target, lane);
        if (!owner) continue;
        Param& p = owner->params[lane.address.param];
        if (p.keys.empty()) p.value = p.at(0);
        for (Keyframe k : lane.keys) {
            k.t += std::max<FrameTime>(0, at);
            auto it = std::lower_bound(p.keys.begin(), p.keys.end(), k.t, [](const Keyframe& x, FrameTime t) { return x.t < t; });
            if (it != p.keys.end() && it->t == k.t)
                *it = k;
            else
                p.keys.insert(it, k);
            ++pasted;
        }
    }
    return pasted;
}

}  // namespace montage
