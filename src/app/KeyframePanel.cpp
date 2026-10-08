#include "KeyframePanel.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <map>

#include "EditorState.h"
#include "Theme.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/History.h"

namespace montage {

namespace {
constexpr int kLabelW = 190;
constexpr int kRulerH = 20;
constexpr int kRowH = 22;
constexpr int kMargin = 10;  // so keys at the clip's ends are not cut off

// The clips' fixed attributes and their generator, then its effects.
std::vector<std::pair<ParamAddress, const Effect*>> owners(const Clip& c) {
    std::vector<std::pair<ParamAddress, const Effect*>> out{{{ParamSlot::Motion, 0, {}}, &c.motion},
                                                           {{ParamSlot::Audio, 0, {}}, &c.audio},
                                                           {{ParamSlot::Timing, 0, {}}, &c.timing}};
    if (c.isGenerator()) out.push_back({{ParamSlot::Generator, 0, {}}, &c.generator});
    for (const Effect& e : c.effects) out.push_back({{ParamSlot::Effect, e.id, {}}, &e});
    return out;
}
}  // namespace

KeyframePanel::KeyframePanel(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    setObjectName(QStringLiteral("keyframePanel"));
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    setMinimumHeight(80);
    connect(state_, &EditorState::selectionChanged, this, &KeyframePanel::rebuild);
    connect(state_, &EditorState::projectChanged, this, &KeyframePanel::rebuild);
    connect(state_, &EditorState::sequenceSwitched, this, &KeyframePanel::rebuild);
    connect(state_, &EditorState::playheadChanged, this, [this] { update(); });
    rebuild();
}

const Clip* KeyframePanel::currentClip() const {
    const Sequence* s = state_->sequence();
    return s && clip_ ? edit::clipById(*s, clip_) : nullptr;
}

void KeyframePanel::rebuild() {
    const Sequence* s = state_->sequence();
    const auto sel = state_->selectedClips();
    const Id clip = s && !sel.empty() ? sel.front() : 0;
    if (clip != clip_) selection_.clear();
    clip_ = clip;
    rows_.clear();
    if (const Clip* c = currentClip()) {
        for (const auto& [addr, effect] : owners(*c)) {
            if (!effect || effect->empty()) continue;
            const EffectInfo* info = findEffectInfo(effect->type);
            for (const auto& [name, param] : effect->params) {
                if (!param.animated()) continue;
                QString label = QString::fromStdString(name);
                if (info)
                    for (const ParamInfo& pi : info->params)
                        if (pi.name == name) label = QString::fromStdString(pi.label);
                ParamAddress a = addr;
                a.param = name;
                rows_.push_back({QString::fromStdString(info ? info->displayName : effect->type) + QStringLiteral(" · ") + label, a});
            }
        }
        // Keep only selected keys that still exist.
        std::erase_if(selection_, [c](const Key& k) {
            const Param* p = findParam(*c, k.address);
            return !p || !p->keyAt(k.t);
        });
    }
    setMinimumHeight(std::max(80, kRulerH + int(rows_.size()) * kRowH + 4));
    update();
}

QRect KeyframePanel::lane(int row) const { return QRect(kLabelW, kRulerH + row * kRowH, width() - kLabelW, kRowH); }

int KeyframePanel::rowAt(int y) const {
    if (y < kRulerH) return -1;
    const int r = (y - kRulerH) / kRowH;
    return r < int(rows_.size()) ? r : -1;
}

double KeyframePanel::xForFrame(FrameTime t) const {
    const Clip* c = currentClip();
    const double span = c ? double(std::max<FrameTime>(1, c->duration - 1)) : 1.0;
    return kLabelW + kMargin + double(t) / span * double(std::max(1, width() - kLabelW - 2 * kMargin));
}

FrameTime KeyframePanel::frameAtX(int x) const {
    const Clip* c = currentClip();
    if (!c) return 0;
    const double span = double(std::max<FrameTime>(1, c->duration - 1));
    const double f = double(x - kLabelW - kMargin) / double(std::max(1, width() - kLabelW - 2 * kMargin)) * span;
    return std::clamp<FrameTime>(FrameTime(std::llround(f)), 0, std::max<FrameTime>(0, c->duration - 1));
}

QPoint KeyframePanel::keyPoint(int row, FrameTime t) const {
    return QPoint(int(std::lround(xForFrame(t))), lane(row).center().y());
}

bool KeyframePanel::keyAt(const QPoint& pos, Key& key) const {
    const int r = rowAt(pos.y());
    const Clip* c = currentClip();
    if (r < 0 || !c || pos.x() < kLabelW) return false;
    const Param* p = findParam(*c, rows_[size_t(r)].address);
    if (!p) return false;
    double best = 6;
    bool found = false;
    for (const Keyframe& k : p->keys) {
        const double d = std::fabs(xForFrame(k.t) - pos.x());
        if (d <= best) {
            best = d;
            key = {rows_[size_t(r)].address, k.t};
            found = true;
        }
    }
    return found;
}

void KeyframePanel::select(const std::set<Key>& keys) {
    selection_ = keys;
    update();
}

FrameTime KeyframePanel::clampShift(FrameTime delta) const {
    const Clip* c = currentClip();
    if (!c) return 0;
    std::map<ParamAddress, std::vector<FrameTime>> byParam;
    for (const Key& k : selection_) byParam[k.address].push_back(k.t);
    for (const auto& [addr, times] : byParam)
        if (const Param* p = findParam(*c, addr)) {
            const auto [down, up] = shiftRange(*p, times, c->duration - 1);
            delta = std::clamp(delta, down, up);
        }
    return delta;
}

bool KeyframePanel::shiftSelected(FrameTime delta) {
    delta = clampShift(delta);
    if (delta == 0 || selection_.empty()) return false;
    std::map<ParamAddress, std::vector<FrameTime>> byParam;
    for (const Key& k : selection_) byParam[k.address].push_back(k.t);
    const Id id = clip_;
    // The selection moves first: the edit rebuilds the rows and keeps only keys that exist.
    const std::set<Key> before = selection_;
    std::set<Key> moved;
    for (const Key& k : before) moved.insert({k.address, k.t + delta});
    selection_ = moved;
    const bool ok = state_->edit(tr("Move Keyframes"), [id, byParam, delta](Project&, Sequence& s) {
        Clip* c = edit::clipById(s, id);
        if (!c) return false;
        for (const auto& [addr, times] : byParam)
            if (Param* p = findParam(*c, addr)) shiftKeys(*p, times, delta);
        return true;
    });
    if (!ok) selection_ = before;
    return ok;
}

bool KeyframePanel::deleteSelected() {
    if (selection_.empty()) return false;
    const auto keys = selection_;
    const Id id = clip_;
    const bool ok = state_->edit(tr("Delete Keyframes"), [id, keys](Project&, Sequence& s) {
        Clip* c = edit::clipById(s, id);
        if (!c) return false;
        bool any = false;
        for (const Key& k : keys)
            if (Param* p = findParam(*c, k.address)) any |= p->removeKey(k.t);
        return any;
    });
    if (ok) selection_.clear();
    return ok;
}

bool KeyframePanel::setInterpolation(Interp interp) {
    if (selection_.empty()) return false;
    const auto keys = selection_;
    const Id id = clip_;
    return state_->edit(tr("Keyframe Interpolation"), [id, keys, interp](Project&, Sequence& s) {
        Clip* c = edit::clipById(s, id);
        if (!c) return false;
        bool any = false;
        for (const Key& k : keys)
            if (Param* p = findParam(*c, k.address))
                for (Keyframe& kf : p->keys)
                    if (kf.t == k.t && kf.interp != interp) {
                        kf.interp = interp;
                        any = true;
                    }
        return any;
    });
}

void KeyframePanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), palette().window());
    const Clip* c = currentClip();
    p.setPen(palette().color(QPalette::PlaceholderText));
    if (!c) {
        p.drawText(rect(), Qt::AlignCenter, tr("Select a clip to see its keyframes"));
        return;
    }
    if (rows_.empty()) {
        p.drawText(rect().adjusted(8, 0, -8, 0), Qt::AlignCenter | Qt::TextWordWrap,
                   tr("No keyframes on %1 yet. Animate a parameter with ◷ in the Inspector, or drag its line on the timeline.")
                       .arg(QString::fromStdString(c->name)));
        return;
    }
    const Sequence* s = state_->sequence();
    // Ruler: the clip's start and end, and the playhead.
    p.fillRect(QRect(kLabelW, 0, width() - kLabelW, kRulerH), palette().base());
    QFont small = font();
    small.setPointSize(std::max(7, small.pointSize() - 1));
    p.setFont(small);
    p.setPen(palette().color(QPalette::Text));
    p.drawText(QRect(4, 0, kLabelW - 8, kRulerH), Qt::AlignVCenter | Qt::AlignLeft,
               p.fontMetrics().elidedText(QString::fromStdString(c->name), Qt::ElideRight, kLabelW - 8));
    p.drawText(QRect(kLabelW + 4, 0, 120, kRulerH), Qt::AlignVCenter, QString::fromStdString(formatTimecode(c->start, s->fps)));
    p.drawText(QRect(width() - 124, 0, 120, kRulerH), Qt::AlignVCenter | Qt::AlignRight,
               QString::fromStdString(formatTimecode(c->end() - 1, s->fps)));
    // Rows.
    for (int r = 0; r < int(rows_.size()); ++r) {
        const QRect l = lane(r);
        if (r % 2) p.fillRect(QRect(0, l.top(), width(), kRowH), palette().alternateBase());
        p.setPen(palette().color(QPalette::Text));
        p.drawText(QRect(6, l.top(), kLabelW - 10, kRowH), Qt::AlignVCenter,
                   p.fontMetrics().elidedText(rows_[size_t(r)].label, Qt::ElideRight, kLabelW - 10));
        const Param* prm = findParam(*c, rows_[size_t(r)].address);
        if (!prm) continue;
        p.setRenderHint(QPainter::Antialiasing);
        // A line through each row's keys, dashed where it holds.
        for (size_t i = 0; i + 1 < prm->keys.size(); ++i) {
            p.setPen(QPen(palette().color(QPalette::Mid), 1, prm->keys[i].interp == Interp::Hold ? Qt::DashLine : Qt::SolidLine));
            p.drawLine(QPointF(xForFrame(prm->keys[i].t), l.center().y()), QPointF(xForFrame(prm->keys[i + 1].t), l.center().y()));
        }
        for (const Keyframe& k : prm->keys) {
            const bool sel = selection_.count({rows_[size_t(r)].address, k.t}) > 0;
            const QPointF at(xForFrame(k.t), l.center().y());
            QPolygonF d;
            if (k.interp == Interp::Hold) d << at + QPointF(-4, -5) << at + QPointF(4, -5) << at + QPointF(4, 5) << at + QPointF(-4, 5);
            else d << at + QPointF(0, -6) << at + QPointF(6, 0) << at + QPointF(0, 6) << at + QPointF(-6, 0);
            p.setPen(QPen(QColor(0, 0, 0, 180), 1));
            p.setBrush(sel ? theme::kSnap : (k.interp == Interp::Smooth ? QColor(120, 200, 255) : QColor(220, 220, 220)));
            p.drawPolygon(d);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
    }
    p.setPen(QColor(0, 0, 0, 90));
    p.drawLine(kLabelW, 0, kLabelW, height());
    // The playhead, where it is inside the clip.
    const FrameTime ph = state_->playhead() - c->start;
    if (ph >= 0 && ph < c->duration) {
        p.setPen(QPen(theme::kPlayhead, 1));
        const int x = int(std::lround(xForFrame(ph)));
        p.drawLine(x, 0, x, height());
    }
    if (drag_ == Drag::Box && dragging_) {
        p.setPen(QPen(theme::kSelection, 1, Qt::DashLine));
        p.setBrush(QColor(theme::kSelection.red(), theme::kSelection.green(), theme::kSelection.blue(), 40));
        p.drawRect(box_.normalized());
    }
}

