#include "CompositeNodePanel.h"
#include "Language.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
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
#include "core/ProjectIO.h"

namespace montage {

namespace {

constexpr double kNodeW = 120, kNodeH = 54, kEndW = 76, kEndH = 40, kPortR = 6;
const QColor kLink{0xd8, 0xb0, 0x4c};

// A port: a node's output, or one of its inputs (node -1: the graph's output, input 0).
struct Port {
    int node = 0;
    bool output = false;
    int input = 0;
};

QColor tintOf(const std::string& kind) {
    if (kind == "input") return {0x2f, 0x3d, 0x52};
    if (kind == "merge") return {0x3d, 0x35, 0x2a};
    if (kind == "grade") return {0x3a, 0x34, 0x4a};
    if (kind == "key" || kind == "mask") return {0x2c, 0x40, 0x33};
    return theme::kPanelAlt;
}

}  // namespace

// The graph drawn: nodes at their places (graph units, scaled to fit), links as curves.
class CompositeNodeCanvas : public QWidget {
public:
    CompositeNodeCanvas(CompositeNodePanel* panel, EditorState* state) : QWidget(panel), panel_(panel), state_(state) {
        setFocusPolicy(Qt::ClickFocus);
        setMouseTracking(true);
        setMinimumSize(240, 140);
    }

    const Clip* clip() const {
        const Sequence* s = state_->sequence();
        const Id id = panel_->clip();
        return s && id ? edit::clipById(*s, id) : nullptr;
    }

    // How many input ports a node shows (a merge: one free after those fed).
    int portCount(const CompositeNode& n) const {
        if (n.kind != "merge") return compositeInputCount(n);
        int fed = 0;
        for (const CompositeLink& l : clip()->composite.links)
            if (l.to == n.key) fed = std::max(fed, l.input + 1);
        return std::min(kMaxMergeInputs, fed + 1);
    }

    // A node's box in graph units (-1: the output's).
    QRectF box(int key) const {
        const Clip* c = clip();
        if (!c) return {};
        const CompositeGraph& g = c->composite;
        if (key == -1) {
            double maxX = 0, y = 0;
            for (const CompositeNode& n : g.nodes) maxX = std::max(maxX, n.x);
            if (const CompositeLink* l = compositeLinkInto(g, 0, 0))
                if (const CompositeNode* last = findCompositeNode(g, l->from)) y = last->y;
            return {maxX + 160, y + (kNodeH - kEndH) / 2, kEndW, kEndH};
        }
        const CompositeNode* n = findCompositeNode(g, key);
        if (!n) return {};
        return {n->x, n->y, kNodeW, std::max(kNodeH, 16.0 * (portCount(*n) + 1))};
    }

    std::vector<std::pair<Port, QPointF>> ports(int key) const {
        std::vector<std::pair<Port, QPointF>> out;
        const QRectF r = box(key);
        if (key == -1) return {{Port{-1, false, 0}, {r.left(), r.center().y()}}};
        const CompositeNode* n = findCompositeNode(clip()->composite, key);
        if (!n) return out;
        const int ins = portCount(*n);
        for (int i = 0; i < ins; ++i) out.push_back({Port{key, false, i}, {r.left(), r.top() + r.height() * (i + 1) / (ins + 1)}});
        out.push_back({Port{key, true, 0}, {r.right(), r.center().y()}});
        return out;
    }

    QPointF toView(QPointF g) const { return (g - origin_) * scale_ + QPointF(margin_, margin_); }
    QRectF toView(const QRectF& r) const { return {toView(r.topLeft()), toView(r.bottomRight())}; }

    void fit() {
        const Clip* c = clip();
        if (!c || c->composite.empty()) return;
        QRectF all = box(-1);
        for (const CompositeNode& n : c->composite.nodes) all = all.united(box(n.key));
        origin_ = all.topLeft();
        const double sx = (width() - 2 * margin_) / std::max(1.0, all.width()), sy = (height() - 2 * margin_) / std::max(1.0, all.height());
        scale_ = std::clamp(std::min(sx, sy), 0.3, 1.0);
    }

