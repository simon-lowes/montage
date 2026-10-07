#include "InspectorWidget.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <utility>

#include "EditorState.h"
#include "Theme.h"
#include "audio/PluginEffect.h"

namespace montage {

namespace {

Effect* findEffect(Clip& c, Id id) {
    if (c.motion.id == id) return &c.motion;
    if (c.audio.id == id) return &c.audio;
    if (c.generator.id == id) return &c.generator;
    for (auto& e : c.effects)
        if (e.id == id) return &e;
    return nullptr;
}

QToolButton* smallButton(QWidget* parent, const QString& text, const QString& tip) {
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setFixedSize(20, 20);
    return b;
}

QIcon colorIcon(const QColor& c) {
    QPixmap pm(28, 14);
    pm.fill(c);
    QPainter p(&pm);
    p.setPen(QColor(255, 255, 255, 120));
    p.drawRect(pm.rect().adjusted(0, 0, -1, -1));
    return QIcon(pm);
}

}  // namespace

InspectorWidget::InspectorWidget(EditorState* state, QWidget* parent) : QScrollArea(parent), state_(state) {
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(state_, &EditorState::selectionChanged, this, &InspectorWidget::rebuild);
    connect(state_, &EditorState::sequenceSwitched, this, &InspectorWidget::rebuild);
    connect(state_, &EditorState::projectChanged, this, [this] {
        if (signature() != signature_) rebuild();
        else refreshValues();
    });
    connect(state_, &EditorState::playheadChanged, this, &InspectorWidget::refreshValues);
    rebuild();
}

QString InspectorWidget::signature() const {
    const Sequence* s = state_->sequence();
    if (!s) return {};
    if (Id tid = state_->selectedTransition()) {
        Transition* t = edit::transitionById(const_cast<Sequence&>(*s), tid);
        return t ? QString("T%1:%2").arg(tid).arg(QString::fromStdString(t->type)) : QString();
    }
    const Clip* c = state_->primaryClip();
    if (!c) return {};
    QString sig = QString("C%1:%2").arg(c->id).arg(QString::fromStdString(c->generator.type));
    for (const auto& e : c->effects) sig += QString(":%1%2").arg(e.id).arg(e.enabled ? "+" : "-");
    return sig;
}

QFormLayout* InspectorWidget::addSection(const QString& title, QWidget* headerExtra, bool startCollapsed) {
    auto* box = new QWidget(content_);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto* header = new QWidget(box);
    header->setStyleSheet(QString("background: %1;").arg(theme::kPanelAlt.name()));
    auto* h = new QHBoxLayout(header);
    h->setContentsMargins(4, 2, 4, 2);
    auto* toggle = new QToolButton(header);
    toggle->setArrowType(startCollapsed ? Qt::RightArrow : Qt::DownArrow);
    toggle->setAutoRaise(true);
    toggle->setFixedSize(16, 16);
    auto* label = new QLabel(QString("<b>%1</b>").arg(title.toHtmlEscaped()), header);
    h->addWidget(toggle);
    h->addWidget(label, 1);
    if (headerExtra) {
        headerExtra->setParent(header);
        h->addWidget(headerExtra);
    }
    v->addWidget(header);
    auto* body = new QWidget(box);
    auto* form = new QFormLayout(body);
    form->setContentsMargins(10, 4, 6, 8);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setVerticalSpacing(3);
    body->setVisible(!startCollapsed);
    v->addWidget(body);
    connect(toggle, &QToolButton::clicked, body, [toggle, body] {
        body->setVisible(!body->isVisible());
        toggle->setArrowType(body->isVisible() ? Qt::DownArrow : Qt::RightArrow);
    });
    layout_->addWidget(box);
    return form;
}

void InspectorWidget::rebuild() {
    signature_ = signature();
    refreshers_.clear();
    content_ = new QWidget;
    layout_ = new QVBoxLayout(content_);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(4);
    const Sequence* s = state_->sequence();
    if (s && state_->selectedTransition()) {
        TrackRef where;
        if (Transition* t = edit::transitionById(const_cast<Sequence&>(*s), state_->selectedTransition(), &where))
            buildTransition(*t, where.kind);
    } else if (const Clip* c = state_->primaryClip()) {
        auto loc = edit::locate(*s, c->id);
        buildClip(*c, loc ? loc->track.kind : TrackKind::Video);
    } else {
        auto* l = new QLabel(tr("Select a clip or transition in the timeline to edit its properties and effects."), content_);
        l->setWordWrap(true);
        l->setAlignment(Qt::AlignCenter);
        l->setStyleSheet(QString("color: %1; padding: 24px;").arg(theme::kTextDim.name()));
        layout_->addWidget(l);
    }
    layout_->addStretch();
    setWidget(content_);
    refreshValues();
}

void InspectorWidget::refreshValues() {
    for (auto& f : refreshers_) f();
}

// ---------------------------------------------------------------------------

void InspectorWidget::buildClip(const Clip& clip, TrackKind kind) {
    const Id clipId = clip.id;
    auto clipNow = [this, clipId]() -> const Clip* {
        const Sequence* s = state_->sequence();
        return s ? edit::clipById(*s, clipId) : nullptr;
    };
    auto localTime = [this, clipNow]() -> FrameTime {
        const Clip* c = clipNow();
        if (!c) return 0;
        return std::clamp<FrameTime>(state_->playhead() - c->start, 0, std::max<FrameTime>(0, c->duration - 1));
    };
    auto originFn = [clipNow]() -> FrameTime {
        const Clip* c = clipNow();
        return c ? c->start : 0;
    };
    auto target = [&](Id effectId) {
        Target t;
        t.resolve = [clipId, effectId](Sequence& s) -> Effect* {
            Clip* c = edit::clipById(s, clipId);
            return c ? findEffect(*c, effectId) : nullptr;
        };
        t.time = localTime;
        t.origin = originFn;
        t.key = QString("p%1:%2").arg(clipId).arg(effectId);
        return t;
    };

    // ---- Clip ---------------------------------------------------------------
    QFormLayout* form = addSection(clip.isGenerator() ? tr("Clip (Generated)") : tr("Clip"));
    auto* name = new QLineEdit(QString::fromStdString(clip.name), content_);
    form->addRow(tr("Name"), name);
    connect(name, &QLineEdit::textEdited, this, [this, clipId](const QString& text) {
        state_->edit(tr("Rename Clip"), [clipId, text](Project&, Sequence& s) {
            Clip* c = edit::clipById(s, clipId);
            if (!c) return false;
            c->name = text.toStdString();
            return true;
        }, QString("name%1").arg(clipId));
    });
    auto* timing = new QLabel(content_);
    timing->setFont(theme::monoFont(9));
    timing->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
    form->addRow(tr("Timing"), timing);
    auto* speed = new QDoubleSpinBox(content_);
    speed->setRange(1, 10000);
    speed->setDecimals(1);
    speed->setSuffix(" %");
    speed->setKeyboardTracking(false);
    auto* reverse = new QCheckBox(tr("Reverse"), content_);
    auto* rippleSpeed = new QCheckBox(tr("Ripple"), content_);
    rippleSpeed->setChecked(true);
    rippleSpeed->setToolTip(tr("Shift following clips when the duration changes"));
    auto* speedRow = new QWidget(content_);
    auto* sh = new QHBoxLayout(speedRow);
    sh->setContentsMargins(0, 0, 0, 0);
    sh->addWidget(speed, 1);
    sh->addWidget(reverse);
    sh->addWidget(rippleSpeed);
    form->addRow(tr("Speed"), speedRow);
    auto applySpeed = [this, clipId, speed, reverse, rippleSpeed] {
        double sp = speed->value() / 100.0;
        bool rev = reverse->isChecked(), rip = rippleSpeed->isChecked();
        state_->apply(tr("Speed / Duration"), [=](Project& p, Sequence& s) { return edit::setSpeed(p, s, clipId, sp, rip, rev); });
    };
    connect(speed, &QDoubleSpinBox::valueChanged, this, applySpeed);
    connect(reverse, &QCheckBox::toggled, this, applySpeed);
    auto* enabled = new QCheckBox(tr("Enabled"), content_);
    form->addRow(QString(), enabled);
    connect(enabled, &QCheckBox::toggled, this, [this, clipId](bool on) {
        state_->edit(on ? tr("Enable Clip") : tr("Disable Clip"), [clipId, on](Project&, Sequence& s) {
            Clip* c = edit::clipById(s, clipId);
            if (!c) return false;
            c->enabled = on;
            return true;
        });
    });
    auto* labelBox = new QComboBox(content_);
    for (int i = 0; i < theme::labelCount(); ++i) {
        QColor c = theme::labelColor(i);
        labelBox->addItem(c.isValid() ? colorIcon(c) : QIcon(), tr(theme::labelName(i)));
    }
    form->addRow(tr("Label"), labelBox);
    connect(labelBox, &QComboBox::activated, this, [this, clipId](int idx) {
        state_->edit(tr("Clip Label"), [clipId, idx](Project&, Sequence& s) {
            for (Id id : edit::linkedClips(s, clipId))
                if (Clip* c = edit::clipById(s, id)) c->colorLabel = idx;
            return true;
        });
    });
    QComboBox* blend = nullptr;
    if (kind == TrackKind::Video) {
        blend = new QComboBox(content_);
        for (const auto& m : blendModes()) {
            QString label = QString::fromStdString(m);
            label.replace('_', ' ');
            label[0] = label[0].toUpper();
            blend->addItem(label, QString::fromStdString(m));
        }
        form->addRow(tr("Blend Mode"), blend);
        connect(blend, &QComboBox::activated, this, [this, clipId, blend](int idx) {
            std::string mode = blend->itemData(idx).toString().toStdString();
            state_->edit(tr("Blend Mode"), [clipId, mode](Project&, Sequence& s) {
                Clip* c = edit::clipById(s, clipId);
                if (!c) return false;
                c->blendMode = mode;
                return true;
            });
        });
    }
    refreshers_.push_back([=, this] {
        const Clip* c = clipNow();
        if (!c) return;
        const Sequence* s = state_->sequence();
        if (!name->hasFocus()) name->setText(QString::fromStdString(c->name));
        timing->setText(tr("%1 → %2  (%3)").arg(timecodeString(s, c->start), timecodeString(s, c->end()), timecodeString(s, c->duration)));
        QSignalBlocker b1(speed), b2(reverse), b3(enabled), b4(labelBox);
        speed->setValue(c->speed * 100);
        reverse->setChecked(c->reverse);
        enabled->setChecked(c->enabled);
        labelBox->setCurrentIndex(std::clamp(c->colorLabel, 0, theme::labelCount() - 1));
        if (blend) {
            QSignalBlocker b5(blend);
            blend->setCurrentIndex(std::max(0, blend->findData(QString::fromStdString(c->blendMode))));
        }
    });

    // ---- Generator ---------------------------------------------------------
    if (clip.isGenerator()) {
        if (const EffectInfo* info = findEffectInfo(clip.generator.type)) {
            QFormLayout* g = addSection(QString::fromStdString(info->displayName));
            addParamRows(g, *info, target(clip.generator.id));
        }
    }
    // ---- Fixed attributes ----------------------------------------------------
    if (kind == TrackKind::Video) {
        if (const EffectInfo* info = findEffectInfo("transform")) {
            auto* reset = smallButton(nullptr, QStringLiteral("↺"), tr("Reset Transform"));
            QFormLayout* t = addSection(tr("Transform"), reset);
            addParamRows(t, *info, target(clip.motion.id));
            Id mid = clip.motion.id;
            connect(reset, &QToolButton::clicked, this, [this, clipId, mid] {
                state_->edit(tr("Reset Transform"), [clipId, mid](Project&, Sequence& s) {
                    Clip* c = edit::clipById(s, clipId);
                    if (!c) return false;
                    c->motion = makeEffect("transform", mid);
                    return true;
                });
            });
        }
    } else if (const EffectInfo* info = findEffectInfo("volume")) {
        QFormLayout* a = addSection(tr("Volume"));
        addParamRows(a, *info, target(clip.audio.id));
    }
    // ---- Effect stack ------------------------------------------------------------
    for (size_t i = 0; i < clip.effects.size(); ++i) {
        const Effect& e = clip.effects[i];
        const EffectInfo* catalog = findEffectInfo(e.type);
        if (!catalog) continue;
        // Plugin effects carry their own parameter list and name.
        EffectInfo shown = *catalog;
        shown.displayName = plugins::effectName(e);
        shown.params = effectParams(e);
        const EffectInfo* info = &shown;
        Id eid = e.id;
        auto* tools = new QWidget;
        auto* th = new QHBoxLayout(tools);
        th->setContentsMargins(0, 0, 0, 0);
        th->setSpacing(0);
        auto* on = new QCheckBox(tools);
        on->setChecked(e.enabled);
        on->setToolTip(tr("Enable / bypass effect"));
        auto* up = smallButton(tools, QStringLiteral("▲"), tr("Move Up"));
        auto* down = smallButton(tools, QStringLiteral("▼"), tr("Move Down"));
        auto* reset = smallButton(tools, QStringLiteral("↺"), tr("Reset"));
        auto* del = smallButton(tools, QStringLiteral("✕"), tr("Remove Effect"));
        for (QWidget* w : {static_cast<QWidget*>(on), static_cast<QWidget*>(up), static_cast<QWidget*>(down),
                           static_cast<QWidget*>(reset), static_cast<QWidget*>(del)})
            th->addWidget(w);
        QFormLayout* f = addSection(QString::fromStdString(info->displayName), tools);
        addParamRows(f, *info, target(eid));
        if (kind == TrackKind::Video && supportsMask(e.type)) {
            // Shape masks and the HSL qualifier; folded away until one is used.
            QFormLayout* mf = addSection(tr("%1 Mask").arg(QString::fromStdString(info->displayName)), nullptr,
                                         !hasMask(e, state_->playhead() - clip.start));
            addParamRows(mf, maskInfo(), target(eid));
        }
        auto mutateStack = [this, clipId, eid](const QString& label, std::function<void(std::vector<Effect>&, size_t, Project&)> fn) {
            state_->edit(label, [=](Project& p, Sequence& s) {
                Clip* c = edit::clipById(s, clipId);
                if (!c) return false;
                for (size_t k = 0; k < c->effects.size(); ++k)
                    if (c->effects[k].id == eid) {
                        fn(c->effects, k, p);
                        return true;
                    }
                return false;
            });
        };
        connect(on, &QCheckBox::toggled, this, [=](bool en) {
            mutateStack(en ? tr("Enable Effect") : tr("Bypass Effect"), [en](std::vector<Effect>& v, size_t k, Project&) { v[k].enabled = en; });
        });
        connect(up, &QToolButton::clicked, this, [=] {
            mutateStack(tr("Reorder Effects"), [](std::vector<Effect>& v, size_t k, Project&) {
                if (k > 0) std::swap(v[k], v[k - 1]);
            });
        });
        connect(down, &QToolButton::clicked, this, [=] {
            mutateStack(tr("Reorder Effects"), [](std::vector<Effect>& v, size_t k, Project&) {
                if (k + 1 < v.size()) std::swap(v[k], v[k + 1]);
            });
        });
        connect(reset, &QToolButton::clicked, this, [=] {
            mutateStack(tr("Reset Effect"), [](std::vector<Effect>& v, size_t k, Project&) {
                if (v[k].type == "plugin") {
                    // Keep the plugin; put its parameters back to their defaults.
                    for (const ParamInfo& pi : effectParams(v[k])) v[k].params[pi.name] = Param(pi.def);
                } else {
                    v[k] = makeEffect(v[k].type, v[k].id);
                }
            });
        });
        connect(del, &QToolButton::clicked, this, [=] {
            mutateStack(tr("Remove Effect"), [](std::vector<Effect>& v, size_t k, Project&) { v.erase(v.begin() + long(k)); });
        });
    }
    addEffectMenu(kind, clipId);
}

void InspectorWidget::addEffectMenu(TrackKind kind, Id clipId) {
    auto* btn = new QPushButton(kind == TrackKind::Video ? tr("Add Video Effect…") : tr("Add Audio Effect…"), content_);
    auto* menu = new QMenu(btn);
    std::map<std::string, QMenu*> groups;
    for (const EffectInfo* info : effectsInCategory(kind == TrackKind::Video ? EffectCategory::VideoFilter : EffectCategory::AudioFilter)) {
        QMenu*& g = groups[info->group];
        if (!g) g = menu->addMenu(QString::fromStdString(info->group));
        std::string type = info->type;
        QString label = QString::fromStdString(info->displayName);
        g->addAction(label, this, [this, clipId, type, label] {
            state_->edit(tr("Add %1").arg(label), [clipId, type](Project& p, Sequence& s) {
                Clip* c = edit::clipById(s, clipId);
                if (!c) return false;
                c->effects.push_back(makeEffect(p, type));
                return true;
            });
        });
    }
    if (kind == TrackKind::Audio) {
        // Installed plugins this build can run, grouped by vendor.
        std::map<QString, QMenu*> vendors;
        QMenu* pluginMenu = nullptr;
        for (const plugins::Descriptor& d : plugins::Registry::instance().plugins()) {
            if (!plugins::canHost(d.format) || d.instrument || plugins::Registry::instance().isPluginDisabled(d.id)) continue;
            if (!pluginMenu) pluginMenu = menu->addMenu(tr("Plugins"));
            const QString vendor = d.vendor.empty() ? tr("Other") : QString::fromStdString(d.vendor);
            QMenu*& vm = vendors[vendor];
            if (!vm) vm = pluginMenu->addMenu(vendor);
            const std::string type = plugins::pluginType(d);
            const QString label = QString::fromStdString(d.name);
            vm->addAction(label, this, [this, clipId, type, label] {
                QString error;
                state_->edit(tr("Add %1").arg(label), [clipId, type, &error](Project& p, Sequence& s) {
                    Clip* c = edit::clipById(s, clipId);
                    if (!c) return false;
                    std::string err;
                    auto e = plugins::makeEffectOfType(p, type, &err);
                    if (!e) {
                        error = QString::fromStdString(err);
                        return false;
                    }
                    c->effects.push_back(*e);
                    return true;
                });
                if (!error.isEmpty()) state_->message(tr("Could not load %1: %2").arg(label, error));
            });
        }
    }
    btn->setMenu(menu);
    layout_->addWidget(btn);
}

void InspectorWidget::buildTransition(const Transition& tr0, TrackKind kind) {
    const Id tid = tr0.id;
    QFormLayout* form = addSection(tr("Transition"));
    auto* type = new QComboBox(content_);
    for (const EffectInfo* info : effectsInCategory(kind == TrackKind::Video ? EffectCategory::VideoTransition : EffectCategory::AudioTransition))
        type->addItem(QString::fromStdString(info->displayName), QString::fromStdString(info->type));
    form->addRow(tr("Type"), type);
    auto* dur = new QSpinBox(content_);
    dur->setRange(1, 100000);
    dur->setSuffix(tr(" frames"));
    dur->setKeyboardTracking(false);
    form->addRow(tr("Duration"), dur);
    auto* align = new QLabel(content_);
    align->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
    form->addRow(tr("Alignment"), align);
    connect(type, &QComboBox::activated, this, [this, tid, type](int idx) {
        std::string ty = type->itemData(idx).toString().toStdString();
        state_->edit(tr("Change Transition"), [tid, ty](Project& p, Sequence& s) {
            Transition* t = edit::transitionById(s, tid);
            if (!t) return false;
            t->type = ty;
            t->params = makeEffect(p, ty);
            return true;
        });
    });
    connect(dur, &QSpinBox::valueChanged, this, [this, tid](int frames) {
        state_->edit(tr("Transition Duration"), [tid, frames](Project&, Sequence& s) {
            Transition* t = edit::transitionById(s, tid);
            if (!t) return false;
            t->duration = frames;
            for (auto* list : {&s.videoTracks, &s.audioTracks})
                for (auto& tr : *list) edit::normalize(tr);
            return true;
        }, QString("trdur%1").arg(tid));
    });
    refreshers_.push_back([=, this] {
        const Sequence* s = state_->sequence();
        if (!s) return;
        Transition* t = edit::transitionById(const_cast<Sequence&>(*s), tid);
        if (!t) return;
        QSignalBlocker b1(type), b2(dur);
        type->setCurrentIndex(std::max(0, type->findData(QString::fromStdString(t->type))));
        dur->setValue(int(t->duration));
        align->setText(t->clipA && t->clipB ? tr("Centre at cut") : (t->clipA ? tr("End of clip (fade out)") : tr("Start of clip (fade in)")));
    });
    if (const EffectInfo* info = findEffectInfo(tr0.type); info && !info->params.empty()) {
        Target t;
        t.resolve = [tid](Sequence& s) -> Effect* {
            Transition* tr = edit::transitionById(s, tid);
            return tr ? &tr->params : nullptr;
        };
        t.time = [] { return FrameTime(0); };
        t.origin = [] { return FrameTime(0); };
        t.key = QString("tr%1").arg(tid);
        t.keyframes = false;
        addParamRows(addSection(tr("Parameters")), *info, t);
    }
    auto* del = new QPushButton(tr("Delete Transition"), content_);
    connect(del, &QPushButton::clicked, this, [this, tid] {
        state_->apply(tr("Delete Transition"), [tid](Project&, Sequence& s) { return edit::removeTransition(s, tid); });
    });
    layout_->addWidget(del);
}

// ---------------------------------------------------------------------------
// Parameter rows

void InspectorWidget::addParamRows(QFormLayout* form, const EffectInfo& info, const Target& target) {
    for (const auto& pi : info.params) addParamRow(form, pi, target);
    for (const auto& si : info.strings) addStringRow(form, si, target);
}

void InspectorWidget::addParamRow(QFormLayout* form, const ParamInfo& pi, const Target& target) {
    auto* row = new QWidget(content_);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(3);
    const std::string name = pi.name;
    const QString label = tr(pi.label.c_str());
    const QString mergeKey = target.key + ":" + QString::fromStdString(name);
    std::vector<std::string> names = {name};
    if (pi.kind == ParamKind::Color) names = {name + ".r", name + ".g", name + ".b"};

    // Reads the current value(s) from the live project.
    auto read = [this, target, pi](const std::string& n) -> double {
        const Sequence* s = state_->sequence();
        if (!s) return 0;
        Effect* e = target.resolve(const_cast<Sequence&>(*s));
        // A parameter not stored yet (e.g. an unused mask) shows its default.
        return e ? e->p(n, target.time(), n == pi.name ? pi.def : 0) : 0;
    };
    auto readParam = [this, target](const std::string& n) -> const Param* {
        const Sequence* s = state_->sequence();
        if (!s) return nullptr;
        Effect* e = target.resolve(const_cast<Sequence&>(*s));
        if (!e) return nullptr;
        auto it = e->params.find(n);
        return it == e->params.end() ? nullptr : &it->second;
    };
    auto write = [this, target, label, mergeKey](std::vector<std::pair<std::string, double>> values) {
        FrameTime t = target.time();
        state_->edit(tr("Change %1").arg(label), [target, values, t](Project&, Sequence& s) {
            Effect* e = target.resolve(s);
            if (!e) return false;
            for (const auto& [n, v] : values) e->params[n].set(t, v);
            return true;
        }, mergeKey);
    };

    std::function<void()> refreshWidget;
    switch (pi.kind) {
        case ParamKind::Bool: {
            auto* cb = new QCheckBox(row);
            h->addWidget(cb, 1);
            connect(cb, &QCheckBox::toggled, this, [write, name](bool on) { write({{name, on ? 1.0 : 0.0}}); });
            refreshWidget = [cb, read, name] {
                QSignalBlocker b(cb);
                cb->setChecked(read(name) > 0.5);
            };
            break;
        }
        case ParamKind::Choice: {
            auto* combo = new QComboBox(row);
            for (const auto& c : pi.choices) combo->addItem(tr(c.c_str()));
            h->addWidget(combo, 1);
            connect(combo, &QComboBox::activated, this, [write, name](int idx) { write({{name, double(idx)}}); });
            refreshWidget = [combo, read, name] {
                QSignalBlocker b(combo);
                combo->setCurrentIndex(int(std::lround(read(name))));
            };
            break;
        }
        case ParamKind::Color: {
            auto* btn = new QToolButton(row);
            btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            btn->setIconSize(QSize(28, 14));
            h->addWidget(btn, 1);
            connect(btn, &QToolButton::clicked, this, [this, read, write, names, label] {
                QColor start = QColor::fromRgbF(float(std::clamp(read(names[0]), 0.0, 1.0)), float(std::clamp(read(names[1]), 0.0, 1.0)),
                                                float(std::clamp(read(names[2]), 0.0, 1.0)));
                QColorDialog dlg(start, this);
                dlg.setWindowTitle(label);
                dlg.setOption(QColorDialog::DontUseNativeDialog);
                connect(&dlg, &QColorDialog::currentColorChanged, this, [write, names](const QColor& c) {
                    write({{names[0], c.redF()}, {names[1], c.greenF()}, {names[2], c.blueF()}});
                });
                if (dlg.exec() != QDialog::Accepted) {
                    write({{names[0], start.redF()}, {names[1], start.greenF()}, {names[2], start.blueF()}});
                }
            });
            refreshWidget = [btn, read, names] {
                QColor c = QColor::fromRgbF(float(std::clamp(read(names[0]), 0.0, 1.0)), float(std::clamp(read(names[1]), 0.0, 1.0)),
                                            float(std::clamp(read(names[2]), 0.0, 1.0)));
                btn->setIcon(colorIcon(c));
                btn->setText(c.name().toUpper());
            };
            break;
        }
        default: {
            auto* spin = new QDoubleSpinBox(row);
            int decimals = pi.step >= 1 ? 0 : (pi.step >= 0.1 ? 1 : (pi.step >= 0.01 ? 2 : 3));
            spin->setDecimals(decimals);
            spin->setRange(pi.min, pi.max);
            spin->setSingleStep(pi.step);
            spin->setKeyboardTracking(false);
            spin->setAccelerated(true);
            if (pi.kind == ParamKind::Percent) spin->setSuffix(" %");
            if (pi.kind == ParamKind::Angle) spin->setSuffix(" °");
            spin->setMinimumWidth(78);
            QSlider* slider = nullptr;
            if (pi.max - pi.min <= 1000 && pi.kind != ParamKind::Angle) {
                slider = new QSlider(Qt::Horizontal, row);
                slider->setRange(0, 1000);
                h->addWidget(slider, 1);
                connect(slider, &QSlider::valueChanged, this, [spin, pi](int v) {
                    spin->setValue(pi.min + (pi.max - pi.min) * v / 1000.0);
                });
                connect(slider, &QSlider::sliderReleased, this, [this] { state_->notifyChanged(); });
            }
            h->addWidget(spin, slider ? 0 : 1);
            connect(spin, &QDoubleSpinBox::valueChanged, this, [write, name](double v) { write({{name, v}}); });
            // Double-click the label area resets to default: provided via context menu below.
            spin->setContextMenuPolicy(Qt::CustomContextMenu);
            double def = pi.def;
            connect(spin, &QWidget::customContextMenuRequested, this, [spin, write, name, def, this](const QPoint& p) {
                QMenu m;
                m.addAction(tr("Reset to Default"), this, [write, name, def] { write({{name, def}}); });
                m.exec(spin->mapToGlobal(p));
            });
            refreshWidget = [spin, slider, read, name, pi] {
                double v = read(name);
                QSignalBlocker b(spin);
                spin->setValue(v);
                if (slider) {
                    QSignalBlocker b2(slider);
                    slider->setValue(int(std::lround((v - pi.min) / (pi.max - pi.min) * 1000.0)));
                }
            };
            break;
        }
    }

    // Keyframe controls: stopwatch (animate), previous, add/remove, next.
    if (target.keyframes && pi.keyframeable) {
        auto* watch = smallButton(row, QStringLiteral("◷"), tr("Toggle animation (keyframes)"));
        watch->setCheckable(true);
        auto* prev = smallButton(row, QStringLiteral("◂"), tr("Previous keyframe"));
        auto* key = smallButton(row, QStringLiteral("◇"), tr("Add / remove keyframe at the playhead"));
        key->setCheckable(true);
        auto* next = smallButton(row, QStringLiteral("▸"), tr("Next keyframe"));
        for (QToolButton* b : {watch, prev, key, next}) h->addWidget(b);
        connect(watch, &QToolButton::clicked, this, [this, target, names, label](bool on) {
            FrameTime t = target.time();
            state_->edit(on ? tr("Animate %1").arg(label) : tr("Stop Animating %1").arg(label), [target, names, on, t](Project&, Sequence& s) {
                Effect* e = target.resolve(s);
                if (!e) return false;
                for (const auto& n : names) {
                    Param& p = e->params[n];
                    double v = p.at(t);
                    if (on) p.addKey(t, v);
                    else {
                        p.keys.clear();
                        p.value = v;
                    }
                }
                return true;
            });
        });
        connect(key, &QToolButton::clicked, this, [this, target, names, label] {
            FrameTime t = target.time();
            state_->edit(tr("Keyframe %1").arg(label), [target, names, t](Project&, Sequence& s) {
                Effect* e = target.resolve(s);
                if (!e) return false;
                for (const auto& n : names) {
                    Param& p = e->params[n];
                    if (p.keyAt(t)) p.removeKey(t);
                    else p.addKey(t, p.at(t));
                }
                return true;
            });
        });
        auto jump = [this, target, readParam, names](bool forward) {
            const Param* p = readParam(names[0]);
            if (!p) return;
            FrameTime t = target.time();
            FrameTime best = -1;
            for (const auto& k : p->keys) {
                if (forward && k.t > t && (best < 0 || k.t < best)) best = k.t;
                if (!forward && k.t < t && (best < 0 || k.t > best)) best = k.t;
            }
            if (best >= 0) state_->setPlayhead(target.origin() + best);
        };
        connect(prev, &QToolButton::clicked, this, [jump] { jump(false); });
        connect(next, &QToolButton::clicked, this, [jump] { jump(true); });
        auto inner = refreshWidget;
        refreshWidget = [inner, watch, key, prev, next, readParam, names, target] {
            inner();
            const Param* p = readParam(names[0]);
            bool animated = p && p->animated();
            bool onKey = p && p->keyAt(target.time());
            QSignalBlocker b1(watch), b2(key);
            watch->setChecked(animated);
            key->setChecked(onKey);
            key->setText(onKey ? QStringLiteral("◆") : QStringLiteral("◇"));
            key->setEnabled(animated);
            prev->setEnabled(animated);
            next->setEnabled(animated);
        };
    }
    form->addRow(label, row);
    refreshers_.push_back(refreshWidget);
}

void InspectorWidget::addStringRow(QFormLayout* form, const StringParamInfo& si, const Target& target) {
    const std::string name = si.name;
    const QString label = tr(si.label.c_str());
    const QString mergeKey = target.key + ":s:" + QString::fromStdString(name);
    auto read = [this, target, name]() -> QString {
        const Sequence* s = state_->sequence();
        if (!s) return {};
        Effect* e = target.resolve(const_cast<Sequence&>(*s));
        return e ? QString::fromStdString(e->s(name)) : QString();
    };
    auto write = [this, target, name, label, mergeKey](const QString& v) {
        std::string val = v.toStdString();
        state_->edit(tr("Change %1").arg(label), [target, name, val](Project&, Sequence& s) {
            Effect* e = target.resolve(s);
            if (!e) return false;
            e->strings[name] = val;
            return true;
        }, mergeKey);
    };
    switch (si.kind) {
        case StringKind::MultilineText: {
            auto* edit = new QPlainTextEdit(content_);
            edit->setFixedHeight(64);
            form->addRow(label, edit);
            connect(edit, &QPlainTextEdit::textChanged, this, [edit, write] { write(edit->toPlainText()); });
            refreshers_.push_back([edit, read] {
                if (edit->hasFocus() || edit->toPlainText() == read()) return;
                QSignalBlocker b(edit);
                edit->setPlainText(read());
            });
            break;
        }
        case StringKind::Font: {
            auto* fonts = new QFontComboBox(content_);
            form->addRow(label, fonts);
            connect(fonts, &QFontComboBox::currentFontChanged, this, [write](const QFont& f) { write(f.family()); });
            refreshers_.push_back([fonts, read] {
                QSignalBlocker b(fonts);
                fonts->setCurrentFont(QFont(read()));
            });
            break;
        }
        case StringKind::File: {
            auto* row = new QWidget(content_);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            auto* line = new QLineEdit(row);
            auto* browse = new QToolButton(row);
            browse->setText(QStringLiteral("…"));
            h->addWidget(line, 1);
            h->addWidget(browse);
            form->addRow(label, row);
            connect(line, &QLineEdit::editingFinished, this, [line, write] { write(line->text()); });
            connect(browse, &QToolButton::clicked, this, [this, line, write, label] {
                QString f = QFileDialog::getOpenFileName(this, label, line->text(), tr("LUT files (*.cube);;All files (*)"));
                if (!f.isEmpty()) write(f);
            });
            refreshers_.push_back([line, read] {
                if (!line->hasFocus()) line->setText(read());
            });
            break;
        }
        case StringKind::Curve: {
            auto* row = new QWidget(content_);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            auto* line = new QLineEdit(row);
            line->setToolTip(tr("Control points as x,y pairs in 0..1, e.g. \"0,0 0.25,0.2 0.75,0.8 1,1\""));
            auto* presets = new QComboBox(row);
            presets->addItem(tr("Presets"));
            const std::pair<const char*, const char*> list[] = {{"Linear", "0,0 1,1"},
                                                                {"S-Curve (contrast)", "0,0 0.25,0.18 0.75,0.82 1,1"},
                                                                {"Soft Contrast", "0,0 0.25,0.22 0.75,0.78 1,1"},
                                                                {"Lift Shadows", "0,0.06 0.3,0.33 1,1"},
                                                                {"Crush Blacks", "0,0 0.12,0 1,1"},
                                                                {"Fade", "0,0.1 1,0.92"},
                                                                {"Invert", "0,1 1,0"}};
            for (const auto& [n, v] : list) presets->addItem(tr(n), QString(v));
            h->addWidget(line, 1);
            h->addWidget(presets);
            form->addRow(label, row);
            connect(line, &QLineEdit::editingFinished, this, [line, write] { write(line->text()); });
            connect(presets, &QComboBox::activated, this, [presets, write](int idx) {
                if (idx > 0) write(presets->itemData(idx).toString());
                presets->setCurrentIndex(0);
            });
            refreshers_.push_back([line, read] {
                if (!line->hasFocus()) line->setText(read());
            });
            break;
        }
        case StringKind::Choice: {
            auto* combo = new QComboBox(content_);
            for (const auto& c : si.choices) combo->addItem(QString::fromStdString(c));
            form->addRow(label, combo);
            connect(combo, &QComboBox::textActivated, this, write);
            refreshers_.push_back([combo, read] {
                QSignalBlocker b(combo);
                combo->setCurrentText(read());
            });
            break;
        }
        default: {
            auto* line = new QLineEdit(content_);
            form->addRow(label, line);
            connect(line, &QLineEdit::textEdited, this, write);
            refreshers_.push_back([line, read] {
                if (!line->hasFocus()) line->setText(read());
            });
            break;
        }
    }
}

}  // namespace montage
