#include "GradeNodePanel.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>

#include "EditorState.h"
#include "Theme.h"
#include "core/EditOps.h"
#include "core/Effects.h"

namespace montage {

namespace {

constexpr double kNodeW = 120, kNodeH = 54, kEndW = 76, kEndH = 40, kPortR = 6;
const QColor kPictureLink{0x5c, 0xc8, 0x6e};
const QColor kKeyLink{0x4d, 0x9b, 0xff};

// A port: a node's (0: the clip's picture as an output, the graph's output as an input) output or input.
struct Port {
    int node = 0;
    bool output = false;
    int input = 0;     // an input's number
    bool key = false;  // carries a key (a corrector's key output or input, a key mixer's)
};

}  // namespace

// The graph drawn: nodes at their places (graph units, scaled to fit), links as curves.
class GradeNodeCanvas : public QWidget {
public:
    GradeNodeCanvas(GradeNodePanel* panel, EditorState* state) : QWidget(panel), panel_(panel), state_(state) {
        setFocusPolicy(Qt::ClickFocus);
        setMouseTracking(true);
        setMinimumSize(240, 140);
    }

    const Clip* clip() const {
        const Sequence* s = state_->sequence();
        const Id id = panel_->clip();
        return s && id ? edit::clipById(*s, id) : nullptr;
    }

    // A node's box in graph units (0: the picture's, -1: the output's).
    QRectF box(int key) const {
        const Clip* c = clip();
        if (!c) return {};
        const GradeGraph& g = c->grade;
        double minX = 0, maxX = 0, sumY = 0;
        bool first = true;
        for (const GradeNode& n : g.nodes) {
            minX = first ? n.x : std::min(minX, n.x);
            maxX = first ? n.x : std::max(maxX, n.x);
            sumY += n.y;
            first = false;
        }
        const double midY = g.nodes.empty() ? 0 : sumY / double(g.nodes.size());
        if (key == 0) return {minX - 160 - kEndW + kNodeW, midY + (kNodeH - kEndH) / 2, kEndW, kEndH};
        if (key == -1) return {maxX + 160, midY + (kNodeH - kEndH) / 2, kEndW, kEndH};
        const GradeNode* n = findGradeNode(g, key);
        if (!n) return {};
        const int ins = n->kind == "corrector" ? 2 : inputCount(*n) + 1;
        return {n->x, n->y, kNodeW, std::max(kNodeH, 16.0 * (ins + 1))};
    }

    // How many inputs feed a mixer.
    int inputCount(const GradeNode& n) const {
        int count = 0;
        for (const GradeLink& l : clip()->grade.links)
            if (l.to == n.key) count = std::max(count, l.input + 1);
        return count;
    }

    // The ports of a node (0: the picture; -1: the output), in graph units.
    std::vector<std::pair<Port, QPointF>> ports(int key) const {
        std::vector<std::pair<Port, QPointF>> out;
        const QRectF r = box(key);
        if (key == 0) return {{Port{0, true, 0, false}, {r.right(), r.center().y()}}};
        if (key == -1) return {{Port{0, false, 0, false}, {r.left(), r.center().y()}}};
        const GradeNode* n = findGradeNode(clip()->grade, key);
        if (!n) return out;
        if (n->kind == "corrector") {
            out.push_back({Port{key, false, 0, false}, {r.left(), r.top() + r.height() * 0.35}});
            out.push_back({Port{key, false, 1, true}, {r.left(), r.top() + r.height() * 0.78}});
            out.push_back({Port{key, true, 0, false}, {r.right(), r.top() + r.height() * 0.35}});
            out.push_back({Port{key, true, 0, true}, {r.right(), r.top() + r.height() * 0.78}});
            return out;
        }
        const int ins = inputCount(*n) + 1;  // (one free)
        const bool keys = n->kind == "key_mixer";
        for (int i = 0; i < ins; ++i) out.push_back({Port{key, false, i, keys}, {r.left(), r.top() + r.height() * (i + 1) / (ins + 1)}});
        out.push_back({Port{key, true, 0, keys}, {r.right(), r.center().y()}});
        return out;
    }

