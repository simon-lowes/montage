#include "CurveEditor.h"

#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

#include "Theme.h"
#include "render/Processing.h"

namespace montage {

namespace {
constexpr double kGrab = 7;  // px
}

CurveEditor::CurveEditor(Mode mode, QWidget* parent) : QWidget(parent), mode_(mode) {
    setMouseTracking(true);
    setToolTip(mode == Mode::Tone ? tr("Drag points; click the line to add one; double-click a point to remove it")
                                  : tr("Drag up or down to change these colours; click to add a point; double-click a point to remove it"));
    setPoints(QString());
}

void CurveEditor::setPoints(const QString& points) {
    pts_.clear();
    for (const QString& tok : points.split(' ', Qt::SkipEmptyParts)) {
        const QStringList xy = tok.split(',');
        if (xy.size() != 2) continue;
        bool ox = false, oy = false;
        const double x = xy[0].toDouble(&ox), y = xy[1].toDouble(&oy);
        if (ox && oy) pts_.push_back({std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0)});
    }
    if (mode_ == Mode::Tone && pts_.size() < 2) pts_ = {{0, 0}, {1, 1}};
    sortPoints();
    drag_ = -1;
    update();
}

QString CurveEditor::points() const {
    QStringList out;
    for (const QPointF& p : pts_) out << QStringLiteral("%1,%2").arg(p.x(), 0, 'g', 4).arg(p.y(), 0, 'g', 4);
    return out.join(' ');
}

double CurveEditor::valueAt(double x) const {
    const std::string s = points().toStdString();
    const int n = 1024;
    const auto lut = mode_ == Mode::Tone ? buildCurve(s, n) : buildFlatCurve(s, n, mode_ == Mode::Hue);
    const double f = std::clamp(x, 0.0, 1.0) * n;
    const int i = std::min(n - 1, int(f));
    return lut[size_t(i)] + (lut[size_t(i) + 1] - lut[size_t(i)]) * (f - i);
}

QRectF CurveEditor::plot() const { return QRectF(rect()).adjusted(6, 6, -6, -6); }

QPointF CurveEditor::toWidget(QPointF c) const {
    const QRectF r = plot();
    return {r.left() + c.x() * r.width(), r.bottom() - c.y() * r.height()};
}

QPointF CurveEditor::fromWidget(QPointF p) const {
    const QRectF r = plot();
    return {std::clamp((p.x() - r.left()) / r.width(), 0.0, 1.0), std::clamp((r.bottom() - p.y()) / r.height(), 0.0, 1.0)};
}

int CurveEditor::pointAt(QPointF pos) const {
    int best = -1;
    double bestD = kGrab;
    for (size_t i = 0; i < pts_.size(); ++i) {
        const double d = std::hypot(toWidget(pts_[i]).x() - pos.x(), toWidget(pts_[i]).y() - pos.y());
        if (d <= bestD) {
            bestD = d;
            best = int(i);
        }
    }
    return best;
}

void CurveEditor::sortPoints() {
    std::stable_sort(pts_.begin(), pts_.end(), [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); });
}

void CurveEditor::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plot();
    p.fillRect(rect(), theme::kPanelAlt);
    // Background: what the x axis is.
    if (mode_ == Mode::Hue) {
        QLinearGradient g(r.topLeft(), r.topRight());
        for (int i = 0; i <= 6; ++i) g.setColorAt(i / 6.0, QColor::fromHsvF(float(std::fmod(i / 6.0, 1.0)), 0.55f, 0.45f));
        p.fillRect(r, g);
    } else {
        QLinearGradient g(r.topLeft(), r.topRight());
        g.setColorAt(0, QColor(20, 20, 20));
        g.setColorAt(1, QColor(120, 120, 120));
        p.fillRect(r, mode_ == Mode::Tone ? QBrush(QColor(28, 28, 30)) : QBrush(g));
    }
    p.setPen(QPen(QColor(255, 255, 255, 30), 1));
    for (int i = 1; i < 4; ++i) {
        p.drawLine(QPointF(r.left() + r.width() * i / 4, r.top()), QPointF(r.left() + r.width() * i / 4, r.bottom()));
        p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4), QPointF(r.right(), r.top() + r.height() * i / 4));
    }
    // What "no change" looks like.
    p.setPen(QPen(QColor(255, 255, 255, 70), 1, Qt::DashLine));
    if (mode_ == Mode::Tone) p.drawLine(toWidget({0, 0}), toWidget({1, 1}));
    else p.drawLine(toWidget({0, 0.5}), toWidget({1, 0.5}));
    // The curve as it renders.
    QPainterPath path;
    const int steps = std::max(32, int(r.width()));
    for (int i = 0; i <= steps; ++i) {
        const double x = double(i) / steps;
        const QPointF w = toWidget({x, valueAt(x)});
        if (i == 0) path.moveTo(w);
        else path.lineTo(w);
    }
    p.setPen(QPen(theme::kAccent, 2));
    p.drawPath(path);
    p.setPen(QPen(Qt::black, 1));
    p.setBrush(Qt::white);
    for (size_t i = 0; i < pts_.size(); ++i) p.drawEllipse(toWidget(pts_[i]), int(i) == drag_ ? 5 : 4, int(i) == drag_ ? 5 : 4);
}

void CurveEditor::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    drag_ = pointAt(e->position());
    if (drag_ < 0) {
        // A new point on the curve where it was clicked.
        const QPointF c = fromWidget(e->position());
        pts_.push_back(mode_ == Mode::Tone ? QPointF(c.x(), c.y()) : QPointF(c.x(), valueAt(c.x())));
        sortPoints();
        drag_ = pointAt(toWidget(pts_.back()));
        for (size_t i = 0; i < pts_.size(); ++i)
            if (std::fabs(pts_[i].x() - c.x()) < 1e-9) drag_ = int(i);
        emit edited(points(), false);
    }
    update();
}

void CurveEditor::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ < 0 || !(e->buttons() & Qt::LeftButton)) {
        setCursor(pointAt(e->position()) >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
        return;
    }
    QPointF c = fromWidget(e->position());
    // Points keep their order; a tone curve's ends stay at the edges.
    const double lo = drag_ > 0 ? pts_[size_t(drag_) - 1].x() + 1e-3 : 0.0;
    const double hi = drag_ + 1 < int(pts_.size()) ? pts_[size_t(drag_) + 1].x() - 1e-3 : 1.0;
    c.setX(std::clamp(c.x(), lo, hi));
    if (mode_ == Mode::Tone && (drag_ == 0 || drag_ == int(pts_.size()) - 1)) c.setX(drag_ == 0 ? 0.0 : 1.0);
    pts_[size_t(drag_)] = c;
    emit edited(points(), false);
    update();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || drag_ < 0) return;
    drag_ = -1;
    emit edited(points(), true);
    update();
}

void CurveEditor::mouseDoubleClickEvent(QMouseEvent* e) {
    const int i = pointAt(e->position());
    if (i < 0) return;
    // A tone curve keeps its two ends.
    if (mode_ == Mode::Tone && (i == 0 || i == int(pts_.size()) - 1)) return;
    pts_.erase(pts_.begin() + i);
    drag_ = -1;
    emit edited(points(), true);
    update();
}

}  // namespace montage
