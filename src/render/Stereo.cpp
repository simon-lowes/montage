#include "Stereo.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "media/SuperScale.h"

namespace montage {

const std::vector<std::string>& stereoViewNames() {
    static const std::vector<std::string> names = {"left", "right", "anaglyph", "sbs", "sbs_half", "tb", "tb_half", "difference"};
    return names;
}

bool stereoViewFromName(const std::string& name, StereoView& out) {
    const auto& names = stereoViewNames();
    const auto it = std::find(names.begin(), names.end(), name);
    if (it == names.end()) return false;
    out = StereoView(it - names.begin());
    return true;
}

std::string stereoViewName(StereoView v) {
    const auto& names = stereoViewNames();
    const size_t i = size_t(v);
    return i < names.size() ? names[i] : names[0];
}

void stereoPacking(StereoView v, int& across, int& down) {
    across = v == StereoView::SideBySide ? 2 : 1;
    down = v == StereoView::TopBottom ? 2 : 1;
}

Image combineStereo(const Image& left, const Image& right, StereoView view) {
    if (left.empty() || right.width != left.width || right.height != left.height) return left;
    const int W = left.width, H = left.height;
    switch (view) {
        case StereoView::Left: return left;
        case StereoView::Right: return right;
        case StereoView::Anaglyph: {
            // Half-colour anaglyph: red is the left eye's brightness (full-colour red fights the right eye), green and
            // blue the right eye's own.
            Image out(W, H, Image::Uninitialized{});
            for (int y = 0; y < H; ++y) {
                const float* l = left.row(y);
                const float* r = right.row(y);
                float* o = out.row(y);
                for (int x = 0; x < W; ++x, l += 4, r += 4, o += 4) {
                    o[0] = 0.299f * l[0] + 0.587f * l[1] + 0.114f * l[2];
                    o[1] = r[1];
                    o[2] = r[2];
                    o[3] = std::max(l[3], r[3]);
                }
            }
            return out;
        }
        case StereoView::Difference: {
            // Where the eyes differ, brightened: zero where they agree (the screen plane).
            Image out(W, H, Image::Uninitialized{});
            for (int y = 0; y < H; ++y) {
                const float* l = left.row(y);
                const float* r = right.row(y);
                float* o = out.row(y);
                for (int x = 0; x < W; ++x, l += 4, r += 4, o += 4) {
                    for (int c = 0; c < 3; ++c) o[c] = std::min(1.0f, 2.0f * std::fabs(l[c] - r[c]));
                    o[3] = 1;
                }
            }
            return out;
        }
        case StereoView::SideBySide:
        case StereoView::SideBySideHalf: {
            const bool half = view == StereoView::SideBySideHalf;
            const int ew = half ? std::max(1, W / 2) : W;
            const Image a = half ? resizeImage(left, ew, H) : left, b = half ? resizeImage(right, W - ew, H) : right;
            Image out(half ? W : 2 * W, H, Image::Uninitialized{});
            for (int y = 0; y < H; ++y) {
                std::memcpy(out.row(y), a.row(y), size_t(a.width) * 4 * sizeof(float));
                std::memcpy(out.row(y) + size_t(a.width) * 4, b.row(y), size_t(b.width) * 4 * sizeof(float));
            }
            return out;
        }
        case StereoView::TopBottom:
        case StereoView::TopBottomHalf: {
            const bool half = view == StereoView::TopBottomHalf;
            const int eh = half ? std::max(1, H / 2) : H;
            const Image a = half ? resizeImage(left, W, eh) : left, b = half ? resizeImage(right, W, H - eh) : right;
            Image out(W, half ? H : 2 * H, Image::Uninitialized{});
            for (int y = 0; y < a.height; ++y) std::memcpy(out.row(y), a.row(y), size_t(W) * 4 * sizeof(float));
            for (int y = 0; y < b.height; ++y) std::memcpy(out.row(a.height + y), b.row(y), size_t(W) * 4 * sizeof(float));
            return out;
        }
    }
    return left;
}

}  // namespace montage