    QPointF toView(QPointF g) const { return (g - origin_) * scale_ + QPointF(margin_, margin_); }
    QPointF toGraph(QPointF v) const { return (v - QPointF(margin_, margin_)) / scale_ + origin_; }
    QRectF toView(const QRectF& r) const { return {toView(r.topLeft()), toView(r.bottomRight())}; }

    // Fits everything in the widget (never larger than drawn at 1:1).
    void fit() {
        const Clip* c = clip();
        if (!c || c->grade.empty()) return;
        QRectF all = box(0).united(box(-1));
        for (const GradeNode& n : c->grade.nodes) all = all.united(box(n.key));
        origin_ = all.topLeft();
        const double sx = (width() - 2 * margin_) / std::max(1.0, all.width()), sy = (height() - 2 * margin_) / std::max(1.0, all.height());
        scale_ = std::clamp(std::min(sx, sy), 0.3, 1.0);
    }

    std::vector<int> keys() const {
        std::vector<int> k;
        if (const Clip* c = clip())
            for (const GradeNode& n : c->grade.nodes) k.push_back(n.key);
        return k;
    }

    std::optional<Port> portAt(QPointF v) const {
        std::vector<int> all = keys();
        all.push_back(0);
        all.push_back(-1);
        for (int k : all)
            for (const auto& [port, at] : ports(k))
                if (QLineF(toView(at), v).length() <= kPortR + 3) return port;
        return std::nullopt;
    }

    // The node under a point (0: none; the picture and output are not nodes).
    int nodeAt(QPointF v) const {
        const std::vector<int> all = keys();
        for (auto it = all.rbegin(); it != all.rend(); ++it)
            if (toView(box(*it)).contains(v)) return *it;
        return 0;
    }

    QPointF portView(const Port& p) const {
        const int boxKey = p.node ? p.node : (p.output ? 0 : -1);
        for (const auto& [port, at] : ports(boxKey))
            if (port.output == p.output && port.input == p.input && port.key == p.key) return toView(at);
        return {};
    }

    QPainterPath linkPath(const GradeLink& l) const {
        // (a picture mixer has one output, whatever it gives)
        const GradeNode* from = l.from ? findGradeNode(clip()->grade, l.from) : nullptr;
        const QPointF a = portView(Port{l.from, true, 0, l.key && (!from || !isGradeMixer(*from))});
        const Port in{l.to, false, l.input, l.key};
        const QPointF b = portView(in);
        QPainterPath path(a);
        const double dx = std::max(30.0, std::abs(b.x() - a.x()) / 2);
        path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
        return path;
    }

