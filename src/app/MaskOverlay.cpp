#include "MaskOverlay.h"

#include <QFutureWatcher>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>

#include "EditorState.h"
#include "MonitorPanel.h"
#include "ModelPacks.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "render/ClipAnalysis.h"
#include "render/Compositor.h"

namespace montage {

namespace {
constexpr double kHandleRadius = 9;
}

MaskOverlay::MaskOverlay(EditorState* state, ViewerWidget* viewer) : QObject(viewer), state_(state), viewer_(viewer) {
    pool_.setMaxThreadCount(1);
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

std::vector<MaskOverlay::Pin> MaskOverlay::pins() const {
    std::vector<Pin> out;
    const Sequence* s = state_->sequence();
    const FrameTime t = state_->playhead();
    if (!s) return out;
    for (Id id : state_->selectedClips()) {
        auto loc = edit::locate(*s, id);
        if (!loc || loc->track.kind != TrackKind::Video) continue;
        const Clip& c = trackAt(*s, loc->track)->clips[loc->index];
        if (!c.contains(t)) continue;
        for (const Effect& e : c.effects) {
            if (!e.enabled || e.type != "corner_pin") continue;
            const TrackQuad q = cornerPinQuad(e, t - c.start);
            Pin pin{c.id, e.id};
            for (int k = 0; k < 4; ++k) {
                pin.u[k] = q.p[k].x;
                pin.v[k] = q.p[k].y;
            }
            out.push_back(pin);
        }
    }
    return out;
}

bool MaskOverlay::cornerHandle(const Pin& pin, int k, QPointF& out) const {
    return k >= 0 && k < 4 && toWidget(pin.clip, pin.u[k], pin.v[k], out);
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

bool MaskOverlay::toWidget(const Shape& s, double u, double v, QPointF& out) const { return toWidget(s.clip, u, v, out); }

bool MaskOverlay::fromWidget(const Shape& s, const QPointF& pt, double& u, double& v) const {
    return fromWidget(s.clip, pt, u, v);
}

bool MaskOverlay::toWidget(Id clip, double u, double v, QPointF& out) const {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, clip) : nullptr;
    const QRectF r = viewer_->imageRect();
    double x = 0, y = 0;
    if (!c || r.isEmpty() || !clipFrameToSequence(state_->project(), *seq, *c, state_->playhead(), u, v, x, y)) return false;
    out = QPointF(r.left() + x * r.width() / seq->width, r.top() + y * r.height() / seq->height);
    return true;
}

bool MaskOverlay::fromWidget(Id clip, const QPointF& pt, double& u, double& v) const {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, clip) : nullptr;
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
    paintPins(p);
    paintObjects(p);
}

void MaskOverlay::paintPins(QPainter& p) const {
    const auto all = pins();
    if (all.empty()) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (const Pin& pin : all) {
        QPointF c[4];
        bool ok = true;
        for (int k = 0; k < 4; ++k) ok = ok && cornerHandle(pin, k, c[k]);
        if (!ok) continue;
        QPolygonF quad;
        for (const QPointF& pt : c) quad << pt;
        quad << c[0];
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 160), 3));
        p.drawPolyline(quad);
        p.setPen(QPen(QColor(90, 200, 255), 1.5, Qt::DashLine));
        p.drawPolyline(quad);
        p.setBrush(QColor(90, 200, 255));
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        for (const QPointF& pt : c) p.drawEllipse(pt, 5, 5);
    }
    p.restore();
}

void MaskOverlay::applyPin(const Pin& pin) {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, pin.clip) : nullptr;
    if (!c) return;
    const FrameTime lt = state_->playhead() - c->start;
    state_->edit(tr("Adjust Corner Pin"), [pin, lt](Project&, Sequence& sq) {
        Clip* cl = edit::clipById(sq, pin.clip);
        if (!cl) return false;
        static const char* const names[4][2] = {{"tl_x", "tl_y"}, {"tr_x", "tr_y"}, {"br_x", "br_y"}, {"bl_x", "bl_y"}};
        for (Effect& e : cl->effects)
            if (e.id == pin.effect) {
                // Keyframed corners get a key at the playhead; others change outright.
                for (int k = 0; k < 4; ++k) {
                    e.params[names[k][0]].set(lt, pin.u[k]);
                    e.params[names[k][1]].set(lt, pin.v[k]);
                }
                return true;
            }
        return false;
    }, QStringLiteral("pin-%1-%2").arg(pin.effect).arg(dragSerial_));
}

