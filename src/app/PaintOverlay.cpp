#include "PaintOverlay.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <map>

#include "EditorState.h"
#include "MonitorPanel.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "render/Compositor.h"
#include "render/MeshWarp.h"
#include "render/Processing.h"
#include "render/Restoration.h"

namespace montage {

PaintOverlay::PaintOverlay(EditorState* state, ViewerWidget* viewer) : QObject(viewer), state_(state), viewer_(viewer) {
    viewer_->installEventFilter(this);
    viewer_->setMouseTracking(true);
    viewer_->addOverlay([this](QPainter& p, const QRectF& r) { paint(p, r); });
    for (auto sig : {&EditorState::projectChanged, &EditorState::selectionChanged, &EditorState::sequenceSwitched,
                     &EditorState::paintEffectChanged})
        connect(state_, sig, viewer_, qOverload<>(&QWidget::update));
    connect(state_, &EditorState::playheadChanged, viewer_, qOverload<>(&QWidget::update));
    connect(state_, &EditorState::paintEffectChanged, this, [this] {
        source_.reset();
        stroke_.clear();
        dragging_ = -1;
    });
}

std::optional<PaintOverlay::Target> PaintOverlay::target() const {
    const Id armed = state_->paintEffect();
    const Sequence* s = state_->sequence();
    if (!armed || !s) return std::nullopt;
    const FrameTime t = state_->playhead();
    for (Id id : state_->selectedClips()) {
        auto loc = edit::locate(*s, id);
        if (!loc || loc->track.kind != TrackKind::Video) continue;
        const Clip& c = trackAt(*s, loc->track)->clips[loc->index];
        if (!c.contains(t)) continue;
        // The clip frame as the render takes it (Stop Motion steps it).
        FrameTime lt = t - c.start;
        for (const Effect& e : c.effects)
            if (e.enabled && e.type == "stop_motion") lt -= lt % std::max<FrameTime>(1, FrameTime(std::lround(e.p("hold", lt, 3))));
        for (const Effect& e : c.effects)
            if (e.id == armed && e.enabled &&
                (e.type == "paint" || e.type == "dust_spots" || e.type == "depth_blur" || e.type == "mesh_warp" || e.type == "grid_warp")) {
                // Dust spots are taken out where a video or picture file is read (not in nested sequences or generators).
                const MediaItem* m = state_->project().findMedia(c.mediaId);
                if (e.type == "dust_spots" && (!m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image))) return std::nullopt;
                Target tg;
                tg.clip = c.id;
                tg.effect = e.id;
                tg.dust = e.type == "dust_spots";
                tg.focus = e.type == "depth_blur";
                tg.mesh = e.type == "mesh_warp";
                tg.grid = e.type == "grid_warp";
                tg.lt = lt;
                tg.brush = e.p("brush", tg.lt, tg.dust ? 24 : 30);
                return tg;
            }
    }
    return std::nullopt;
}

bool PaintOverlay::toWidget(Id clip, double u, double v, QPointF& out) const {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, clip) : nullptr;
    const QRectF r = viewer_->imageRect();
    double x = 0, y = 0;
    if (!c || r.isEmpty() || !clipFrameToSequence(state_->project(), *seq, *c, state_->playhead(), u, v, x, y)) return false;
    out = QPointF(r.left() + x * r.width() / seq->width, r.top() + y * r.height() / seq->height);
    return true;
}

bool PaintOverlay::fromWidget(Id clip, const QPointF& pt, double& u, double& v) const {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, clip) : nullptr;
    const QRectF r = viewer_->imageRect();
    if (!c || r.isEmpty()) return false;
    const double x = (pt.x() - r.left()) * seq->width / r.width(), y = (pt.y() - r.top()) * seq->height / r.height();
    return sequenceToClipFrame(state_->project(), *seq, *c, state_->playhead(), x, y, u, v);
}

double PaintOverlay::brushRadius(const Target& t, const QPointF& at) const {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, t.clip) : nullptr;
    const QRectF r = viewer_->imageRect();
    double fw = 0, fh = 0, u0, v0, u1, v1;
    if (!c || r.isEmpty() || !clipFrameSize(state_->project(), *seq, *c, fw, fh) || fw <= 0) return 0.01;
    const double widget = t.brush / 2 * r.width() / seq->width;
    if (!fromWidget(t.clip, at, u0, v0) || !fromWidget(t.clip, at + QPointF(widget, 0), u1, v1)) return 0.01;
    return std::max(1e-4, std::hypot(u1 - u0, (v1 - v0) * fh / fw));
}

