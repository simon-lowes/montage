#include "render/Spherical.h"

#include <algorithm>
#include <cmath>

#include "core/Effects.h"

namespace montage {

namespace {

constexpr double kDeg = M_PI / 180;

struct Mat3 {
    double m[3][3];
    void apply(double& x, double& y, double& z) const {
        const double a = m[0][0] * x + m[0][1] * y + m[0][2] * z, b = m[1][0] * x + m[1][1] * y + m[1][2] * z,
                     c = m[2][0] * x + m[2][1] * y + m[2][2] * z;
        x = a, y = b, z = c;
    }
};

Mat3 mul(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

// The camera's turn: roll about the view axis, then pitch up, then yaw right (x right, y up, z ahead).
Mat3 cameraTurn(double yaw, double pitch, double roll) {
    const double cy = std::cos(yaw * kDeg), sy = std::sin(yaw * kDeg), cp = std::cos(pitch * kDeg), sp = std::sin(pitch * kDeg),
                 cr = std::cos(roll * kDeg), sr = std::sin(roll * kDeg);
    const Mat3 Y{{{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}}};
    const Mat3 P{{{1, 0, 0}, {0, cp, sp}, {0, -sp, cp}}};
    const Mat3 R{{{cr, sr, 0}, {-sr, cr, 0}, {0, 0, 1}}};
    return mul(Y, mul(P, R));
}

// Bilinear, wrapping round in longitude and clamped at the poles.
void sample(const Image& src, double x, double y, float* out) {
    const int W = src.width, H = src.height;
    x -= 0.5, y = std::clamp(y - 0.5, 0.0, double(H - 1));
    const double fx = std::floor(x), fy = std::floor(y);
    const float tx = float(x - fx), ty = float(y - fy);
    const int x0 = ((int(fx) % W) + W) % W, x1 = (x0 + 1) % W;
    const int y0 = int(fy), y1 = std::min(H - 1, y0 + 1);
    const float* a = src.at(x0, y0);
    const float* b = src.at(x1, y0);
    const float* c = src.at(x0, y1);
    const float* d = src.at(x1, y1);
    for (int k = 0; k < 4; ++k) out[k] = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
}

}  // namespace

void equirectPoint(double yaw, double pitch, int w, int h, double& x, double& y) {
    x = (yaw / 360 + 0.5) * w;
    y = (0.5 - pitch / 180) * h;
}

Image reframeEquirect(const Image& equirect, double yaw, double pitch, double roll, double fov, SphereView view, int w,
                      int h) {
    if (equirect.empty() || w <= 0 || h <= 0) return {};
    Image out(w, h, Image::Uninitialized{});
    const Mat3 turn = cameraTurn(yaw, pitch, roll);
    const double W = equirect.width, H = equirect.height;
    const bool flat = view == SphereView::Flat;
    const double half = flat ? std::tan(std::clamp(fov, 1.0, 170.0) * kDeg / 2)          // the image plane at z = 1
                             : 2 * std::tan(std::clamp(fov, 1.0, 330.0) * kDeg / 4);  // stereographic radius at the edge
    parallelRows(h, [&](int y0, int y1) {
        for (int py = y0; py < y1; ++py) {
            float* row = out.row(py);
            for (int px = 0; px < w; ++px) {
                // Across the width -half..half, square pixels.
                const double u = ((px + 0.5) / w * 2 - 1) * half, v = (1 - (py + 0.5) / h * 2) * half * h / w;
                double x, y, z;
                if (flat) {
                    x = u, y = v, z = 1;
                } else {
                    // The angle from straight down (little planet) or up (tunnel): rho = 2 tan(angle / 2).
                    const double rho = std::hypot(u, v), a = 2 * std::atan(rho / 2);
                    const double s = rho > 1e-12 ? std::sin(a) / rho : 0;
                    x = u * s, z = v * s, y = view == SphereView::LittlePlanet ? -std::cos(a) : std::cos(a);
                    if (view == SphereView::Tunnel) z = -z;
                }
                turn.apply(x, y, z);
                const double lon = std::atan2(x, z), lat = std::atan2(y, std::hypot(x, z));
                sample(equirect, (lon / (2 * M_PI) + 0.5) * W, (0.5 - lat / M_PI) * H, row + size_t(px) * 4);
            }
        }
    });
    return out;
}

namespace edit {

Result setReframe360(Project& p, Sequence& s, Id clip, const ReframeView& view, FrameTime key) {
    const auto loc = locate(s, clip);
    Clip* c = clipById(s, clip);
    if (!loc || !c) return Result::fail("No such clip");
    const MediaItem* m = c->mediaId ? p.findMedia(c->mediaId) : nullptr;
    if (loc->track.kind != TrackKind::Video || !m || !m->hasVideo || m->kind == MediaKind::Sequence)
        return Result::fail("Reframe 360° works on clips of 360° footage");
    if (key >= c->duration) return Result::fail("The key is past the end of the clip");
    auto it = std::find_if(c->effects.begin(), c->effects.end(), [](const Effect& e) { return e.type == "reframe_360"; });
    if (it == c->effects.end()) {
        c->effects.insert(c->effects.begin(), makeEffect(p, "reframe_360"));
        it = c->effects.begin();
    }
    it->enabled = true;
    auto put = [&](const char* name, const std::optional<double>& v) {
        if (!v) return;
        Param& prm = it->params[name];
        if (key >= 0) {
            prm.addKey(key, *v, Interp::Smooth);
        } else {
            prm.keys.clear();
            prm.value = *v;
        }
    };
    put("yaw", view.yaw);
    put("pitch", view.pitch ? std::optional<double>(std::clamp(*view.pitch, -90.0, 90.0)) : std::nullopt);
    put("roll", view.roll);
    put("fov", view.fov ? std::optional<double>(std::clamp(*view.fov, 20.0, 330.0)) : std::nullopt);
    if (view.projection) {
        Param& prm = it->params["projection"];
        prm.keys.clear();
        prm.value = double(int(*view.projection));
    }
    return {};
}

}  // namespace edit

}  // namespace montage
