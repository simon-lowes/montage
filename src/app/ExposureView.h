// Montage — exposure checks on the monitors, as cinema cameras and Resolve show them: zebra stripes over what is at
// or near clipping (or over a skin-tone band), and false colour, where the brightness of each pixel picks a colour
// from a scale (ARRI's: purple for crushed black, blue just above it, green for 18 % grey, pink one stop over,
// yellow near clipping, red clipped, grey elsewhere). Only the picture shown changes, never what is exported.
#pragma once

#include <QColor>
#include <QImage>

namespace montage {

enum class ExposureView { Off, Zebras100, Zebras70, FalseColor };

// Brightness is the picture's Rec.709 luma as shown (0 black, 1 white).
QColor falseColorFor(double luma);
// Zebras100 stripes pixels at 98 % or more; Zebras70 those between 65 and 75 % (faces exposed for skin).
QImage exposureView(const QImage& img, ExposureView mode);

}  // namespace montage