    std::vector<int> keys() const {
        std::vector<int> k;
        if (const Clip* c = clip())
            for (const CompositeNode& n : c->composite.nodes) k.push_back(n.key);
        return k;
    }

    std::optional<Port> portAt(QPointF v) const {
        std::vector<int> all = keys();
        all.push_back(-1);
        for (int k : all)
            for (const auto& [port, at] : ports(k))
                if (QLineF(toView(at), v).length() <= kPortR + 3) return port;
        return std::nullopt;
    }

    int nodeAt(QPointF v) const {
        const std::vector<int> all = keys();
        for (auto it = all.rbegin(); it != all.rend(); ++it)
            if (toView(box(*it)).contains(v)) return *it;
        return 0;
    }

    QPointF portView(const Port& p) const {
        for (const auto& [port, at] : ports(p.node))
            if (port.output == p.output && port.input == p.input) return toView(at);
        return {};
    }

    QPainterPath linkPath(const CompositeLink& l) const {
        const QPointF a = portView(Port{l.from, true, 0});
        const QPointF b = portView(Port{l.to ? l.to : -1, false, l.input});
        QPainterPath path(a);
        const double dx = std::max(30.0, std::abs(b.x() - a.x()) / 2);
        path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
        return path;
    }

    const CompositeLink* linkAt(QPointF v) const {
        const Clip* c = clip();
        if (!c) return nullptr;
        for (const CompositeLink& l : c->composite.links) {
            const QPainterPath path = linkPath(l);
            for (int i = 0; i <= 40; ++i)
                if (QLineF(path.pointAtPercent(i / 40.0), v).length() < 6) return &l;
        }
        return nullptr;
    }