void PaintOverlay::paint(QPainter& p, const QRectF& picture) {
    const std::optional<Target> t = target();
    if (!t) return;
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, t->clip) : nullptr;
    const Effect* e = nullptr;
    if (c)
        for (const Effect& x : c->effects)
            if (x.id == t->effect) e = &x;
    if (!e) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Qt::NoBrush);
    const double wpx = picture.width() / seq->width;  // widget pixels a sequence pixel
    if (t->mesh || t->grid) {
        const std::vector<std::pair<int, QPointF>> hs = handles(*t, *e);
        auto shown = [&](int id, QPointF at) { return id == dragging_ && drawing_.effect == t->effect ? mouse_ + grab_ : at; };
        if (t->grid) {
            // The grid's lines, then its points.
            const int cols = std::clamp(int(std::lround(e->p("cols", 0, 3))), 1, 12);
            std::map<int, QPointF> at;
            for (const auto& [id, w] : hs) at[id] = shown(id, w);
            p.setPen(QPen(QColor(255, 255, 255, 170), 1));
            for (const auto& [id, w] : at) {
                if (id % 1000 < cols && at.count(id + 1)) p.drawLine(w, at[id + 1]);
                if (at.count(id + 1000)) p.drawLine(w, at[id + 1000]);
            }
            for (const auto& [id, w] : at) {
                p.setPen(QPen(QColor(0, 0, 0, 160), 3));
                p.drawRect(QRectF(w - QPointF(4, 4), QSizeF(8, 8)));
                p.setPen(QPen(QColor(255, 200, 0, 230), 1.5));
                p.drawRect(QRectF(w - QPointF(4, 4), QSizeF(8, 8)));
            }
        } else {
            // Each pin as a dot in its kind's colour (yellow Position, red Stiff, green Bend, blue Overlap), its reach
            // as a dashed circle.
            static const QColor colours[4] = {QColor(255, 210, 0), QColor(255, 80, 60), QColor(80, 220, 120), QColor(80, 170, 255)};
            for (const auto& [k, w0] : hs) {
                const int kind = meshPinKind(*e, k);
                const QPointF w = shown(k, w0);
                if (kind != 0) {
                    const double r = e->p("pin" + std::to_string(k) + ".radius", t->lt, 80) * wpx;
                    p.setPen(QPen(colours[kind], 1, Qt::DashLine));
                    p.drawEllipse(w, r, r);
                }
                p.setPen(QPen(QColor(0, 0, 0, 170), 2));
                p.setBrush(colours[kind]);
                p.drawEllipse(w, 5, 5);
                p.setBrush(Qt::NoBrush);
            }
        }
        p.restore();
        return;
    }
    if (t->focus) {
        // The focus point (when it focuses on one), and a target under the mouse.
        auto target = [&](QPointF at, const QColor& colour) {
            p.setPen(QPen(QColor(0, 0, 0, 160), 3));
            p.drawEllipse(at, 9, 9);
            p.setPen(QPen(colour, 1.5));
            p.drawEllipse(at, 9, 9);
            for (QPointF d : {QPointF(1, 0), QPointF(-1, 0), QPointF(0, 1), QPointF(0, -1)}) p.drawLine(at + d * 5, at + d * 14);
        };
        QPointF at;
        if (std::lround(e->p("follow", t->lt, 0)) == 1 && toWidget(t->clip, e->p("point_x", t->lt, 0.5), e->p("point_y", t->lt, 0.5), at))
            target(at, QColor(255, 200, 0, 230));
        if (inside_) target(mouse_, QColor(255, 255, 255, 200));
        p.restore();
        return;
    }
    // This frame's spots or strokes.
    p.setPen(QPen(QColor(255, 200, 0, 200), 1.2, Qt::DashLine));
    if (t->dust) {
        for (const DustSpot& d : dustSpotsFromString(e->s("spots"))) {
            QPointF ctr, edge;
            if (d.frame != t->lt || !toWidget(t->clip, d.x, d.y, ctr) || !toWidget(t->clip, d.x + d.r, d.y, edge)) continue;
            const double rad = QLineF(ctr, edge).length();
            p.drawEllipse(ctr, rad, rad);
        }
    } else {
        auto drawLine = [&](const std::vector<std::array<double, 2>>& pts) {
            QPainterPath path;
            for (size_t i = 0; i < pts.size(); ++i) {
                QPointF w;
                if (!toWidget(t->clip, pts[i][0], pts[i][1], w)) continue;
                if (path.elementCount() == 0) path.moveTo(w);
                else path.lineTo(w);
            }
            if (pts.size() == 1 && path.elementCount()) p.drawEllipse(path.currentPosition(), 2, 2);
            else p.drawPath(path);
        };
        // Where each shows on this frame (a tracked one moved with the mask).
        double fw = 0, fh = 0;
        if (clipFrameSize(state_->project(), *seq, *c, fw, fh) && fw > 0 && fh > 0)
            for (PaintStrokeAt s : paintStrokesAt(*e, t->lt, int(std::lround(fw)), int(std::lround(fh)))) {
                for (auto& q : s.points) q[0] /= std::lround(fw), q[1] /= std::lround(fh);
                drawLine(s.points);
            }
        p.setPen(QPen(QColor(255, 255, 255, 230), 1.5));
        drawLine(stroke_);
    }
    if (inside_) {
        const double rad = t->brush / 2 * wpx;
        p.setPen(QPen(QColor(0, 0, 0, 160), 3));
        p.drawEllipse(mouse_, rad, rad);
        p.setPen(QPen(QColor(255, 255, 255, 230), 1.2));
        p.drawEllipse(mouse_, rad, rad);
        // Where a clone or heal copies from.
        double u, v;
        if (!t->dust && fromWidget(t->clip, mouse_, u, v)) {
            QPointF src;
            const bool have = source_ ? toWidget(t->clip, (*source_)[0], (*source_)[1], src)
                                      : toWidget(t->clip, u + e->p("source_x", t->lt, 0.05), v + e->p("source_y", t->lt, 0), src);
            if (have) {
                p.setPen(QPen(QColor(0, 220, 255, 230), 1.5));
                p.drawLine(src + QPointF(-6, 0), src + QPointF(6, 0));
                p.drawLine(src + QPointF(0, -6), src + QPointF(0, 6));
            }
        }
    }
    p.restore();
}