void KeyframePanel::mousePressEvent(QMouseEvent* e) {
    setFocus();
    if (e->button() != Qt::LeftButton) return;
    press_ = e->pos();
    dragging_ = false;
    dragDelta_ = 0;
    const bool add = e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier);
    Key k;
    if (e->pos().y() < kRulerH && e->pos().x() >= kLabelW) {
        drag_ = Drag::Scrub;
        if (const Clip* c = currentClip()) state_->setPlayhead(c->start + frameAtX(e->pos().x()));
        return;
    }
    if (keyAt(e->pos(), k)) {
        if (add && selection_.count(k)) selection_.erase(k);
        else if (add) selection_.insert(k);
        else if (!selection_.count(k)) selection_ = {k};
        drag_ = selection_.count(k) ? Drag::Keys : Drag::None;
        dragStart_ = selection_;
        update();
        return;
    }
    if (!add) selection_.clear();
    drag_ = Drag::Box;
    box_ = QRect(e->pos(), e->pos());
    update();
}

void KeyframePanel::mouseMoveEvent(QMouseEvent* e) {
    if (!(e->buttons() & Qt::LeftButton) || drag_ == Drag::None) return;
    if (!dragging_ && (e->pos() - press_).manhattanLength() < 3) return;
    const bool start = !dragging_;
    dragging_ = true;
    switch (drag_) {
        case Drag::Scrub:
            if (const Clip* c = currentClip()) state_->setPlayhead(c->start + frameAtX(e->pos().x()));
            break;
        case Drag::Box: box_.setBottomRight(e->pos()); break;
        case Drag::Keys: {
            if (start) state_->beginGesture(tr("Move Keyframes"));
            // From where the drag began: the gesture re-applies to the state at its start.
            const std::set<Key> base = dragStart_;
            selection_ = base;
            const Clip* c = currentClip();
            if (!c) break;
            const FrameTime raw = frameAtX(e->pos().x()) - frameAtX(press_.x());
            // Clamp against the clip as it was when the drag began.
            const Sequence* origin = state_->gestureBase();
            const Clip* oc = origin ? edit::clipById(*origin, clip_) : nullptr;
            FrameTime delta = raw;
            if (oc) {
                std::map<ParamAddress, std::vector<FrameTime>> byParam;
                for (const Key& k : base) byParam[k.address].push_back(k.t);
                for (const auto& [addr, times] : byParam)
                    if (const Param* p = findParam(*oc, addr)) {
                        const auto [down, up] = shiftRange(*p, times, oc->duration - 1);
                        delta = std::clamp(delta, down, up);
                    }
                const Id id = clip_;
                state_->updateGesture([id, byParam, delta](Project&, Sequence& s) {
                    if (Clip* cc = edit::clipById(s, id))
                        for (const auto& [addr, times] : byParam)
                            if (Param* p = findParam(*cc, addr)) shiftKeys(*p, times, delta);
                });
            }
            dragDelta_ = delta;
            std::set<Key> moved;
            for (const Key& k : base) moved.insert({k.address, k.t + delta});
            selection_ = moved;
            break;
        }
        case Drag::None: break;
    }
    update();
}