    QString subtitle(const CompositeNode& n) const {
        const Sequence* s = state_->sequence();
        if (n.kind == "input") {
            if (n.source == "clip") return CompositeNodePanel::tr("the clip");
            if (n.source == "track") return CompositeNodePanel::tr("track V%1").arg(n.track + 1);
            if (n.source == "generator") {
                const EffectInfo* info = findEffectInfo(n.generator.type);
                return info ? language::engine(info->displayName) : QString::fromStdString(n.generator.type);
            }
            const MediaItem* m = state_->project().findMedia(n.media);
            return m ? QString::fromStdString(m->name) : CompositeNodePanel::tr("missing media");
        }
        if (n.kind == "merge") return n.blend == "depth" ? CompositeNodePanel::tr("by depth") : QString::fromStdString(n.blend);
        if (n.kind == "track") {
            const Clip* f = s ? edit::clipById(*s, n.follow) : nullptr;
            return f ? CompositeNodePanel::tr("follows %1").arg(QString::fromStdString(f->name)) : CompositeNodePanel::tr("follows nothing yet");
        }
        const int own = int(n.effects.size()) - (compositeSettings(n) ? 1 : 0);
        if (n.kind == "grade" && !n.grade.empty()) return CompositeNodePanel::tr("%n colour node(s)", "", int(n.grade.nodes.size()));
        if (n.kind == "transform" || n.kind == "mask") return own ? CompositeNodePanel::tr("+ %n effect(s)", "", own) : QString();
        return CompositeNodePanel::tr("%n effect(s)", "", own);
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
            p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap,
                       CompositeNodePanel::tr("Select a picture clip to see its compositing nodes."));
            return;
        }
        if (c->composite.empty()) {
            p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap,
                       CompositeNodePanel::tr("This clip's picture comes as it is. Add a node to composite other pictures into it."));
            return;
        }
        fit();
        for (const CompositeLink& l : c->composite.links) {
            p.setPen(QPen(kLink, 2));
            p.setBrush(Qt::NoBrush);
            p.drawPath(linkPath(l));
        }
        if (drag_) {
            p.setPen(QPen(kLink, 2, Qt::DashLine));
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
        drawBox(toView(box(-1)), CompositeNodePanel::tr("Output"), CompositeNodePanel::tr("the clip's picture"), false, true, theme::kPanelAlt);
        const int picked = panel_->selectedNode();
        for (const CompositeNode& n : c->composite.nodes)
            drawBox(toView(box(n.key)), QString::fromStdString(compositeNodeName(n)), subtitle(n), n.key == picked, n.enabled, tintOf(n.kind));
        std::vector<int> all = keys();
        all.push_back(-1);
        for (int k : all)
            for (const auto& [port, at] : ports(k)) {
                p.setPen(QPen(theme::kWindow, 1));
                p.setBrush(kLink);
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
            const CompositeNode* n = findCompositeNode(clip()->composite, key);
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
        if (!moved_) state_->beginGesture(CompositeNodePanel::tr("Move Node"));
        moved_ = true;
        const QPointF d = (v - pressedAt_) / scale_;
        const Id id = panel_->clip();
        const int key = moving_;
        const QPointF to = startPos_ + d;
        state_->updateGesture([id, key, to](Project& p, Sequence& s) {
            const Clip* c = edit::clipById(s, id);
            const CompositeNode* n = c ? findCompositeNode(c->composite, key) : nullptr;
            if (!n) return;
            CompositeNode m = *n;
            m.x = std::round(to.x());
            m.y = std::round(to.y());
            edit::setCompositeNode(p, s, id, m);
        });
    }

    void mouseReleaseEvent(QMouseEvent* e) override {
        if (drag_) {
            const Port from = *drag_;
            drag_.reset();
            if (std::optional<Port> to = portAt(e->position()); to && !to->output)
                panel_->connectNodes({from.node, to->node == -1 ? 0 : to->node, to->input});
            update();
            return;
        }
        if (moving_ && moved_) state_->endGesture(true);
        moving_ = 0;
        moved_ = false;
    }

    void mouseDoubleClickEvent(QMouseEvent* e) override {
        const int key = nodeAt(e->position());
        const CompositeNode* n = key ? findCompositeNode(clip()->composite, key) : nullptr;
        if (!n) return;
        bool ok = false;
        const QString label = QInputDialog::getText(this, CompositeNodePanel::tr("Rename Node"), CompositeNodePanel::tr("Label:"), QLineEdit::Normal,
                                                    QString::fromStdString(n->label), &ok);
        if (ok) panel_->renameSelected(label);
    }

    bool event(QEvent* e) override {
        // Delete and Ctrl+D are the node's while one is picked here, not the timeline's.
        if (e->type() == QEvent::ShortcutOverride && nodeKey(*static_cast<QKeyEvent*>(e))) {
            e->accept();
            return true;
        }
        return QWidget::event(e);
    }

    std::function<void()> nodeKey(const QKeyEvent& k) const {
        const Qt::KeyboardModifiers mods = k.modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        CompositeNodePanel* p = panel_;
        if ((k.key() == Qt::Key_Delete || k.key() == Qt::Key_Backspace) && !mods && p->selectedNode()) return [p] { p->removeSelected(); };
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
        if (!c || c->composite.empty()) return;
        QMenu menu(this);
        if (const CompositeLink* l = linkAt(e->pos()); l && l->to) {
            const int to = l->to, input = l->input;
            menu.addAction(CompositeNodePanel::tr("Disconnect"), this, [this, to, input] { panel_->disconnectInput(to, input); });
            menu.exec(e->globalPos());
            return;
        }
        if (const int key = nodeAt(e->pos())) panel_->selectNode(key);
        const CompositeNode* n = findCompositeNode(c->composite, panel_->selectedNode());
        if (n && n->kind == "merge") {
            QMenu* blend = menu.addMenu(CompositeNodePanel::tr("Blend Mode"));
            std::vector<std::string> modes = blendModes();
            modes.push_back("depth");
            for (const std::string& m : modes) {
                QAction* a = blend->addAction(m == "depth" ? CompositeNodePanel::tr("By Depth") : QString::fromStdString(m));
                a->setCheckable(true);
                a->setChecked(m == n->blend);
                const QString mode = QString::fromStdString(m);
                connect(a, &QAction::triggered, this, [this, mode] { panel_->setSelectedBlend(mode); });
            }
        }
        if (n && n->kind == "track") {
            QMenu* follow = menu.addMenu(CompositeNodePanel::tr("Follow Clip"));
            const Sequence* s = state_->sequence();
            for (size_t ti = 0; s && ti < s->videoTracks.size(); ++ti)
                for (const Clip& x : s->videoTracks[ti].clips) {
                    if (!x.motion.params.empty() || std::any_of(x.effects.begin(), x.effects.end(), [](const Effect& f) { return f.type == "corner_pin"; })) {
                        QAction* a = follow->addAction(QStringLiteral("V%1 · %2").arg(ti + 1).arg(QString::fromStdString(x.name)));
                        a->setCheckable(true);
                        a->setChecked(x.id == n->follow);
                        const Id id = x.id, clipId = panel_->clip();
                        const int key = n->key;
                        connect(a, &QAction::triggered, this, [this, id, clipId, key] {
                            state_->apply(CompositeNodePanel::tr("Follow Clip"), [=](Project& p, Sequence& sq) {
                                const Clip* cc = edit::clipById(sq, clipId);
                                const CompositeNode* m = cc ? findCompositeNode(cc->composite, key) : nullptr;
                                if (!m) return edit::Result::fail("No such node");
                                CompositeNode changed = *m;
                                changed.follow = id;
                                return edit::setCompositeNode(p, sq, clipId, changed);
                            });
                        });
                    }
                }
            if (follow->isEmpty()) follow->addAction(CompositeNodePanel::tr("No clips to follow"))->setEnabled(false);
        }
        if (!menu.isEmpty()) menu.addSeparator();
        for (QAction* a : panel_->actions()) menu.addAction(a);
        menu.exec(e->globalPos());
    }

private:
    CompositeNodePanel* panel_;
    EditorState* state_;
    QPointF origin_;
    double scale_ = 1, margin_ = 24;
    std::optional<Port> drag_;
    QPointF dragAt_, pressedAt_, startPos_;
    int moving_ = 0;
    bool moved_ = false;
    friend class CompositeNodePanel;
};

