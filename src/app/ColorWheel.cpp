#include "ColorWheel.h"

#include <QConicalGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QRadialGradient>
#include <cmath>

#include "Theme.h"

namespace montage {

ColorWheel::ColorWheel(const QString& title, double scale, QWidget* parent) : QWidget(parent), title_(title), scale_(scale) {
    setToolTip(tr("%1: drag towards a colour to tint that part of the picture; double-click to reset").arg(title));
    setCursor(Qt::CrossCursor);
}

void ColorWheel::puckToRgb(QPointF puck, double scale, double& r, double& g, double& b) {
    // Red at 0°, green at 120°, blue at 240°: offsets that sum to nothing.
    const double rho = std::min(1.0, std::hypot(puck.x(), puck.y())), th = std::atan2(puck.y(), puck.x());
    const double m = rho * scale;
    r = m * std::cos(th);
    g = m * std::cos(th - 2 * M_PI / 3);
    b = m * std::cos(th - 4 * M_PI / 3);
}

QPointF ColorWheel::rgbToPuck(double r, double g, double b, double scale) {
    const double x = r - (g + b) / 2, y = std::sqrt(3.0) / 2 * (g - b);
    const double m = std::hypot(x, y) / 1.5;
    if (m < 1e-12 || scale <= 0) return {0, 0};
    const double rho = std::min(1.0, m / scale);
    return {rho * x / std::hypot(x, y), rho * y / std::hypot(x, y)};
}

void ColorWheel::setBalance(double r, double g, double b) {
    puck_ = rgbToPuck(r, g, b, scale_);
    update();
}

void ColorWheel::balance(double& r, double& g, double& b) const { puckToRgb(puck_, scale_, r, g, b); }

void ColorWheel::setPuck(QPointF p, bool final) {
    const double len = std::hypot(p.x(), p.y());
    puck_ = len > 1 ? p / len : p;
    double r, g, b;
    balance(r, g, b);
    emit changed(r, g, b, final);
    update();
}

QRectF ColorWheel::disc() const {
    const double size = std::min(width(), height() - 18) - 8;
    return QRectF((width() - size) / 2, 4, size, size);
}

QPointF ColorWheel::puckFrom(QPointF pos) const {
    const QRectF d = disc();
    return {(pos.x() - d.center().x()) / (d.width() / 2), -(pos.y() - d.center().y()) / (d.height() / 2)};
}

void ColorWheel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF d = disc();
    // Hues round the rim (red to the right, counter-clockwise), fading to grey in the middle.
    QConicalGradient hues(d.center(), 0);
    for (int i = 0; i <= 6; ++i) hues.setColorAt(i / 6.0, QColor::fromHsvF(float(std::fmod(i / 6.0, 1.0)), 0.75f, 0.75f));
    p.setPen(Qt::NoPen);
    p.setBrush(hues);
    p.drawEllipse(d);
    QRadialGradient fade(d.center(), d.width() / 2);
    fade.setColorAt(0, QColor(90, 90, 90, 255));
    fade.setColorAt(1, QColor(90, 90, 90, 0));
    p.setBrush(fade);
    p.drawEllipse(d);
    p.setPen(QPen(QColor(0, 0, 0, 120), 1));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(d);
    p.drawLine(QPointF(d.center().x() - 5, d.center().y()), QPointF(d.center().x() + 5, d.center().y()));
    p.drawLine(QPointF(d.center().x(), d.center().y() - 5), QPointF(d.center().x(), d.center().y() + 5));
    const QPointF at(d.center().x() + puck_.x() * d.width() / 2, d.center().y() - puck_.y() * d.height() / 2);
    p.setPen(QPen(Qt::black, 1.5));
    p.setBrush(Qt::white);
    p.drawEllipse(at, 5, 5);
    p.setPen(theme::kTextDim);
    p.drawText(QRectF(0, height() - 16, width(), 16), Qt::AlignCenter, title_);
}

void ColorWheel::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    dragging_ = true;
    setPuck(puckFrom(e->position()), false);
}

void ColorWheel::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_) setPuck(puckFrom(e->position()), false);
}

void ColorWheel::mouseReleaseEvent(QMouseEvent* e) {
    if (!dragging_ || e->button() != Qt::LeftButton) return;
    dragging_ = false;
    setPuck(puckFrom(e->position()), true);
}

void ColorWheel::mouseDoubleClickEvent(QMouseEvent*) {
    dragging_ = false;
    setPuck({0, 0}, true);
}

}  // namespace montage
