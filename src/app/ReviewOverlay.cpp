#include "ReviewOverlay.h"

#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <cmath>

#include "EditorState.h"
#include "MonitorPanel.h"

namespace montage {

ReviewOverlay::ReviewOverlay(EditorState* state, ViewerWidget* viewer) : QObject(viewer), state_(state), viewer_(viewer) {
    QPointer<ReviewOverlay> self(this);
    viewer_->addOverlay([self](QPainter& p, const QRectF& r) {
        if (self && self->shown_) paintStrokes(p, r, self->strokesNow());
    });
    connect(state_, &EditorState::playheadChanged, this, [this] { viewer_->update(); });
    connect(state_, &EditorState::projectChanged, this, [this] { viewer_->update(); });
    connect(state_, &EditorState::sequenceSwitched, this, [this] { viewer_->update(); });
}

void ReviewOverlay::setShown(bool on) {
    shown_ = on;
    viewer_->update();
}

std::vector<ReviewStroke> ReviewOverlay::strokesNow() const {
    std::vector<ReviewStroke> out;
    const Sequence* s = state_->sequence();
    if (!s) return out;
    const FrameTime at = s->playhead;
    for (const Marker& m : s->markers) {
        if (m.drawing.empty()) continue;
        const FrameTime from = m.drawingFrame();  // (where it was drawn, when the marker had to sit beside it)
        if (m.duration > 1 ? (at < from || at >= from + m.duration) : at != from) continue;
        for (ReviewStroke& st : parseReviewDrawing(m.drawing)) out.push_back(std::move(st));
    }
    return out;
}

void ReviewOverlay::paintStrokes(QPainter& p, const QRectF& r, const std::vector<ReviewStroke>& strokes) {
    if (strokes.empty() || r.isEmpty()) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    auto at = [&](const std::array<double, 2>& q) { return QPointF(r.left() + q[0] * r.width(), r.top() + q[1] * r.height()); };
    for (const ReviewStroke& st : strokes) {
        const double width = std::max(1.5, st.width * r.width());
        QPainterPath path;
        const QPointF a = at(st.points.front()), b = at(st.points.back());
        if (st.tool == "pen") {
            path.moveTo(a);
            if (st.points.size() == 1) path.lineTo(a + QPointF(0.01, 0));
            for (size_t i = 1; i < st.points.size(); ++i) path.lineTo(at(st.points[i]));
        } else if (st.tool == "box") {
            path.addRect(QRectF(a, b).normalized());
        } else if (st.tool == "ellipse") {
            path.addEllipse(QRectF(a, b).normalized());
        } else {  // arrow: the line and a head at its end
            const double angle = std::atan2(b.y() - a.y(), b.x() - a.x()), head = std::max(10.0, width * 4);
            path.moveTo(a);
            path.lineTo(b);
            path.moveTo(b - head * QPointF(std::cos(angle - 0.45), std::sin(angle - 0.45)));
            path.lineTo(b);
            path.lineTo(b - head * QPointF(std::cos(angle + 0.45), std::sin(angle + 0.45)));
        }
        // A soft dark edge, as on the page, so a light stroke shows on a light picture.
        p.setPen(QPen(QColor(0, 0, 0, 110), width + 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
        p.setPen(QPen(QColor(QString::fromStdString(st.color)), width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(path);
    }
    p.restore();
}

}  // namespace montage
