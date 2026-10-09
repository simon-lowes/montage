#include "render/ClipPlacement.h"

#include <algorithm>

#include "core/Effects.h"
#include "render/Compositor.h"

namespace montage {

bool clipFrameQuad(const Project& p, const Sequence& s, const Clip& c, FrameTime t, std::array<double, 4>& xs, std::array<double, 4>& ys) {
    const FrameTime lt = t - c.start;
    const double cl = std::clamp(c.motion.p("crop_left", lt) / 100, 0.0, 1.0), cr = std::clamp(c.motion.p("crop_right", lt) / 100, 0.0, 1.0);
    const double ct = std::clamp(c.motion.p("crop_top", lt) / 100, 0.0, 1.0), cb = std::clamp(c.motion.p("crop_bottom", lt) / 100, 0.0, 1.0);
    const double us[4] = {cl, 1 - cr, 1 - cr, cl}, vs[4] = {ct, ct, 1 - cb, 1 - cb};
    for (int k = 0; k < 4; ++k)
        if (!clipFrameToSequence(p, s, c, t, us[k], vs[k], xs[size_t(k)], ys[size_t(k)])) return false;
    return true;
}

bool parseAlign(const std::string& name, Align& out) {
    static const std::pair<const char*, Align> names[] = {{"center", Align::Center},     {"centre", Align::Center},       {"top", Align::Top},
                                                          {"bottom", Align::Bottom},     {"left", Align::Left},           {"right", Align::Right},
                                                          {"top_left", Align::TopLeft},  {"top_right", Align::TopRight},  {"bottom_left", Align::BottomLeft},
                                                          {"bottom_right", Align::BottomRight}};
    for (const auto& [n, a] : names)
        if (name == n) return out = a, true;
    return false;
}

namespace edit {

Result alignClip(Project& p, Sequence& s, Id clip, FrameTime t, Align where, double inset) {
    Clip* c = clipById(s, clip);
    const auto loc = locate(s, clip);
    if (!c || !loc) return Result::fail("No such clip");
    if (loc->track.kind != TrackKind::Video) return Result::fail("Only picture clips can be lined up");
    std::array<double, 4> xs, ys;
    if (!clipFrameQuad(p, s, *c, t, xs, ys)) return Result::fail("The clip has no picture");
    const double x0 = *std::min_element(xs.begin(), xs.end()), x1 = *std::max_element(xs.begin(), xs.end());
    const double y0 = *std::min_element(ys.begin(), ys.end()), y1 = *std::max_element(ys.begin(), ys.end());
    const double m = std::clamp(inset, 0.0, 0.45) * s.height, W = s.width, H = s.height;
    const bool left = where == Align::Left || where == Align::TopLeft || where == Align::BottomLeft;
    const bool right = where == Align::Right || where == Align::TopRight || where == Align::BottomRight;
    const bool top = where == Align::Top || where == Align::TopLeft || where == Align::TopRight;
    const bool bottom = where == Align::Bottom || where == Align::BottomLeft || where == Align::BottomRight;
    const double dx = left ? m - x0 : right ? (W - m) - x1 : W / 2 - (x0 + x1) / 2;
    const double dy = top ? m - y0 : bottom ? (H - m) - y1 : H / 2 - (y0 + y1) / 2;
    if (c->motion.empty()) c->motion = makeEffect(p, "transform");
    const FrameTime lt = t - c->start;
    c->motion.params["pos_x"].set(lt, c->motion.p("pos_x", lt) + dx);
    c->motion.params["pos_y"].set(lt, c->motion.p("pos_y", lt) + dy);
    return {};
}

}  // namespace edit

}  // namespace montage