// ---- Object masks ------------------------------------------------------------------

std::vector<MaskOverlay::ObjectTarget> MaskOverlay::objectTargets() const {
    std::vector<ObjectTarget> out;
    const Sequence* s = state_->sequence();
    const FrameTime t = state_->playhead();
    if (!s) return out;
    for (Id id : state_->selectedClips()) {
        auto loc = edit::locate(*s, id);
        if (!loc || loc->track.kind != TrackKind::Video) continue;
        const Clip& c = trackAt(*s, loc->track)->clips[loc->index];
        const MediaItem* m = c.mediaId ? state_->project().findMedia(c.mediaId) : nullptr;
        if (!c.contains(t) || !m || m->kind != MediaKind::Video) continue;
        const FrameTime lt = t - c.start;
        for (const Effect& e : c.effects)
            if (e.enabled && std::lround(e.p("mask.shape", lt)) == 3)
                out.push_back({c.id, e.id, lt, e.object, clipObjectFrame(state_->project(), *s, c, lt)});
    }
    return out;
}

void MaskOverlay::paintObjects(QPainter& p) const {
    const auto targets = objectTargets();
    if (targets.empty()) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const ObjectTarget& t : targets) {
        // The clip's frame in the widget: the segmentation is drawn into it as a tint.
        QPointF c00, c10, c11, c01;
        if (!toWidget(t.clip, 0, 0, c00) || !toWidget(t.clip, 1, 0, c10) || !toWidget(t.clip, 1, 1, c11) ||
            !toWidget(t.clip, 0, 1, c01))
            continue;
        std::vector<float> logits;
        if (t.object && t.object->logits(t.frame, logits)) {
            const QString key = QStringLiteral("%1:%2").arg(quintptr(t.object.get())).arg(t.frame);
            if (key != tintKey_) {
                tint_ = QImage(kObjectGrid, kObjectGrid, QImage::Format_ARGB32_Premultiplied);
                for (int y = 0; y < kObjectGrid; ++y) {
                    auto* row = reinterpret_cast<QRgb*>(tint_.scanLine(y));
                    for (int x = 0; x < kObjectGrid; ++x) {
                        const float a = std::clamp(0.5f + logits[size_t(y) * kObjectGrid + size_t(x)], 0.f, 1.f) * 0.42f;
                        row[x] = qPremultiply(qRgba(70, 160, 255, int(a * 255)));
                    }
                }
                tintKey_ = key;
            }
            QTransform tr;
            if (QTransform::quadToQuad(QPolygonF(QRectF(0, 0, kObjectGrid, kObjectGrid)), QPolygonF({c00, c10, c11, c01}), tr)) {
                p.save();
                p.setTransform(tr, true);
                p.drawImage(QRectF(0, 0, kObjectGrid, kObjectGrid), tint_);
                p.restore();
            }
        }
        // The clicks on this frame: + the object, - not the object, and the box.
        if (t.object)
            if (auto it = t.object->prompts.find(t.frame); it != t.object->prompts.end()) {
                QPointF box[2];
                int corners = 0;
                for (const ObjectPoint& pt : it->second) {
                    QPointF w;
                    if (!toWidget(t.clip, pt.x, pt.y, w)) continue;
                    if (pt.label >= 2) {
                        box[pt.label - 2] = w;
                        ++corners;
                        continue;
                    }
                    const QColor fill = pt.label == 1 ? QColor(60, 200, 90) : QColor(230, 70, 60);
                    p.setPen(QPen(QColor(0, 0, 0, 180), 1.5));
                    p.setBrush(fill);
                    p.drawEllipse(w, 6, 6);
                    p.setPen(QPen(Qt::white, 1.6));
                    p.drawLine(w + QPointF(-3, 0), w + QPointF(3, 0));
                    if (pt.label == 1) p.drawLine(w + QPointF(0, -3), w + QPointF(0, 3));
                }
                if (corners == 2) {
                    p.setBrush(Qt::NoBrush);
                    p.setPen(QPen(QColor(0, 0, 0, 160), 3));
                    p.drawRect(QRectF(box[0], box[1]).normalized());
                    p.setPen(QPen(QColor(255, 214, 90), 1.5, Qt::DashLine));
                    p.drawRect(QRectF(box[0], box[1]).normalized());
                }
            }
    }
    if (objectGesture_ && QLineF(pressPos_, dragPos_).length() >= 6) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 214, 90), 1.5, Qt::DashLine));
        p.drawRect(QRectF(pressPos_, dragPos_).normalized());
    }
    if (pending_ > 0) {
        const QRectF r = viewer_->imageRect();
        p.setPen(Qt::white);
        p.drawText(r.adjusted(8, 6, -8, -6), Qt::AlignLeft | Qt::AlignTop, tr("Finding the object…"));
    }
    p.restore();
}