std::vector<std::pair<int, QPointF>> PaintOverlay::handles(const Target& t, const Effect& e) const {
    std::vector<std::pair<int, QPointF>> out;
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, t.clip) : nullptr;
    double fw = 0, fh = 0;
    if (!c || !clipFrameSize(state_->project(), *seq, *c, fw, fh) || fw < 1 || fh < 1) return out;
    if (t.grid) {
        const int rows = std::clamp(int(std::lround(e.p("rows", 0, 3))), 1, 12), cols = std::clamp(int(std::lround(e.p("cols", 0, 3))), 1, 12);
        for (int r = 0; r <= rows; ++r)
            for (int col = 0; col <= cols; ++col) {
                const MeshPoint q = gridWarpPointAt(e, double(t.lt), r, col);
                QPointF w;
                if (toWidget(t.clip, q[0], q[1], w)) out.emplace_back(r * 1000 + col, w);
            }
        return out;
    }
    // Position pins where they are put; the others where the warp takes them.
    const int W = int(std::lround(fw)), H = int(std::lround(fh));
    const double scale = fw / std::max(1, seq->width);
    std::vector<int> others;
    std::vector<MeshPoint> rest;
    for (int k : meshPins(e)) {
        const std::string pre = "pin" + std::to_string(k) + ".";
        const double x = e.p(pre + "x", t.lt, 0.5), y = e.p(pre + "y", t.lt, 0.5);
        QPointF w;
        if (meshPinKind(e, k) == 0) {
            if (toWidget(t.clip, e.p(pre + "to_x", t.lt, x), e.p(pre + "to_y", t.lt, y), w)) out.emplace_back(k, w);
        } else {
            others.push_back(k);
            rest.push_back({x * W, y * H});
        }
    }
    if (!others.empty()) {
        const std::vector<MeshPoint> moved = meshWarpMoved(e, double(t.lt), W, H, scale, rest);
        for (size_t i = 0; i < others.size(); ++i) {
            QPointF w;
            if (toWidget(t.clip, moved[i][0] / W, moved[i][1] / H, w)) out.emplace_back(others[i], w);
        }
    }
    return out;
}

