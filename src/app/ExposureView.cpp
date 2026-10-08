#include "ExposureView.h"

#include <algorithm>

namespace montage {

QColor falseColorFor(double y) {
    if (y >= 0.99) return QColor(230, 30, 30);      // clipped
    if (y >= 0.97) return QColor(250, 230, 40);     // about to clip
    if (y >= 0.52 && y <= 0.56) return QColor(240, 130, 190);  // a stop over grey: skin
    if (y >= 0.38 && y <= 0.42) return QColor(70, 190, 70);    // 18 % grey
    if (y >= 0.025 && y <= 0.04) return QColor(40, 90, 230);   // just above black
    if (y < 0.025) return QColor(130, 40, 170);     // crushed
    const int g = std::clamp(int(y * 255 + 0.5), 0, 255);
    return QColor(g, g, g);
}

QImage exposureView(const QImage& src, ExposureView mode) {
    if (mode == ExposureView::Off || src.isNull()) return src;
    QImage img = src.convertToFormat(QImage::Format_RGB32);
    const int w = img.width(), h = img.height();
    for (int y = 0; y < h; ++y) {
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const QRgb c = line[x];
            const double luma = (0.2126 * qRed(c) + 0.7152 * qGreen(c) + 0.0722 * qBlue(c)) / 255.0;
            if (mode == ExposureView::FalseColor) {
                line[x] = falseColorFor(luma).rgb();
                continue;
            }
            const bool hit = mode == ExposureView::Zebras100 ? luma >= 0.98 : luma >= 0.65 && luma <= 0.75;
            // Diagonal stripes 8 px apart, half dark and half light, so they show on any colour.
            if (hit) line[x] = ((x + y) / 4) % 2 ? qRgb(20, 20, 20) : qRgb(235, 235, 235);
        }
    }
    return img;
}

}  // namespace montage