bool MaskOverlay::objectPress(const QPointF& pos) {
    for (const ObjectTarget& t : objectTargets()) {
        double u, v;
        if (!fromWidget(t.clip, pos, u, v) || u < 0 || v < 0 || u > 1 || v > 1) continue;
        objectGesture_ = true;
        objectTarget_ = t;
        pressPos_ = dragPos_ = pos;
        return true;
    }
    return false;
}

void MaskOverlay::objectRelease(const QPointF& pos, Qt::KeyboardModifiers mods) {
    objectGesture_ = false;
    viewer_->update();
    const ObjectTarget t = objectTarget_;
    double u0, v0, u1, v1;
    if (!fromWidget(t.clip, pressPos_, u0, v0) || !fromWidget(t.clip, pos, u1, v1)) return;
    auto clamp01 = [](double x) { return std::clamp(x, 0.0, 1.0); };
    std::vector<ObjectPoint> pts;
    if (t.object)
        if (auto it = t.object->prompts.find(t.frame); it != t.object->prompts.end()) pts = it->second;
    if (QLineF(pressPos_, pos).length() >= 6) {
        // A box around the object (one per frame).
        pts.erase(std::remove_if(pts.begin(), pts.end(), [](const ObjectPoint& p) { return p.label >= 2; }), pts.end());
        pts.insert(pts.begin(), {{clamp01(std::min(u0, u1)), clamp01(std::min(v0, v1)), 2},
                                 {clamp01(std::max(u0, u1)), clamp01(std::max(v0, v1)), 3}});
    } else if (mods & (Qt::ControlModifier | Qt::MetaModifier)) {
        // Remove the click under the pointer (both corners for the box).
        int nearest = -1;
        double best = 10;
        for (size_t i = 0; i < pts.size(); ++i) {
            QPointF w;
            if (toWidget(t.clip, pts[i].x, pts[i].y, w) && QLineF(w, pos).length() < best) {
                best = QLineF(w, pos).length();
                nearest = int(i);
            }
        }
        if (nearest < 0) return;
        if (pts[size_t(nearest)].label >= 2)
            pts.erase(std::remove_if(pts.begin(), pts.end(), [](const ObjectPoint& p) { return p.label >= 2; }), pts.end());
        else
            pts.erase(pts.begin() + nearest);
    } else {
        pts.push_back({clamp01(u1), clamp01(v1), (mods & Qt::AltModifier) ? 0 : 1});
    }
    pickObject(t, pts);
}