void KeyframePanel::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    if (drag_ == Drag::Keys && dragging_) state_->endGesture(true);
    if (drag_ == Drag::Box) {
        if (dragging_) {
            // Every key inside the box.
            const QRect r = box_.normalized();
            if (const Clip* c = currentClip())
                for (int row = 0; row < int(rows_.size()); ++row)
                    if (const Param* p = findParam(*c, rows_[size_t(row)].address))
                        for (const Keyframe& k : p->keys)
                            if (r.contains(keyPoint(row, k.t))) selection_.insert({rows_[size_t(row)].address, k.t});
        } else if (const Clip* c = currentClip(); c && e->pos().x() >= kLabelW) {
            // A click on an empty lane moves the playhead there.
            state_->setPlayhead(c->start + frameAtX(e->pos().x()));
        }
    }
    drag_ = Drag::None;
    dragging_ = false;
    rebuild();
}

void KeyframePanel::mouseDoubleClickEvent(QMouseEvent* e) {
    const int r = rowAt(e->pos().y());
    const Clip* c = currentClip();
    if (r < 0 || !c || e->pos().x() < kLabelW) return;
    Key existing;
    if (keyAt(e->pos(), existing)) {
        state_->setPlayhead(c->start + existing.t);
        return;
    }
    // A new key with the value the parameter has there.
    const ParamAddress addr = rows_[size_t(r)].address;
    const FrameTime t = frameAtX(e->pos().x());
    const Id id = clip_;
    if (state_->edit(tr("Add Keyframe"), [id, addr, t](Project&, Sequence& s) {
            Clip* cc = edit::clipById(s, id);
            Param* p = cc ? findParam(*cc, addr) : nullptr;
            if (!p || p->keyAt(t)) return false;
            p->addKey(t, p->at(t));
            return true;
        }))
        selection_ = {{addr, t}};
    update();
}

