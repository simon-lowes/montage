#include "ExposureView.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "render/Gamut.h"

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

QImage gamutView(const QImage& picture, const QImage& signal, const QString& space, Primaries target) {
    if (picture.isNull() || signal.isNull()) return picture;
    const ColorSpace* cs = findColorSpace(space.toStdString());
    if (!cs) cs = &rec709Space();
    const GamutCheck check(*cs, target);
    if (!check.possible()) return picture.convertToFormat(QImage::Format_RGB32);  // (a Rec.709 frame inside P3...)
    // Each pixel of the signal checked once (16-bit codes to light by table), then the picture striped where its
    // pixel falls on one outside. Near black, where chroma noise has no colour to speak of, is left alone (as the
    // chromaticity scope leaves it).
    const QImage codes = signal.convertToFormat(QImage::Format_RGBA64);
    const std::vector<float>& lin = linearTable16(cs->transfer);
    const int sw = codes.width(), sh = codes.height();
    constexpr quint16 kFloor = quint16(0.02 * 65535);
    std::vector<uint8_t> out(size_t(sw) * size_t(sh), 0);
    for (int y = 0; y < sh; ++y) {
        const auto* px = reinterpret_cast<const quint16*>(codes.constScanLine(y));
        for (int x = 0; x < sw; ++x, px += 4) {
            if (std::max({px[0], px[1], px[2]}) < kFloor) continue;
            const double l[3] = {lin[px[0]], lin[px[1]], lin[px[2]]};
            out[size_t(y) * size_t(sw) + size_t(x)] = check.outsideLinear(l);
        }
    }
    QImage img = picture.convertToFormat(QImage::Format_RGB32);
    const int w = img.width(), h = img.height();
    for (int y = 0; y < h; ++y) {
        auto* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        const uint8_t* row = out.data() + size_t(std::min(sh - 1, int(int64_t(y) * sh / h))) * size_t(sw);
        for (int x = 0; x < w; ++x)
            // Magenta and black stripes, the other way from the zebras.
            if (row[std::min(sw - 1, int(int64_t(x) * sw / w))]) line[x] = ((x - y + 4096) / 4) % 2 ? qRgb(20, 0, 20) : qRgb(255, 40, 220);
    }
    return img;
}

}  // namespace montage
