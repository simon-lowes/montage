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
    viewer_->addOverlay([this](QPainter& p, const QRectF& r) { paint(p, r); });
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
            if (!e.enabled || (shape != 1 && shape != 2 && shape != 5 && shape != 6)) continue;
            Shape sh;
            sh.clip = c.id;
            sh.effect = e.id;
            sh.shape = shape;
            const MaskBox b = maskBox(e, lt);
            sh.x = b.x;
            sh.y = b.y;
            sh.w = b.w;
            sh.h = b.h;
            sh.rotation = b.rotation;
            if (shape == 5) {
                sh.path = maskPath(e, lt);
                sh.open = e.p("mask.open", lt) > 0.5;
            }
            out.push_back(std::move(sh));
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

bool MaskOverlay::frameSize(const Shape& s, double& mw, double& mh) const {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, s.clip) : nullptr;
    return c && clipFrameSize(state_->project(), *seq, *c, mw, mh);
}

bool MaskOverlay::boxToWidget(const Shape& s, double bx, double by, QPointF& out) const {
    double mw = 1, mh = 1, u = 0, v = 0;
    if (!frameSize(s, mw, mh)) return false;
    boxToFrame(s.box(), mw, mh, bx, by, u, v);
    return toWidget(s, u, v, out);
}

bool MaskOverlay::widgetToBox(const Shape& s, const QPointF& pt, double& bx, double& by) const {
    double mw = 1, mh = 1, u = 0, v = 0;
    if (!frameSize(s, mw, mh) || !fromWidget(s, pt, u, v)) return false;
    frameToBox(s.box(), mw, mh, u, v, bx, by);
    return true;
}

bool MaskOverlay::rotateHandle(const Shape& s, QPointF& out) const {
    QPointF ctr, top;
    if (s.drawing() || !boxToWidget(s, 0, 0, ctr) || !boxToWidget(s, 0, -0.5, top)) return false;
    QPointF d = top - ctr;
    const double len = std::hypot(d.x(), d.y());
    d = len > 1 ? d / len : QPointF(0, -1);
    out = top + d * 22;
    return true;
}

QPolygonF MaskOverlay::pathPolygon(const Shape& s) const {
    QPolygonF poly;
    for (const auto& [x, y] : flattenMaskPath(s.path, !s.open, 24)) {
        QPointF w;
        if (boxToWidget(s, x, y, w)) poly << w;
    }
    return poly;
}

void MaskOverlay::paintPath(QPainter& p, const Shape& s) const {
    if (s.path.empty()) return;
    QPolygonF poly = pathPolygon(s);
    const bool closed = !s.drawing();
    if (closed && !poly.isEmpty()) poly << poly.front();
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0, 0, 0, 160), 3));
    p.drawPolyline(poly);
    p.setPen(QPen(QColor(255, 214, 90), 1.5));
    p.drawPolyline(poly);
    for (size_t i = 0; i < s.path.size(); ++i) {
        const PathPoint& pt = s.path[i];
        QPointF a;
        if (!boxToWidget(s, pt.x, pt.y, a)) continue;
        if (closed && pt.smooth())
            for (bool out : {false, true}) {
                QPointF h;
                if (!boxToWidget(s, pt.x + (out ? pt.ox : pt.ix), pt.y + (out ? pt.oy : pt.iy), h)) continue;
                p.setPen(QPen(QColor(255, 214, 90, 200), 1));
                p.drawLine(a, h);
                p.setBrush(QColor(255, 214, 90));
                p.setPen(QPen(QColor(0, 0, 0, 160), 1));
                p.drawEllipse(h, 3.5, 3.5);
            }
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        // Corners are squares, smooth points circles; the first point is larger while drawing (click it to close).
        p.setBrush(QColor(255, 214, 90));
        const double r = (!closed && i == 0 && s.path.size() >= 3) ? 6 : 4;
        if (pt.smooth()) p.drawEllipse(a, r, r);
        else p.drawRect(QRectF(a.x() - r, a.y() - r, 2 * r, 2 * r));
    }
    if (!closed) return;
    QPointF ctr, rot;
    if (boxToWidget(s, 0, 0, ctr) && rotateHandle(s, rot)) {
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        p.drawLine(ctr + QPointF(-6, 0), ctr + QPointF(6, 0));
        p.drawLine(ctr + QPointF(0, -6), ctr + QPointF(0, 6));
        p.setBrush(QColor(255, 214, 90));
        p.drawEllipse(rot, 4.5, 4.5);
    }
}