int PaintOverlay::handleAt(const Target& t, const Effect& e, const QPointF& at) const {
    int best = -1;
    double nearest = 9;
    for (const auto& [id, w] : handles(t, e))
        if (const double d = QLineF(w, at).length(); d <= nearest) nearest = d, best = id;
    return best;
}

void PaintOverlay::placePin(const Target& t, const QPointF& at) {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, t.clip) : nullptr;
    const Effect* e = c ? edit::ownedEffect(const_cast<Sequence&>(*seq), t.clip, t.effect) : nullptr;
    double fw = 0, fh = 0, u, v;
    if (!e || !clipFrameSize(state_->project(), *seq, *c, fw, fh) || fw < 1 || fh < 1 || !fromWidget(t.clip, at, u, v)) return;
    // Where on the picture at rest the point clicked is (as the mesh is bent now).
    const int W = int(std::lround(fw)), H = int(std::lround(fh));
    MeshPoint rest{u * W, v * H};
    if (!meshWarpRestOf(*e, double(t.lt), W, H, fw / std::max(1, seq->width), {u * W, v * H}, rest)) rest = {u * W, v * H};
    const Id clip = t.clip, effect = t.effect;
    const int kind = state_->meshPinKind();
    state_->edit(tr("Add Pin"), [=](Project&, Sequence& sq) {
        Effect* ef = edit::ownedEffect(sq, clip, effect);
        if (!ef) return false;
        const int k = addMeshPin(*ef, kind, rest[0] / W, rest[1] / H);
        if (kind == 0) {
            ef->params["pin" + std::to_string(k) + ".to_x"] = Param(u);
            ef->params["pin" + std::to_string(k) + ".to_y"] = Param(v);
        }
        return true;
    });
}

void PaintOverlay::dropHandle(const Target& t, const QPointF& at, bool remove) {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, t.clip) : nullptr;
    const Effect* e = c ? edit::ownedEffect(const_cast<Sequence&>(*seq), t.clip, t.effect) : nullptr;
    const int id = dragging_;
    dragging_ = -1;
    double fw = 0, fh = 0, u, v;
    if (!e || id < 0 || !clipFrameSize(state_->project(), *seq, *c, fw, fh) || fw < 1 || fh < 1 || !fromWidget(t.clip, at, u, v)) return;
    const Id clip = t.clip, effect = t.effect;
    const FrameTime lt = t.lt;
    if (t.grid) {
        const int rows = std::clamp(int(std::lround(e->p("rows", 0, 3))), 1, 12), cols = std::clamp(int(std::lround(e->p("cols", 0, 3))), 1, 12);
        const int r = id / 1000, col = id % 1000;
        const double dx = u - double(col) / cols, dy = v - double(r) / rows;
        state_->edit(tr("Move Grid Point"), [=](Project&, Sequence& sq) {
            Effect* ef = edit::ownedEffect(sq, clip, effect);
            if (!ef) return false;
            const std::string n = gridWarpPoint(r, col);
            ef->params[n + ".dx"].set(lt, dx);
            ef->params[n + ".dy"].set(lt, dy);
            return true;
        });
        return;
    }
    if (remove) {
        state_->edit(tr("Remove Pin"), [=](Project&, Sequence& sq) {
            Effect* ef = edit::ownedEffect(sq, clip, effect);
            const std::vector<int> pins = ef ? meshPins(*ef) : std::vector<int>{};
            if (std::find(pins.begin(), pins.end(), id) == pins.end()) return false;
            removeMeshPin(*ef, id);
            return true;
        });
        return;
    }
    const int kind = meshPinKind(*e, id);
    MeshPoint rest{0, 0};
    const int W = int(std::lround(fw)), H = int(std::lround(fh));
    // Any other pin: put where on the picture at rest the point it is dropped on is.
    if (kind != 0 && !meshWarpRestOf(*e, double(lt), W, H, fw / std::max(1, seq->width), {u * W, v * H}, rest)) return;
    state_->edit(tr("Move Pin"), [=](Project&, Sequence& sq) {
        Effect* ef = edit::ownedEffect(sq, clip, effect);
        const std::vector<int> pins = ef ? meshPins(*ef) : std::vector<int>{};
        if (std::find(pins.begin(), pins.end(), id) == pins.end() || meshPinKind(*ef, id) != kind) return false;  // (undone meanwhile)
        const std::string pre = "pin" + std::to_string(id) + ".";
        if (kind == 0) {
            // A key here when it is keyed.
            ef->params[pre + "to_x"].set(lt, u);
            ef->params[pre + "to_y"].set(lt, v);
        } else {
            ef->params[pre + "x"] = Param(rest[0] / W);
            ef->params[pre + "y"] = Param(rest[1] / H);
        }
        return true;
    });
}

