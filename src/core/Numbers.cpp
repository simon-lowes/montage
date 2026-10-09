#include "Numbers.h"

#include <cmath>
#include <locale>
#include <sstream>

namespace montage {

std::string formatNumber(double v, int digits) {
    if (!std::isfinite(v)) return "0";
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s.precision(digits);
    s << v;
    return s.str();
}

double parseNumber(const std::string& text, double fallback) {
    std::istringstream s(text.size() > 1 && text[0] == '+' ? text.substr(1) : text);
    s.imbue(std::locale::classic());
    double v = 0;
    s >> v;
    return s.fail() ? fallback : v;
}

}  // namespace montage