void MaskOverlay::pickObject(const ObjectTarget& t, const std::vector<ObjectPoint>& points) {
    if (!points.empty() && !ensureObjectModel(viewer_->window())) return;
    const bool changed = state_->edit(points.empty() ? tr("Clear Object Clicks") : tr("Pick Object"), [t, points](Project& p, Sequence& s) {
        Clip* c = edit::clipById(s, t.clip);
        Effect* e = c ? edit::ownedEffect(s, t.clip, t.effect) : nullptr;
        if (!c || !e) return false;
        e->object = withObjectPrompts(p, s, *c, *e, t.local, points);
        return e->object != nullptr;
    });
    if (!changed || points.empty()) return;
    // Segment the frame from a copy of the project, off the UI thread.
    auto project = std::make_shared<const Project>(state_->project());
    const Id seq = state_->sequence()->id;
    using Out = std::pair<std::shared_ptr<const ObjectMask>, std::string>;
    auto* watcher = new QFutureWatcher<Out>(this);
    ++pending_;
    viewer_->update();
    QPointer<EditorState> st(state_);
    connect(watcher, &QFutureWatcher<Out>::finished, this, [this, watcher, st, t, points] {
        watcher->deleteLater();
        --pending_;
        viewer_->update();
        const Out r = watcher->result();
        if (!st) return;
        if (!r.first) {
            if (!r.second.empty()) st->message(QString::fromStdString(r.second), 6000);
            return;
        }
        // Kept only while the clicks it answers are still the ones on that frame.
        st->amend([t, points, r](Project&, Sequence& s) {
            Effect* e = edit::ownedEffect(s, t.clip, t.effect);
            if (!e || !e->object) return false;
            const int64_t n = t.frame;
            auto have = e->object->prompts.find(n);
            auto seg = r.first->frames.find(n);
            if (have == e->object->prompts.end() || have->second != points || seg == r.first->frames.end()) return false;
            auto o = std::make_shared<ObjectMask>(*e->object);
            o->frames[n] = seg->second;
            e->object = o;
            return true;
        });
    });
    watcher->setFuture(QtConcurrent::run(&pool_, [project, seq, t]() -> Out {
        const Sequence* s = project->findSequence(seq);
        const Clip* c = s ? edit::clipById(*s, t.clip) : nullptr;
        const Effect* e = s ? edit::ownedEffect(const_cast<Sequence&>(*s), t.clip, t.effect) : nullptr;
        if (!c || !e || !e->object) return {};
        std::string err;
        auto o = segmentClipObjectFrame(*project, *s, *c, *e->object, t.local, &err);
        return {o, err};
    }));
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
        for (const Pin& pin : pins())
            for (int k = 0; k < 4; ++k) {
                QPointF h;
                if (cornerHandle(pin, k, h) && QLineF(pos, h).length() <= kHandleRadius) {
                    grab_ = Grab::Corner;
                    draggedPin_ = pin;
                    grabCorner_ = k;
                    ++dragSerial_;
                    return true;
                }
            }
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
        return objectPress(pos);
    }
    if (objectGesture_ && e->type() == QEvent::MouseMove) {
        dragPos_ = static_cast<QMouseEvent*>(e)->position();
        viewer_->update();
        return true;
    }
    if (objectGesture_ && e->type() == QEvent::MouseButtonRelease) {
        auto* me = static_cast<QMouseEvent*>(e);
        objectRelease(me->position(), me->modifiers());
        return true;
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
            for (const Pin& pin : pins())
                for (int k = 0; k < 4; ++k) {
                    QPointF h;
                    if (cornerHandle(pin, k, h) && QLineF(me->position(), h).length() <= kHandleRadius) over = true;
                }
            if (over) viewer_->setCursor(Qt::SizeAllCursor);
            else viewer_->unsetCursor();
            return false;
        }
        if (grab_ == Grab::Corner) {
            double u, v;
            if (!fromWidget(draggedPin_.clip, me->position(), u, v)) return true;
            draggedPin_.u[grabCorner_] = std::clamp(u, -1.0, 2.0);
            draggedPin_.v[grabCorner_] = std::clamp(v, -1.0, 2.0);
            applyPin(draggedPin_);
            return true;
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
