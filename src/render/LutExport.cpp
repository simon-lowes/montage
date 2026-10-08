#include "LutExport.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

#include "core/Effects.h"

namespace montage {

bool isColorOnlyEffect(const Effect& e, FrameTime t) {
    static const std::set<std::string> kinds{"color_correct", "curves", "hue_curves", "hue_sat",      "lut", "color_space_transform",
                                             "ocio",          "levels", "invert",     "black_white", "broadcast_safe"};
    if (!kinds.count(e.type) || hasMask(e, t)) return false;
    if (e.type == "broadcast_safe" && e.p("highlight", t, 0) >= 0.5) return false;  // the stripes are drawn in place
    return true;
}

Lut3D bakeLut(const std::vector<Effect>& effects, FrameTime t, int size, std::vector<std::string>* skipped) {
    size = std::clamp(size, 2, 129);
    Lut3D lut;
    lut.size = size;
    // The lattice as a picture: red across, green across in blocks, blue down.
    Image img(size * size, size, Image::Uninitialized{});
    const float step = 1.0f / float(size - 1);
    for (int b = 0; b < size; ++b)
        for (int g = 0; g < size; ++g)
            for (int r = 0; r < size; ++r) {
                float* px = img.at(r + g * size, b);
                px[0] = float(r) * step, px[1] = float(g) * step, px[2] = float(b) * step, px[3] = 1;
            }
    for (const Effect& e : effects) {
        if (!e.enabled) continue;
        if (!isColorOnlyEffect(e, t)) {
            if (skipped) {
                const EffectInfo* info = findEffectInfo(e.type);
                skipped->push_back(info ? info->displayName : e.type);
            }
            continue;
        }
        applyVideoEffect(e, t, img, 1.0);
    }
    lut.data.resize(size_t(size) * size_t(size) * size_t(size) * 3);
    for (int b = 0; b < size; ++b)
        for (int g = 0; g < size; ++g)
            for (int r = 0; r < size; ++r) {
                const float* px = img.at(r + g * size, b);
                float* d = &lut.data[(size_t(r) + size_t(size) * (size_t(g) + size_t(size) * size_t(b))) * 3];
                d[0] = px[0], d[1] = px[1], d[2] = px[2];
            }
    return lut;
}

bool writeCubeLut(const Lut3D& lut, const std::string& path, const std::string& title, std::string* error) {
    if (lut.is1D || lut.size < 2 || lut.data.size() != size_t(lut.size) * size_t(lut.size) * size_t(lut.size) * 3) {
        if (error) *error = "Not a 3D LUT";
        return false;
    }
    std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary);
    if (!out) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    out << "# Made by Montage\n";
    if (!title.empty()) {
        std::string t = title;
        std::replace(t.begin(), t.end(), '"', '\'');
        out << "TITLE \"" << t << "\"\n";
    }
    out << "LUT_3D_SIZE " << lut.size << "\n";
    out << "DOMAIN_MIN " << lut.domainMin[0] << ' ' << lut.domainMin[1] << ' ' << lut.domainMin[2] << "\n";
    out << "DOMAIN_MAX " << lut.domainMax[0] << ' ' << lut.domainMax[1] << ' ' << lut.domainMax[2] << "\n";
    char buf[96];
    for (size_t i = 0; i < lut.data.size(); i += 3) {
        std::snprintf(buf, sizeof buf, "%.6f %.6f %.6f\n", double(lut.data[i]), double(lut.data[i + 1]), double(lut.data[i + 2]));
        out << buf;
    }
    out.close();
    if (!out) {
        if (error) *error = "Cannot write " + path;
        return false;
    }
    forgetCubeLut(path);  // a clip using this file sees the new one
    return true;
}

}  // namespace montage
