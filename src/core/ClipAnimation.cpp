#include "ClipAnimation.h"

#include <algorithm>
#include <cmath>

namespace montage {

namespace {

double easeOut(double u) { return 1 - std::pow(1 - u, 3); }
double smooth(double u) { return u * u * (3 - 2 * u); }
// Overshoots a little before settling (a pop).
double backOut(double u) {
    const double c1 = 1.70158, c3 = c1 + 1;
    return 1 + c3 * std::pow(u - 1, 3) + c1 * std::pow(u - 1, 2);
}
double bounceOut(double u) {
    const double n = 7.5625, d = 2.75;
    if (u < 1 / d) return n * u * u;
    if (u < 2 / d) return u -= 1.5 / d, n * u * u + 0.75;
    if (u < 2.5 / d) return u -= 2.25 / d, n * u * u + 0.9375;
    return u -= 2.625 / d, n * u * u + 0.984375;
}

// An entrance at progress u (0 at the start, 1 once in), or an exit at u (1 before it starts, 0 at the end).
void entrance(const std::string& type, double u, bool exit, AnimationPose& p) {
    const double e = easeOut(u), away = 1 - e, dir = exit ? -1 : 1;
    if (type == "fade") {
        p.opacity *= smooth(u);
    } else if (type == "slide_left") {
        p.dx += dir * away;  // in from the right, out to the left
    } else if (type == "slide_right") {
        p.dx -= dir * away;
    } else if (type == "slide_up") {
        p.dy += dir * away;  // in from below, out at the top
    } else if (type == "slide_down") {
        p.dy -= dir * away;
    } else if (type == "zoom_in") {
        p.scale *= exit ? 1 + 0.6 * away : 0.5 + 0.5 * e;  // grows into place; grows away
        p.opacity *= smooth(u);
    } else if (type == "zoom_out") {
        p.scale *= exit ? 0.5 + 0.5 * e : 1.6 - 0.6 * e;  // shrinks into place; shrinks away
        p.opacity *= smooth(u);
    } else if (type == "pop") {
        p.scale *= std::max(0.0, backOut(u));
        p.opacity *= std::min(1.0, u * 4);
    } else if (type == "spin") {
        p.rotation += dir * -180 * away;
        p.scale *= 0.2 + 0.8 * e;
        p.opacity *= smooth(u);
    } else if (type == "drop") {
        if (exit) p.dy += (1 - u) * (1 - u) * 1.1;  // falls away
        else p.dy -= (1 - bounceOut(u)) * 0.6;    // drops in and bounces
    } else if (type == "rise") {
        p.dy += dir * 0.08 * away;
        p.opacity *= smooth(u);
    }
}

}  // namespace

const std::vector<AnimationPreset>& animationPresets(AnimationSlot slot) {
    static const std::vector<AnimationPreset> moves = {
        {"fade", "Fade"},       {"slide_left", "Slide Left"}, {"slide_right", "Slide Right"}, {"slide_up", "Slide Up"},
        {"slide_down", "Slide Down"}, {"zoom_in", "Zoom In"}, {"zoom_out", "Zoom Out"},       {"pop", "Pop"},
        {"spin", "Spin"},       {"drop", "Drop"},             {"rise", "Rise"}};
    static const std::vector<AnimationPreset> combos = {{"wiggle", "Wiggle"}, {"pulse", "Pulse"}, {"shake", "Shake"},
                                                        {"float", "Float"},   {"swing", "Swing"}, {"push_in", "Slow Push In"}};
    return slot == AnimationSlot::Combo ? combos : moves;
}

const AnimationPreset* findAnimationPreset(AnimationSlot slot, const std::string& id) {
    for (const AnimationPreset& a : animationPresets(slot))
        if (id == a.id) return &a;
    return nullptr;
}

bool hasClipAnimation(const Clip& c) { return !c.animIn.type.empty() || !c.animOut.type.empty() || !c.animLoop.type.empty(); }

AnimationPose clipAnimationPose(const Clip& c, double local, double fps) {
    AnimationPose p;
    if (!hasClipAnimation(c) || fps <= 0) return p;
    const double length = double(c.duration);
    double in = c.animIn.type.empty() ? 0 : std::max(1.0, c.animIn.seconds * fps);
    double out = c.animOut.type.empty() ? 0 : std::max(1.0, c.animOut.seconds * fps);
    // Both fit in the clip.
    if (in + out > length && in + out > 0) {
        const double k = length / (in + out);
        in *= k;
        out *= k;
    }
    if (in > 0 && local < in) entrance(c.animIn.type, std::clamp(local / in, 0.0, 1.0), false, p);
    if (out > 0 && local > length - out) entrance(c.animOut.type, std::clamp((length - local) / out, 0.0, 1.0), true, p);
    if (!c.animLoop.type.empty()) {
        const double period = std::max(0.1, c.animLoop.seconds), t = local / fps, w = 2 * M_PI * t / period;
        const std::string& k = c.animLoop.type;
        if (k == "wiggle") p.rotation += 4 * std::sin(w);
        else if (k == "pulse") p.scale *= 1 + 0.05 * (0.5 - 0.5 * std::cos(w));
        else if (k == "shake") {
            p.dx += 0.008 * (std::sin(w * 7.3) + 0.5 * std::sin(w * 13.1));
            p.dy += 0.008 * (std::cos(w * 5.9) + 0.5 * std::sin(w * 11.7));
        } else if (k == "float") p.dy += 0.015 * std::sin(w);
        else if (k == "swing") p.rotation += 10 * std::sin(w);
        else if (k == "push_in") p.scale *= 1 + 0.1 * std::clamp(local / std::max(1.0, length), 0.0, 1.0);
    }
    return p;
}

namespace edit {

Result setClipAnimation(Sequence& s, Id clipId, AnimationSlot slot, const std::string& typeIn, double seconds) {
    Clip* c = clipById(s, clipId);
    if (!c) return Result::fail("No such clip");
    if (!c->isGenerator()) {
        bool onVideo = false;
        for (const Track& t : s.videoTracks)
            for (const Clip& k : t.clips) onVideo = onVideo || k.id == clipId;
        if (!onVideo) return Result::fail("Animations move pictures: choose a clip on a video track");
    }
    const std::string type = typeIn == "none" ? std::string() : typeIn;
    if (!type.empty() && !findAnimationPreset(slot, type)) return Result::fail("Unknown animation \"" + typeIn + "\"");
    if (!type.empty() && !(seconds >= 0.1 && seconds <= 10)) return Result::fail("An animation lasts 0.1 to 10 seconds");
    ClipAnimation& a = slot == AnimationSlot::In ? c->animIn : slot == AnimationSlot::Out ? c->animOut : c->animLoop;
    const ClipAnimation want{type, type.empty() ? 0.5 : seconds};
    if (a == want) return Result::fail("");
    a = want;
    return {};
}

}  // namespace edit

}  // namespace montage
