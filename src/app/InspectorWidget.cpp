#include "InspectorWidget.h"
#include "SpellUi.h"
#include "core/ColorGroups.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QInputDialog>
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
#include <QProgressDialog>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>

#include "EditorState.h"
#include "ModelPacks.h"
#include "media/DepthMap.h"
#include "media/Matting.h"
#include "media/Rife.h"
#include "PluginEditorWindow.h"
#include "Theme.h"
#include "audio/PluginEffect.h"
#include "core/ClipAnimation.h"
#include "core/GradeVersions.h"
#include "core/EditOps.h"
#include "core/MaskPath.h"
#include "ColorWheel.h"
#include "ColorWarperEditor.h"
#include "CurveEditor.h"
#include "render/ClipAnalysis.h"
#include "render/Compositor.h"
#include "render/Ocio.h"

namespace montage {

namespace {

Effect* findEffect(Clip& c, Id id) {
    if (c.motion.id == id) return &c.motion;
    if (c.audio.id == id) return &c.audio;
    if (!c.timing.empty() && c.timing.id != 0 && c.timing.id == id) return &c.timing;
    if (c.generator.id == id) return &c.generator;
    for (auto& e : c.effects)
        if (e.id == id) return &e;
    return nullptr;
}

// The same effect in another clip: the fixed attributes by kind, a generator of the same type, else the effect of
// the same type at the same place among those of its type.
Effect* matchingEffect(Clip& from, Id effectId, Clip& to) {
    if (from.motion.id == effectId) return &to.motion;
    if (from.audio.id == effectId) return &to.audio;
    if (!from.timing.empty() && from.timing.id == effectId) return to.timing.empty() ? nullptr : &to.timing;
    if (from.generator.id == effectId) return to.generator.type == from.generator.type ? &to.generator : nullptr;
    const auto it = std::find_if(from.effects.begin(), from.effects.end(), [&](const Effect& e) { return e.id == effectId; });
    if (it == from.effects.end()) return nullptr;
    const std::string type = it->type;
    int nth = int(std::count_if(from.effects.begin(), it, [&](const Effect& e) { return e.type == type; }));  // of its type before it
    for (Effect& e : to.effects)
        if (e.type == type && nth-- == 0) return &e;
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
    // A narrow dock scrolls sideways rather than cutting off the right of each row.
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
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
    if (!c) {
        // An inspected track, bus or master chain: rebuild when its effects change.
        std::vector<Effect>* chain = state_->inspectedChain()
                                         ? edit::effectChain(const_cast<Sequence&>(*s), state_->inspectedChain())
                                         : nullptr;
        if (!chain) return {};
        QString sig = QString("X%1").arg(state_->inspectedChain());
        for (const auto& e : *chain) sig += QString(":%1%2").arg(e.id).arg(e.enabled ? "+" : "-");
        return sig;
    }
    QString sig = QString("C%1:%2").arg(c->id).arg(QString::fromStdString(c->generator.type));
    // Object and Bézier masks have their own controls: rebuild when one is chosen.
    for (const auto& e : c->effects) {
        const long shape = std::lround(e.p("mask.shape", 0));
        sig += QString(":%1%2%3").arg(e.id).arg(e.enabled ? "+" : "-").arg(shape == 3 ? "o" : shape == 5 ? "p" : "");
    }
    // Its colour group: rebuild when it joins or leaves one, or the group's effects change.
    if (const ColorGroup* g = colorGroupOf(*s, *c)) {
        sig += QString("|G%1:%2:").arg(g->id).arg(QString::fromStdString(g->name));
        for (const auto* chain : {&g->pre, &g->post}) {
            for (const auto& e : *chain) sig += QString(":%1%2").arg(e.id).arg(e.enabled ? "+" : "-");
            sig += '/';
        }
    }
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
    } else if (s && state_->inspectedChain()) {
        buildChain(state_->inspectedChain());
    } else {
        auto* l = new QLabel(tr("Select a clip or transition in the timeline to edit its properties and effects."), content_);
        l->setWordWrap(true);
        l->setAlignment(Qt::AlignCenter);
        l->setStyleSheet(QString("color: %1; padding: 24px;").arg(theme::kTextDim.name()));
        layout_->addWidget(l);
    }
    layout_->addStretch();
    // Choices with long names ("Position, Scale & Rotation") would widen the whole Inspector: they take what room there
    // is, and their lists still show everything.
    for (QComboBox* box : content_->findChildren<QComboBox*>()) {
        box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        box->setMinimumContentsLength(std::min(box->minimumContentsLength() > 0 ? box->minimumContentsLength() : 8, 8));
    }
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
        t.clip = clipId;
        t.effect = effectId;
        return t;
    };

