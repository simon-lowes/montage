#include "ColorWarp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace montage {

WarpPoint ColorWarp::at(int spoke, int ring) const {
    spoke = ((spoke % kWarpSpokes) + kWarpSpokes) % kWarpSpokes;
    for (const WarpPoint& p : points)
        if (p.spoke == spoke && p.ring == ring) return p;
    return {spoke, ring};
}

void ColorWarp::set(const WarpPoint& p) {
    points.erase(std::remove_if(points.begin(), points.end(), [&](const WarpPoint& q) { return q.spoke == p.spoke && q.ring == p.ring; }),
                 points.end());
    if (p.dh != 0 || p.ds != 0 || p.dl != 0) points.push_back(p);
    std::sort(points.begin(), points.end(), [](const WarpPoint& a, const WarpPoint& b) {
        return a.ring != b.ring ? a.ring < b.ring : a.spoke < b.spoke;
    });
}

bool parseColorWarp(const std::string& text, ColorWarp& out) {
    ColorWarp w;
    std::istringstream is(text);
    std::string group;
    while (std::getline(is, group, ';')) {
        if (group.find_first_not_of(" \t\n") == std::string::npos) continue;
        WarpPoint p;
        if (std::sscanf(group.c_str(), " %d , %d , %lf , %lf , %lf", &p.spoke, &p.ring, &p.dh, &p.ds, &p.dl) != 5) return false;
        if (p.spoke < 0 || p.spoke >= kWarpSpokes || p.ring < 1 || p.ring > kWarpRings) return false;
        w.set(p);
    }
    out = std::move(w);
    return true;
}

std::string colorWarpToString(const ColorWarp& w) {
    std::string out;
    char buf[96];
    for (const WarpPoint& p : w.points) {
        std::snprintf(buf, sizeof buf, "%s%d,%d,%.4g,%.4g,%.4g", out.empty() ? "" : ";", p.spoke, p.ring, p.dh, p.ds, p.dl);
        out += buf;
    }
    return out;
}

WarpField::WarpField(const ColorWarp& w) {
    for (const WarpPoint& p : w.points)
        if (p.spoke >= 0 && p.spoke < kWarpSpokes && p.ring >= 1 && p.ring <= kWarpRings) {
            dh[p.spoke][p.ring] = p.dh;
            ds[p.spoke][p.ring] = p.ds;
            dl[p.spoke][p.ring] = p.dl;
        }
}

void WarpField::apply(double hue, double sat, double& newHue, double& newSat, double& valueScale) const {
    // The four mesh points round the colour (the centre, ring 0, never moves), blended bilinearly.
    const double a = (hue - std::floor(hue)) * kWarpSpokes;
    const int i0 = int(std::floor(a)) % kWarpSpokes, i1 = (i0 + 1) % kWarpSpokes;
    const double fa = a - std::floor(a);
    const double b = std::clamp(sat, 0.0, 1.0) * kWarpRings;
    const int j0 = std::min(int(std::floor(b)), kWarpRings - 1), j1 = j0 + 1;
    const double fb = b - j0;
    const double w00 = (1 - fa) * (1 - fb), w10 = fa * (1 - fb), w01 = (1 - fa) * fb, w11 = fa * fb;
    auto blend = [&](const double (&t)[kWarpSpokes][kWarpRings + 1]) {
        return t[i0][j0] * w00 + t[i1][j0] * w10 + t[i0][j1] * w01 + t[i1][j1] * w11;
    };
    newHue = hue + blend(dh) / 360.0;
    newHue -= std::floor(newHue);
    newSat = std::clamp(sat + blend(ds), 0.0, 1.0);
    valueScale = std::exp2(blend(dl));
}

void warpHueSat(const ColorWarp& w, double hue, double sat, double& newHue, double& newSat, double& valueScale) {
    WarpField(w).apply(hue, sat, newHue, newSat, valueScale);
}

}  // namespace montage
