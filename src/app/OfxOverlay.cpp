#include "OfxOverlay.h"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTabletEvent>
#include <QTimer>

#include "EditorState.h"
#include "MonitorPanel.h"
#include "core/EditOps.h"
#include "render/Compositor.h"

namespace montage {

namespace {

// The OfxKeySyms code for a Qt key (X11 keysyms: Latin-1 as itself, the rest from 0xff00).
int keysym(const QKeyEvent* k) {
    switch (k->key()) {
    case Qt::Key_Left: return 0xff51;
    case Qt::Key_Up: return 0xff52;
    case Qt::Key_Right: return 0xff53;
    case Qt::Key_Down: return 0xff54;
    case Qt::Key_Return: return 0xff0d;
    case Qt::Key_Enter: return 0xff8d;
    case Qt::Key_Escape: return 0xff1b;
    case Qt::Key_Backspace: return 0xff08;
    case Qt::Key_Delete: return 0xffff;
    case Qt::Key_Tab: return 0xff09;
    case Qt::Key_Home: return 0xff50;
    case Qt::Key_End: return 0xff57;
    case Qt::Key_PageUp: return 0xff55;
    case Qt::Key_PageDown: return 0xff56;
    case Qt::Key_Shift: return 0xffe1;
    case Qt::Key_Control: return 0xffe3;
    case Qt::Key_Alt: return 0xffe9;
    case Qt::Key_Meta: return 0xffe7;
    default: break;
    }
    const QString text = k->text();
    if (text.size() == 1 && text[0].unicode() >= 0x20 && text[0].unicode() <= 0xff) return text[0].unicode();
    if (k->key() >= 0x20 && k->key() <= 0xff) return k->key();
    return 0;
}

}  // namespace

OfxOverlay::OfxOverlay(EditorState* state, ViewerWidget* viewer) : QObject(viewer), state_(state), viewer_(viewer) {
    // (controls in a plug-in host that are not answering: looked at again until they do, or are closed)
    poll_ = new QTimer(this);
    poll_->setSingleShot(true);
    poll_->setInterval(100);
    connect(poll_, &QTimer::timeout, viewer_, qOverload<>(&QWidget::update));
    viewer_->installEventFilter(this);
    viewer_->setMouseTracking(true);
    viewer_->addOverlay([this](QPainter& p, const QRectF& r) { paint(p, r); });
    for (auto sig : {&EditorState::projectChanged, &EditorState::selectionChanged, &EditorState::sequenceSwitched})
        connect(state_, sig, viewer_, qOverload<>(&QWidget::update));
    connect(state_, &EditorState::playheadChanged, viewer_, qOverload<>(&QWidget::update));
}

OfxOverlay::~OfxOverlay() = default;

std::vector<OfxOverlay::Target> OfxOverlay::targets() const {
    std::vector<Target> out;
    const Sequence* s = state_->sequence();
    if (!s) return out;
    const FrameTime t = state_->playhead();
    for (Id id : state_->selectedClips()) {
        const auto loc = edit::locate(*s, id);
        if (!loc || loc->track.kind != TrackKind::Video) continue;
        const Clip& c = trackAt(*s, loc->track)->clips[loc->index];
        if (!c.contains(t)) continue;
        auto has = [](const Effect& e) {
            if (!e.enabled || e.type != "ofx") return false;
            const auto d = ofx::descriptionsFromJson(e.s("ofx_description"));
            return !d.empty() && d.front().overlay > 0;
        };
        if (c.generator.type == "ofx" && has(c.generator)) out.push_back({c.id, c.generator.id, true, t - c.start});
        for (const Effect& e : c.effects)
            if (has(e)) out.push_back({c.id, e.id, false, t - c.start});
        break;  // (the primary selected clip under the playhead)
    }
    return out;
}

bool OfxOverlay::view(Id clip, ofx::View& v) const {
    const Sequence* seq = state_->sequence();
    const Clip* c = seq ? edit::clipById(*seq, clip) : nullptr;
    const QRectF r = viewer_->imageRect();
    double fw = 0, fh = 0;
    if (!c || r.isEmpty() || !clipEffectFrameSize(state_->project(), *seq, *c, state_->playhead(), fw, fh) || fw <= 0 || fh <= 0)
        return false;
    // Canonical (x, y), y up from the frame's bottom, is clip frame (x / fw, 1 - y / fh); three points give the affine.
    double pts[3][2];
    const double uv[3][2] = {{0, 1}, {1, 1}, {0, 0}};
    for (int k = 0; k < 3; ++k) {
        double x = 0, y = 0;
        if (!clipFrameToSequence(state_->project(), *seq, *c, state_->playhead(), uv[k][0], uv[k][1], x, y)) return false;
        pts[k][0] = r.left() + x * r.width() / seq->width;
        pts[k][1] = r.top() + y * r.height() / seq->height;
    }
    v.width = viewer_->width();
    v.height = viewer_->height();
    v.frameW = fw;
    v.frameH = fh;
    v.renderScale = 1;
    v.pixelRatio = viewer_->devicePixelRatioF();
    v.m[0] = (pts[1][0] - pts[0][0]) / fw;
    v.m[1] = (pts[2][0] - pts[0][0]) / fh;
    v.m[2] = pts[0][0];
    v.m[3] = (pts[1][1] - pts[0][1]) / fw;
    v.m[4] = (pts[2][1] - pts[0][1]) / fh;
    v.m[5] = pts[0][1];
    return true;
}

const Effect* OfxOverlay::effectOf(const Target& t) const {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, t.clip) : nullptr;
    if (!c) return nullptr;
    if (t.generator) return c->generator.id == t.effect ? &c->generator : nullptr;
    for (const Effect& e : c->effects)
        if (e.id == t.effect) return &e;
    return nullptr;
}

