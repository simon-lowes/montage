#include "KeyframePanel.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <map>

#include "EditorState.h"
#include "Theme.h"
#include "core/EditOps.h"
#include "core/Effects.h"
#include "core/History.h"
#include "core/MaskPath.h"

namespace montage {

namespace {
// A drawn mask's path is keyed as a whole, shown as one row: its first coordinate.
bool isPathRow(const ParamAddress& a) { return a.slot == ParamSlot::Effect && a.param == kMaskPathParam; }

std::vector<int> graphRowsAll(size_t n) {
    std::vector<int> all(n);
    for (size_t i = 0; i < n; ++i) all[i] = int(i);
    return all;
}

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
    graphButton_ = new QToolButton(this);
    graphButton_->setObjectName(QStringLiteral("keyframeGraph"));
    graphButton_->setText(tr("Graph"));
    graphButton_->setCheckable(true);
    graphButton_->setAutoRaise(true);
    graphButton_->setToolTip(tr("Show the values as curves, to shape them with Bezier handles"));
    connect(graphButton_, &QToolButton::toggled, this, [this](bool on) { setGraph(on); });
    rebuild();
}

void KeyframePanel::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    graphButton_->setGeometry(kLabelW - 58, 1, 54, kRulerH - 2);
}

void KeyframePanel::setGraph(bool on) {
    graph_ = on;
    {
        QSignalBlocker b(graphButton_);
        graphButton_->setChecked(on);
    }
    update();
}