void MaskOverlay::paintGradient(QPainter& p, const Shape& s) const {
    // The middle of the ramp, its two ends (dashed) and an arrow towards the side the effect covers.
    const double reach = 4 / std::max(0.01, s.w);
    p.save();
    p.setClipRect(viewer_->imageRect());
    p.setBrush(Qt::NoBrush);
    for (double by : {-0.5, 0.0, 0.5}) {
        QPointF a, b;
        if (!boxToWidget(s, -reach, by, a) || !boxToWidget(s, reach, by, b)) continue;
        p.setPen(QPen(QColor(0, 0, 0, 160), 3));
        p.drawLine(a, b);
        p.setPen(QPen(QColor(255, 214, 90), 1.5, by == 0 ? Qt::SolidLine : Qt::DashLine));
        p.drawLine(a, b);
    }
    p.restore();
    QPointF ctr, top, wh, hh, rot;
    if (!boxToWidget(s, 0, 0, ctr) || !boxToWidget(s, 0, -0.5, top) || !handles(s, ctr, wh, hh)) return;
    p.setPen(QPen(QColor(255, 214, 90), 1.5));
    p.drawLine(ctr, top);
    QPointF d = top - ctr;
    const double len = std::hypot(d.x(), d.y());
    if (len > 8) {
        d /= len;
        const QPointF n(-d.y(), d.x());
        p.setBrush(QColor(255, 214, 90));
        p.drawPolygon(QPolygonF({top, top - d * 8 + n * 4, top - d * 8 - n * 4}));
    }
    p.setBrush(QColor(255, 214, 90));
    p.setPen(QPen(QColor(0, 0, 0, 160), 1));
    p.drawRect(QRectF(hh.x() - 4, hh.y() - 4, 8, 8));
    if (rotateHandle(s, rot)) p.drawEllipse(rot, 4.5, 4.5);
    p.drawLine(ctr + QPointF(-6, 0), ctr + QPointF(6, 0));
    p.drawLine(ctr + QPointF(0, -6), ctr + QPointF(0, 6));
}

void MaskOverlay::paint(QPainter& p, const QRectF&) const {
    const Sequence* seq = state_->sequence();
    if (!seq) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    for (const Shape& s : shapes()) {
        if (s.shape == 5) {
            paintPath(p, s);
            continue;
        }
        if (s.shape == 6) {
            paintGradient(p, s);
            continue;
        }
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
            if (QPointF rot; rotateHandle(s, rot)) p.drawEllipse(rot, 4.5, 4.5);
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
                e.params["mask.rotation"].set(lt, s.rotation);
                return true;
            }
        return false;
    }, QStringLiteral("mask-%1-%2").arg(s.effect).arg(dragSerial_));
}

bool MaskOverlay::editMask(const Shape& s, const QString& label, const std::function<bool(Effect&, FrameTime)>& fn) {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, s.clip) : nullptr;
    if (!c) return false;
    const FrameTime lt = state_->playhead() - c->start;
    const Id clip = s.clip, effect = s.effect;
    return state_->edit(label, [clip, effect, lt, fn](Project&, Sequence& sq) {
        Effect* e = edit::ownedEffect(sq, clip, effect);
        return e && fn(*e, lt);
    }, QStringLiteral("mask-%1-%2").arg(effect).arg(dragSerial_));
}

std::optional<MaskOverlay::Shape> MaskOverlay::refreshed(const Shape& s) const {
    for (const Shape& now : shapes())
        if (now.clip == s.clip && now.effect == s.effect) return now;
    return std::nullopt;
}

void MaskOverlay::applyPath(const Shape& s) {
    editMask(s, tr("Adjust Mask Path"), [path = s.path](Effect& e, FrameTime lt) {
        setMaskPath(e, lt, path);
        return true;
    });
}