ofx::Interact* OfxOverlay::interactFor(const Target& t, const Effect& e) {
    Open& o = open_[{t.clip, t.effect}];
    // (one that failed is left alone from then on, as OpenFX asks; one that will not open is not tried again either)
    if (o.interact && o.interact->failed()) {
        o.interact.reset();
        o.plugin = e.s("ofx_id") + "\n";
    }
    if (o.interact && o.plugin != e.s("ofx_id")) o.interact.reset();
    if (!o.interact && o.plugin != e.s("ofx_id") + "\n") {
        std::string error;
        o.interact = ofx::Interact::open(e, &error);
        // (one that will not open is not tried again on every repaint)
        o.plugin = o.interact ? e.s("ofx_id") : e.s("ofx_id") + "\n";
        if (!o.interact && !error.empty()) state_->message(QString::fromStdString(error), 6000);
    }
    return o.interact.get();
}

bool OfxOverlay::unresponsive(Id clip, Id effect) const {
    const auto it = open_.find({clip, effect});
    return it != open_.end() && it->second.interact && it->second.interact->unresponsive();
}

bool OfxOverlay::inHost(Id clip, Id effect) const {
    const auto it = open_.find({clip, effect});
    return it != open_.end() && it->second.interact && it->second.interact->remote();
}

void OfxOverlay::paint(QPainter& p, const QRectF&) {
    const std::vector<Target> ts = targets();
    // Controls of effects no longer shown are closed.
    for (auto it = open_.begin(); it != open_.end();) {
        bool shown = false;
        for (const Target& t : ts) shown = shown || (t.clip == it->first.first && t.effect == it->first.second);
        if (!shown && !(down_ && pressed_.clip == it->first.first && pressed_.effect == it->first.second)) it = open_.erase(it);
        else ++it;
    }
    int notes = 0;
    for (const Target& t : ts) {
        const Effect* e = effectOf(t);
        ofx::View v;
        if (!e || !view(t.clip, v)) continue;
        ofx::Interact* in = interactFor(t, *e);
        if (!in) continue;
        ofx::Overlay o;
        const bool drew = in->draw(*e, double(t.lt), v, o);
        const QString name = QString::fromStdString(e->s("ofx_name"));
        // (controls in a host that did not answer in time keep their last picture up, marked)
        if (drew || !o.gl.isNull()) ofx::paintOverlay(p, o, v);
        if (!drew) {
            const std::string why = in->failure();
            state_->message(why.empty() ? tr("%1's on-screen controls stopped working").arg(name)
                                        : tr("%1's on-screen controls stopped: it %2").arg(name, QString::fromStdString(why)),
                            8000);
        } else if (in->unresponsive()) {
            const QString note = tr("%1's on-screen controls are not responding").arg(name);
            p.save();
            const QFontMetrics fm(p.font());
            const QRect box(8, 8 + notes * (fm.height() + 8), fm.horizontalAdvance(note) + 12, fm.height() + 4);
            p.fillRect(box, QColor(0, 0, 0, 170));
            p.setPen(QColor(255, 200, 60));
            p.drawText(box, Qt::AlignCenter, note);
            p.restore();
            ++notes;
            if (!poll_->isActive()) poll_->start();
        }
    }
}

void OfxOverlay::apply(const Target& t, const Effect& edited, bool gesture) {
    const Id clip = t.clip, effect = t.effect;
    const bool generator = t.generator;
    const auto params = edited.params;
    const auto strings = edited.strings;
    auto fn = [clip, effect, generator, params, strings](Project&, Sequence& s) {
        Clip* c = edit::clipById(s, clip);
        if (!c) return false;
        Effect* e = nullptr;
        if (generator && c->generator.id == effect) e = &c->generator;
        for (Effect& x : c->effects)
            if (!generator && x.id == effect) e = &x;
        if (!e) return false;
        e->params = params;
        e->strings = strings;
        return true;
    };
    if (gesture) state_->updateGesture([fn](Project& p, Sequence& s) { fn(p, s); });
    else state_->edit(tr("Change %1").arg(QString::fromStdString(edited.s("ofx_name"))), fn);
}

