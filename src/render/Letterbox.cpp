#include "Letterbox.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "media/Decoder.h"
#include "render/Compositor.h"

namespace montage {

namespace {

constexpr float kBlack = 0.07f;  // brighter than this somewhere is picture

}  // namespace

bool detectBars(const std::string& path, double from, double to, Bars& out, std::string* error, int samples,
                const std::atomic<bool>* cancel) {
    out = Bars{};
    VideoDecoder dec;
    if (!dec.open(path, error)) return false;
    const double len = dec.duration();
    from = std::clamp(from, 0.0, std::max(0.0, len));
    to = to > from ? std::min(to, len > 0 ? len : to) : len;
    // The smallest picture box any frame shows (frames black all over say nothing).
    int top = -1, bottom = -1, left = -1, right = -1, W = 0, H = 0;
    for (int k = 0; k < std::max(1, samples); ++k) {
        if (cancel && cancel->load()) {
            if (error) *error = "Cancelled";
            return false;
        }
        const double t = from + (to - from) * (k + 0.5) / std::max(1, samples);
        const int tw = std::min(320, std::max(1, dec.displayWidth()));
        const int th = std::max(1, int(std::lround(double(tw) * dec.displayHeight() / std::max(1, dec.displayWidth()))));
        Frame16Ptr f = dec.frameAt(t, tw, th);
        if (!f) continue;
        const Image img = toImage(*f);
        W = img.width, H = img.height;
        int y0 = H, y1 = -1, x0 = W, x1 = -1;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float* p = img.at(x, y);
                if (std::max({p[0], p[1], p[2]}) > kBlack) {
                    y0 = std::min(y0, y), y1 = std::max(y1, y);
                    x0 = std::min(x0, x), x1 = std::max(x1, x);
                }
            }
        if (y1 < 0) continue;  // black all over
        top = top < 0 ? y0 : std::min(top, y0);
        bottom = bottom < 0 ? H - 1 - y1 : std::min(bottom, H - 1 - y1);
        left = left < 0 ? x0 : std::min(left, x0);
        right = right < 0 ? W - 1 - x1 : std::min(right, W - 1 - x1);
    }
    if (top < 0 || W <= 0 || H <= 0) return true;  // nothing but black: no bars to speak of
    auto frac = [](int n, int size) { const double f = double(n) / size; return f >= 0.01 ? f : 0.0; };
    out.top = frac(top, H), out.bottom = frac(bottom, H), out.left = frac(left, W), out.right = frac(right, W);
    // Leave at least a third of the picture.
    if (out.top + out.bottom > 0.66) out.top = out.bottom = 0;
    if (out.left + out.right > 0.66) out.left = out.right = 0;
    return true;
}

namespace edit {

Result removeLetterbox(Project& p, Sequence& s, Id clip, const Bars& bars) {
    Clip* c = clipById(s, clip);
    if (!c) return Result::fail("No such clip");
    if (!bars.any()) return Result::fail("");
    double mw = 0, mh = 0;
    if (!clipFrameSize(p, s, *c, mw, mh) || mw <= 0 || mh <= 0) return Result::fail("Only a picture can be cropped");
    // The picture fitted as it is now, then scaled so what is left of it fills the frame.
    const int fit = int(c->motion.p("fit", 0, 0));
    double f0 = 1;
    if (fit == 0) f0 = std::min(s.width / mw, s.height / mh);
    else if (fit == 1) f0 = std::max(s.width / mw, s.height / mh);
    else if (fit == 2) f0 = 1;  // stretched: treated as fitted width-wise below
    const double cw = mw * (1 - bars.left - bars.right), ch = mh * (1 - bars.top - bars.bottom);
    const double scale = fit == 2 ? 1 : std::max(s.width / cw, s.height / ch) / f0;
    Effect& m = c->motion;
    m.params["crop_left"] = Param(bars.left * 100);
    m.params["crop_right"] = Param(bars.right * 100);
    m.params["crop_top"] = Param(bars.top * 100);
    m.params["crop_bottom"] = Param(bars.bottom * 100);
    m.params["scale"] = Param(scale * 100);
    m.params["scale_x"] = Param(100.0);
    m.params["scale_y"] = Param(100.0);
    // What is left, centred.
    m.params["pos_x"] = Param(-(bars.left - bars.right) / 2 * mw * f0 * scale);
    m.params["pos_y"] = Param(-(bars.top - bars.bottom) / 2 * mh * f0 * scale);
    return {};
}

}  // namespace edit

}  // namespace montage
