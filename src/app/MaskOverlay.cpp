#include "MaskOverlay.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

#include "EditorState.h"
#include "MonitorPanel.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "render/Compositor.h"

namespace montage {

namespace {
constexpr double kHandleRadius = 9;
}

MaskOverlay::MaskOverlay(EditorState* state, ViewerWidget* viewer) : QObject(viewer), state_(state), viewer_(viewer) {
    viewer_->installEventFilter(this);
    viewer_->setMouseTracking(true);
    viewer_->setOverlay([this](QPainter& p, const QRectF& r) { paint(p, r); });
    for (auto sig : {&EditorState::projectChanged, &EditorState::selectionChanged, &EditorState::sequenceSwitched})
        connect(state_, sig, viewer_, qOverload<>(&QWidget::update));
    connect(state_, &EditorState::playheadChanged, viewer_, qOverload<>(&QWidget::update));
}

std::vector<MaskOverlay::Shape> MaskOverlay::shapes() const {
    std::vector<Shape> out;
    const Sequence* s = state_->sequence();
    const FrameTime t = state_->playhead();
    if (!s) return out;
    for (Id id : state_->selectedClips()) {
        auto loc = edit::locate(*s, id);
        if (!loc || loc->track.kind != TrackKind::Video) continue;
        const Clip& c = trackAt(*s, loc->track)->clips[loc->index];
        if (!c.contains(t)) continue;
        const FrameTime lt = t - c.start;
        for (const Effect& e : c.effects) {
            const int shape = int(std::lround(e.p("mask.shape", lt)));
            if (!e.enabled || (shape != 1 && shape != 2)) continue;
            out.push_back({c.id, e.id, shape, e.p("mask.x", lt, 0.5), e.p("mask.y", lt, 0.5), e.p("mask.w", lt, 0.4),
                           e.p("mask.h", lt, 0.4), e.p("mask.rotation", lt)});
        }
    }
    return out;
}

void MaskOverlay::localToFrame(const Shape& s, double lx, double ly, double& u, double& v) const {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, s.clip) : nullptr;
    double mw = 1, mh = 1;
    if (!c || !clipFrameSize(state_->project(), *seq, *c, mw, mh)) mw = mh = 1;
    const double r = s.rotation * M_PI / 180.0;
    u = s.x + (lx * std::cos(r) - ly * std::sin(r)) / mw;
    v = s.y + (lx * std::sin(r) + ly * std::cos(r)) / mh;
}

bool MaskOverlay::toWidget(const Shape& s, double u, double v, QPointF& out) const {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, s.clip) : nullptr;
    const QRectF r = viewer_->imageRect();
    double x = 0, y = 0;
    if (!c || r.isEmpty() || !clipFrameToSequence(state_->project(), *seq, *c, state_->playhead(), u, v, x, y)) return false;
    out = QPointF(r.left() + x * r.width() / seq->width, r.top() + y * r.height() / seq->height);
    return true;
}

bool MaskOverlay::fromWidget(const Shape& s, const QPointF& pt, double& u, double& v) const {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, s.clip) : nullptr;
    const QRectF r = viewer_->imageRect();
    if (!c || r.isEmpty()) return false;
    const double x = (pt.x() - r.left()) * seq->width / r.width(), y = (pt.y() - r.top()) * seq->height / r.height();
    return sequenceToClipFrame(state_->project(), *seq, *c, state_->playhead(), x, y, u, v);
}

bool MaskOverlay::handles(const Shape& s, QPointF& center, QPointF& widthHandle, QPointF& heightHandle) const {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, s.clip) : nullptr;
    double mw = 1, mh = 1;
    if (!c || !clipFrameSize(state_->project(), *seq, *c, mw, mh)) return false;
    double u = 0, v = 0;
    if (!toWidget(s, s.x, s.y, center)) return false;
    localToFrame(s, s.w * mw / 2, 0, u, v);
    if (!toWidget(s, u, v, widthHandle)) return false;
    localToFrame(s, 0, s.h * mh / 2, u, v);
    return toWidget(s, u, v, heightHandle);
}

void MaskOverlay::paint(QPainter& p, const QRectF&) const {
    const Sequence* seq = state_->sequence();
    if (!seq) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (const Shape& s : shapes()) {
        const Clip* c = edit::clipById(*seq, s.clip);
        double mw = 1, mh = 1;
        if (!c || !clipFrameSize(state_->project(), *seq, *c, mw, mh)) continue;
        const double a = s.w * mw / 2, b = s.h * mh / 2;
        QPainterPath path;
        const int n = s.shape == 1 ? 72 : 4;
        for (int i = 0; i <= n; ++i) {
            double lx, ly;
            if (s.shape == 1) {
                const double phi = 2 * M_PI * i / n;
                lx = a * std::cos(phi);
                ly = b * std::sin(phi);
            } else {
                static const int sx[5] = {-1, 1, 1, -1, -1}, sy[5] = {-1, -1, 1, 1, -1};
                lx = a * sx[i];
                ly = b * sy[i];
            }
            double u, v;
            localToFrame(s, lx, ly, u, v);
            QPointF w;
            if (!toWidget(s, u, v, w)) continue;
            if (i == 0) path.moveTo(w);
            else path.lineTo(w);
        }
        p.setPen(QPen(QColor(0, 0, 0, 160), 3));
        p.drawPath(path);
        p.setPen(QPen(QColor(255, 214, 90), 1.5));
        p.drawPath(path);
        QPointF ctr, wh, hh;
        if (handles(s, ctr, wh, hh)) {
            p.setBrush(QColor(255, 214, 90));
            p.setPen(QPen(QColor(0, 0, 0, 160), 1));
            for (const QPointF& h : {wh, hh}) p.drawRect(QRectF(h.x() - 4, h.y() - 4, 8, 8));
            p.drawLine(ctr + QPointF(-6, 0), ctr + QPointF(6, 0));
            p.drawLine(ctr + QPointF(0, -6), ctr + QPointF(0, 6));
        }
    }
    p.restore();
}