    // ---- Clip ---------------------------------------------------------------
    QFormLayout* form = addSection(clip.isGenerator() ? tr("Clip (Generated)") : tr("Clip"));
    if (const size_t n = otherSelected(target(clip.motion.id)).size(); n > 0) {
        auto* many = new QLabel(tr("Values changed here go to all %1 selected clips").arg(n + 1), content_);
        many->setObjectName(QStringLiteral("inspectorMultiClip"));
        many->setWordWrap(true);
        many->setStyleSheet(QString("color: %1;").arg(theme::kAccent.name()));
        form->addRow(many);
    }
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
    timing->setWordWrap(true);  // in a narrow dock the length goes under the in and out
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
    // The speed, then its options on a row of their own (the Inspector stays narrow enough for a side dock).
    form->addRow(tr("Speed"), speed);
    auto* speedRow = new QWidget(content_);
    auto* sh = new QHBoxLayout(speedRow);
    sh->setContentsMargins(0, 0, 0, 0);
    sh->addWidget(reverse);
    sh->addWidget(rippleSpeed);
    sh->addStretch(1);
    form->addRow(QString(), speedRow);
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
        // Grade versions (Resolve's local versions): which grade is shown, and new ones.
        auto* gradeRow = new QWidget(content_);
        auto* gh = new QHBoxLayout(gradeRow);
        gh->setContentsMargins(0, 0, 0, 0);
        auto* versions = new QComboBox(gradeRow);
        versions->setObjectName(QStringLiteral("gradeVersions"));
        versions->setToolTip(tr("Grade versions: switch between this clip's grades"));
        auto* more = new QToolButton(gradeRow);
        more->setText(QStringLiteral("+"));
        more->setObjectName(QStringLiteral("gradeVersionMenu"));
        more->setPopupMode(QToolButton::InstantPopup);
        auto* menu = new QMenu(more);
        auto run = [this, clipId](const QString& label, auto fn) {
            state_->apply(label, [clipId, fn](Project& p, Sequence& s) { return fn(p, s, clipId); });
        };
        menu->addAction(tr("New Version (Copy of This Grade)"), this, [run] {
            run(tr("New Grade Version"), [](Project& p, Sequence& s, Id id) { return edit::addGradeVersion(p, s, id); });
        })->setObjectName(QStringLiteral("newGradeVersion"));
        menu->addAction(tr("New Empty Version"), this, [run] {
            run(tr("New Grade Version"), [](Project& p, Sequence& s, Id id) { return edit::addGradeVersion(p, s, id, {}, true); });
        });
        menu->addAction(tr("Rename Version…"), this, [this, clipId, versions, clipNow] {
            const Clip* c = clipNow();
            if (!c || c->gradeVersions.empty()) return;
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Rename Version"), tr("Name:"), QLineEdit::Normal, versions->currentText(), &ok);
            const int index = c->gradeVersion;
            if (ok && !name.trimmed().isEmpty())
                state_->apply(tr("Rename Grade Version"), [clipId, index, n = name.trimmed().toStdString()](Project&, Sequence& s) {
                    return edit::renameGradeVersion(s, clipId, index, n);
                });
        });
        menu->addAction(tr("Delete Version"), this, [this, clipId, clipNow] {
            const Clip* c = clipNow();
            if (!c) return;
            const int index = c->gradeVersion;
            state_->apply(tr("Delete Grade Version"), [clipId, index](Project&, Sequence& s) { return edit::removeGradeVersion(s, clipId, index); });
        });
        more->setMenu(menu);
        gh->addWidget(versions, 1);
        gh->addWidget(more);
        form->addRow(tr("Grade"), gradeRow);
        connect(versions, &QComboBox::activated, this, [this, clipId](int index) {
            state_->apply(tr("Switch Grade Version"), [clipId, index](Project&, Sequence& s) { return edit::switchGradeVersion(s, clipId, index); });
        });
        refreshers_.push_back([=, this] {
            const Clip* c = clipNow();
            if (!c) return;
            QSignalBlocker b(versions);
            versions->clear();
            if (c->gradeVersions.empty()) versions->addItem(tr("Version 1"));
            for (const GradeVersion& v : c->gradeVersions) versions->addItem(QString::fromStdString(v.name));
            versions->setCurrentIndex(c->gradeVersions.empty() ? 0 : c->gradeVersion);
            versions->setEnabled(c->gradeVersions.size() > 1);
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
            // Follow: the clip moves with what is under its position in the footage beneath
            // (a title on a moving car, a blur on a face), like Final Cut's object tracker.
            auto* row = new QWidget(content_);
            auto* rh = new QHBoxLayout(row);
            rh->setContentsMargins(0, 0, 0, 0);
            auto* back = new QToolButton(row);
            back->setText(tr("◀ Follow"));
            back->setObjectName(QStringLiteral("followBack"));
            back->setToolTip(tr("Move this clip with the footage beneath it, backwards from the playhead"));
            auto* fwd = new QToolButton(row);
            fwd->setText(tr("Follow ▶"));
            fwd->setObjectName(QStringLiteral("followForward"));
            fwd->setToolTip(tr("Move this clip with the footage beneath it, from the playhead on.\n"
                               "Put its Position on what it should follow first; Anchor then offsets it from that point."));
            auto* model = new QComboBox(row);
            model->setObjectName(QStringLiteral("followModel"));
            model->addItems({tr("Position"), tr("Position & Scale"), tr("Position, Scale & Rotation")});
            auto* size = new QComboBox(row);
            size->setObjectName(QStringLiteral("followSize"));
            size->setToolTip(tr("How much around the point to follow"));
            size->addItem(tr("Small"), 0.1);
            size->addItem(tr("Medium"), 0.2);
            size->addItem(tr("Large"), 0.35);
            size->setCurrentIndex(1);
            rh->addWidget(back);
            rh->addWidget(fwd);
            rh->addStretch(1);
            t->addRow(tr("Follow:"), row);
            // What it follows on a row of its own, so the Inspector stays narrow.
            auto* how = new QWidget(content_);
            auto* hh = new QHBoxLayout(how);
            hh->setContentsMargins(0, 0, 0, 0);
            hh->addWidget(model, 1);
            hh->addWidget(size);
            t->addRow(QString(), how);
            for (auto [button, forward] : {std::pair{back, false}, std::pair{fwd, true}})
                connect(button, &QToolButton::clicked, this, [this, clipId, model, size, forward = forward] {
                    const int m = model->currentIndex();
                    const double sz = size->currentData().toDouble();
                    QTimer::singleShot(0, this, [this, clipId, forward, m, sz] { followFootage(clipId, forward, m, sz); });
                });
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
        // Animation presets (CapCut's In / Out / Combo): a kind and how long it lasts, for each.
        QFormLayout* an = addSection(tr("Animation"), nullptr, !hasClipAnimation(clip));
        const struct {
            AnimationSlot slot;
            QString label;
            const char* object;
        } animSlots[] = {{AnimationSlot::In, tr("In"), "animIn"}, {AnimationSlot::Out, tr("Out"), "animOut"}, {AnimationSlot::Combo, tr("Combo"), "animCombo"}};
        for (const auto& sl : animSlots) {
            auto* row = new QWidget(content_);
            auto* rh = new QHBoxLayout(row);
            rh->setContentsMargins(0, 0, 0, 0);
            auto* kindBox = new QComboBox(row);
            kindBox->setObjectName(QString::fromLatin1(sl.object));
            kindBox->addItem(tr("None"), QString());
            for (const AnimationPreset& a : animationPresets(sl.slot)) kindBox->addItem(tr(a.name), QString::fromLatin1(a.id));
            auto* secs = new QDoubleSpinBox(row);
            secs->setObjectName(QString::fromLatin1(sl.object) + QStringLiteral("Seconds"));
            secs->setRange(0.1, 10);
            secs->setSingleStep(0.1);
            secs->setDecimals(1);
            secs->setSuffix(tr(" s"));
            secs->setToolTip(sl.slot == AnimationSlot::Combo ? tr("How long each repeat takes") : tr("How long it lasts"));
            rh->addWidget(kindBox, 1);
            rh->addWidget(secs);
            an->addRow(sl.label, row);
            const AnimationSlot slot = sl.slot;
            auto apply = [this, clipId, slot, kindBox, secs] {
                const std::string type = kindBox->currentData().toString().toStdString();
                const double sec = secs->value();
                state_->apply(tr("Clip Animation"), [clipId, slot, type, sec](Project&, Sequence& s) {
                    return edit::setClipAnimation(s, clipId, slot, type, sec);
                });
            };
            connect(kindBox, &QComboBox::activated, this, apply);
            connect(secs, &QDoubleSpinBox::editingFinished, this, apply);
            refreshers_.push_back([=, this] {
                const Clip* c = clipNow();
                if (!c) return;
                const ClipAnimation& a = slot == AnimationSlot::In ? c->animIn : slot == AnimationSlot::Out ? c->animOut : c->animLoop;
                QSignalBlocker b1(kindBox), b2(secs);
                kindBox->setCurrentIndex(std::max(0, kindBox->findData(QString::fromStdString(a.type))));
                secs->setValue(a.type.empty() ? (slot == AnimationSlot::Combo ? 1.0 : 0.5) : a.seconds);
            });
        }
    } else if (const EffectInfo* info = findEffectInfo("volume")) {
        QFormLayout* a = addSection(tr("Volume"));
        addParamRows(a, *info, target(clip.audio.id));
    }
    // Time Remapping: a speed curve inside the clip (clips saved before it existed get one on first use).
    if (!clip.isGenerator() && !clip.reverse) {
        if (const EffectInfo* info = findEffectInfo("time")) {
            Target tt;
            tt.resolve = [clipId](Sequence& s) -> Effect* {
                Clip* c = edit::clipById(s, clipId);
                if (!c) return nullptr;
                if (c->timing.empty()) c->timing = makeEffect("time", 0);  // never looked up by id
                return &c->timing;
            };
            tt.time = localTime;
            tt.origin = originFn;
            tt.key = QString("t%1").arg(clipId);
            // Picture and sound stay in step: linked clips get the same curve.
            tt.afterWrite = [clipId](Sequence& s) {
                const Clip* c = edit::clipById(s, clipId);
                if (!c) return;
                for (Id other : edit::linkedClips(s, clipId))
                    if (Clip* k = edit::clipById(s, other); k && other != clipId && !k->reverse) {
                        const Id keep = k->timing.empty() ? 0 : k->timing.id;
                        k->timing = c->timing;
                        k->timing.id = keep;
                    }
            };
            auto* reset = smallButton(nullptr, QStringLiteral("↺"), tr("Back to constant speed"));
            QFormLayout* tf = addSection(tr("Time Remapping"), reset, !clip.ramped());
            addParamRows(tf, *info, tt);
            if (!clip.timing.empty() && std::lround(clip.timing.p("sampling", 0)) == 3 && rifeAvailable() && !rifeModel().installed()) {
                // AI frames fall back to optical flow until the model is here.
                auto* get = new QPushButton(tr("Download RIFE Model…"), content_);
                get->setObjectName(QStringLiteral("getRifeModel"));
                connect(get, &QPushButton::clicked, this, [this] {
                    QTimer::singleShot(0, this, [this] {
                        if (ensureEffectModel(window(), "rife")) state_->amend([](Project&, Sequence&) { return true; });
                    });
                });
                tf->addRow(tr("AI Frames:"), get);
            }
            connect(reset, &QToolButton::clicked, this, [this, clipId] {
                state_->edit(tr("Reset Time Remapping"), [clipId](Project&, Sequence& s) {
                    for (Id id : edit::linkedClips(s, clipId))
                        if (Clip* k = edit::clipById(s, id); k && !k->timing.empty()) k->timing = makeEffect("time", k->timing.id);
                    return true;
                });
            });
        }
    }
    // ---- Effect stack ------------------------------------------------------------
    buildEffectStack(clipId, kind, clip.effects, localTime);
    // ---- Its colour group's grades, before and after its own (core/ColorGroups.h) --------
    const Sequence* seqNow = state_->sequence();
    if (const ColorGroup* g = kind == TrackKind::Video && seqNow ? colorGroupOf(*seqNow, clip) : nullptr) {
        const QString name = QString::fromStdString(g->name);
        const int members = int(colorGroupMembers(*seqNow, g->id).size());
        const std::vector<Effect> pre = g->pre, post = g->post;  // copies: building can change the project
        const Id preOwner = g->id, postOwner = g->postId;
        for (int stage = 0; stage < 2; ++stage) {
            QFormLayout* f = addSection(stage == 0 ? tr("Group Pre-Clip: %1").arg(name) : tr("Group Post-Clip: %1").arg(name));
            auto* note = new QLabel(stage == 0 ? tr("Runs on each of the group's %n clip(s) before its own effects, to match the shots.", "", members)
                                               : tr("Runs on each of the group's %n clip(s) after its own effects: the group's look.", "", members),
                                    content_);
            note->setObjectName(stage == 0 ? QStringLiteral("colorGroupPre") : QStringLiteral("colorGroupPost"));
            note->setWordWrap(true);
            note->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
            f->addRow(note);
            buildEffectStack(stage == 0 ? preOwner : postOwner, TrackKind::Video, stage == 0 ? pre : post, localTime);
        }
    }
}

void InspectorWidget::buildChain(Id owner) {
    const Sequence* s = state_->sequence();
    std::vector<Effect>* chain = s ? edit::effectChain(const_cast<Sequence&>(*s), owner) : nullptr;
    if (!chain) return;
    QString title = tr("Master");
    if (owner != s->id) {
        for (size_t i = 0; i < s->audioTracks.size(); ++i)
            if (s->audioTracks[i].id == owner)
                title = tr("Track %1").arg(s->audioTracks[i].name.empty() ? QStringLiteral("A%1").arg(i + 1)
                                                                          : QString::fromStdString(s->audioTracks[i].name));
        for (const Bus& b : s->buses)
            if (b.id == owner) title = tr("Bus %1").arg(QString::fromStdString(b.name));
    }
    QFormLayout* form = addSection(title);
    auto* note = new QLabel(chain->empty() ? tr("No effects yet. Effects here process everything this %1 carries, "
                                                 "before its fader.")
                                                 .arg(owner == s->id ? tr("mix") : tr("channel"))
                                           : tr("%n effect(s), processed top to bottom before the fader.", "",
                                                int(chain->size())),
                            content_);
    note->setWordWrap(true);
    note->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
    form->addRow(note);
    buildEffectStack(owner, TrackKind::Audio, *chain, [this] { return state_->playhead(); });
}

void InspectorWidget::buildEffectStack(Id owner, TrackKind kind, const std::vector<Effect>& effects,
                                       const std::function<FrameTime()>& localTime) {
    auto target = [&](Id effectId) {
        Target t;
        t.resolve = [owner, effectId](Sequence& s) -> Effect* { return edit::ownedEffect(s, owner, effectId); };
        t.time = localTime;
        t.origin = [this, owner]() -> FrameTime {
            FrameTime origin = 0;
            const Sequence* s = state_->sequence();
            if (s) edit::effectChain(const_cast<Sequence&>(*s), owner, &origin);
            return origin;
        };
        t.key = QString("p%1:%2").arg(owner).arg(effectId);
        t.owner = owner;
        return t;
    };
    for (size_t i = 0; i < effects.size(); ++i) {
        const Effect& e = effects[i];
        const EffectInfo* catalog = findEffectInfo(e.type);
        if (!catalog) continue;
        // Plugin effects carry their own parameter list and name.
        EffectInfo shown = *catalog;
        shown.displayName = plugins::effectName(e);
        shown.params = effectParams(e);
        // A plugin with a key input: which track it hears there (as the Compressor's sidechain).
        if (e.type == "plugin" && (e.s("key_input") == "1" || plugins::knownSidechain(e.s("plugin_id")))) {
            StringParamInfo key;
            key.name = "sidechain";
            key.label = tr("Sidechain").toStdString();
            key.kind = StringKind::Track;
            shown.strings.push_back(key);
        }
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
        QToolButton* editor = nullptr;
        if (e.type == "plugin") {
            editor = smallButton(tools, tr("Editor"), tr("Open the plugin's own editor"));
            editor->setObjectName("pluginEditor");
            th->addWidget(editor);
            connect(editor, &QToolButton::clicked, this, [this, owner, eid] {
                QString err;
                if (!PluginEditorWindow::open(state_, owner, eid, window(), &err) && !err.isEmpty()) state_->message(err, 6000);
            });
        }
        for (QWidget* w : {static_cast<QWidget*>(on), static_cast<QWidget*>(up), static_cast<QWidget*>(down),
                           static_cast<QWidget*>(reset), static_cast<QWidget*>(del)})
            th->addWidget(w);
        QFormLayout* f = addSection(QString::fromStdString(info->displayName), tools);
        if (e.type == "color_correct") addColorWheels(f, target(eid));
        if (e.type == "hdr_palette") addHdrPalette(f, *info, target(eid));
        else addParamRows(f, *info, target(eid));
        const bool onClip = state_->sequence() && edit::clipById(*state_->sequence(), owner);
        if ((e.type == "stabilize" || e.type == "rolling_shutter") && onClip) {
            // The analysis lives in the effect; it is redone on demand.
            auto* row = new QWidget(content_);
            auto* rh = new QHBoxLayout(row);
            rh->setContentsMargins(0, 0, 0, 0);
            auto* status = new QLabel(row);
            CameraMotion cm;
            status->setText(cameraMotionFromString(e.s("motion"), cm) ? tr("Analysed: %n frame(s)", "", int(cm.steps.size()))
                                                                      : tr("Not analysed yet"));
            auto* analyze = new QPushButton(tr("Analyze"), row);
            analyze->setObjectName(QStringLiteral("analyzeStabilize"));
            analyze->setToolTip(tr("Measure the camera's movement in this clip"));
            rh->addWidget(status, 1);
            rh->addWidget(analyze);
            f->addRow(QString(), row);
            // Deferred: the analysis waits in an event loop, and the Inspector may rebuild meanwhile.
            connect(analyze, &QPushButton::clicked, this,
                    [this, owner, eid] { QTimer::singleShot(0, this, [this, owner, eid] { analyzeStabilize(owner, eid); }); });
        }
        if (e.type == "corner_pin" && onClip) {
            // Planar tracking: the corners follow a flat surface (a screen, a sign) in the footage.
            auto* row = new QWidget(content_);
            auto* rh = new QHBoxLayout(row);
            rh->setContentsMargins(0, 0, 0, 0);
            auto* back = new QToolButton(row);
            back->setText(tr("◀ Track"));
            back->setObjectName(QStringLiteral("trackCornersBack"));
            back->setToolTip(tr("Track the surface under the corners backwards from the playhead"));
            auto* fwd = new QToolButton(row);
            fwd->setText(tr("Track ▶"));
            fwd->setObjectName(QStringLiteral("trackCornersForward"));
            fwd->setToolTip(tr("Track the surface under the corners from the playhead on.\n"
                               "Drag the corners onto it in the Program monitor first; the clip beneath "
                               "this one is tracked, or this clip's own footage if nothing is beneath."));
            rh->addWidget(back);
            rh->addWidget(fwd);
            rh->addStretch(1);
            f->addRow(tr("Track:"), row);
            for (auto [button, forward] : {std::pair{back, false}, std::pair{fwd, true}})
                connect(button, &QToolButton::clicked, this, [this, owner, eid, forward = forward] {
                    QTimer::singleShot(0, this, [this, owner, eid, forward] { trackCorners(owner, eid, forward); });
                });
        }
        if (kind == TrackKind::Video && supportsMask(e.type)) {
            // Shape masks and the HSL qualifier; folded away until one is used.
            QFormLayout* mf = addSection(tr("%1 Mask").arg(QString::fromStdString(info->displayName)), nullptr,
                                         !hasMask(e, localTime()));
            addParamRows(mf, maskInfo(), target(eid));
            if (e.p("mask.depth", localTime()) > 0.5 && depthAvailable() && !depthModel().installed()) {
                // The depth qualifier selects nothing until the model is here.
                auto* get = new QPushButton(tr("Download Depth Model…"), content_);
                get->setObjectName(QStringLiteral("getDepthModel"));
                connect(get, &QPushButton::clicked, this, [this] {
                    QTimer::singleShot(0, this, [this] {
                        if (ensureEffectModel(window(), "mask.depth")) state_->amend([](Project&, Sequence&) { return true; });
                    });
                });
                mf->addRow(tr("Depth:"), get);
            }
            if (std::lround(e.p("mask.shape", localTime())) == 4 && mattingAvailable() && !mattingModel().installed()) {
                // A People mask selects nothing until the model is here.
                auto* get = new QPushButton(tr("Download People Model…"), content_);
                get->setObjectName(QStringLiteral("getMatteModel"));
                connect(get, &QPushButton::clicked, this, [this] {
                    QTimer::singleShot(0, this, [this] {
                        if (ensureEffectModel(window(), "mask.people")) state_->amend([](Project&, Sequence&) { return true; });
                    });
                });
                mf->addRow(tr("People:"), get);
            }
            if (onClip && std::lround(e.p("mask.shape", localTime())) == 5) {
                // A path drawn in the viewer, and animated as a whole.
                auto* status = new QLabel(content_);
                status->setObjectName(QStringLiteral("maskPathStatus"));
                status->setWordWrap(true);
                auto current = [this, owner, eid]() -> const Effect* {
                    const Sequence* sq = state_->sequence();
                    return sq ? edit::ownedEffect(const_cast<Sequence&>(*sq), owner, eid) : nullptr;
                };
                auto row = new QWidget(content_);
                auto* rh = new QHBoxLayout(row);
                rh->setContentsMargins(0, 0, 0, 0);
                auto* close = new QToolButton(row);
                close->setText(tr("Close Path"));
                close->setObjectName(QStringLiteral("closeMaskPath"));
                close->setToolTip(tr("Finish drawing: join the last point to the first"));
                auto* redraw = new QToolButton(row);
                redraw->setText(tr("Redraw"));
                redraw->setObjectName(QStringLiteral("redrawMaskPath"));
                redraw->setToolTip(tr("Remove the path's points and draw it again"));
                auto* animate = new QCheckBox(tr("Animate Path"), row);
                animate->setObjectName(QStringLiteral("animateMaskPath"));
                animate->setToolTip(tr("Key the whole path at the playhead: moving a point on another frame keys it there too"));
                rh->addWidget(close);
                rh->addWidget(redraw);
                rh->addWidget(animate);
                rh->addStretch(1);
                auto refresh = [this, status, close, animate, current, localTime] {
                    const Effect* ef = current();
                    if (!ef) return;
                    const int n = maskPathCount(*ef);
                    const bool open = n < 3 || ef->p("mask.open", localTime()) > 0.5;
                    status->setText(open ? tr("Click in the viewer to place points, dragging to curve them. Click the first point to close the path.")
                                         : tr("%1 points. Drag points and handles; click the outline to add a point; Alt-click a point: corner or smooth; Ctrl-click: remove it.")
                                               .arg(n));
                    close->setEnabled(open && n >= 3);
                    QSignalBlocker b(animate);
                    animate->setChecked(maskPathAnimated(*ef));
                    animate->setEnabled(n > 0);
                };
                refresh();
                refreshers_.push_back(refresh);
                mf->addRow(QString(), status);
                mf->addRow(tr("Path:"), row);
                auto editPath = [this, owner, eid](const QString& label, std::function<bool(Effect&, FrameTime, double, double)> fn) {
                    const Sequence* sq = state_->sequence();
                    const Clip* cl = sq ? edit::clipById(*sq, owner) : nullptr;
                    double mw = 1, mh = 1;
                    if (!cl || !clipFrameSize(state_->project(), *sq, *cl, mw, mh)) return;
                    const FrameTime lt = state_->playhead() - cl->start;
                    state_->edit(label, [owner, eid, lt, mw, mh, fn](Project&, Sequence& s) {
                        Effect* ef = edit::ownedEffect(s, owner, eid);
                        return ef && fn(*ef, lt, mw, mh);
                    });
                };
                connect(close, &QToolButton::clicked, this, [editPath] {
                    editPath(tr("Close Mask Path"), [](Effect& ef, FrameTime lt, double mw, double mh) { return closeMaskPath(ef, lt, mw, mh); });
                });
                connect(redraw, &QToolButton::clicked, this, [editPath] {
                    editPath(tr("Redraw Mask Path"), [](Effect& ef, FrameTime lt, double, double) {
                        if (maskPathCount(ef) == 0) return false;
                        setMaskPathAnimated(ef, lt, false);
                        setMaskPath(ef, lt, {});
                        ef.params["mask.open"] = Param(1);
                        return true;
                    });
                });
                connect(animate, &QCheckBox::toggled, this, [editPath](bool on) {
                    editPath(on ? tr("Animate Mask Path") : tr("Stop Animating Mask Path"), [on](Effect& ef, FrameTime lt, double, double) {
                        if (maskPathCount(ef) == 0 || maskPathAnimated(ef) == on) return false;
                        setMaskPathAnimated(ef, lt, on);
                        return true;
                    });
                });
            }
            if (onClip && std::lround(e.p("mask.shape", localTime())) == 3) {
                // An object picked in the viewer, then followed through the clip.
                auto* status = new QLabel(content_);
                status->setObjectName(QStringLiteral("objectStatus"));
                status->setWordWrap(true);
                auto read = [this, owner, eid]() -> QString {
                    const Sequence* sq = state_->sequence();
                    const Effect* ef = sq ? edit::ownedEffect(const_cast<Sequence&>(*sq), owner, eid) : nullptr;
                    if (!ef || !ef->object || ef->object->prompts.empty())
                        return tr("Click the object in the viewer. Alt-click what is not part of it, or drag a box around it.");
                    return tr("%n frame(s) clicked", "", int(ef->object->prompts.size())) + QStringLiteral(", ") +
                           tr("%n segmented", "", int(ef->object->frames.size()));
                };
                status->setText(read());
                refreshers_.push_back([status, read] { status->setText(read()); });
                mf->addRow(QString(), status);
                auto* row = new QWidget(content_);
                auto* rh = new QHBoxLayout(row);
                rh->setContentsMargins(0, 0, 0, 0);
                auto* back = new QToolButton(row);
                back->setText(tr("◀ Track"));
                back->setObjectName(QStringLiteral("trackObjectBack"));
                back->setToolTip(tr("Follow the object backwards from the playhead to the clip's start"));
                auto* fwd = new QToolButton(row);
                fwd->setText(tr("Track ▶"));
                fwd->setObjectName(QStringLiteral("trackObjectForward"));
                fwd->setToolTip(tr("Follow the object from the playhead to the clip's end"));
                auto* clear = new QToolButton(row);
                clear->setText(tr("Clear Frame"));
                clear->setObjectName(QStringLiteral("clearObjectFrame"));
                clear->setToolTip(tr("Remove the clicks on this frame"));
                auto* reset = new QToolButton(row);
                reset->setText(tr("Clear All"));
                reset->setObjectName(QStringLiteral("clearObject"));
                reset->setToolTip(tr("Remove every click and the tracked object"));
                for (QToolButton* b : {back, fwd, clear, reset}) rh->addWidget(b);
                rh->addStretch(1);
                mf->addRow(tr("Object:"), row);
                for (auto [button, forward] : {std::pair{back, false}, std::pair{fwd, true}})
                    connect(button, &QToolButton::clicked, this, [this, owner, eid, forward = forward] {
                        QTimer::singleShot(0, this, [this, owner, eid, forward] { trackObject(owner, eid, forward); });
                    });
                connect(clear, &QToolButton::clicked, this, [this, owner, eid] {
                    const Sequence* sq = state_->sequence();
                    const Clip* cl = sq ? edit::clipById(*sq, owner) : nullptr;
                    if (!cl) return;
                    const FrameTime lt = state_->playhead() - cl->start;
                    state_->edit(tr("Clear Object Clicks"), [owner, eid, lt](Project& p, Sequence& s) {
                        Clip* c = edit::clipById(s, owner);
                        Effect* ef = c ? edit::ownedEffect(s, owner, eid) : nullptr;
                        if (!c || !ef || !ef->object) return false;
                        ef->object = withObjectPrompts(p, s, *c, *ef, lt, {});
                        return true;
                    });
                });
                connect(reset, &QToolButton::clicked, this, [this, owner, eid] {
                    state_->edit(tr("Clear Object"), [owner, eid](Project&, Sequence& s) {
                        Effect* ef = edit::ownedEffect(s, owner, eid);
                        if (!ef || !ef->object) return false;
                        ef->object.reset();
                        return true;
                    });
                });
            } else if (onClip) {
                // Make the mask follow what it covers, from the playhead on (or back).
                auto* row = new QWidget(content_);
                auto* rh = new QHBoxLayout(row);
                rh->setContentsMargins(0, 0, 0, 0);
                auto* back = new QToolButton(row);
                back->setText(tr("◀ Track"));
                back->setObjectName(QStringLiteral("trackMaskBack"));
                back->setToolTip(tr("Track the mask backwards from the playhead to the clip's start"));
                auto* fwd = new QToolButton(row);
                fwd->setText(tr("Track ▶"));
                fwd->setObjectName(QStringLiteral("trackMaskForward"));
                fwd->setToolTip(tr("Track the mask from the playhead to the clip's end"));
                auto* model = new QComboBox(row);
                model->setObjectName(QStringLiteral("trackModel"));
                model->addItems({tr("Position"), tr("Position & Scale"), tr("Position, Scale & Rotation")});
                rh->addWidget(back);
                rh->addWidget(fwd);
                rh->addWidget(model, 1);
                mf->addRow(tr("Track:"), row);
                for (auto [button, forward] : {std::pair{back, false}, std::pair{fwd, true}})
                    connect(button, &QToolButton::clicked, this, [this, owner, eid, model, forward = forward] {
                        const int m = model->currentIndex();
                        QTimer::singleShot(0, this, [this, owner, eid, forward, m] { trackMask(owner, eid, forward, m); });
                    });
            }
        }
        auto mutateStack = [this, owner, eid](const QString& label, std::function<void(std::vector<Effect>&, size_t, Project&)> fn) {
            state_->edit(label, [=](Project& p, Sequence& s) {
                std::vector<Effect>* chain = edit::effectChain(s, owner);
                if (!chain) return false;
                for (size_t k = 0; k < chain->size(); ++k)
                    if ((*chain)[k].id == eid) {
                        fn(*chain, k, p);
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
    addEffectMenu(kind, owner);
}

void InspectorWidget::addEffectMenu(TrackKind kind, Id owner) {
    auto* btn = new QPushButton(kind == TrackKind::Video ? tr("Add Video Effect…") : tr("Add Audio Effect…"), content_);
    auto* menu = new QMenu(btn);
    std::map<std::string, QMenu*> groups;
    for (const EffectInfo* info : effectsInCategory(kind == TrackKind::Video ? EffectCategory::VideoFilter : EffectCategory::AudioFilter)) {
        QMenu*& g = groups[info->group];
        if (!g) g = menu->addMenu(QString::fromStdString(info->group));
        std::string type = info->type;
        QString label = QString::fromStdString(info->displayName);
        g->addAction(label, this, [this, owner, type, label] {
            if (!ensureEffectModel(window(), type)) return;
            Id added = 0;
            state_->edit(tr("Add %1").arg(label), [owner, type, &added](Project& p, Sequence& s) {
                std::vector<Effect>* chain = edit::effectChain(s, owner);
                if (!chain) return false;
                Effect e = makeEffect(p, type);
                added = e.id;
                // Stabilize and Rolling Shutter Repair move the whole frame: they go first, before any filter.
                if (type == "stabilize" || type == "rolling_shutter") chain->insert(chain->begin(), e);
                else chain->push_back(e);
                return true;
            });
            if ((type == "stabilize" || type == "rolling_shutter") && added && state_->sequence() && edit::clipById(*state_->sequence(), owner))
                QTimer::singleShot(0, this, [this, owner, added] { analyzeStabilize(owner, added); });
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
            vm->addAction(label, this, [this, owner, type, label] {
                QString error;
                state_->edit(tr("Add %1").arg(label), [owner, type, &error](Project& p, Sequence& s) {
                    std::vector<Effect>* chain = edit::effectChain(s, owner);
                    if (!chain) return false;
                    std::string err;
                    auto e = plugins::makeEffectOfType(p, type, &err);
                    if (!e) {
                        error = QString::fromStdString(err);
                        return false;
                    }
                    chain->push_back(*e);
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

ColorWheel* InspectorWidget::addWheel(QWidget* parent, const QString& title, const std::array<std::string, 3>& names, double scale,
                                      double neutral, const Target& target) {
    // The wheel sets its three channel controls; the part they share (set with the channel sliders) is kept, so a
    // wheel only shifts the balance.
    auto* wheel = new ColorWheel(title, scale, parent);
    wheel->setObjectName(QStringLiteral("wheel_") + QString::fromStdString(names[0]).section('_', 0, 0));
    wheel->setMinimumSize(80, 96);
    auto read = [this, target, names, neutral](double v[3]) {
        const Sequence* s = state_->sequence();
        Effect* e = s ? target.resolve(const_cast<Sequence&>(*s)) : nullptr;
        for (int i = 0; i < 3; ++i) v[i] = (e ? e->p(names[size_t(i)], target.time(), neutral) : neutral) - neutral;
    };
    const QString label = tr("%1 Balance").arg(title);
    const QString mergeKey = target.key + ":wheel:" + QString::fromStdString(names[0]);
    connect(wheel, &ColorWheel::changed, this, [this, target, names, neutral, read, label, mergeKey](double r, double g, double b, bool) {
        double now[3];
        read(now);
        const double common = (now[0] + now[1] + now[2]) / 3;
        const double v[3] = {common + r + neutral, common + g + neutral, common + b + neutral};
        const FrameTime t = target.time();
        const std::vector<Id> others = otherSelected(target);
        const FrameTime playhead = state_->playhead();
        state_->edit(label, [this, target, names, v, t, others, playhead](Project&, Sequence& s) {
            Effect* e = target.resolve(s);
            if (!e) return false;
            for (int i = 0; i < 3; ++i) e->params[names[size_t(i)]].set(t, v[i]);
            applyToOthers(s, target, others, playhead, [&](Effect& o, FrameTime ot) {
                for (int i = 0; i < 3; ++i) o.params[names[size_t(i)]].set(ot, v[i]);
            });
            if (target.afterWrite) target.afterWrite(s);
            return true;
        }, mergeKey);
    });
    auto refresh = [wheel, read] {
        if (wheel->isDragging()) return;
        double v[3];
        read(v);
        wheel->setBalance(v[0], v[1], v[2]);
    };
    refresh();
    refreshers_.push_back(refresh);
    return wheel;
}

void InspectorWidget::addColorWheels(QFormLayout* form, const Target& target) {
    struct Wheel {
        const char* title;
        std::array<std::string, 3> names;
        double scale, neutral;
    };
    static const Wheel wheels[3] = {{QT_TR_NOOP("Lift"), {"lift_r", "lift_g", "lift_b"}, 0.25, 0},
                                    {QT_TR_NOOP("Gamma"), {"gamma_r", "gamma_g", "gamma_b"}, 0.5, 0},
                                    {QT_TR_NOOP("Gain"), {"gain_r", "gain_g", "gain_b"}, 0.5, 1}};
    auto* row = new QWidget(content_);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(4);
    for (const Wheel& w : wheels) h->addWidget(addWheel(row, tr(w.title), w.names, w.scale, w.neutral, target), 1);
    form->addRow(row);
}

void InspectorWidget::addHdrPalette(QFormLayout* form, const EffectInfo& info, const Target& target) {
    // One zone at a time (Global, then the six from Black to Specular): its colour wheel and its sliders.
    auto* zone = new QComboBox(content_);
    zone->setObjectName(QStringLiteral("hdrZone"));
    zone->addItem(tr("Global"));
    for (const HdrZone& z : hdrZones()) zone->addItem(tr(z.label));
    hdrZone_ = std::clamp(hdrZone_, 0, zone->count() - 1);
    zone->setCurrentIndex(hdrZone_);
    connect(zone, &QComboBox::currentIndexChanged, this, [this](int i) {
        hdrZone_ = i;
        QTimer::singleShot(0, this, [this] { rebuild(); });  // not while the combo box is still signalling
    });
    form->addRow(tr("Zone"), zone);
    std::string prefix = "global";
    std::vector<std::string> names{"exposure", "saturation", "contrast", "pivot", "black_offset", "mix"};
    if (hdrZone_ > 0) {
        prefix = hdrZones()[size_t(hdrZone_ - 1)].name;
        names.clear();
        for (const char* n : {"_exposure", "_saturation", "_range", "_falloff"}) names.push_back(prefix + n);
    }
    auto* row = new QWidget(content_);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->addStretch(1);
    h->addWidget(addWheel(row, zone->currentText(), {prefix + "_r", prefix + "_g", prefix + "_b"}, 0.5, 0, target), 2);
    h->addStretch(1);
    form->addRow(row);
    for (const std::string& n : names)
        for (const ParamInfo& pi : info.params)
            if (pi.name == n) addParamRow(form, pi, target);
}

std::vector<Id> InspectorWidget::otherSelected(const Target& target) const {
    std::vector<Id> out;
    const Sequence* s = state_->sequence();
    if (!target.clip || !s) return out;
    const auto here = edit::locate(*s, target.clip);
    for (Id id : state_->selectedClips()) {
        const auto loc = edit::locate(*s, id);
        if (id != target.clip && here && loc && loc->track.kind == here->track.kind) out.push_back(id);
    }
    return out;
}

void InspectorWidget::applyToOthers(Sequence& s, const Target& target, const std::vector<Id>& others, FrameTime playhead,
                                    const std::function<void(Effect&, FrameTime)>& fn) const {
    Clip* from = target.clip ? edit::clipById(s, target.clip) : nullptr;
    if (!from) return;
    for (Id id : others) {
        Clip* c = edit::clipById(s, id);
        if (!c || c == from) continue;
        if (Effect* e = matchingEffect(*from, target.effect, *c))
            fn(*e, std::clamp<FrameTime>(playhead - c->start, 0, std::max<FrameTime>(0, c->duration - 1)));
    }
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
        const std::vector<Id> others = otherSelected(target);
        const FrameTime playhead = state_->playhead();
        state_->edit(tr("Change %1").arg(label), [this, target, values, t, others, playhead](Project&, Sequence& s) {
            Effect* e = target.resolve(s);
            if (!e) return false;
            for (const auto& [n, v] : values) e->params[n].set(t, v);
            applyToOthers(s, target, others, playhead, [&](Effect& o, FrameTime ot) {
                for (const auto& [n, v] : values) o.params[n].set(ot, v);
            });
            if (target.afterWrite) target.afterWrite(s);
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
                if (target.afterWrite) target.afterWrite(s);
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
                if (target.afterWrite) target.afterWrite(s);
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
            enableSpellCheck(edit, state_, [] { return titleSpellingLanguage(); });
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
            const QString filter = si.fileFilter.empty() ? tr("All files (*)") : QString::fromStdString(si.fileFilter);
            connect(browse, &QToolButton::clicked, this, [this, line, write, label, filter] {
                QString f = QFileDialog::getOpenFileName(this, label, line->text(), filter);
                if (!f.isEmpty()) write(f);
            });
            refreshers_.push_back([line, read] {
                if (!line->hasFocus()) line->setText(read());
            });
            break;
        }
        case StringKind::HueCurve:
        case StringKind::LevelCurve: {
            // Shaped by hand: a flat line changes nothing.
            auto* box = new QWidget(content_);
            auto* v = new QVBoxLayout(box);
            v->setContentsMargins(0, 0, 0, 0);
            auto* editor = new CurveEditor(si.kind == StringKind::HueCurve ? CurveEditor::Mode::Hue : CurveEditor::Mode::Level, box);
            editor->setObjectName(QString::fromStdString("curve_" + name));
            auto* reset = new QToolButton(box);
            reset->setText(tr("Reset"));
            v->addWidget(editor);
            v->addWidget(reset, 0, Qt::AlignRight);
            form->addRow(label, box);
            connect(editor, &CurveEditor::edited, this, [write](const QString& pts, bool) { write(pts); });
            connect(reset, &QToolButton::clicked, this, [editor, write] {
                editor->setPoints(QString());
                write(QString());
            });
            refreshers_.push_back([editor, read] {
                if (!editor->dragging() && editor->points() != read()) editor->setPoints(read());
            });
            break;
        }
        case StringKind::ColorWarp: {
            // The mesh to drag, the selected point's brightness, and putting points back.
            auto* box = new QWidget(content_);
            auto* v = new QVBoxLayout(box);
            v->setContentsMargins(0, 0, 0, 0);
            auto* editor = new ColorWarperEditor(box);
            editor->setObjectName(QString::fromStdString("warp_" + name));
            auto* row = new QWidget(box);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            auto* buttons = new QWidget(box);
            auto* bh = new QHBoxLayout(buttons);
            bh->setContentsMargins(0, 0, 0, 0);
            auto* luma = new QDoubleSpinBox(row);
            luma->setObjectName(QStringLiteral("warpLuma"));
            luma->setRange(-2, 2);
            luma->setSingleStep(0.05);
            luma->setDecimals(2);
            luma->setSuffix(tr(" stops"));
            luma->setToolTip(tr("Brighten or darken the selected point's colours"));
            luma->setEnabled(false);
            auto* resetPoint = new QToolButton(buttons);
            resetPoint->setText(tr("Reset Point"));
            resetPoint->setEnabled(false);
            auto* resetAll = new QToolButton(buttons);
            resetAll->setText(tr("Reset All"));
            resetAll->setObjectName(QStringLiteral("warpResetAll"));
            h->addWidget(new QLabel(tr("Selected point's brightness:"), row));
            h->addWidget(luma);
            h->addStretch(1);
            bh->addStretch(1);
            bh->addWidget(resetPoint);
            bh->addWidget(resetAll);
            // The wheel across the whole Inspector, its title above it.
            v->addWidget(new QLabel(label, box));
            v->addWidget(editor);
            v->addWidget(row);
            v->addWidget(buttons);
            form->addRow(box);
            connect(editor, &ColorWarperEditor::edited, this, [write](const QString& mesh, bool) { write(mesh); });
            connect(editor, &ColorWarperEditor::selectionChanged, this, [editor, luma, resetPoint] {
                const bool on = editor->selectedSpoke() >= 0;
                QSignalBlocker b(luma);
                luma->setEnabled(on);
                resetPoint->setEnabled(on);
                luma->setValue(on ? editor->warp().at(editor->selectedSpoke(), editor->selectedRing()).dl : 0.0);
            });
            connect(luma, &QDoubleSpinBox::valueChanged, this, [editor](double stops) { editor->setSelectedLuma(stops); });
            connect(resetPoint, &QToolButton::clicked, editor, &ColorWarperEditor::resetSelected);
            connect(resetAll, &QToolButton::clicked, this, [editor, write] {
                editor->setMesh(QString());
                write(QString());
            });
            refreshers_.push_back([editor, read] {
                if (!editor->dragging() && editor->mesh() != read()) editor->setMesh(read());
            });
            break;
        }
        case StringKind::Curve: {
            // The curve to drag, and its points as text below it.
            auto* editor = new CurveEditor(CurveEditor::Mode::Tone, content_);
            editor->setObjectName(QString::fromStdString("curve_" + name));
            form->addRow(label, editor);
            connect(editor, &CurveEditor::edited, this, [write](const QString& pts, bool) { write(pts); });
            refreshers_.push_back([editor, read] {
                const QString pts = read().isEmpty() ? QStringLiteral("0,0 1,1") : read();
                if (!editor->dragging() && editor->points() != pts) editor->setPoints(pts);
            });
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
            form->addRow(QString(), row);
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
        case StringKind::Dynamic: {
            // Entries come from the effect's other settings (the OCIO config's spaces, displays...).
            auto* combo = new QComboBox(content_);
            combo->setEditable(true);
            combo->setInsertPolicy(QComboBox::NoInsert);
            combo->setObjectName(QString::fromStdString("dynamic_" + name));
            form->addRow(label, combo);
            connect(combo, &QComboBox::textActivated, this, write);
            refreshers_.push_back([this, combo, read, target, name] {
                if (combo->hasFocus() || (combo->view() && combo->view()->isVisible())) return;
                QStringList items;
                if (const Sequence* s = state_->sequence())
                    if (const Effect* e = target.resolve(const_cast<Sequence&>(*s)); e && e->type == "ocio")
                        for (const auto& c : ocioChoices(*e, name)) items << QString::fromStdString(c);
                QSignalBlocker b(combo);
                QStringList current;
                for (int i = 0; i < combo->count(); ++i) current << combo->itemText(i);
                if (current != items) {
                    combo->clear();
                    combo->addItems(items);
                }
                combo->setCurrentText(read());
            });
            break;
        }
        case StringKind::Track: {
            // An audio track (a sidechain key): None, then the sequence's audio tracks by name; the track's id is kept.
            auto* combo = new QComboBox(content_);
            combo->setObjectName(QString::fromStdString("track_" + name));
            form->addRow(label, combo);
            connect(combo, &QComboBox::activated, this, [combo, write](int i) { write(combo->itemData(i).toString()); });
            refreshers_.push_back([this, combo, read, target] {
                QSignalBlocker b(combo);
                combo->clear();
                combo->addItem(tr("None"), QString());
                if (const Sequence* s = state_->sequence()) {
                    // Not the track the effect is on (a key cannot be its own signal): the track itself, or the clip's.
                    Id own = target.owner;
                    for (Id clip : {target.clip, target.owner})
                        if (const auto loc = clip ? edit::locate(*s, clip) : std::nullopt)
                            own = s->audioTracks.size() > size_t(loc->track.index) && loc->track.kind == TrackKind::Audio
                                      ? s->audioTracks[size_t(loc->track.index)].id
                                      : own;
                    for (size_t k = 0; k < s->audioTracks.size(); ++k) {
                        const Track& t = s->audioTracks[k];
                        if (t.id == own) continue;
                        combo->addItem(QStringLiteral("A%1  %2").arg(k + 1).arg(QString::fromStdString(t.name)), QString::number(t.id));
                    }
                }
                const int at = combo->findData(read());
                combo->setCurrentIndex(at < 0 ? 0 : at);
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

bool InspectorWidget::runAnalysis(
    const QString& title, const std::function<bool(const std::function<void(double)>&, const std::atomic<bool>*, std::string*)>& work) {
    QProgressDialog progress(title, tr("Cancel"), 0, 1000, window());
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(400);
    auto done = std::make_shared<std::atomic<double>>(0.0);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    connect(&progress, &QProgressDialog::canceled, this, [cancel] { *cancel = true; });
    QTimer tick;
    connect(&tick, &QTimer::timeout, &progress, [&progress, done] { progress.setValue(int(*done * 1000)); });
    tick.start(100);
    using Out = std::pair<bool, std::string>;
    QFutureWatcher<Out> watcher;
    QEventLoop wait;
    connect(&watcher, &QFutureWatcher<Out>::finished, &wait, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([work, done, cancel] {
        std::string err;
        const bool ok = work([done](double f) { *done = f; }, cancel.get(), &err);
        return Out{ok, err};
    }));
    if (!watcher.isFinished()) wait.exec();
    tick.stop();
    progress.disconnect(this);  // closing a progress dialog emits canceled()
    progress.close();
    const Out r = watcher.result();
    if (!r.first && !*cancel && !r.second.empty()) state_->message(QString::fromStdString(r.second), 6000);
    return r.first && !*cancel;
}

void InspectorWidget::analyzeStabilize(Id clip, Id effect) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    if (!c) return;
    // The analysis reads a copy: editing can go on meanwhile.
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    std::string motion;
    const bool ok = runAnalysis(tr("Analysing camera movement..."), [&, project, seqId, clip](const auto& progress, const auto* cancel, std::string* err) {
        const Sequence* sq = project->findSequence(seqId);
        const Clip* cl = sq ? edit::clipById(*sq, clip) : nullptr;
        return cl && analyzeClipStabilization(*project, *sq, *cl, motion, progress, cancel, err);
    });
    if (!ok) return;
    state_->edit(tr("Analyze Stabilization"), [clip, effect, motion](Project&, Sequence& sq) {
        Effect* e = edit::ownedEffect(sq, clip, effect);
        if (!e) return false;
        e->strings["motion"] = motion;
        return true;
    });
}

void InspectorWidget::trackMask(Id clip, Id effect, bool forward, int model) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    const Effect* e = s ? edit::ownedEffect(const_cast<Sequence&>(*s), clip, effect) : nullptr;
    if (!c || !e) return;
    if (!hasMask(*e, state_->playhead() - c->start)) {
        state_->message(tr("Draw a mask first (choose a shape), then track it"), 5000);
        return;
    }
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    const FrameTime from = std::clamp<FrameTime>(state_->playhead() - c->start, 0, c->duration - 1);
    const MotionModel m = model == 0 ? MotionModel::Translation : model == 1 ? MotionModel::TranslationScale : MotionModel::Similarity;
    std::vector<std::pair<FrameTime, TrackRegion>> keys;
    const bool ok = runAnalysis(tr("Tracking the mask..."), [&, project, seqId](const auto& progress, const auto* cancel, std::string* err) {
        const Sequence* sq = project->findSequence(seqId);
        const Clip* cl = sq ? edit::clipById(*sq, clip) : nullptr;
        const Effect* ef = sq ? edit::ownedEffect(const_cast<Sequence&>(*sq), clip, effect) : nullptr;
        return cl && ef && trackClipMask(*project, *sq, *cl, *ef, from, forward, m, keys, progress, cancel, err);
    });
    if (!ok) return;
    state_->edit(tr("Track Mask"), [clip, effect, keys](Project&, Sequence& sq) {
        Effect* ef = edit::ownedEffect(sq, clip, effect);
        if (!ef) return false;
        applyMaskTrack(*ef, keys);
        return true;
    });
    state_->message(tr("Tracked %n frame(s)", "", int(keys.size())), 4000);
}

void InspectorWidget::trackCorners(Id clip, Id effect, bool forward) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    const Effect* e = s ? edit::ownedEffect(const_cast<Sequence&>(*s), clip, effect) : nullptr;
    if (!c || !e) return;
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    const FrameTime from = std::clamp<FrameTime>(state_->playhead() - c->start, 0, c->duration - 1);
    std::vector<std::pair<FrameTime, TrackQuad>> keys;
    const bool ok = runAnalysis(tr("Tracking the surface..."), [&, project, seqId](const auto& progress, const auto* cancel, std::string* err) {
        const Sequence* sq = project->findSequence(seqId);
        const Clip* cl = sq ? edit::clipById(*sq, clip) : nullptr;
        const Effect* ef = sq ? edit::ownedEffect(const_cast<Sequence&>(*sq), clip, effect) : nullptr;
        return cl && ef && trackClipCorners(*project, *sq, *cl, *ef, from, forward, keys, progress, cancel, err);
    });
    if (!ok) return;
    state_->edit(tr("Track Corners"), [clip, effect, keys](Project&, Sequence& sq) {
        Effect* ef = edit::ownedEffect(sq, clip, effect);
        if (!ef) return false;
        applyCornerTrack(*ef, keys);
        return true;
    });
    state_->message(tr("Tracked %n frame(s)", "", int(keys.size())), 4000);
}

void InspectorWidget::followFootage(Id clip, bool forward, int model, double size) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    if (!c) return;
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    const FrameTime from = std::clamp<FrameTime>(state_->playhead() - c->start, 0, c->duration - 1);
    const MotionModel m = model == 0 ? MotionModel::Translation : model == 1 ? MotionModel::TranslationScale : MotionModel::Similarity;
    std::vector<FollowKey> keys;
    const bool ok = runAnalysis(tr("Following the footage..."), [&, project, seqId](const auto& progress, const auto* cancel, std::string* err) {
        const Sequence* sq = project->findSequence(seqId);
        const Clip* cl = sq ? edit::clipById(*sq, clip) : nullptr;
        return cl && trackClipFollow(*project, *sq, *cl, from, forward, m, size, keys, progress, cancel, err);
    });
    if (!ok) return;
    state_->edit(tr("Follow"), [clip, keys, m](Project&, Sequence& sq) {
        Clip* cl = edit::clipById(sq, clip);
        if (!cl) return false;
        applyFollow(*cl, keys, m);
        return true;
    });
    state_->message(tr("Followed for %n frame(s)", "", int(keys.size())), 4000);
}

void InspectorWidget::trackObject(Id clip, Id effect, bool forward) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    const Effect* e = s ? edit::ownedEffect(const_cast<Sequence&>(*s), clip, effect) : nullptr;
    if (!c || !e) return;
    if (!e->object || e->object->prompts.empty()) {
        state_->message(tr("Click the object in the viewer first, then track it"), 5000);
        return;
    }
    if (!ensureObjectModel(window())) return;
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    const FrameTime from = std::clamp<FrameTime>(state_->playhead() - c->start, 0, c->duration - 1);
    std::shared_ptr<const ObjectMask> result;
    const bool ok = runAnalysis(tr("Tracking the object..."), [&, project, seqId](const auto& progress, const auto* cancel, std::string* err) {
        const Sequence* sq = project->findSequence(seqId);
        const Clip* cl = sq ? edit::clipById(*sq, clip) : nullptr;
        const Effect* ef = sq ? edit::ownedEffect(const_cast<Sequence&>(*sq), clip, effect) : nullptr;
        if (!cl || !ef || !ef->object) return false;
        result = trackClipObject(*project, *sq, *cl, *ef->object, from, forward, progress, cancel, err);
        return result != nullptr;
    });
    if (!ok) return;
    state_->edit(tr("Track Object"), [clip, effect, result](Project&, Sequence& sq) {
        Effect* ef = edit::ownedEffect(sq, clip, effect);
        if (!ef) return false;
        ef->object = result;
        return true;
    });
    state_->message(tr("Tracked the object: %n frame(s) segmented", "", int(result->frames.size())), 4000);
}

}  // namespace montage