void PaintOverlay::pickFocus(const Target& t, double u, double v) {
    const Id clip = t.clip, effect = t.effect;
    const FrameTime lt = t.lt;
    state_->edit(tr("Pick Focus"), [=](Project&, Sequence& sq) {
        Effect* e = edit::ownedEffect(sq, clip, effect);
        if (!e) return false;
        e->params["follow"] = Param(1.0);
        // Already keyed: a key here on both (eased, so a rack from one to the next starts and ends gently).
        Param& px = e->params["point_x"];
        Param& py = e->params["point_y"];
        if (px.animated() || py.animated()) {
            px.addKey(lt, u, Interp::Smooth);
            py.addKey(lt, v, Interp::Smooth);
        } else {
            px = Param(u);
            py = Param(v);
        }
        return true;
    });
}

bool PaintOverlay::markSpot(const Target& t, const QPointF& pos, bool remove) {
    double u, v;
    if (!fromWidget(t.clip, pos, u, v)) return false;
    const double r = brushRadius(t, pos);
    const Id clip = t.clip, effect = t.effect;
    const FrameTime lt = t.lt;
    return state_->edit(remove ? tr("Remove Dust Spot") : tr("Mark Dust Spot"), [=](Project&, Sequence& sq) {
        Effect* e = edit::ownedEffect(sq, clip, effect);
        if (!e) return false;
        std::vector<DustSpot> spots = dustSpotsFromString(e->s("spots"));
        if (remove) {
            // The nearest of this frame's spots under the click.
            auto it = std::min_element(spots.begin(), spots.end(), [&](const DustSpot& a, const DustSpot& b) {
                const double da = a.frame == lt ? std::hypot(a.x - u, a.y - v) : 1e9, db = b.frame == lt ? std::hypot(b.x - u, b.y - v) : 1e9;
                return da < db;
            });
            if (it == spots.end() || it->frame != lt || std::hypot(it->x - u, it->y - v) > it->r * 1.5) return false;
            spots.erase(it);
        } else {
            spots.push_back({lt, u, v, r});
        }
        e->strings["spots"] = dustSpotsToString(spots);
        return true;
    });
}

void PaintOverlay::finishStroke() {
    if (stroke_.empty() || !drawing_.effect) return;
    const Target t = drawing_;
    std::vector<std::array<double, 2>> pts;
    pts.swap(stroke_);
    const std::optional<std::array<double, 2>> source = source_;
    source_.reset();
    const double radius = brushRadius(t, mouse_);
    const Id clip = t.clip, effect = t.effect;
    const FrameTime lt = t.lt;
    state_->edit(tr("Paint Stroke"), [=](Project&, Sequence& sq) {
        Effect* e = edit::ownedEffect(sq, clip, effect);
        if (!e) return false;
        PaintStroke s;
        s.heal = std::lround(e->p("tool", lt, 0)) == 1;
        s.frame = lt;
        s.hold = std::lround(e->p("span", lt, 0)) == 1;
        s.track = e->p("track", lt, 0) > 0.5;
        s.softness = std::clamp(e->p("softness", lt, 50) / 100, 0.0, 1.0);
        s.radius = radius;
        s.points = pts;
        if (source) {
            // Copies from where Alt-click said, relative to where the stroke began; later strokes keep that offset.
            s.dx = (*source)[0] - pts[0][0];
            s.dy = (*source)[1] - pts[0][1];
            e->params["source_x"] = Param(s.dx);
            e->params["source_y"] = Param(s.dy);
        } else {
            s.dx = e->p("source_x", lt, 0.05);
            s.dy = e->p("source_y", lt, 0);
        }
        std::vector<PaintStroke> all = paintStrokesFromString(e->s("strokes"));
        all.push_back(std::move(s));
        e->strings["strokes"] = paintStrokesToString(all);
        return true;
    });
    drawing_ = {};
}

