#include "Automation.h"

#include <algorithm>
#include <cmath>

namespace montage {

namespace {

// A lane's value at a fractional frame: whole frames as the lane gives them, linear between.
double laneAt(const Param& p, double frame) {
    const FrameTime f0 = FrameTime(std::floor(frame));
    const double u = frame - double(f0), a = p.at(f0);
    return u > 1e-9 ? a + (p.at(f0 + 1) - a) * u : a;
}

bool reads(const Track& t) {
    const AutomationMode m = trackAutomation(t);
    return m == AutomationMode::Read || m == AutomationMode::Latch || m == AutomationMode::Touch;
}

}  // namespace

const char* automationModeName(AutomationMode m) {
    switch (m) {
        case AutomationMode::Off: return "Off";
        case AutomationMode::Read: return "Read";
        case AutomationMode::Write: return "Write";
        case AutomationMode::Latch: return "Latch";
        case AutomationMode::Touch: return "Touch";
    }
    return "Read";
}

AutomationMode trackAutomation(const Track& t) { return AutomationMode(std::clamp(t.automation, 0, 4)); }

double trackVolumeAt(const Track& t, double frame) { return reads(t) && t.volumeAuto.animated() ? laneAt(t.volumeAuto, frame) : t.volumeDb; }

double trackPanAt(const Track& t, double frame) {
    return reads(t) && t.panAuto.animated() ? std::clamp(laneAt(t.panAuto, frame), -1.0, 1.0) : t.pan;
}

void thinKeys(std::vector<Keyframe>& keys, double tolerance) {
    if (keys.size() < 3) return;
    std::vector<char> keep(keys.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> stack{{0, keys.size() - 1}};
    while (!stack.empty()) {
        const auto [a, b] = stack.back();
        stack.pop_back();
        if (b <= a + 1) continue;
        double worst = -1;
        size_t at = a;
        const double span = double(keys[b].t - keys[a].t);
        for (size_t i = a + 1; i < b; ++i) {
            const double u = span > 0 ? double(keys[i].t - keys[a].t) / span : 0;
            const double d = std::fabs(keys[i].v - (keys[a].v + (keys[b].v - keys[a].v) * u));
            if (d > worst) worst = d, at = i;
        }
        if (worst > tolerance) {
            keep[at] = 1;
            stack.push_back({a, at});
            stack.push_back({at, b});
        }
    }
    std::vector<Keyframe> out;
    for (size_t i = 0; i < keys.size(); ++i)
        if (keep[i]) out.push_back(keys[i]);
    keys = std::move(out);
}

AutomationRecorder::AutomationRecorder(AutomationMode mode, Param lane, double still, FrameTime start, FrameTime glide)
    : mode_(mode), lane_(std::move(lane)), still_(still), glide_(std::max<FrameTime>(1, glide)), passStart_(start), last_(start - 1),
      lastValue_(still) {
    if (mode_ == AutomationMode::Write) writing_ = true;
}

double AutomationRecorder::tick(FrameTime t, double fader, bool held) {
    if (t <= last_) return writing_ ? fader : (mode_ == AutomationMode::Off ? fader : original(t));  // playback only runs forward
    last_ = t;
    auto record = [&] {
        current_.push_back({t, fader});
        lastValue_ = fader;
        return fader;
    };
    switch (mode_) {
        case AutomationMode::Off: return fader;
        case AutomationMode::Read: return original(t);
        case AutomationMode::Write: return record();
        case AutomationMode::Latch:
            if (!writing_ && held) writing_ = true, passStart_ = t;
            return writing_ ? record() : original(t);
        case AutomationMode::Touch:
            if (held) {
                if (!writing_) writing_ = true, passStart_ = t;
                return record();
            }
            if (writing_) endPass(t - 1);
            return original(t);
    }
    return fader;
}

void AutomationRecorder::endPass(FrameTime t) {
    if (!current_.empty()) {
        Pass p;
        p.from = passStart_;
        p.to = std::max(t, current_.back().t);
        p.keys = std::move(current_);
        p.glide = mode_ == AutomationMode::Touch;
        passes_.push_back(std::move(p));
    }
    current_.clear();
    writing_ = false;
}

Param AutomationRecorder::finish(FrameTime stop, double tolerance) {
    if (writing_) {
        // Write and Latch hold the last value to where playback stopped; Touch ends where it was let go.
        if (mode_ != AutomationMode::Touch && !current_.empty() && current_.back().t < stop) current_.push_back({stop, lastValue_});
        endPass(stop);
    }
    Param out = lane_;
    if (!out.animated()) out.value = still_;
    for (Pass& p : passes_) {
        const FrameTime from = p.from, to = p.keys.back().t;
        const FrameTime resume = p.glide ? to + glide_ : to + 1;
        const double before = original(from - 1), after = original(resume);
        std::erase_if(out.keys, [&](const Keyframe& k) { return k.t >= from && k.t <= resume; });
        thinKeys(p.keys, tolerance);
        // The level either side of the pass stays as it was.
        if (from > 0 && !out.keyAt(from - 1)) out.keys.push_back({from - 1, before});
        for (const Keyframe& k : p.keys) out.keys.push_back(k);
        out.keys.push_back({resume, after});
    }
    std::sort(out.keys.begin(), out.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
    passes_.clear();
    // A pass that left a flat lane at the fader's level (pan never touched in Write) adds nothing.
    if (!lane_.animated() && std::all_of(out.keys.begin(), out.keys.end(), [&](const Keyframe& k) { return std::fabs(k.v - still_) <= tolerance; }))
        return Param(still_);
    return out;
}

}  // namespace montage