bool OfxOverlay::pen(ofx::Interact::Pen kind, QEvent* ev) {
    QPointF pos;
    double pressure = 1;
    if (ev->type() == QEvent::TabletPress || ev->type() == QEvent::TabletMove || ev->type() == QEvent::TabletRelease) {
        auto* te = static_cast<QTabletEvent*>(ev);
        pos = te->position();
        pressure = te->pressure();
    } else {
        pos = static_cast<QMouseEvent*>(ev)->position();
    }
    std::vector<Target> ts;
    if (kind == ofx::Interact::Pen::Down) ts = targets();
    else if (down_) ts = {pressed_};
    else ts = targets();  // (a pen moving with no button: the plugins may highlight what is under it)
    // The topmost effect first (the last in the chain is drawn last).
    for (auto it = ts.rbegin(); it != ts.rend(); ++it) {
        const Target& t = *it;
        const Effect* e = effectOf(t);
        ofx::View v;
        if (!e || !view(t.clip, v)) continue;
        ofx::Interact* in = interactFor(t, *e);
        if (!in) continue;
        Effect edited = *e;
        bool changed = false;
        const bool took = in->pen(kind, *e, double(t.lt), v, pos.x(), pos.y(), pressure, edited, changed);
        if (kind == ofx::Interact::Pen::Down && took) {
            pressed_ = t;
            down_ = true;
            changed_ = false;
            state_->beginGesture(tr("Adjust %1").arg(QString::fromStdString(e->s("ofx_name"))));
        }
        if (changed && down_) {
            apply(t, edited, true);
            changed_ = true;
        } else if (changed) {
            apply(t, edited, false);
        }
        if (kind == ofx::Interact::Pen::Up && down_) {
            down_ = false;
            if (state_->inGesture()) state_->endGesture(changed_);
        }
        viewer_->update();
        if (took || kind == ofx::Interact::Pen::Up) return took;
        // (a press or a move not taken goes on to the controls below)
    }
    if (kind == ofx::Interact::Pen::Up && down_) {
        down_ = false;
        if (state_->inGesture()) state_->endGesture(changed_);
    }
    return false;
}

bool OfxOverlay::eventFilter(QObject* obj, QEvent* e) {
    if (obj != viewer_) return false;
    switch (e->type()) {
    case QEvent::FocusIn:
    case QEvent::FocusOut:
        // (the controls showing hear that the monitor gained or lost the keyboard)
        for (const Target& t : targets()) {
            const auto it = open_.find({t.clip, t.effect});
            const Effect* ef = effectOf(t);
            ofx::View v;
            if (it == open_.end() || !it->second.interact || !ef || !view(t.clip, v)) continue;
            it->second.interact->focus(e->type() == QEvent::FocusIn, *ef, double(t.lt), v);
        }
        return false;
    // (a tablet event taken is accepted, so no mouse event is made from it as well)
    case QEvent::MouseButtonPress:
    case QEvent::TabletPress: {
        if (e->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(e)->button() != Qt::LeftButton) return false;
        if (down_) return true;  // (already down: the other of a tablet press and the mouse press made from it)
        const bool took = pen(ofx::Interact::Pen::Down, e);
        if (took) e->accept();
        return took;
    }
    case QEvent::MouseMove:
    case QEvent::TabletMove: {
        const bool took = pen(ofx::Interact::Pen::Move, e);
        if (took || down_) e->accept();
        return down_ || took;
    }
    case QEvent::MouseButtonRelease:
    case QEvent::TabletRelease:
        if (e->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(e)->button() != Qt::LeftButton) return false;
        if (!down_) return false;
        pen(ofx::Interact::Pen::Up, e);
        e->accept();
        return true;
    case QEvent::KeyPress:
    case QEvent::KeyRelease: {
        auto* k = static_cast<QKeyEvent*>(e);
        const int sym = keysym(k);
        if (!sym || !viewer_->underMouse()) return false;
        for (const Target& t : targets()) {
            const Effect* ef = effectOf(t);
            ofx::View v;
            if (!ef || !view(t.clip, v)) continue;
            ofx::Interact* in = interactFor(t, *ef);
            if (!in) continue;
            Effect edited = *ef;
            bool changed = false;
            if (in->key(e->type() == QEvent::KeyPress, sym, k->text().toStdString(), *ef, double(t.lt), v, edited, changed)) {
                if (changed) apply(t, edited, false);
                viewer_->update();
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

}  // namespace montage