    // The link under a point, or null.
    const GradeLink* linkAt(QPointF v) const {
        const Clip* c = clip();
        if (!c) return nullptr;
        for (const GradeLink& l : c->grade.links) {
            const QPainterPath path = linkPath(l);
            for (int i = 0; i <= 40; ++i)
                if (QLineF(path.pointAtPercent(i / 40.0), v).length() < 6) return &l;
        }
        return nullptr;
    }

protected:
    void resizeEvent(QResizeEvent*) override { fit(); }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), theme::kPanel);
        const Clip* c = clip();
        p.setPen(theme::kTextDim);
        if (!c) {
            p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, GradeNodePanel::tr("Select a picture clip to see its colour nodes."));
            return;
        }
        if (c->grade.empty()) {
            p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap,
                       GradeNodePanel::tr("This clip's effects run as a stack. Use Nodes to put its colour effects in nodes, one each."));
            return;
        }
        fit();
        const Sequence* s = state_->sequence();
        for (const GradeLink& l : c->grade.links) {
            p.setPen(QPen(l.key ? kKeyLink : kPictureLink, 2));
            p.setBrush(Qt::NoBrush);
            p.drawPath(linkPath(l));
        }
        if (drag_) {
            p.setPen(QPen(drag_->key ? kKeyLink : kPictureLink, 2, Qt::DashLine));
            p.drawLine(portView(*drag_), dragAt_);
        }
        auto drawBox = [&](const QRectF& r, const QString& title, const QString& sub, bool picked, bool on, const QColor& tint) {
            p.setPen(QPen(picked ? theme::kAccent : theme::kBorder, picked ? 2 : 1));
            p.setBrush(on ? tint : theme::kDisabledClip);
            p.drawRoundedRect(r, 6, 6);
            p.setPen(theme::kText);
            QFont f = font();
            f.setBold(true);
            p.setFont(f);
            p.drawText(r.adjusted(8, 4, -8, -r.height() / 2), Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(title, Qt::ElideRight, int(r.width() - 16)));
            p.setFont(font());
            p.setPen(theme::kTextDim);
            p.drawText(r.adjusted(8, r.height() / 2, -8, -4), Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(sub, Qt::ElideRight, int(r.width() - 16)));
        };
        drawBox(toView(box(0)), GradeNodePanel::tr("Picture"), GradeNodePanel::tr("the clip"), false, true, theme::kPanelAlt);
        drawBox(toView(box(-1)), GradeNodePanel::tr("Output"), QString(), false, true, theme::kPanelAlt);
        const int picked = panel_->selectedNode();
        for (const GradeNode& n : c->grade.nodes) {
            QString sub;
            QColor tint = theme::kPanelAlt;
            if (n.kind == "corrector") {
                const int count = int(gradeNodeEffects(*s, *c, n.key).size());
                const SharedNode* sn = findSharedNode(*s, n.shared);
                sub = sn ? GradeNodePanel::tr("shared · %n effect(s)", "", count) : GradeNodePanel::tr("%n effect(s)", "", count);
                if (const Effect* st = gradeNodeSettings(*c, n.key); st && st->enabled && hasMask(*st, 0)) sub += GradeNodePanel::tr(" · window");
                if (sn) tint = QColor(0x3a, 0x34, 0x4a);
            } else {
                sub = n.kind == "parallel" ? GradeNodePanel::tr("averages its inputs") : n.kind == "layer" ? GradeNodePanel::tr("stacks its inputs")
                                                                                         : GradeNodePanel::tr("joins keys");
                tint = QColor(0x30, 0x3a, 0x3a);
            }
            drawBox(toView(box(n.key)), QString::fromStdString(gradeNodeName(n)), sub, n.key == picked, n.enabled, tint);
        }
        std::vector<int> all = keys();
        all.push_back(0);
        all.push_back(-1);
        for (int k : all)
            for (const auto& [port, at] : ports(k)) {
                p.setPen(QPen(theme::kWindow, 1));
                p.setBrush(port.key ? kKeyLink : kPictureLink);
                p.drawEllipse(toView(at), kPortR, kPortR);
            }
    }

    void mousePressEvent(QMouseEvent* e) override {
        const QPointF v = e->position();
        if (e->button() != Qt::LeftButton) return;
        if (std::optional<Port> port = portAt(v); port && port->output) {
            drag_ = port;
            dragAt_ = v;
            return;
        }
        const int key = nodeAt(v);
        panel_->selectNode(key);
        if (key) {
            moving_ = key;
            pressedAt_ = v;
            const GradeNode* n = findGradeNode(clip()->grade, key);
            startPos_ = {n->x, n->y};
            moved_ = false;
        }
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        const QPointF v = e->position();
        if (drag_) {
            dragAt_ = v;
            update();
            return;
        }
        if (!moving_ || !(e->buttons() & Qt::LeftButton)) return;
        if (!moved_ && QLineF(v, pressedAt_).length() < 3) return;
        if (!moved_) state_->beginGesture(GradeNodePanel::tr("Move Node"));
        moved_ = true;
        const QPointF d = (v - pressedAt_) / scale_;
        const Id id = panel_->clip();
        const int key = moving_;
        const QPointF to = startPos_ + d;
        state_->updateGesture([id, key, to](Project&, Sequence& s) {
            const Clip* c = edit::clipById(s, id);
            const GradeNode* n = c ? findGradeNode(c->grade, key) : nullptr;
            if (!n) return;
            GradeNode m = *n;
            m.x = std::round(to.x());
            m.y = std::round(to.y());
            edit::setGradeNode(s, id, m);
        });
    }

    void mouseReleaseEvent(QMouseEvent* e) override {
        if (drag_) {
            const Port from = *drag_;
            drag_.reset();
            if (std::optional<Port> to = portAt(e->position()); to && !to->output)
                panel_->connectNodes({from.node, to->node, to->input, from.key});
            update();
            return;
        }
        if (moving_ && moved_) state_->endGesture(true);
        moving_ = 0;
        moved_ = false;
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override {
        const int key = nodeAt(e->position());
        const GradeNode* n = key ? findGradeNode(clip()->grade, key) : nullptr;
        if (!n) return;
        bool ok = false;
        const QString label = QInputDialog::getText(this, GradeNodePanel::tr("Rename Node"), GradeNodePanel::tr("Label:"), QLineEdit::Normal,
                                                    QString::fromStdString(n->label), &ok);
        if (ok) panel_->renameSelected(label);
    }

    bool event(QEvent* e) override {
        // Delete is the node's while one is picked here, not the timeline's (its shortcut would remove the clip).
        // So are the node keys (Alt+S, P, L, O; Ctrl+D), not the menus' or the timeline's, while the canvas has focus.
        if (e->type() == QEvent::ShortcutOverride && nodeKey(*static_cast<QKeyEvent*>(e))) {
            e->accept();
            return true;
        }
        return QWidget::event(e);
    }

    // What a key does here, or null.
    std::function<void()> nodeKey(const QKeyEvent& k) const {
        const Qt::KeyboardModifiers mods = k.modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        GradeNodePanel* p = panel_;
        if ((k.key() == Qt::Key_Delete || k.key() == Qt::Key_Backspace) && !mods && p->selectedNode()) return [p] { p->removeSelected(); };
        if (!p->clip()) return {};
        if (mods == Qt::AltModifier) {
            const std::optional<edit::NodeHow> how = k.key() == Qt::Key_S ? std::optional(edit::NodeHow::Serial)
                                                   : k.key() == Qt::Key_P ? std::optional(edit::NodeHow::Parallel)
                                                   : k.key() == Qt::Key_L ? std::optional(edit::NodeHow::Layer)
                                                   : k.key() == Qt::Key_O ? std::optional(edit::NodeHow::Outside)
                                                                          : std::nullopt;
            if (how) return [p, h = *how] { p->addNode(h); };
        }
        if (mods == Qt::ControlModifier && k.key() == Qt::Key_D && p->selectedNode()) return [p] { p->toggleSelected(); };
        return {};
    }

    void keyPressEvent(QKeyEvent* e) override {
        if (const auto fn = nodeKey(*e)) {
            fn();
            return;
        }
        QWidget::keyPressEvent(e);
    }

    void contextMenuEvent(QContextMenuEvent* e) override {
        const Clip* c = clip();
        if (!c || c->grade.empty()) return;
        QMenu menu(this);
        if (const GradeLink* l = linkAt(e->pos()); l && l->to) {
            const int to = l->to, input = l->input;
            menu.addAction(GradeNodePanel::tr("Disconnect"), this, [this, to, input] { panel_->disconnectInput(to, input); });
            menu.exec(e->globalPos());
            return;
        }
        if (const int key = nodeAt(e->pos())) panel_->selectNode(key);
        for (QAction* a : panel_->actions()) menu.addAction(a);
        menu.exec(e->globalPos());
    }

private:
    GradeNodePanel* panel_;
    EditorState* state_;
    QPointF origin_;
    double scale_ = 1, margin_ = 24;
    std::optional<Port> drag_;
    QPointF dragAt_, pressedAt_, startPos_;
    int moving_ = 0;
    bool moved_ = false;
    friend class GradeNodePanel;
};