CompositeNodePanel::CompositeNodePanel(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
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
    input_ = button(tr("Input"), tr("Merge another picture over the picked node: a bin item, another track, a title, a 3D model or a colour"),
                    "compositeAddInput");
    inputMenu_ = new QMenu(input_);
    input_->setMenu(inputMenu_);
    input_->setPopupMode(QToolButton::InstantPopup);
    merge_ = button(tr("Merge"), tr("Add a merge after the picked node: it becomes the background"), "compositeAddMerge");
    transform_ = button(tr("Transform"), tr("Add a transform after the picked node"), "compositeAddTransform");
    mask_ = button(tr("Mask"), tr("Add a mask after the picked node (its second input can be a mask picture)"), "compositeAddMask");
    key_ = button(tr("Key"), tr("Add a key after the picked node"), "compositeAddKey");
    track_ = button(tr("Track"), tr("Add a tracker after the picked node: it moves with another clip's tracked transform or Corner Pin"),
                    "compositeAddTrack");
    effect_ = button(tr("Effect"), tr("Add an effect node after the picked node"), "compositeAddEffect");
    grade_ = button(tr("Grade"), tr("Add a grade after the picked node"), "compositeAddGrade");
    remove_ = button(tr("Remove"), tr("Remove the picked node (Delete)"), "compositeRemove");
    presets_ = button(tr("Presets"), tr("Save the graph as a preset, or apply one"), "compositePresets");
    presetMenu_ = new QMenu(presets_);
    presets_->setMenu(presetMenu_);
    presets_->setPopupMode(QToolButton::InstantPopup);
    h->addStretch(1);
    v->addWidget(bar);
    canvas_ = new CompositeNodeCanvas(this, state_);
    v->addWidget(canvas_, 1);

    connect(merge_, &QToolButton::clicked, this, [this] { addNode("merge"); });
    connect(transform_, &QToolButton::clicked, this, [this] { addNode("transform"); });
    connect(mask_, &QToolButton::clicked, this, [this] { addNode("mask"); });
    connect(key_, &QToolButton::clicked, this, [this] { addNode("key"); });
    connect(track_, &QToolButton::clicked, this, [this] { addNode("track"); });
    connect(effect_, &QToolButton::clicked, this, [this] { addNode("effect"); });
    connect(grade_, &QToolButton::clicked, this, [this] { addNode("grade"); });
    connect(remove_, &QToolButton::clicked, this, &CompositeNodePanel::removeSelected);
    connect(inputMenu_, &QMenu::aboutToShow, this, [this] {
        inputMenu_->clear();
        inputMenu_->addAction(tr("The Clip"), this, [this] { addInput("clip"); });
        QMenu* media = inputMenu_->addMenu(tr("Media"));
        for (const MediaItem& m : state_->project().media)
            if (m.kind != MediaKind::Audio) {
                const Id id = m.id;
                media->addAction(QString::fromStdString(m.name), this, [this, id] { addInput("media", id); });
            }
        if (media->isEmpty()) media->addAction(tr("No pictures in the project"))->setEnabled(false);
        QMenu* tracks = inputMenu_->addMenu(tr("Track"));
        const Sequence* s = state_->sequence();
        const auto loc = s && clip() ? edit::locate(*s, clip()) : std::nullopt;
        for (size_t ti = 0; s && ti < s->videoTracks.size(); ++ti)
            if (!loc || int(ti) != loc->track.index) {
                const int track = int(ti);
                tracks->addAction(tr("V%1").arg(ti + 1), this, [this, track] { addInput("track", 0, track); });
            }
        if (tracks->isEmpty()) tracks->addAction(tr("No other video tracks"))->setEnabled(false);
        QMenu* gens = inputMenu_->addMenu(tr("Generator"));
        for (const char* type : {"title", "title3d", "model3d", "shape", "color", "gradient", "particles"})
            if (const EffectInfo* info = findEffectInfo(type)) {
                const QString t = QString::fromLatin1(type);
                gens->addAction(language::engine(info->displayName), this, [this, t] { addInput("generator", 0, -1, t); });
            }
    });
    connect(presetMenu_, &QMenu::aboutToShow, this, [this] {
        presetMenu_->clear();
        QAction* save = presetMenu_->addAction(tr("Save as Preset…"), this, [this] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Save Compositing Preset"), tr("Preset name:"), QLineEdit::Normal, QString(), &ok);
            if (ok && !name.trimmed().isEmpty()) savePreset(name.trimmed());
        });
        const Clip* c = clip() ? state_->primaryClip() : nullptr;
        save->setEnabled(c && !c->composite.empty());
        presetMenu_->addSeparator();
        const QDir dir(presetsFolder());
        for (const QFileInfo& f : dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            const QString path = f.absoluteFilePath();
            presetMenu_->addAction(f.completeBaseName(), this, [this, path] { applyPreset(path); })->setEnabled(c);
        }
        presetMenu_->addSeparator();
        presetMenu_->addAction(tr("Remove the Graph"), this, &CompositeNodePanel::clearGraph)->setEnabled(c && !c->composite.empty());
    });

    auto action = [this](const QString& text, const QKeySequence& keys, const std::function<void()>& fn) {
        auto* a = new QAction(keys.isEmpty() ? text : text + '\t' + keys.toString(QKeySequence::NativeText), this);
        connect(a, &QAction::triggered, this, fn);
        addAction(a);
        return a;
    };
    action(tr("Enable / Disable Node"), QKeySequence(Qt::CTRL | Qt::Key_D), [this] { toggleSelected(); });
    action(tr("Remove Node"), {}, [this] { removeSelected(); });

    for (auto sig : {&EditorState::projectChanged, &EditorState::selectionChanged, &EditorState::sequenceSwitched, &EditorState::compositeNodeSelected})
        connect(state_, sig, this, &CompositeNodePanel::sync);
    sync();
}

