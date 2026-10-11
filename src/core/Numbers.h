// Montage — numbers written into files and media paths, the same in every locale: a C-library printf or atof
// follows the user's locale (Qt applications set it from the environment), so a German or French system would
// write 1,5 for 1.5 and read 0.5 as 0.
#pragma once

#include <string>

namespace montage {

// `v` with up to `digits` significant digits and a '.' for the decimal point, shortest form ("1.5", "24", "0.04").
std::string formatNumber(double v, int digits = 10);
// The number at the start of `text` written that way (a leading '+' allowed); `fallback` if there is none.
double parseNumber(const std::string& text, double fallback = 0);

}  // namespace montage
