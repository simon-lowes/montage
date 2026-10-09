#include "TransformOverlay.h"

#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "MonitorPanel.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "render/Compositor.h"

namespace montage {

namespace {

constexpr double kHandle = 7;       // grab distance of a handle, widget pixels
constexpr double kRotateBand = 24;  // how far outside a corner turns the clip
constexpr double kSnap = 8;         // snapping distance, widget pixels

double cross(const QPointF& a, const QPointF& b) { return a.x() * b.y() - a.y() * b.x(); }
double dot(const QPointF& a, const QPointF& b) { return a.x() * b.x() + a.y() * b.y(); }

// A clip the overlay can move: a picture on a video track, not a whole-frame adjustment or a 360° view.
bool movable(const Clip& c) {
    if (!c.enabled || (c.isGenerator() && c.generator.type == "adjustment")) return false;
    return std::none_of(c.effects.begin(), c.effects.end(), [](const Effect& e) { return e.enabled && e.type == "reframe_360"; });
}

}  // namespace

TransformOverlay::TransformOverlay(EditorState* state, ViewerWidget* viewer) : QObject(viewer), state_(state), viewer_(viewer) {
    viewer_->installEventFilter(this);
    viewer_->setMouseTracking(true);
    viewer_->addOverlay([this](QPainter& p, const QRectF& r) { paint(p, r); });
}

Id TransformOverlay::target() const {
    const Sequence* s = state_->sequence();
    const Clip* c = state_->primaryClip();
    if (!s || !c || !c->contains(state_->playhead()) || !movable(*c)) return 0;
    const auto loc = edit::locate(*s, c->id);
    if (!loc || loc->track.kind != TrackKind::Video || trackAt(*s, loc->track)->muted) return 0;
    std::array<QPointF, 4> corners;
    QPointF anchor;
    return box(c->id, corners, anchor) ? c->id : 0;
}

bool TransformOverlay::box(Id clip, std::array<QPointF, 4>& corners, QPointF& anchor) const {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    const QRectF r = viewer_->imageRect();
    if (!c || r.isEmpty() || viewer_->comparing() || viewer_->twoUp()) return false;
    const FrameTime t = state_->playhead(), lt = t - c->start;
    const double cl = std::clamp(c->motion.p("crop_left", lt) / 100, 0.0, 1.0), cr = std::clamp(c->motion.p("crop_right", lt) / 100, 0.0, 1.0);
    const double ct = std::clamp(c->motion.p("crop_top", lt) / 100, 0.0, 1.0), cb = std::clamp(c->motion.p("crop_bottom", lt) / 100, 0.0, 1.0);
    const double us[4] = {cl, 1 - cr, 1 - cr, cl}, vs[4] = {ct, ct, 1 - cb, 1 - cb};
    auto widget = [&](double x, double y) { return QPointF(r.left() + x * r.width() / s->width, r.top() + y * r.height() / s->height); };
    for (int k = 0; k < 4; ++k) {
        double x = 0, y = 0;
        if (!clipFrameToSequence(state_->project(), *s, *c, t, us[k], vs[k], x, y)) return false;
        corners[size_t(k)] = widget(x, y);
    }
    // The anchor sits at the clip's position.
    anchor = widget(s->width / 2.0 + c->motion.p("pos_x", lt), s->height / 2.0 + c->motion.p("pos_y", lt));
    return true;
}

QPointF TransformOverlay::toSequence(const QPointF& w) const {
    const Sequence* s = state_->sequence();
    const QRectF r = viewer_->imageRect();
    if (!s || r.isEmpty()) return {};
    return {(w.x() - r.left()) * s->width / r.width(), (w.y() - r.top()) * s->height / r.height()};
}

TransformOverlay::Grab TransformOverlay::hit(Id clip, const QPointF& pos, int& corner) const {
    std::array<QPointF, 4> c;
    QPointF anchor;
    if (!box(clip, c, anchor)) return Grab::None;
    for (int k = 0; k < 4; ++k)
        if (QLineF(pos, c[size_t(k)]).length() <= kHandle) return corner = k, Grab::Corner;
    for (int k = 0; k < 4; ++k) {
        const QPointF mid = (c[size_t(k)] + c[size_t((k + 1) % 4)]) / 2;
        if (QLineF(pos, mid).length() <= kHandle) return corner = k, (k % 2 == 0 ? Grab::EdgeY : Grab::EdgeX);
    }
    const QPolygonF poly({c[0], c[1], c[2], c[3]});
    if (poly.containsPoint(pos, Qt::OddEvenFill)) return Grab::Move;
    for (int k = 0; k < 4; ++k)
        if (QLineF(pos, c[size_t(k)]).length() <= kRotateBand) return corner = k, Grab::Rotate;
    return Grab::None;
}

Id TransformOverlay::clipAt(const QPointF& pos) const {
    const Sequence* s = state_->sequence();
    if (!s) return 0;
    const FrameTime t = state_->playhead();
    for (int i = int(s->videoTracks.size()) - 1; i >= 0; --i) {
        const Track& tr = s->videoTracks[size_t(i)];
        if (tr.muted) continue;
        for (const Clip& c : tr.clips) {
            if (!c.contains(t) || !movable(c)) continue;
            std::array<QPointF, 4> corners;
            QPointF anchor;
            if (box(c.id, corners, anchor) && QPolygonF({corners[0], corners[1], corners[2], corners[3]}).containsPoint(pos, Qt::OddEvenFill))
                return c.id;
        }
    }
    return 0;
}

void TransformOverlay::paint(QPainter& p, const QRectF&) const {
    const Id clip = grab_ != Grab::None ? clip_ : target();
    std::array<QPointF, 4> c;
    QPointF anchor;
    if (!clip || !box(clip, c, anchor)) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = viewer_->imageRect();
    const Sequence* s = state_->sequence();
    // Guides where a move snapped.
    if (s && (snapX_ >= 0 || snapY_ >= 0)) {
        p.setPen(QPen(QColor(255, 60, 200), 1, Qt::DashLine));
        if (snapX_ >= 0) {
            const double x = r.left() + snapX_ * r.width() / s->width;
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
        if (snapY_ >= 0) {
            const double y = r.top() + snapY_ * r.height() / s->height;
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }
    }
    p.setPen(QPen(QColor(90, 170, 255), 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(QPolygonF({c[0], c[1], c[2], c[3]}));
    p.setBrush(Qt::white);
    for (int k = 0; k < 4; ++k) {
        p.drawRect(QRectF(c[size_t(k)] - QPointF(4, 4), QSizeF(8, 8)));
        p.drawRect(QRectF((c[size_t(k)] + c[size_t((k + 1) % 4)]) / 2 - QPointF(3, 3), QSizeF(6, 6)));
    }
    // The anchor: a small cross in a circle.
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(anchor, 5, 5);
    p.drawLine(anchor - QPointF(8, 0), anchor + QPointF(8, 0));
    p.drawLine(anchor - QPointF(0, 8), anchor + QPointF(0, 8));
    p.restore();
}

void TransformOverlay::drag(const QPointF& pos, Qt::KeyboardModifiers mods) {
    const Sequence* s = state_->gestureBase();
    const Clip* base = s ? edit::clipById(*s, clip_) : nullptr;
    if (!base) return;
    const QPointF now = toSequence(pos), d = now - press_;
    const QRectF r = viewer_->imageRect();
    const double tol = kSnap * s->width / std::max(1.0, r.width());
    double posX = posX_, posY = posY_, scale = scale_, scaleX = scaleX_, scaleY = scaleY_, rotation = rotation_;
    snapX_ = snapY_ = -1;
    switch (grab_) {
        case Grab::Move: {
            double dx = d.x(), dy = d.y();
            if (mods & Qt::ShiftModifier) (std::fabs(dx) > std::fabs(dy) ? dy : dx) = 0;  // along one axis
            if (!(mods & Qt::ControlModifier)) {
                // The moved box's centre and edges against the frame's centre and edges.
                double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
                for (const QPointF& c : corners_) minX = std::min(minX, c.x()), maxX = std::max(maxX, c.x()), minY = std::min(minY, c.y()), maxY = std::max(maxY, c.y());
                // The nearest of centre, near edge and far edge within reach wins.
                auto snap = [&](double lo, double mid, double hi, double frame, double& delta, double& guide) {
                    double best = tol, shift = 0;
                    const std::pair<double, double> pairs[] = {{mid, frame / 2}, {lo, 0.0}, {hi, frame}};
                    for (const auto& [edge, line] : pairs)
                        if (const double off = line - (edge + delta); std::fabs(off) < best) best = std::fabs(off), guide = line, shift = off;
                    delta += shift;
                };
                snap(minX, (minX + maxX) / 2, maxX, s->width, dx, snapX_);
                snap(minY, (minY + maxY) / 2, maxY, s->height, dy, snapY_);
            }
            posX = posX_ + dx;
            posY = posY_ + dy;
            break;
        }
        case Grab::Corner: {
            const double from = QLineF(anchor_, press_).length(), to = QLineF(anchor_, now).length();
            if (from > 1e-6) scale = std::max(1.0, scale_ * to / from);
            break;
        }
        case Grab::EdgeX:
        case Grab::EdgeY: {
            // Along the box's own axis, from the anchor.
            const QPointF axis = grab_ == Grab::EdgeX ? corners_[1] - corners_[0] : corners_[3] - corners_[0];
            const double len = std::hypot(axis.x(), axis.y());
            if (len < 1e-6) break;
            const QPointF u = axis / len;
            const double from = dot(press_ - anchor_, u), to = dot(now - anchor_, u);
            if (std::fabs(from) < 1e-6) break;
            const double f = std::max(0.01, to / from);
            (grab_ == Grab::EdgeX ? scaleX : scaleY) = (grab_ == Grab::EdgeX ? scaleX_ : scaleY_) * f;
            break;
        }
        case Grab::Rotate: {
            const QPointF a = press_ - anchor_, b = now - anchor_;
            double turn = std::atan2(cross(a, b), dot(a, b)) * 180 / M_PI;
            rotation = rotation_ + turn;
            if (mods & Qt::ShiftModifier) rotation = std::round(rotation / 15) * 15;
            break;
        }
        case Grab::None: return;
    }
    const Id clip = clip_;
    const FrameTime lt = state_->playhead() - base->start;
    const Grab g = grab_;
    state_->updateGesture([=](Project& p, Sequence& seq) {
        Clip* c = edit::clipById(seq, clip);
        if (!c) return;
        if (c->motion.empty()) c->motion = makeEffect(p, "transform");
        auto set = [&](const char* name, double v) { c->motion.params[name].set(lt, v); };
        if (g == Grab::Move) set("pos_x", posX), set("pos_y", posY);
        if (g == Grab::Corner) set("scale", scale);
        if (g == Grab::EdgeX) set("scale_x", scaleX);
        if (g == Grab::EdgeY) set("scale_y", scaleY);
        if (g == Grab::Rotate) set("rotation", rotation);
    });
    viewer_->update();
}

bool TransformOverlay::eventFilter(QObject* obj, QEvent* e) {
    if (obj != viewer_) return false;
    switch (e->type()) {
        case QEvent::MouseButtonPress: {
            auto* me = static_cast<QMouseEvent*>(e);
            if (me->button() != Qt::LeftButton || viewer_->lookAround() || state_->inGesture()) return false;
            const QPointF pos = me->position();
            int corner = 0;
            Id clip = target();
            Grab g = clip ? hit(clip, pos, corner) : Grab::None;
            if (g == Grab::None) {
                // A click on another clip's picture selects it, ready to move.
                clip = clipAt(pos);
                if (!clip) return false;
                state_->setSelection({clip}, false);
                g = Grab::Move;
            }
            const Sequence* s = state_->sequence();
            const Clip* c = edit::clipById(*s, clip);
            QPointF anchor;
            std::array<QPointF, 4> corners;
            if (!c || !box(clip, corners, anchor)) return false;
            const FrameTime lt = state_->playhead() - c->start;
            clip_ = clip;
            grab_ = g;
            press_ = toSequence(pos);
            anchor_ = toSequence(anchor);
            for (int k = 0; k < 4; ++k) corners_[size_t(k)] = toSequence(corners[size_t(k)]);
            posX_ = c->motion.p("pos_x", lt), posY_ = c->motion.p("pos_y", lt);
            scale_ = c->motion.p("scale", lt, 100), scaleX_ = c->motion.p("scale_x", lt, 100), scaleY_ = c->motion.p("scale_y", lt, 100);
            rotation_ = c->motion.p("rotation", lt);
            static const char* const kLabels[] = {"", "Move", "Scale", "Stretch", "Stretch", "Rotate"};
            state_->beginGesture(tr(kLabels[int(g)]));
            return true;
        }
        case QEvent::MouseMove: {
            auto* me = static_cast<QMouseEvent*>(e);
            if (grab_ != Grab::None) {
                drag(me->position(), me->modifiers());
                return true;
            }
            // The pointer shows what a press would do.
            int corner = 0;
            const Id clip = (me->buttons() == Qt::NoButton && !viewer_->lookAround()) ? target() : 0;
            switch (clip ? hit(clip, me->position(), corner) : Grab::None) {
                case Grab::Move: viewer_->setCursor(Qt::SizeAllCursor); break;
                case Grab::Corner: viewer_->setCursor(corner % 2 == 0 ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor); break;
                case Grab::EdgeX: viewer_->setCursor(Qt::SizeHorCursor); break;
                case Grab::EdgeY: viewer_->setCursor(Qt::SizeVerCursor); break;
                case Grab::Rotate: viewer_->setCursor(Qt::CrossCursor); break;
                case Grab::None:
                    if (!viewer_->lookAround()) viewer_->unsetCursor();
                    break;
            }
            return false;
        }
        case QEvent::MouseButtonRelease: {
            if (grab_ == Grab::None) return false;
            grab_ = Grab::None;
            snapX_ = snapY_ = -1;
            if (state_->inGesture()) state_->endGesture(true);
            viewer_->update();
            return true;
        }
        default: return false;
    }
}

}  // namespace montage