bool PaintOverlay::eventFilter(QObject* obj, QEvent* e) {
    if (obj != viewer_) return false;
    switch (e->type()) {
    case QEvent::Leave:
        inside_ = false;
        viewer_->update();
        return false;
    case QEvent::ShortcutOverride:
        // Esc is also a menu shortcut (End Trim Mode); while painting it stops the painting instead.
        if (static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape && state_->paintEffect()) {
            e->accept();
            return true;
        }
        return false;
    case QEvent::KeyPress:
        if (static_cast<QKeyEvent*>(e)->key() == Qt::Key_Escape && state_->paintEffect()) {
            stroke_.clear();
            state_->setPaintEffect(0);
            return true;
        }
        return false;
    case QEvent::MouseButtonPress: {
        auto* me = static_cast<QMouseEvent*>(e);
        const std::optional<Target> t = target();
        if (!t || me->button() != Qt::LeftButton) return false;
        mouse_ = me->position();
        inside_ = true;
        double u, v;
        if (!fromWidget(t->clip, mouse_, u, v)) return true;
        if (t->mesh || t->grid) {
            const Sequence* seq = state_->sequence();
            const Effect* ef = seq ? edit::ownedEffect(const_cast<Sequence&>(*seq), t->clip, t->effect) : nullptr;
            if (!ef) return true;
            const int id = handleAt(*t, *ef, mouse_);
            drawing_ = *t;
            if (id >= 0 && t->mesh && (me->modifiers() & Qt::ControlModifier)) {
                dragging_ = id;
                dropHandle(*t, mouse_, true);
            } else if (id >= 0) {
                dragging_ = id;
                pressAt_ = mouse_;
                for (const auto& [h, w] : handles(*t, *ef))
                    if (h == id) grab_ = w - mouse_;
            } else if (t->mesh && u >= 0 && u <= 1 && v >= 0 && v <= 1) {
                placePin(*t, mouse_);
            }
            viewer_->update();
            return true;
        }
        if (t->dust) {
            markSpot(*t, mouse_, me->modifiers() & Qt::ControlModifier);
            return true;
        }
        if (t->focus) {
            if (u >= 0 && u <= 1 && v >= 0 && v <= 1) pickFocus(*t, u, v);  // (not beside the clip's picture)
            return true;
        }
        if (me->modifiers() & Qt::AltModifier) {
            source_ = std::array<double, 2>{u, v};
            viewer_->update();
            return true;
        }
        drawing_ = *t;
        stroke_ = {{u, v}};
        viewer_->update();
        return true;
    }
    case QEvent::MouseMove: {
        auto* me = static_cast<QMouseEvent*>(e);
        const std::optional<Target> t = target();
        if (!t) return false;
        mouse_ = me->position();
        inside_ = viewer_->imageRect().contains(mouse_);
        if (dragging_ >= 0) {
            viewer_->update();
            return true;
        }
        double u, v;
        if (!stroke_.empty() && (me->buttons() & Qt::LeftButton) && fromWidget(drawing_.clip, mouse_, u, v)) {
            // A point each time it has moved a fifth of the brush.
            const auto& last = stroke_.back();
            if (std::hypot(u - last[0], v - last[1]) >= brushRadius(drawing_, mouse_) * 0.2) stroke_.push_back({u, v});
        }
        viewer_->update();
        return !stroke_.empty();
    }
    case QEvent::MouseButtonRelease: {
        auto* me = static_cast<QMouseEvent*>(e);
        if (me->button() == Qt::LeftButton && dragging_ >= 0) {
            mouse_ = me->position();
            // (a click on a handle without moving it leaves it where it is)
            if (QLineF(pressAt_, mouse_).length() < 3) dragging_ = -1;
            else dropHandle(drawing_, mouse_ + grab_, false);
            drawing_ = {};
            viewer_->update();
            return true;
        }
        if (me->button() != Qt::LeftButton || stroke_.empty()) return target().has_value() && me->button() == Qt::LeftButton;
        mouse_ = me->position();
        double u, v;
        if (fromWidget(drawing_.clip, mouse_, u, v) && std::hypot(u - stroke_.back()[0], v - stroke_.back()[1]) > 1e-6)
            stroke_.push_back({u, v});
        finishStroke();
        viewer_->update();
        return true;
    }
    default:
        return false;
    }
}

}  // namespace montage
