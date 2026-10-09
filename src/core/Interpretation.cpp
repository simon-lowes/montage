#include "Interpretation.h"

#include "Numbers.h"

#include <algorithm>
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

bool validRawHighlights(const std::string& mode) { return mode.empty() || mode == "clip" || mode == "blend" || mode == "rebuild"; }

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
        else if (key == "pitch") out.keepPitch = value == "1";
        else if (key == "rawexp") out.rawExposure = parseNumber(value);
        else if (key == "rawtemp") out.rawTemperature = std::max(0.0, parseNumber(value));
        else if (key == "rawtint") out.rawTint = parseNumber(value);
        else if (key == "rawhl" && validRawHighlights(value)) out.rawHighlights = value == "clip" ? "" : value;
        else if (key == "rawhalf") out.rawHalf = value == "1";
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
    auto number = [](double v) { return formatNumber(v, 6); };
    if (i.rawExposure != 0) add("rawexp=" + number(i.rawExposure));
    if (i.rawTemperature > 0) add("rawtemp=" + number(i.rawTemperature));
    if (i.rawTint != 0) add("rawtint=" + number(i.rawTint));
    if (!i.rawHighlights.empty() && i.rawHighlights != "clip") add("rawhl=" + i.rawHighlights);
    if (i.rawHalf) add("rawhalf=1");
    return items.empty() ? out : out + kMark + items;
}

Interpretation interpretationOf(const MediaItem& m) {
    Interpretation i;
    parseInterpretation(m.path, i);
    return i;
}

}  // namespace montage
