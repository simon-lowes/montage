#include "SurroundPanner.h"

#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <cmath>

#include "Theme.h"
#include "core/Surround.h"

namespace montage {

SurroundPanner::SurroundPanner(QWidget* parent) : QWidget(parent) {
    setFixedSize(58, 58);
    setCursor(Qt::CrossCursor);
    setToolTip(tr("Surround: drag to place the track among the speakers (nearer the middle spreads it out).\n"
                  "Wheel: narrower or wider. Double-click: front, as in stereo. Right-click: width and LFE."));
}

void SurroundPanner::setSpeakerLayout(const std::string& layout) {
    layout_ = layout;
    update();
}

void SurroundPanner::setPan(const SurroundPan& p) {
    pan_ = p;
    update();
}

QPointF SurroundPanner::toWidget(double x, double y) const {
    const double r = std::min(width(), height()) / 2.0 - 5;
    return {width() / 2.0 + x * r, height() / 2.0 - y * r};
}

void SurroundPanner::fromWidget(QPointF p, double& x, double& y) const {
    const double r = std::min(width(), height()) / 2.0 - 5;
    x = (p.x() - width() / 2.0) / r;
    y = (height() / 2.0 - p.y()) / r;
    // Inside the circle.
    const double d = std::hypot(x, y);
    if (d > 1) x /= d, y /= d;
}

void SurroundPanner::emitChange(bool final) {
    update();
    emit changed(pan_, final);
}

void SurroundPanner::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPointF c(width() / 2.0, height() / 2.0);
    const double r = std::min(width(), height()) / 2.0 - 5;
    p.setPen(QPen(theme::kTextDim, 1));
    p.setBrush(QColor(0, 0, 0, 60));
    p.drawEllipse(c, r, r);
    // The speakers.
    for (const Speaker& sp : layoutSpeakers(layout_)) {
        if (sp.lfe) continue;
        const double a = sp.angle * M_PI / 180;
        const QPointF at = toWidget(std::sin(a), std::cos(a));
        p.setPen(Qt::NoPen);
        p.setBrush(theme::kTextDim);
        p.drawRect(QRectF(at.x() - 2, at.y() - 2, 4, 4));
    }
    // The track: its two channels and the point between them.
    const double angle = std::atan2(pan_.x, pan_.y), dist = std::min(1.0, std::hypot(pan_.x, pan_.y));
    const double half = 30 * std::clamp(pan_.width, 0.0, 1.0) * M_PI / 180;
    for (double side : {-1.0, 1.0}) {
        const QPointF ch = toWidget(dist * std::sin(angle + side * half), dist * std::cos(angle + side * half));
        p.setPen(QPen(theme::kAccent, 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(ch, 2.5, 2.5);
    }
    const QPointF at = toWidget(pan_.x, pan_.y);
    p.setPen(QPen(Qt::black, 1));
    p.setBrush(pan_.lfeDb > -99 ? QColor(255, 170, 90) : theme::kAccent);
    p.drawEllipse(at, 4, 4);
}

void SurroundPanner::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    dragging_ = true;
    fromWidget(e->position(), pan_.x, pan_.y);
    emitChange(false);
}

void SurroundPanner::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return;
    fromWidget(e->position(), pan_.x, pan_.y);
    emitChange(false);
}

void SurroundPanner::mouseReleaseEvent(QMouseEvent* e) {
    if (!dragging_ || e->button() != Qt::LeftButton) return;
    dragging_ = false;
    fromWidget(e->position(), pan_.x, pan_.y);
    emitChange(true);
}

void SurroundPanner::mouseDoubleClickEvent(QMouseEvent*) {
    dragging_ = false;
    pan_.x = 0, pan_.y = 1;
    emitChange(true);
}

void SurroundPanner::wheelEvent(QWheelEvent* e) {
    pan_.width = std::clamp(pan_.width + (e->angleDelta().y() > 0 ? 0.1 : -0.1), 0.0, 1.0);
    emitChange(true);
}

void SurroundPanner::contextMenuEvent(QContextMenuEvent* e) {
    QMenu menu(this);
    QMenu* w = menu.addMenu(tr("Width"));
    for (int pct : {0, 25, 50, 75, 100}) {
        QAction* a = w->addAction(pct == 0 ? tr("A point (mono)") : tr("%1 %").arg(pct), this, [this, pct] {
            pan_.width = pct / 100.0;
            emitChange(true);
        });
        a->setCheckable(true);
        a->setChecked(std::lround(pan_.width * 100) == pct);
    }
    QMenu* lfe = menu.addMenu(tr("LFE"));
    for (int db : {-100, -18, -12, -6, 0}) {
        QAction* a = lfe->addAction(db <= -99 ? tr("Off") : tr("%1 dB").arg(db), this, [this, db] {
            pan_.lfeDb = db;
            emitChange(true);
        });
        a->setCheckable(true);
        a->setChecked(std::lround(pan_.lfeDb) == db || (db <= -99 && pan_.lfeDb <= -99));
    }
    menu.addSeparator();
    menu.addAction(tr("Front (as in stereo)"), this, [this] {
        pan_ = SurroundPan{};
        emitChange(true);
    });
    menu.addAction(tr("Centre speaker (dialogue)"), this, [this] {
        pan_.x = 0, pan_.y = 1, pan_.width = 0;
        emitChange(true);
    });
    menu.exec(e->globalPos());
}

}  // namespace montage