void MaskOverlay::apply(const Shape& s) {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, s.clip) : nullptr;
    if (!c) return;
    const FrameTime lt = state_->playhead() - c->start;
    state_->edit(tr("Adjust Mask"), [s, lt](Project&, Sequence& sq) {
        Clip* cl = edit::clipById(sq, s.clip);
        if (!cl) return false;
        for (Effect& e : cl->effects)
            if (e.id == s.effect) {
                // Keyframed parameters get a key at the playhead; others change outright.
                e.params["mask.x"].set(lt, s.x);
                e.params["mask.y"].set(lt, s.y);
                e.params["mask.w"].set(lt, s.w);
                e.params["mask.h"].set(lt, s.h);
                return true;
            }
        return false;
    }, QStringLiteral("mask-%1-%2").arg(s.effect).arg(dragSerial_));
}

bool MaskOverlay::eventFilter(QObject* obj, QEvent* e) {
    if (obj != viewer_) return false;
    if (e->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(e);
        if (me->button() != Qt::LeftButton) return false;
        const QPointF pos = me->position();
        for (const Shape& s : shapes()) {
            QPointF ctr, wh, hh;
            if (!handles(s, ctr, wh, hh)) continue;
            Grab g = Grab::None;
            if (QLineF(pos, wh).length() <= kHandleRadius) g = Grab::Width;
            else if (QLineF(pos, hh).length() <= kHandleRadius) g = Grab::Height;
            else {
                // Inside the shape moves it.
                double u, v;
                if (fromWidget(s, pos, u, v)) {
                    const Sequence* seq = state_->sequence();
                    const Clip* c = edit::clipById(*seq, s.clip);
                    double mw = 1, mh = 1;
                    clipFrameSize(state_->project(), *seq, *c, mw, mh);
                    const double r = -s.rotation * M_PI / 180.0;
                    const double dx = (u - s.x) * mw, dy = (v - s.y) * mh;
                    const double lx = (dx * std::cos(r) - dy * std::sin(r)) / std::max(1e-9, s.w * mw / 2);
                    const double ly = (dx * std::sin(r) + dy * std::cos(r)) / std::max(1e-9, s.h * mh / 2);
                    const bool inside = s.shape == 1 ? lx * lx + ly * ly <= 1 : std::fabs(lx) <= 1 && std::fabs(ly) <= 1;
                    if (inside) {
                        g = Grab::Move;
                        grabDu_ = u - s.x;
                        grabDv_ = v - s.y;
                    }
                }
            }
            if (g != Grab::None) {
                grab_ = g;
                dragged_ = s;
                ++dragSerial_;
                return true;
            }
        }
        return false;
    }
    if (e->type() == QEvent::MouseMove) {
        auto* me = static_cast<QMouseEvent*>(e);
        if (grab_ == Grab::None) {
            // Hover feedback.
            bool over = false;
            for (const Shape& s : shapes()) {
                QPointF ctr, wh, hh;
                if (handles(s, ctr, wh, hh) &&
                    (QLineF(me->position(), wh).length() <= kHandleRadius || QLineF(me->position(), hh).length() <= kHandleRadius))
                    over = true;
            }
            if (over) viewer_->setCursor(Qt::SizeAllCursor);
            else viewer_->unsetCursor();
            return false;
        }
        double u, v;
        if (!fromWidget(dragged_, me->position(), u, v)) return true;
        Shape s = dragged_;
        if (grab_ == Grab::Move) {
            s.x = u - grabDu_;
            s.y = v - grabDv_;
        } else {
            const Sequence* seq = state_->sequence();
            const Clip* c = edit::clipById(*seq, s.clip);
            double mw = 1, mh = 1;
            if (!c || !clipFrameSize(state_->project(), *seq, *c, mw, mh)) return true;
            const double r = -s.rotation * M_PI / 180.0;
            const double dx = (u - s.x) * mw, dy = (v - s.y) * mh;
            const double lx = dx * std::cos(r) - dy * std::sin(r), ly = dx * std::sin(r) + dy * std::cos(r);
            if (grab_ == Grab::Width) s.w = std::max(0.002, 2 * std::fabs(lx) / mw);
            else s.h = std::max(0.002, 2 * std::fabs(ly) / mh);
        }
        apply(s);
        return true;
    }
    if (e->type() == QEvent::MouseButtonRelease && grab_ != Grab::None) {
        grab_ = Grab::None;
        return true;
    }
    return false;
}

}  // namespace montage