void KeyframePanel::contextMenuEvent(QContextMenuEvent* e) {
    Key k;
    if (keyAt(e->pos(), k) && !selection_.count(k)) selection_ = {k};
    if (selection_.empty()) return;
    update();
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("keyframePanelMenu"));
    const std::pair<Interp, QString> kinds[] = {
        {Interp::Linear, tr("Linear")}, {Interp::Hold, tr("Hold")}, {Interp::Smooth, tr("Smooth (Ease In and Out)")}};
    for (const auto& [interp, name] : kinds)
        menu.addAction(name, this, [this, interp = interp] { setInterpolation(interp); })->setData(int(interp));
    menu.addSeparator();
    menu.addAction(tr("Delete"), this, [this] { deleteSelected(); });
    menu.exec(e->globalPos());
}

bool KeyframePanel::event(QEvent* e) {
    // Delete and the arrow keys act on the selected keys, over the window's shortcuts.
    if (e->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(e);
        const int key = ke->key();
        const bool ours = key == Qt::Key_Delete || key == Qt::Key_Backspace || key == Qt::Key_Left || key == Qt::Key_Right;
        if (!selection_.empty() && ours) {
            ke->accept();
            return true;
        }
    }
    return QWidget::event(e);
}

void KeyframePanel::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
        case Qt::Key_Delete:
        case Qt::Key_Backspace: deleteSelected(); return;
        case Qt::Key_Left: shiftSelected(e->modifiers() & Qt::ShiftModifier ? -10 : -1); return;
        case Qt::Key_Right: shiftSelected(e->modifiers() & Qt::ShiftModifier ? 10 : 1); return;
        default: QWidget::keyPressEvent(e);
    }
}

}  // namespace montage
