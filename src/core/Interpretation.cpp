#include "Interpretation.h"

#include "Numbers.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace montage {

namespace {

constexpr char kMark = '\x1d';  // never in a file name

bool parseRational(const std::string& v, Rational& out) {
    int num = 0, den = 0;
    if (std::sscanf(v.c_str(), "%d,%d", &num, &den) != 2 || num <= 0 || den <= 0) return false;
    out = Rational{num, den};
    return true;
}

}  // namespace

bool validAlphaMode(const std::string& alpha) {
    return alpha.empty() || alpha == "straight" || alpha == "premultiplied" || alpha == "ignore" || alpha == "invert";
}

bool validFieldOrder(const std::string& fields) {
    return fields.empty() || fields == "progressive" || fields == "upper" || fields == "lower";
}

bool parsePulldown(const std::string& value, std::vector<PulldownStretch>& out) {
    out.clear();
    if (value.empty()) return true;
    std::stringstream items(value);
    std::string item;
    while (std::getline(items, item, ',')) {
        long long start = -1;
        int position = -1;
        char order = 0, extra = 0;
        // (position "v": video with no pulldown)
        const bool video = std::sscanf(item.c_str(), "%lld:v:%c%c", &start, &order, &extra) == 2;
        if (!video && std::sscanf(item.c_str(), "%lld:%d:%c%c", &start, &position, &order, &extra) != 3) start = -1;
        if (start < 0 || (!video && (position < 0 || position > 4)) ||
            (order != 't' && order != 'b') || (!out.empty() && start <= out.back().start) || (out.empty() && start != 0)) {
            out.clear();
            return false;
        }
        out.push_back({int64_t(start), video ? -1 : position, order == 't'});
    }
    return !out.empty();
}

std::string pulldownValue(const std::vector<PulldownStretch>& stretches) {
    std::string out;
    for (const PulldownStretch& s : stretches)
        out += (out.empty() ? "" : ",") + std::to_string(s.start) + ":" + (s.position < 0 ? std::string("v") : std::to_string(s.position)) + ":" +
               (s.topFirst ? "t" : "b");
    return out;
}

bool validRawHighlights(const std::string& mode) { return mode.empty() || mode == "clip" || mode == "blend" || mode == "rebuild"; }

bool validStereoLayout(const std::string& layout) {
    return layout.empty() || layout == "none" || layout == "sbs" || layout == "sbs_half" || layout == "tb" || layout == "tb_half" ||
           layout == "mv";
}

std::string stereoLayout(const std::string& detected, const Interpretation& i) {
    if (i.stereo == "none") return {};
    return i.stereo.empty() ? detected : i.stereo;
}

// A free-text value kept clear of the list's own marks (percent-encoded).
static std::string escapeValue(const std::string& v) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : v) {
        if (c == ';' || c == '=' || c == '%' || c < 0x20) out += '%', out += hex[c >> 4], out += hex[c & 15];
        else out += char(c);
    }
    return out;
}

static std::string unescapeValue(const std::string& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%' && i + 2 < v.size() && std::isxdigit(static_cast<unsigned char>(v[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(v[i + 2]))) {
            out += char(std::stoi(v.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += v[i];
        }
    }
    return out;
}

std::string layerPath(const std::string& path, const std::string& layer) {
    Interpretation i;
    parseInterpretation(path, i);
    i.layer = layer;
    return interpretedPath(path, i);
}

std::string gainMapPath(const std::string& path, bool hdr) {
    Interpretation i;
    parseInterpretation(path, i);
    i.gainMap = hdr;
    return interpretedPath(path, i);
}

std::string eyePath(const std::string& path, int eye) {
    Interpretation i;
    parseInterpretation(path, i);
    i.eye = eye ? 1 : 0;
    return interpretedPath(path, i);
}

bool parseInterpretation(const std::string& path, Interpretation& out) {
    out = {};
    const size_t mark = path.find(kMark);
    if (mark == std::string::npos) return false;
    std::stringstream items(path.substr(mark + 1));
    std::string item;
    while (std::getline(items, item, ';')) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = item.substr(0, eq), value = item.substr(eq + 1);
        if (key == "fps") parseRational(value, out.fps);
        else if (key == "file") parseRational(value, out.fileFps);
        else if (key == "par") out.par = std::max(0.0, parseNumber(value));
        else if (key == "alpha" && validAlphaMode(value)) out.alpha = value == "straight" ? "" : value;
        else if (key == "fields" && validFieldOrder(value)) out.fields = value;
        else if (key == "pulldown") {
            if (std::vector<PulldownStretch> st; parsePulldown(value, st)) out.pulldown = value;
        } else if (key == "pitch") out.keepPitch = value == "1";
        else if (key == "rawexp") out.rawExposure = parseNumber(value);
        else if (key == "rawtemp") out.rawTemperature = std::max(0.0, parseNumber(value));
        else if (key == "rawtint") out.rawTint = parseNumber(value);
        else if (key == "rawhl" && validRawHighlights(value)) out.rawHighlights = value == "clip" ? "" : value;
        else if (key == "rawhalf") out.rawHalf = value == "1";
        else if (key == "stereo" && validStereoLayout(value)) out.stereo = value;
        else if (key == "swap") out.swapEyes = value == "1";
        else if (key == "eye") out.eye = value == "1" ? 1 : 0;
        else if (key == "layer") out.layer = unescapeValue(value);
        else if (key == "gainmap") out.gainMap = value == "1";
    }
    if (!out.conformed()) out.fps = out.fileFps = Rational{0, 1};
    return !out.empty();
}

std::string uninterpretedPath(const std::string& path) {
    const size_t mark = path.find(kMark);
    return mark == std::string::npos ? path : path.substr(0, mark);
}

std::string interpretedPath(const std::string& path, const Interpretation& i) {
    std::string out = uninterpretedPath(path);
    if (i.empty()) return out;
    std::string items;
    auto add = [&](const std::string& item) { items += (items.empty() ? "" : ";") + item; };
    if (i.conformed()) {
        add("fps=" + std::to_string(i.fps.num) + "," + std::to_string(i.fps.den));
        add("file=" + std::to_string(i.fileFps.num) + "," + std::to_string(i.fileFps.den));
        if (i.keepPitch) add("pitch=1");
    }
    if (i.par > 0) add("par=" + formatNumber(i.par, 6));
    if (!i.alpha.empty() && i.alpha != "straight") add("alpha=" + i.alpha);
    if (!i.fields.empty()) add("fields=" + i.fields);
    if (!i.pulldown.empty()) add("pulldown=" + i.pulldown);
    auto number = [](double v) { return formatNumber(v, 6); };
    if (i.rawExposure != 0) add("rawexp=" + number(i.rawExposure));
    if (i.rawTemperature > 0) add("rawtemp=" + number(i.rawTemperature));
    if (i.rawTint != 0) add("rawtint=" + number(i.rawTint));
    if (!i.rawHighlights.empty() && i.rawHighlights != "clip") add("rawhl=" + i.rawHighlights);
    if (i.rawHalf) add("rawhalf=1");
    if (!i.stereo.empty()) add("stereo=" + i.stereo);
    if (i.swapEyes) add("swap=1");
    if (i.eye) add("eye=1");
    if (!i.layer.empty()) add("layer=" + escapeValue(i.layer));
    if (i.gainMap) add("gainmap=1");
    return items.empty() ? out : out + kMark + items;
}

Interpretation interpretationOf(const MediaItem& m) {
    Interpretation i;
    parseInterpretation(m.path, i);
    return i;
}

}  // namespace montage