GradeNodePanel::GradeNodePanel(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto* bar = new QWidget(this);
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(4, 2, 4, 2);
    h->setSpacing(2);
    auto button = [&](const QString& text, const QString& tip, const char* name) {
        auto* b = new QToolButton(bar);
        b->setText(text);
        b->setToolTip(tip);
        b->setObjectName(QString::fromLatin1(name));
        b->setAutoRaise(true);
        h->addWidget(b);
        return b;
    };
    convert_ = button(tr("Use Nodes"), tr("Put the clip's colour effects in nodes, one each, in series"), "gradeConvert");
    serial_ = button(tr("Serial"), tr("Add a node after the picked one (Alt+S)"), "gradeAddSerial");
    parallel_ = button(tr("Parallel"), tr("Add a node beside the picked one, joined by a parallel mixer (Alt+P)"), "gradeAddParallel");
    layer_ = button(tr("Layer"), tr("Add a node over the picked one, joined by a layer mixer (Alt+L)"), "gradeAddLayer");
    outside_ = button(tr("Outside"), tr("Add a node grading what the picked node's window leaves out (Alt+O)"), "gradeAddOutside");
    remove_ = button(tr("Remove"), tr("Remove the picked node and its effects (Delete)"), "gradeRemove");
    share_ = button(tr("Share…"), tr("Make the picked node a shared node other clips can use"), "gradeShare");
    shared_ = button(tr("Use Shared"), tr("Add a shared node after the clip's last node"), "gradeUseShared");
    sharedMenu_ = new QMenu(shared_);
    shared_->setMenu(sharedMenu_);
    shared_->setPopupMode(QToolButton::InstantPopup);
    flatten_ = button(tr("Flatten"), tr("Back to an effect stack: the nodes' effects in the order they ran"), "gradeFlatten");
    h->addStretch(1);
    v->addWidget(bar);
    canvas_ = new GradeNodeCanvas(this, state_);
    v->addWidget(canvas_, 1);

    connect(convert_, &QToolButton::clicked, this, &GradeNodePanel::convert);
    connect(serial_, &QToolButton::clicked, this, [this] { addNode(edit::NodeHow::Serial); });
    connect(parallel_, &QToolButton::clicked, this, [this] { addNode(edit::NodeHow::Parallel); });
    connect(layer_, &QToolButton::clicked, this, [this] { addNode(edit::NodeHow::Layer); });
    connect(outside_, &QToolButton::clicked, this, [this] { addNode(edit::NodeHow::Outside); });
    connect(remove_, &QToolButton::clicked, this, &GradeNodePanel::removeSelected);
    connect(flatten_, &QToolButton::clicked, this, &GradeNodePanel::flatten);
    connect(share_, &QToolButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Share Node"), tr("Shared node's name:"), QLineEdit::Normal, QString(), &ok);
        if (ok) shareSelected(name);
    });
    connect(sharedMenu_, &QMenu::aboutToShow, this, [this] {
        sharedMenu_->clear();
        const Sequence* s = state_->sequence();
        if (s)
            for (const SharedNode& n : s->sharedNodes) {
                const Id id = n.id;
                sharedMenu_->addAction(QString::fromStdString(n.name), this, [this, id] { useShared(id); });
            }
        if (sharedMenu_->isEmpty()) sharedMenu_->addAction(tr("No shared nodes yet"))->setEnabled(false);
    });

    // The context menu's and the keys' actions (while the panel has focus).
    // (the keys are the canvas's own: see GradeNodeCanvas::nodeKey)
    auto action = [this](const QString& text, const QKeySequence& keys, const std::function<void()>& fn) {
        auto* a = new QAction(keys.isEmpty() ? text : text + '\t' + keys.toString(QKeySequence::NativeText), this);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
        return a;
    };
    action(tr("Add Serial Node"), QKeySequence(Qt::ALT | Qt::Key_S), [this] { addNode(edit::NodeHow::Serial); });
    action(tr("Add Parallel Node"), QKeySequence(Qt::ALT | Qt::Key_P), [this] { addNode(edit::NodeHow::Parallel); });
    action(tr("Add Layer Node"), QKeySequence(Qt::ALT | Qt::Key_L), [this] { addNode(edit::NodeHow::Layer); });
    action(tr("Add Outside Node"), QKeySequence(Qt::ALT | Qt::Key_O), [this] { addNode(edit::NodeHow::Outside); });
    action(tr("Add Key Mixer"), {}, [this] { addKeyMixer(); });
    action(tr("Enable / Disable Node"), QKeySequence(Qt::CTRL | Qt::Key_D), [this] { toggleSelected(); });
    action(tr("Unshare Node"), {}, [this] { unshareSelected(); });
    action(tr("Remove Node"), {}, [this] { removeSelected(); });

    for (auto sig : {&EditorState::projectChanged, &EditorState::selectionChanged, &EditorState::sequenceSwitched, &EditorState::gradeNodeSelected})
        connect(state_, sig, this, &GradeNodePanel::sync);
    sync();
}