Id CompositeNodePanel::clip() const {
    const Clip* c = state_->primaryClip();
    const Sequence* s = state_->sequence();
    if (!c || !s) return 0;
    const auto loc = edit::locate(*s, c->id);
    return loc && loc->track.kind == TrackKind::Video ? c->id : 0;
}

int CompositeNodePanel::selectedNode() const { return clip() ? state_->selectedCompositeNode() : 0; }

void CompositeNodePanel::selectNode(int key) { state_->selectCompositeNode(key); }

void CompositeNodePanel::sync() {
    const Clip* c = clip() ? state_->primaryClip() : nullptr;
    const CompositeNode* n = c ? findCompositeNode(c->composite, selectedNode()) : nullptr;
    for (QToolButton* b : {input_, merge_, transform_, mask_, key_, track_, effect_, grade_}) b->setEnabled(c);
    remove_->setEnabled(n);
    presets_->setEnabled(c);
    canvas_->fit();
    canvas_->update();
}

bool CompositeNodePanel::addNode(const QString& kind) {
    const Id id = clip();
    if (!id) return false;
    const int after = selectedNode();
    int made = 0;
    const std::string k = kind.toStdString();
    if (!state_->apply(tr("Add %1 Node").arg(kind.left(1).toUpper() + kind.mid(1)),
                       [&](Project& p, Sequence& s) { return edit::addCompositeNode(p, s, id, k, after, &made); }))
        return false;
    selectNode(made);
    return true;
}