bool MaskOverlay::pathPress(const QPointF& pos, Qt::KeyboardModifiers mods) {
    for (const Shape& s : shapes()) {
        if (s.shape != 5) continue;
        const int n = int(s.path.size());
        const bool drawing = s.drawing();
        // Handles first (they can sit over the outline), then the points.
        if (!drawing)
            for (int i = 0; i < n; ++i) {
                const PathPoint& pt = s.path[size_t(i)];
                if (!pt.smooth()) continue;
                for (bool out : {true, false}) {
                    QPointF h;
                    if (!boxToWidget(s, pt.x + (out ? pt.ox : pt.ix), pt.y + (out ? pt.oy : pt.iy), h) ||
                        QLineF(pos, h).length() > kHandleRadius)
                        continue;
                    grab_ = Grab::PathHandle;
                    dragged_ = s;
                    grabIndex_ = i;
                    grabOut_ = out;
                    ++dragSerial_;
                    return true;
                }
            }
        for (int i = 0; i < n; ++i) {
            QPointF a;
            const PathPoint& pt = s.path[size_t(i)];
            if (!boxToWidget(s, pt.x, pt.y, a) || QLineF(pos, a).length() > kHandleRadius) continue;
            ++dragSerial_;
            if (drawing && i == 0 && n >= 3) {
                double mw = 1, mh = 1;
                if (frameSize(s, mw, mh))
                    editMask(s, tr("Close Mask Path"), [mw, mh](Effect& e, FrameTime lt) { return closeMaskPath(e, lt, mw, mh); });
                return true;
            }
            if (!drawing && (mods & (Qt::ControlModifier | Qt::MetaModifier))) {
                if (!editMask(s, tr("Remove Mask Point"), [i](Effect& e, FrameTime lt) { return removeMaskPoint(e, lt, i); }))
                    state_->message(tr("A mask path needs at least three points"), 4000);
                return true;
            }
            if (!drawing && (mods & Qt::AltModifier)) {
                editMask(s, tr("Smooth Mask Point"), [i](Effect& e, FrameTime lt) {
                    toggleMaskPointSmooth(e, lt, i);
                    return true;
                });
                return true;
            }
            double bx = 0, by = 0;
            if (!widgetToBox(s, pos, bx, by)) return true;
            grab_ = Grab::PathPoint;
            dragged_ = s;
            grabIndex_ = i;
            grabBx_ = bx - pt.x;
            grabBy_ = by - pt.y;
            return true;
        }
        double bx = 0, by = 0;
        if (drawing) {
            // The next point: a corner, or a curve if the pointer is dragged before letting go.
            if (!widgetToBox(s, pos, bx, by)) continue;
            ++dragSerial_;
            const bool added = editMask(s, tr("Draw Mask Point"), [bx, by](Effect& e, FrameTime lt) {
                editMaskPath(e, lt, [bx, by](std::vector<PathPoint>& pts) { pts.push_back({bx, by}); });
                e.params["mask.open"] = Param(1);
                return true;
            });
            if (auto now = refreshed(s); added && now && !now->path.empty()) {
                grab_ = Grab::NewPoint;
                dragged_ = *now;
                grabIndex_ = int(now->path.size()) - 1;
            }
            return true;
        }
        // On the outline: a new point there, held to drag.
        int seg = -1;
        double at = 0, best = 6;
        constexpr int kSteps = 24;
        for (int i = 0; i < n; ++i) {
            const PathPoint& a = s.path[size_t(i)];
            const PathPoint& b = s.path[size_t(i + 1) % size_t(n)];
            QPointF prev;
            for (int k = 0; k <= kSteps; ++k) {
                const auto [x, y] = segmentPoint(a, b, double(k) / kSteps);
                QPointF w;
                if (!boxToWidget(s, x, y, w)) break;
                if (k > 0) {
                    const QPointF d = w - prev;
                    const double len2 = QPointF::dotProduct(d, d);
                    const double f = len2 > 0 ? std::clamp(QPointF::dotProduct(pos - prev, d) / len2, 0.0, 1.0) : 0.0;
                    const double dist = QLineF(pos, prev + d * f).length();
                    if (dist < best) {
                        best = dist;
                        seg = i;
                        at = (k - 1 + f) / kSteps;
                    }
                }
                prev = w;
            }
        }
        if (seg < 0) continue;
        ++dragSerial_;
        int index = -1;
        if (!editMask(s, tr("Add Mask Point"), [seg, at, &index](Effect& e, FrameTime lt) {
                index = insertMaskPoint(e, lt, seg, at);
                return index >= 0;
            }))
            return true;
        if (auto now = refreshed(s); now && index >= 0 && index < int(now->path.size()) && widgetToBox(*now, pos, bx, by)) {
            grab_ = Grab::PathPoint;
            dragged_ = *now;
            grabIndex_ = index;
            grabBx_ = bx - now->path[size_t(index)].x;
            grabBy_ = by - now->path[size_t(index)].y;
        }
        return true;
    }
    return false;
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
        if (pathPress(pos, me->modifiers())) return true;
        for (const Shape& s : shapes()) {
            QPointF ctr, wh, hh, rot;
            if (s.drawing() || !handles(s, ctr, wh, hh)) continue;
            Grab g = Grab::None;
            if (rotateHandle(s, rot) && QLineF(pos, rot).length() <= kHandleRadius) g = Grab::Rotate;
            else if (s.shape != 5 && s.shape != 6 && QLineF(pos, wh).length() <= kHandleRadius) g = Grab::Width;
            else if (s.shape != 5 && QLineF(pos, hh).length() <= kHandleRadius) g = Grab::Height;
            else if (s.shape == 5) {
                double u, v;
                if (pathPolygon(s).containsPoint(pos, Qt::WindingFill) && fromWidget(s, pos, u, v)) {
                    g = Grab::Move;
                    grabDu_ = u - s.x;
                    grabDv_ = v - s.y;
                }
            } else {
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
            bool draw = false;
            for (const Shape& s : shapes()) {
                QPointF ctr, wh, hh, rot;
                if (s.drawing()) draw = true;
                if (!s.drawing() && handles(s, ctr, wh, hh) &&
                    ((s.shape != 5 && s.shape != 6 && QLineF(me->position(), wh).length() <= kHandleRadius) ||
                     (s.shape != 5 && QLineF(me->position(), hh).length() <= kHandleRadius) ||
                     (rotateHandle(s, rot) && QLineF(me->position(), rot).length() <= kHandleRadius)))
                    over = true;
                for (const PathPoint& pt : s.path) {
                    QPointF a;
                    if (boxToWidget(s, pt.x, pt.y, a) && QLineF(me->position(), a).length() <= kHandleRadius) over = true;
                }
            }
            for (const Pin& pin : pins())
                for (int k = 0; k < 4; ++k) {
                    QPointF h;
                    if (cornerHandle(pin, k, h) && QLineF(me->position(), h).length() <= kHandleRadius) over = true;
                }
            if (over) viewer_->setCursor(Qt::SizeAllCursor);
            else if (draw) viewer_->setCursor(Qt::CrossCursor);
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
        if (grab_ == Grab::PathPoint || grab_ == Grab::PathHandle || grab_ == Grab::NewPoint) {
            double bx = 0, by = 0;
            if (!widgetToBox(dragged_, me->position(), bx, by)) return true;
            Shape s = dragged_;
            if (grabIndex_ < 0 || grabIndex_ >= int(s.path.size())) return true;
            PathPoint& pt = s.path[size_t(grabIndex_)];
            if (grab_ == Grab::PathPoint) {
                pt.x = bx - grabBx_;
                pt.y = by - grabBy_;
            } else if (grab_ == Grab::NewPoint) {
                // Pulling out a new point's handles, both ways.
                pt.ox = bx - pt.x;
                pt.oy = by - pt.y;
                pt.ix = -pt.ox;
                pt.iy = -pt.oy;
            } else {
                double& hx = grabOut_ ? pt.ox : pt.ix;
                double& hy = grabOut_ ? pt.oy : pt.iy;
                hx = bx - pt.x;
                hy = by - pt.y;
                if (!(me->modifiers() & Qt::AltModifier)) {
                    // The other handle turns with it, keeping its length (Alt breaks the curve into a corner).
                    double& kx = grabOut_ ? pt.ix : pt.ox;
                    double& ky = grabOut_ ? pt.iy : pt.oy;
                    const double keep = std::hypot(kx, ky), len = std::hypot(hx, hy);
                    if (len > 1e-9) {
                        kx = -hx / len * keep;
                        ky = -hy / len * keep;
                    }
                }
            }
            applyPath(s);
            return true;
        }
        double u, v;
        if (!fromWidget(dragged_, me->position(), u, v)) return true;
        Shape s = dragged_;
        if (grab_ == Grab::Rotate) {
            double mw = 1, mh = 1;
            if (!frameSize(s, mw, mh)) return true;
            double a = std::atan2((v - s.y) * mh, (u - s.x) * mw) * 180.0 / M_PI + 90.0;
            if (me->modifiers() & Qt::ShiftModifier) a = std::round(a / 15.0) * 15.0;
            s.rotation = std::remainder(a, 360.0);
        } else if (grab_ == Grab::Move) {
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