Id GradeNodePanel::clip() const {
    const Clip* c = state_->primaryClip();
    const Sequence* s = state_->sequence();
    if (!c || !s) return 0;
    const auto loc = edit::locate(*s, c->id);
    return loc && loc->track.kind == TrackKind::Video ? c->id : 0;
}

int GradeNodePanel::selectedNode() const { return clip() ? state_->selectedGradeNode() : 0; }

void GradeNodePanel::selectNode(int key) { state_->selectGradeNode(key); }

void GradeNodePanel::sync() {
    const Clip* c = clip() ? state_->primaryClip() : nullptr;
    const bool graph = c && !c->grade.empty();
    const GradeNode* n = graph ? findGradeNode(c->grade, selectedNode()) : nullptr;
    const bool corrector = n && n->kind == "corrector";
    convert_->setVisible(!graph);
    convert_->setEnabled(c);
    for (QToolButton* b : {serial_, parallel_, layer_, outside_}) b->setEnabled(c);
    remove_->setEnabled(n);
    share_->setEnabled(corrector && !n->shared);
    shared_->setEnabled(c && state_->sequence() && !state_->sequence()->sharedNodes.empty());
    flatten_->setEnabled(graph);
    canvas_->fit();
    canvas_->update();
}

bool GradeNodePanel::convert() {
    const Id id = clip();
    if (!id) return false;
    const bool done = state_->apply(tr("Use Nodes"), [id](Project&, Sequence& s) { return edit::convertToNodes(s, id); });
    if (done) selectNode(1);
    return done;
}