void KeyframePanel::setGraphRow(int row) {
    graphRow_ = row >= 0 && row < int(rows_.size()) ? row : -1;
    update();
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
                if (!param.animated() || (isMaskPathParam(name) && name != kMaskPathParam)) continue;
                QString label = QString::fromStdString(name);
                if (info)
                    for (const ParamInfo& pi : info->params)
                        if (pi.name == name) label = QString::fromStdString(pi.label);
                if (name == kMaskPathParam) label = tr("Mask Path");
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
    if (graphRow_ >= int(rows_.size())) graphRow_ = -1;
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

double KeyframePanel::xForFrameD(double t) const {
    const Clip* c = currentClip();
    const double span = c ? double(std::max<FrameTime>(1, c->duration - 1)) : 1.0;
    return kLabelW + kMargin + t / span * double(std::max(1, width() - kLabelW - 2 * kMargin));
}

double KeyframePanel::frameAtXD(double x) const {
    const Clip* c = currentClip();
    const double span = c ? double(std::max<FrameTime>(1, c->duration - 1)) : 1.0;
    return (x - kLabelW - kMargin) / double(std::max(1, width() - kLabelW - 2 * kMargin)) * span;
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

std::set<KeyframePanel::Key> KeyframePanel::linked(const std::set<Key>& keys) const {
    std::set<Key> out = keys;
    const Clip* c = currentClip();
    if (!c) return out;
    for (const Key& k : keys) {
        if (!isPathRow(k.address)) continue;
        if (const Effect* e = paramOwner(*c, k.address))
            for (const std::string& name : maskPathParams(*e)) {
                ParamAddress a = k.address;
                a.param = name;
                out.insert({a, k.t});
            }
    }
    return out;
}

FrameTime KeyframePanel::clampShift(FrameTime delta) const {
    const Clip* c = currentClip();
    if (!c) return 0;
    std::map<ParamAddress, std::vector<FrameTime>> byParam;
    for (const Key& k : linked(selection_)) byParam[k.address].push_back(k.t);
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
    for (const Key& k : linked(selection_)) byParam[k.address].push_back(k.t);
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
    const auto keys = linked(selection_);
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
    const auto keys = linked(selection_);
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

bool KeyframePanel::easeSelected(bool in, bool out) {
    if (selection_.empty()) return false;
    const auto keys = linked(selection_);
    const Id id = clip_;
    return state_->edit(in && out ? tr("Easy Ease") : in ? tr("Ease In") : tr("Ease Out"), [id, keys, in, out](Project&, Sequence& s) {
        Clip* c = edit::clipById(s, id);
        if (!c) return false;
        bool any = false;
        for (const Key& k : keys)
            if (Param* p = findParam(*c, k.address)) any |= easeKey(*p, k.t, in, out);
        return any;
    });
}

// ---- The value graph ----------------------------------------------------------

std::vector<int> KeyframePanel::graphRows() const {
    // A mask path has no one value to plot.
    std::vector<int> all;
    if (graphRow_ >= 0 && graphRow_ < int(rows_.size())) {
        if (!isPathRow(rows_[size_t(graphRow_)].address)) all.push_back(graphRow_);
        return all;
    }
    for (int i = 0; i < int(rows_.size()); ++i)
        if (!isPathRow(rows_[size_t(i)].address)) all.push_back(i);
    return all;
}

QRect KeyframePanel::plot() const { return QRect(kLabelW, kRulerH + 8, width() - kLabelW, std::max(10, height() - kRulerH - 16)); }

std::pair<double, double> KeyframePanel::graphRange(int row) const {
    if (auto it = frozen_.find(row); it != frozen_.end()) return it->second;
    const Clip* c = currentClip();
    const Param* p = c && row >= 0 && row < int(rows_.size()) ? findParam(*c, rows_[size_t(row)].address) : nullptr;
    if (!p || p->keys.empty()) return {0, 1};
    double lo = p->keys.front().v, hi = lo;
    const FrameTime step = std::max<FrameTime>(1, c->duration / 400);
    for (FrameTime t = 0; t < c->duration; t += step) lo = std::min(lo, p->at(t)), hi = std::max(hi, p->at(t));
    for (size_t i = 0; i < p->keys.size(); ++i) {
        double inDt, inDv, outDt, outDv;
        keyHandles(p->keys, i, inDt, inDv, outDt, outDv);
        for (double v : {p->keys[i].v, p->keys[i].v + inDv, p->keys[i].v + outDv}) lo = std::min(lo, v), hi = std::max(hi, v);
    }
    if (hi - lo < 1e-9) {
        const double pad = std::max(1.0, std::fabs(lo) * 0.1);
        return {lo - pad, hi + pad};
    }
    const double pad = (hi - lo) * 0.1;
    return {lo - pad, hi + pad};
}

double KeyframePanel::yForValue(int row, double v) const {
    const QRect r = plot();
    const auto [lo, hi] = graphRange(row);
    return r.bottom() - (v - lo) / (hi - lo) * r.height();
}

double KeyframePanel::valueAtY(int row, double y) const {
    const QRect r = plot();
    const auto [lo, hi] = graphRange(row);
    return lo + (r.bottom() - y) / double(std::max(1, r.height())) * (hi - lo);
}

QPointF KeyframePanel::graphPoint(int row, FrameTime t) const {
    const Clip* c = currentClip();
    const Param* p = c && row >= 0 && row < int(rows_.size()) ? findParam(*c, rows_[size_t(row)].address) : nullptr;
    if (!p) return {};
    const Keyframe* k = p->keyAt(t);
    return {xForFrame(t), yForValue(row, k ? k->v : p->at(t))};
}

QPointF KeyframePanel::handlePoint(int row, FrameTime t, bool out) const {
    const Clip* c = currentClip();
    const Param* p = c && row >= 0 && row < int(rows_.size()) ? findParam(*c, rows_[size_t(row)].address) : nullptr;
    if (!p) return {};
    for (size_t i = 0; i < p->keys.size(); ++i) {
        if (p->keys[i].t != t) continue;
        double inDt, inDv, outDt, outDv;
        keyHandles(p->keys, i, inDt, inDv, outDt, outDv);
        const double dt = out ? outDt : inDt, dv = out ? outDv : inDv;
        return {xForFrameD(double(t) + dt), yForValue(row, p->keys[i].v + dv)};
    }
    return {};
}

bool KeyframePanel::graphKeyAt(const QPoint& pos, Key& key, int& row) const {
    const Clip* c = currentClip();
    if (!c || pos.x() < kLabelW) return false;
    double best = 7;
    bool found = false;
    for (int r : graphRows())
        if (const Param* p = findParam(*c, rows_[size_t(r)].address))
            for (const Keyframe& k : p->keys) {
                const QPointF at = graphPoint(r, k.t);
                const double d = std::hypot(at.x() - pos.x(), at.y() - pos.y());
                if (d <= best) best = d, key = {rows_[size_t(r)].address, k.t}, row = r, found = true;
            }
    return found;
}

bool KeyframePanel::handleAt(const QPoint& pos, Key& key, int& row, bool& out) const {
    const Clip* c = currentClip();
    if (!c || pos.x() < kLabelW) return false;
    double best = 7;
    bool found = false;
    for (const Key& k : selection_) {
        int r = -1;
        for (int i : graphRows())
            if (rows_[size_t(i)].address == k.address) r = i;
        const Param* p = r >= 0 ? findParam(*c, k.address) : nullptr;
        if (!p) continue;
        for (size_t i = 0; i < p->keys.size(); ++i) {
            if (p->keys[i].t != k.t) continue;
            const bool hasOut = p->keys[i].interp == Interp::Bezier && i + 1 < p->keys.size();
            const bool hasIn = i > 0 && p->keys[i - 1].interp == Interp::Bezier;
            for (bool o : {true, false}) {
                if ((o && !hasOut) || (!o && !hasIn)) continue;
                const QPointF h = handlePoint(r, k.t, o);
                const double d = std::hypot(h.x() - pos.x(), h.y() - pos.y());
                if (d <= best) best = d, key = k, row = r, out = o, found = true;
            }
        }
    }
    return found;
}

void KeyframePanel::paintGraph(QPainter& p, const Clip& c) {
    static const QColor kCurve[] = {QColor(255, 120, 90), QColor(110, 200, 120), QColor(100, 160, 255), QColor(240, 200, 80),
                                    QColor(200, 120, 230), QColor(90, 210, 210)};
    const QRect r = plot();
    p.fillRect(QRect(kLabelW, kRulerH, width() - kLabelW, height() - kRulerH), palette().base());
    p.setPen(QPen(palette().color(QPalette::Mid), 1, Qt::DotLine));
    for (int i = 0; i <= 4; ++i) p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4.0), QPointF(r.right(), r.top() + r.height() * i / 4.0));
    const std::vector<int> shown = graphRows();
    // Labels: every row, with its colour; a click shows one alone.
    for (int row = 0; row < int(rows_.size()); ++row) {
        const QRect l(0, kRulerH + row * kRowH, kLabelW, kRowH);
        if (row == graphRow_) p.fillRect(l, palette().highlight());
        p.fillRect(QRect(6, l.center().y() - 4, 8, 8), kCurve[row % 6]);
        p.setPen(row == graphRow_ ? palette().color(QPalette::HighlightedText)
                                  : (graphRow_ >= 0 ? palette().color(QPalette::PlaceholderText) : palette().color(QPalette::Text)));
        p.drawText(QRect(20, l.top(), kLabelW - 26, kRowH), Qt::AlignVCenter,
                   p.fontMetrics().elidedText(rows_[size_t(row)].label, Qt::ElideRight, kLabelW - 26));
    }
    // One curve alone shows its values.
    if (graphRow_ >= 0) {
        const auto [lo, hi] = graphRange(graphRow_);
        p.setPen(palette().color(QPalette::PlaceholderText));
        p.drawText(QRect(r.left() + 4, r.top(), 100, 14), Qt::AlignLeft | Qt::AlignTop, QString::number(hi, 'g', 4));
        p.drawText(QRect(r.left() + 4, r.bottom() - 14, 100, 14), Qt::AlignLeft | Qt::AlignBottom, QString::number(lo, 'g', 4));
    }
    p.setRenderHint(QPainter::Antialiasing);
    for (int row : shown) {
        const Param* prm = findParam(c, rows_[size_t(row)].address);
        if (!prm) continue;
        const QColor col = kCurve[row % 6];
        QPainterPath path;
        const FrameTime step = std::max<FrameTime>(1, c.duration / std::max(1, r.width()));
        for (FrameTime t = 0; t < c.duration; t += step) {
            const QPointF pt(xForFrame(t), yForValue(row, prm->at(t)));
            if (t == 0) path.moveTo(pt);
            else path.lineTo(pt);
        }
        p.setPen(QPen(col, 2));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        for (size_t i = 0; i < prm->keys.size(); ++i) {
            const Keyframe& k = prm->keys[i];
            const bool sel = selection_.count({rows_[size_t(row)].address, k.t}) > 0;
            const QPointF at = graphPoint(row, k.t);
            // The handles of selected Bezier keys.
            if (sel) {
                const bool hasOut = k.interp == Interp::Bezier && i + 1 < prm->keys.size();
                const bool hasIn = i > 0 && prm->keys[i - 1].interp == Interp::Bezier;
                for (bool o : {true, false}) {
                    if ((o && !hasOut) || (!o && !hasIn)) continue;
                    const QPointF h = handlePoint(row, k.t, o);
                    p.setPen(QPen(theme::kSnap, 1));
                    p.drawLine(at, h);
                    p.setBrush(palette().base());
                    p.drawEllipse(h, 3.5, 3.5);
                }
            }
            p.setPen(QPen(QColor(0, 0, 0, 200), 1));
            p.setBrush(sel ? theme::kSnap : col);
            if (k.interp == Interp::Bezier) p.drawEllipse(at, 4.5, 4.5);
            else p.drawRect(QRectF(at.x() - 4, at.y() - 4, 8, 8));
        }
    }
    p.setRenderHint(QPainter::Antialiasing, false);
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
    p.drawText(QRect(4, 0, kLabelW - 66, kRulerH), Qt::AlignVCenter | Qt::AlignLeft,
               p.fontMetrics().elidedText(QString::fromStdString(c->name), Qt::ElideRight, kLabelW - 66));
    p.drawText(QRect(kLabelW + 4, 0, 120, kRulerH), Qt::AlignVCenter, QString::fromStdString(formatTimecode(c->start, s->fps)));
    p.drawText(QRect(width() - 124, 0, 120, kRulerH), Qt::AlignVCenter | Qt::AlignRight,
               QString::fromStdString(formatTimecode(c->end() - 1, s->fps)));
    // Rows, or their curves.
    if (graph_) paintGraph(p, *c);
    for (int r = 0; r < int(rows_.size()) && !graph_; ++r) {
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
            p.setPen(QPen(QColor(0, 0, 0, 180), 1));
            p.setBrush(sel ? theme::kSnap
                           : (k.interp == Interp::Smooth   ? QColor(120, 200, 255)
                              : k.interp == Interp::Bezier ? QColor(255, 170, 90)
                                                           : QColor(220, 220, 220)));
            if (k.interp == Interp::Bezier) {
                p.drawEllipse(at, 5.5, 5.5);  // a circle, as in After Effects
                continue;
            }
            if (k.interp == Interp::Hold) d << at + QPointF(-4, -5) << at + QPointF(4, -5) << at + QPointF(4, 5) << at + QPointF(-4, 5);
            else d << at + QPointF(0, -6) << at + QPointF(6, 0) << at + QPointF(0, 6) << at + QPointF(-6, 0);
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
    if (graph_) {
        // A label shows its curve alone (or all again); handles and keys are dragged; the ranges hold still meanwhile.
        if (e->pos().x() < kLabelW) {
            const int r = rowAt(e->pos().y());
            if (r >= 0) setGraphRow(r == graphRow_ ? -1 : r);
            drag_ = Drag::None;
            return;
        }
        int row = -1;
        bool out = true;
        auto freeze = [this] {
            frozen_.clear();
            for (int r : graphRows()) frozen_[r] = graphRange(r);
        };
        if (handleAt(e->pos(), k, row, out)) {
            freeze();
            drag_ = Drag::Handle;
            dragKey_ = k, dragRow_ = row, dragOut_ = out;
            return;
        }
        if (graphKeyAt(e->pos(), k, row)) {
            if (add && selection_.count(k)) selection_.erase(k);
            else if (add) selection_.insert(k);
            else if (!selection_.count(k)) selection_ = {k};
            freeze();
            drag_ = selection_.count(k) ? Drag::GraphKey : Drag::None;
            dragKey_ = k, dragRow_ = row;
            update();
            return;
        }
    } else if (keyAt(e->pos(), k)) {
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
                for (const Key& k : linked(base)) byParam[k.address].push_back(k.t);
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
        case Drag::GraphKey: {
            // The grabbed key, to the frame and value under the mouse (from where the drag began).
            if (start) state_->beginGesture(tr("Move Keyframe"));
            const Sequence* origin = state_->gestureBase();
            const Clip* oc = origin ? edit::clipById(*origin, clip_) : nullptr;
            if (!oc) break;
            const FrameTime to = frameAtX(e->pos().x());
            const double v = valueAtY(dragRow_, e->pos().y());
            const Key k = dragKey_;
            const Id id = clip_;
            const FrameTime last = oc->duration - 1;
            FrameTime moved = k.t;
            state_->updateGesture([id, k, to, v, last, &moved](Project&, Sequence& sq) {
                if (Clip* cc = edit::clipById(sq, id))
                    if (Param* prm = findParam(*cc, k.address)) moved = std::max<FrameTime>(0, moveKey(*prm, k.t, to, v, last));
            });
            selection_ = {{k.address, moved}};
            break;
        }
        case Drag::Handle: {
            if (start) state_->beginGesture(tr("Adjust Keyframe Handle"));
            const Sequence* origin = state_->gestureBase();
            const Clip* oc = origin ? edit::clipById(*origin, clip_) : nullptr;
            const Param* op = oc ? findParam(*oc, dragKey_.address) : nullptr;
            const Keyframe* key = op ? op->keyAt(dragKey_.t) : nullptr;
            if (!key) break;
            const double dt = frameAtXD(e->pos().x()) - double(key->t);
            const double dv = valueAtY(dragRow_, e->pos().y()) - key->v;
            const bool linked = !(e->modifiers() & Qt::AltModifier);
            const Key k = dragKey_;
            const bool out = dragOut_;
            const Id id = clip_;
            state_->updateGesture([id, k, out, dt, dv, linked](Project&, Sequence& sq) {
                if (Clip* cc = edit::clipById(sq, id))
                    if (Param* prm = findParam(*cc, k.address)) setKeyHandle(*prm, k.t, out, dt, dv, linked);
            });
            break;
        }
        case Drag::None: break;
    }
    update();
}

void KeyframePanel::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    if ((drag_ == Drag::Keys || drag_ == Drag::GraphKey || drag_ == Drag::Handle) && dragging_) state_->endGesture(true);
    frozen_.clear();
    if (drag_ == Drag::Box) {
        if (dragging_) {
            // Every key inside the box.
            const QRect r = box_.normalized();
            if (const Clip* c = currentClip())
                for (int row : graph_ ? graphRows() : graphRowsAll(rows_.size()))
                    if (const Param* p = findParam(*c, rows_[size_t(row)].address))
                        for (const Keyframe& k : p->keys)
                            if (r.contains(graph_ ? graphPoint(row, k.t).toPoint() : keyPoint(row, k.t)))
                                selection_.insert({rows_[size_t(row)].address, k.t});
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
    int r = rowAt(e->pos().y());
    const Clip* c = currentClip();
    Key existing;
    if (graph_ && c && e->pos().x() >= kLabelW) {
        // In the graph: on the curve shown alone, or the one nearest the click.
        int row = -1;
        if (graphKeyAt(e->pos(), existing, row)) {
            state_->setPlayhead(c->start + existing.t);
            return;
        }
        const FrameTime t = frameAtX(e->pos().x());
        double best = 1e9;
        r = -1;
        for (int i : graphRows())
            if (const Param* p = findParam(*c, rows_[size_t(i)].address))
                if (const double d = std::fabs(yForValue(i, p->at(t)) - e->pos().y()); d < best) best = d, r = i;
    } else if (r < 0 || !c || e->pos().x() < kLabelW) {
        return;
    } else if (keyAt(e->pos(), existing)) {
        state_->setPlayhead(c->start + existing.t);
        return;
    }
    if (r < 0 || !c) return;
    // A new key with the value the parameter has there.
    const ParamAddress addr = rows_[size_t(r)].address;
    const FrameTime t = frameAtX(e->pos().x());
    const Id id = clip_;
    if (state_->edit(tr("Add Keyframe"), [id, addr, t](Project&, Sequence& s) {
            Clip* cc = edit::clipById(s, id);
            if (isPathRow(addr)) {
                // The whole path, as it is there.
                Effect* e = cc ? paramOwner(*cc, addr) : nullptr;
                if (!e || e->params[kMaskPathParam].keyAt(t)) return false;
                setMaskPathAnimated(*e, t, true);
                return true;
            }
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
    int row = -1;
    if ((graph_ ? graphKeyAt(e->pos(), k, row) : keyAt(e->pos(), k)) && !selection_.count(k)) selection_ = {k};
    if (selection_.empty() && !(hasCopiedKeys() && currentClip())) return;
    update();
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("keyframePanelMenu"));
    if (selection_.empty()) {
        menu.addAction(tr("Paste Keyframes at Playhead"), this, [this] { pasteAtPlayhead(); })->setObjectName(QStringLiteral("pasteKeyframes"));
        menu.exec(e->globalPos());
        return;
    }
    menu.addAction(tr("Copy"), this, [this] { copySelected(); })->setObjectName(QStringLiteral("copyKeyframes"));
    if (hasCopiedKeys())
        menu.addAction(tr("Paste at Playhead"), this, [this] { pasteAtPlayhead(); })->setObjectName(QStringLiteral("pasteKeyframes"));
    menu.addSeparator();
    const std::pair<Interp, QString> kinds[] = {{Interp::Linear, tr("Linear")},
                                                {Interp::Hold, tr("Hold")},
                                                {Interp::Smooth, tr("Smooth (Ease In and Out)")},
                                                {Interp::Bezier, tr("Bezier")}};
    for (const auto& [interp, name] : kinds)
        menu.addAction(name, this, [this, interp = interp] { setInterpolation(interp); })->setData(int(interp));
    menu.addSeparator();
    menu.addAction(tr("Ease In"), this, [this] { easeSelected(true, false); })->setObjectName(QStringLiteral("easeIn"));
    menu.addAction(tr("Ease Out"), this, [this] { easeSelected(false, true); })->setObjectName(QStringLiteral("easeOut"));
    menu.addAction(tr("Easy Ease"), this, [this] { easeSelected(true, true); })->setObjectName(QStringLiteral("easyEase"));
    menu.addSeparator();
    QMenu* after = menu.addMenu(tr("After Last Keyframe"));
    after->setObjectName(QStringLiteral("keyframeRepeat"));
    const std::pair<Repeat, QString> repeats[] = {{Repeat::Hold, tr("Hold")},
                                                  {Repeat::Loop, tr("Loop")},
                                                  {Repeat::PingPong, tr("Ping-Pong")},
                                                  {Repeat::Offset, tr("Loop and Offset")}};
    for (const auto& [rep, name] : repeats) after->addAction(name, this, [this, rep = rep] { setRepeat(rep); });
    menu.addSeparator();
    menu.addAction(tr("Delete"), this, [this] { deleteSelected(); });
    menu.exec(e->globalPos());
}

bool KeyframePanel::setRepeat(Repeat repeat) {
    if (selection_.empty() || !clip_) return false;
    std::set<ParamAddress> params;
    for (const Key& k : linked(selection_)) params.insert(k.address);
    const Id id = clip_;
    return state_->edit(tr("Keyframe Repeat"), [id, params, repeat](Project&, Sequence& s) {
        Clip* c = edit::clipById(s, id);
        if (!c) return false;
        bool changed = false;
        for (const ParamAddress& a : params)
            if (Param* p = findParam(*c, a); p && p->repeat != repeat) {
                p->repeat = repeat;
                changed = true;
            }
        return changed;
    });
}

namespace {
CopiedKeys& keyClipboard() {
    static CopiedKeys keys;
    return keys;
}
}  // namespace

bool KeyframePanel::hasCopiedKeys() { return !keyClipboard().empty(); }

bool KeyframePanel::copySelected() {
    const Clip* c = currentClip();
    if (!c || selection_.empty()) return false;
    std::vector<std::pair<ParamAddress, FrameTime>> keys;
    for (const Key& k : linked(selection_)) keys.push_back({k.address, k.t});
    CopiedKeys copied = copyKeys(*c, keys);
    if (copied.empty()) return false;
    size_t n = 0;
    for (const auto& l : copied.lanes) n += l.keys.size();
    keyClipboard() = std::move(copied);
    state_->message(tr("Copied %n keyframe(s)", nullptr, int(n)));
    return true;
}

int KeyframePanel::pasteAtPlayhead() {
    const Clip* c = currentClip();
    if (!c || keyClipboard().empty()) return 0;
    const Id id = clip_;
    const FrameTime at = std::max<FrameTime>(0, state_->playhead() - c->start);
    const CopiedKeys keys = keyClipboard();
    int pasted = 0;
    state_->edit(tr("Paste Keyframes"), [&](Project&, Sequence& s) {
        Clip* target = edit::clipById(s, id);
        if (!target) return false;
        pasted = pasteKeys(*target, keys, at);
        return pasted > 0;
    });
    if (!pasted) {
        state_->message(tr("This clip has none of the copied parameters"));
        return 0;
    }
    // The pasted keys, selected.
    std::set<Key> sel;
    if (const Clip* now = currentClip())
        for (const auto& lane : keys.lanes)
            if (pasteOwner(const_cast<Clip&>(*now), lane))
                for (const Row& r : rows_)
                    if (r.address.param == lane.address.param && paramOwner(*now, r.address) == pasteOwner(const_cast<Clip&>(*now), lane))
                        for (const Keyframe& k : lane.keys) sel.insert({r.address, k.t + at});
    select(sel);
    return pasted;
}

bool KeyframePanel::event(QEvent* e) {
    // Delete, the arrow keys, copy and paste act on the keys, over the window's shortcuts.
    if (e->type() == QEvent::ShortcutOverride) {
        auto* ke = static_cast<QKeyEvent*>(e);
        const int key = ke->key();
        const bool ours = key == Qt::Key_Delete || key == Qt::Key_Backspace || key == Qt::Key_Left || key == Qt::Key_Right ||
                          ke->matches(QKeySequence::Copy);
        if ((!selection_.empty() && ours) || (ke->matches(QKeySequence::Paste) && hasCopiedKeys() && currentClip())) {
            ke->accept();
            return true;
        }
    }
    return QWidget::event(e);
}

void KeyframePanel::keyPressEvent(QKeyEvent* e) {
    if (e->matches(QKeySequence::Copy)) {
        copySelected();
        return;
    }
    if (e->matches(QKeySequence::Paste)) {
        pasteAtPlayhead();
        return;
    }
    switch (e->key()) {
        case Qt::Key_Delete:
        case Qt::Key_Backspace: deleteSelected(); return;
        case Qt::Key_Left: shiftSelected(e->modifiers() & Qt::ShiftModifier ? -10 : -1); return;
        case Qt::Key_Right: shiftSelected(e->modifiers() & Qt::ShiftModifier ? 10 : 1); return;
        default: QWidget::keyPressEvent(e);
    }
}

}  // namespace montage