bool CompositeNodePanel::addInput(const QString& source, Id media, int track, const QString& generator) {
    const Id id = clip();
    if (!id) return false;
    const int picked = selectedNode();
    int made = 0;
    const bool done = state_->apply(tr("Add Input"), [&](Project& p, Sequence& s) {
        const Clip* c = edit::clipById(s, id);
        int after = picked;
        const CompositeNode* at = c ? findCompositeNode(c->composite, after) : nullptr;
        if (!at || at->kind != "merge") {
            int merge = 0;
            if (edit::Result r = edit::addCompositeNode(p, s, id, "merge", after, &merge); !r.ok) return r;
            after = merge;
        }
        if (edit::Result r = edit::addCompositeNode(p, s, id, "input", after, &made); !r.ok) return r;
        const CompositeNode* n = findCompositeNode(edit::clipById(s, id)->composite, made);
        CompositeNode changed = *n;
        changed.source = source.toStdString();
        changed.media = media;
        changed.track = track;
        if (source == "generator") changed.generator = makeEffect(generator.toStdString(), 0);
        if (source == "clip") return edit::Result{};
        return edit::setCompositeNode(p, s, id, changed);
    });
    if (done) selectNode(made);
    return done;
}

bool CompositeNodePanel::removeSelected() {
    const Id id = clip();
    const int key = selectedNode();
    if (!id || !key) return false;
    if (!state_->apply(tr("Remove Node"), [id, key](Project&, Sequence& s) { return edit::removeCompositeNode(s, id, key); })) return false;
    selectNode(0);
    return true;
}

bool CompositeNodePanel::connectNodes(const CompositeLink& link) {
    const Id id = clip();
    return id && state_->apply(tr("Link Nodes"), [id, link](Project&, Sequence& s) { return edit::connectCompositeNodes(s, id, link); });
}

bool CompositeNodePanel::disconnectInput(int to, int input) {
    const Id id = clip();
    return id && state_->apply(tr("Unlink Nodes"), [id, to, input](Project&, Sequence& s) { return edit::disconnectCompositeNodes(s, id, to, input); });
}