bool GradeNodePanel::addNode(edit::NodeHow how) {
    const Id id = clip();
    if (!id) return false;
    const Clip* c = state_->primaryClip();
    const GradeNode* at = findGradeNode(c->grade, selectedNode());
    const int anchor = at && at->kind != "key_mixer" ? at->key : 0;
    int made = 0;
    const QString label = how == edit::NodeHow::Parallel ? tr("Add Parallel Node") : how == edit::NodeHow::Layer ? tr("Add Layer Node")
                        : how == edit::NodeHow::Outside  ? tr("Add Outside Node")
                                                         : tr("Add Serial Node");
    if (!state_->apply(label, [&](Project& p, Sequence& s) { return edit::addGradeNode(p, s, id, how, anchor, &made); })) return false;
    selectNode(made);
    return true;
}

bool GradeNodePanel::addKeyMixer() {
    const Id id = clip();
    const Clip* c = id ? state_->primaryClip() : nullptr;
    const GradeNode* n = c ? findGradeNode(c->grade, selectedNode()) : nullptr;
    if (!n || n->kind != "corrector") {
        state_->message(tr("Pick the corrector whose key input the key mixer feeds"));
        return false;
    }
    // Joining the key it takes already and the key of the node feeding its picture.
    std::vector<int> keys;
    if (const GradeLink* k = gradeLinkInto(c->grade, n->key, 1); k && k->from) keys.push_back(k->from);
    if (const GradeLink* in = gradeLinkInto(c->grade, n->key, 0); in && in->from && std::find(keys.begin(), keys.end(), in->from) == keys.end())
        keys.push_back(in->from);
    if (keys.empty()) {
        state_->message(tr("Nothing gives a key to join yet: link a node's key into this one"));
        return false;
    }
    const int to = n->key;
    int made = 0;
    if (!state_->apply(tr("Add Key Mixer"), [&](Project&, Sequence& s) { return edit::addKeyMixer(s, id, keys, to, &made); })) return false;
    selectNode(made);
    return true;
}

