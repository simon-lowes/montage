#include "ColorWarperEditor.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace montage {

namespace {
constexpr double kGrab = 8;
}

ColorWarperEditor::ColorWarperEditor(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setToolTip(tr("Drag a point to move its colours to another hue and saturation; click to select it for its brightness; "
                  "double-click to put it back"));
}

void ColorWarperEditor::setMesh(const QString& mesh) {
    ColorWarp w;
    if (!parseColorWarp(mesh.toStdString(), w)) w = {};
    warp_ = std::move(w);
    update();
}

double ColorWarperEditor::radius() const { return std::max(10.0, std::min(width(), height()) / 2.0 - kGrab - 2); }
QPointF ColorWarperEditor::centre() const { return QPointF(width() / 2.0, height() / 2.0); }

QPointF ColorWarperEditor::colourPos(double hue, double sat) const {
    const double a = hue * 2 * M_PI, r = radius() * std::clamp(sat, 0.0, 1.0);
    return centre() + QPointF(r * std::cos(a), -r * std::sin(a));
}

QPointF ColorWarperEditor::pointPos(int spoke, int ring) const {
    if (ring <= 0) return centre();
    const WarpPoint p = warp_.at(spoke, ring);
    return colourPos(warpHue(spoke) + p.dh / 360.0, warpSat(ring) + p.ds);
}

void ColorWarperEditor::fromWidget(const QPointF& pos, double& hue, double& sat) const {
    const QPointF d = pos - centre();
    hue = std::atan2(-d.y(), d.x()) / (2 * M_PI);
    hue -= std::floor(hue);
    sat = std::clamp(std::hypot(d.x(), d.y()) / radius(), 0.0, 1.0);
}

bool ColorWarperEditor::pointAt(const QPointF& pos, int& spoke, int& ring) const {
    // A moved point can sit on another: the selected one first, then the nearest, moved ones winning ties.
    if (selSpoke_ >= 0 && QLineF(pos, pointPos(selSpoke_, selRing_)).length() <= kGrab) {
        spoke = selSpoke_;
        ring = selRing_;
        return true;
    }
    double best = kGrab + 1e-9;
    bool found = false, bestMoved = false;
    for (int j = 1; j <= kWarpRings; ++j)
        for (int i = 0; i < kWarpSpokes; ++i) {
            const double d = QLineF(pos, pointPos(i, j)).length();
            const WarpPoint w = warp_.at(i, j);
            const bool moved = w.dh != 0 || w.ds != 0 || w.dl != 0;
            if (d < best - 0.5 || (d <= best + 0.5 && d <= kGrab && moved && !bestMoved)) {
                best = d;
                spoke = i;
                ring = j;
                found = true;
                bestMoved = moved;
            }
        }
    return found;
}

void ColorWarperEditor::select(int spoke, int ring) {
    selSpoke_ = spoke;
    selRing_ = ring;
    update();
    emit selectionChanged();
}

void ColorWarperEditor::setSelectedLuma(double stops) {
    if (selSpoke_ < 0) return;
    WarpPoint p = warp_.at(selSpoke_, selRing_);
    if (p.dl == stops) return;
    p.dl = stops;
    warp_.set(p);
    update();
    emit edited(mesh(), true);
}

void ColorWarperEditor::resetSelected() {
    if (selSpoke_ < 0) return;
    warp_.set({selSpoke_, selRing_});
    update();
    emit edited(mesh(), true);
    emit selectionChanged();
}

void ColorWarperEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double R = radius();
    const QPointF c = centre();
    // The wheel: each colour at its hue and saturation.
    const int size = int(std::ceil(2 * R));
    if (wheel_.width() != size) {
        wheel_ = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
        wheel_.fill(Qt::transparent);
        for (int y = 0; y < size; ++y) {
            auto* row = reinterpret_cast<QRgb*>(wheel_.scanLine(y));
            for (int x = 0; x < size; ++x) {
                const double dx = x + 0.5 - R, dy = y + 0.5 - R, r = std::hypot(dx, dy);
                if (r > R + 0.5) continue;
                double h = std::atan2(-dy, dx) / (2 * M_PI);
                h -= std::floor(h);
                const QColor col = QColor::fromHsvF(float(h), float(std::min(1.0, r / R)), 0.85f);
                const double edge = std::clamp(R + 0.5 - r, 0.0, 1.0);
                row[x] = qPremultiply(qRgba(col.red(), col.green(), col.blue(), int(255 * edge)));
            }
        }
    }
    p.drawImage(QPointF(c.x() - R, c.y() - R), wheel_);
    // Faint rings and spokes where the mesh started.
    p.setPen(QPen(QColor(0, 0, 0, 50), 1));
    for (int j = 1; j <= kWarpRings; ++j) p.drawEllipse(c, R * warpSat(j), R * warpSat(j));
    // The mesh as it is now.
    p.setPen(QPen(QColor(20, 20, 20, 200), 1.4));
    for (int j = 1; j <= kWarpRings; ++j) {
        QPolygonF ring;
        for (int i = 0; i <= kWarpSpokes; ++i) ring << pointPos(i % kWarpSpokes, j);
        p.drawPolyline(ring);
    }
    for (int i = 0; i < kWarpSpokes; ++i) {
        QPolygonF spoke;
        spoke << c;
        for (int j = 1; j <= kWarpRings; ++j) spoke << pointPos(i, j);
        p.drawPolyline(spoke);
    }
    // The points: moved ones filled, the selected one ringed.
    for (int j = 1; j <= kWarpRings; ++j)
        for (int i = 0; i < kWarpSpokes; ++i) {
            const WarpPoint w = warp_.at(i, j);
            const bool moved = w.dh != 0 || w.ds != 0 || w.dl != 0;
            const QPointF at = pointPos(i, j);
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(moved ? QColor(255, 255, 255) : QColor(255, 255, 255, 110));
            p.drawEllipse(at, moved ? 4.0 : 3.0, moved ? 4.0 : 3.0);
            if (i == selSpoke_ && j == selRing_) {
                p.setBrush(Qt::NoBrush);
                p.setPen(QPen(QColor(255, 214, 90), 2));
                p.drawEllipse(at, 7.0, 7.0);
            }
        }
}

void ColorWarperEditor::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    int i = 0, j = 0;
    if (!pointAt(e->position(), i, j)) {
        select(-1, 0);
        return;
    }
    select(i, j);
    dragSpoke_ = i;
    dragRing_ = j;
    moved_ = false;
}

void ColorWarperEditor::mouseMoveEvent(QMouseEvent* e) {
    if (dragSpoke_ < 0) {
        int i = 0, j = 0;
        setCursor(pointAt(e->position(), i, j) ? Qt::SizeAllCursor : Qt::ArrowCursor);
        return;
    }
    double h = 0, s = 0;
    fromWidget(e->position(), h, s);
    WarpPoint p = warp_.at(dragSpoke_, dragRing_);
    double dh = (h - warpHue(dragSpoke_)) * 360;
    dh -= 360 * std::round(dh / 360);  // the short way round
    p.dh = dh;
    p.ds = s - warpSat(dragRing_);
    warp_.set(p);
    moved_ = true;
    update();
    emit edited(mesh(), false);
}

void ColorWarperEditor::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || dragSpoke_ < 0) return;
    dragSpoke_ = -1;
    if (moved_) emit edited(mesh(), true);
    emit selectionChanged();
}

void ColorWarperEditor::mouseDoubleClickEvent(QMouseEvent* e) {
    int i = 0, j = 0;
    if (!pointAt(e->position(), i, j)) return;
    select(i, j);
    resetSelected();
}

}  // namespace montage