namespace {

// The node changed by `fn`, one undo step.
bool changeNode(EditorState* state, Id id, int key, const QString& label, const std::function<void(CompositeNode&)>& fn) {
    if (!id || !key) return false;
    return state->apply(label, [id, key, fn](Project& p, Sequence& s) {
        const Clip* c = edit::clipById(s, id);
        const CompositeNode* n = c ? findCompositeNode(c->composite, key) : nullptr;
        if (!n) return edit::Result::fail("No such node");
        CompositeNode m = *n;
        fn(m);
        return edit::setCompositeNode(p, s, id, m);
    });
}

}  // namespace

bool CompositeNodePanel::setSelectedEnabled(bool on) {
    return changeNode(state_, clip(), selectedNode(), on ? tr("Enable Node") : tr("Disable Node"), [on](CompositeNode& n) { n.enabled = on; });
}

bool CompositeNodePanel::toggleSelected() {
    const Clip* c = clip() ? state_->primaryClip() : nullptr;
    const CompositeNode* n = c ? findCompositeNode(c->composite, selectedNode()) : nullptr;
    return n && setSelectedEnabled(!n->enabled);
}

bool CompositeNodePanel::renameSelected(const QString& label) {
    return changeNode(state_, clip(), selectedNode(), tr("Rename Node"), [label](CompositeNode& n) { n.label = label.trimmed().toStdString(); });
}

bool CompositeNodePanel::moveNode(int key, double x, double y) {
    return changeNode(state_, clip(), key, tr("Move Node"), [x, y](CompositeNode& n) {
        n.x = x;
        n.y = y;
    });
}

bool CompositeNodePanel::setSelectedBlend(const QString& mode) {
    return changeNode(state_, clip(), selectedNode(), tr("Blend Mode"), [mode](CompositeNode& n) { n.blend = mode.toStdString(); });
}

bool CompositeNodePanel::clearGraph() {
    const Id id = clip();
    if (!id) return false;
    if (!state_->apply(tr("Remove Compositing Graph"), [id](Project&, Sequence& s) { return edit::removeComposite(s, id); })) return false;
    selectNode(0);
    return true;
}

QString CompositeNodePanel::presetsFolder() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/Compositing Presets");
}

bool CompositeNodePanel::savePreset(const QString& name, const QString& path) {
    const Clip* c = clip() ? state_->primaryClip() : nullptr;
    if (!c || c->composite.empty()) return false;
    QString file = path;
    if (file.isEmpty()) {
        QString safe = name;
        safe.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
        QDir().mkpath(presetsFolder());
        file = presetsFolder() + '/' + safe + QStringLiteral(".json");
    }
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly)) {
        state_->message(tr("Could not save the preset: %1").arg(f.errorString()));
        return false;
    }
    f.write(QByteArray::fromStdString(compositeGraphToJsonString(c->composite)));
    if (!f.commit()) {
        state_->message(tr("Could not save the preset: %1").arg(f.errorString()));
        return false;
    }
    state_->message(tr("Saved compositing preset \"%1\"").arg(name));
    return true;
}

bool CompositeNodePanel::applyPreset(const QString& path) {
    const Id id = clip();
    if (!id) return false;
    QFile f(path);
    CompositeGraph g;
    if (!f.open(QIODevice::ReadOnly) || !compositeGraphFromJsonString(f.readAll().toStdString(), g)) {
        state_->message(tr("Not a compositing preset: %1").arg(QFileInfo(path).fileName()));
        return false;
    }
    if (!state_->apply(tr("Apply Compositing Preset"), [&](Project& p, Sequence& s) { return edit::applyComposite(p, s, id, g); })) return false;
    selectNode(0);
    return true;
}

QWidget* CompositeNodePanel::canvas() const { return canvas_; }

QPointF CompositeNodePanel::nodeCentre(int key) const {
    canvas_->fit();
    return canvas_->toView(canvas_->box(key)).center();
}

QPointF CompositeNodePanel::portPoint(int node, bool output, int input) const {
    canvas_->fit();
    return canvas_->portView(Port{node, output, input});
}

}  // namespace montage