bool GradeNodePanel::removeSelected() {
    const Id id = clip();
    const int key = selectedNode();
    if (!id || !key) return false;
    if (!state_->apply(tr("Remove Node"), [id, key](Project& p, Sequence& s) { return edit::removeGradeNode(p, s, id, key); })) return false;
    selectNode(0);
    return true;
}

bool GradeNodePanel::connectNodes(const GradeLink& link) {
    const Id id = clip();
    return id && state_->apply(tr("Link Nodes"), [id, link](Project&, Sequence& s) { return edit::connectGradeNodes(s, id, link); });
}

bool GradeNodePanel::disconnectInput(int to, int input) {
    const Id id = clip();
    return id && state_->apply(tr("Unlink Nodes"), [id, to, input](Project&, Sequence& s) { return edit::disconnectGradeNodes(s, id, to, input); });
}

namespace {

// The picked node changed by `fn`, one undo step.
bool changeNode(EditorState* state, Id id, int key, const QString& label, const std::function<void(GradeNode&)>& fn) {
    if (!id || !key) return false;
    return state->apply(label, [id, key, fn](Project&, Sequence& s) {
        const Clip* c = edit::clipById(s, id);
        const GradeNode* n = c ? findGradeNode(c->grade, key) : nullptr;
        if (!n) return edit::Result::fail("No such node");
        GradeNode m = *n;
        fn(m);
        return edit::setGradeNode(s, id, m);
    });
}

}  // namespace

bool GradeNodePanel::setSelectedEnabled(bool on) {
    return changeNode(state_, clip(), selectedNode(), on ? tr("Enable Node") : tr("Disable Node"), [on](GradeNode& n) { n.enabled = on; });
}

bool GradeNodePanel::toggleSelected() {
    const Clip* c = clip() ? state_->primaryClip() : nullptr;
    const GradeNode* n = c ? findGradeNode(c->grade, selectedNode()) : nullptr;
    return n && setSelectedEnabled(!n->enabled);
}

bool GradeNodePanel::renameSelected(const QString& label) {
    return changeNode(state_, clip(), selectedNode(), tr("Rename Node"), [label](GradeNode& n) { n.label = label.trimmed().toStdString(); });
}

bool GradeNodePanel::moveNode(int key, double x, double y) {
    return changeNode(state_, clip(), key, tr("Move Node"), [x, y](GradeNode& n) {
        n.x = x;
        n.y = y;
    });
}

bool GradeNodePanel::shareSelected(const QString& name) {
    const Id id = clip();
    const int key = selectedNode();
    if (!id || !key) return false;
    return state_->apply(tr("Share Node"), [&](Project& p, Sequence& s) { return edit::shareGradeNode(p, s, id, key, name.trimmed().toStdString()); });
}

bool GradeNodePanel::useShared(Id shared) {
    const Id id = clip();
    if (!id) return false;
    int made = 0;
    if (!state_->apply(tr("Use Shared Node"), [&](Project& p, Sequence& s) { return edit::useSharedNode(p, s, id, shared, &made); })) return false;
    selectNode(made);
    return true;
}

bool GradeNodePanel::unshareSelected() {
    const Id id = clip();
    const int key = selectedNode();
    if (!id || !key) return false;
    return state_->apply(tr("Unshare Node"), [&](Project& p, Sequence& s) { return edit::unshareGradeNode(p, s, id, key); });
}

bool GradeNodePanel::flatten() {
    const Id id = clip();
    if (!id) return false;
    if (!state_->apply(tr("Flatten Nodes"), [id](Project&, Sequence& s) { return edit::flattenGrade(s, id); })) return false;
    selectNode(0);
    return true;
}

QWidget* GradeNodePanel::canvas() const { return canvas_; }

QPointF GradeNodePanel::nodeCentre(int key) const {
    canvas_->fit();
    return canvas_->toView(canvas_->box(key)).center();
}

QPointF GradeNodePanel::portPoint(int node, bool output, int input, bool key) const {
    canvas_->fit();
    return canvas_->portView(Port{node, output, input, key});
}

}  // namespace montage
