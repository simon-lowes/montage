#include "InspectorWidget.h"
#include "Language.h"
#include "SpellUi.h"
#include "core/ColorGroups.h"
#include "core/CompositeGraph.h"
#include "core/GradeGraph.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QStandardItemModel>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
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
#include "LensCorrectionDialog.h"
#include "ModelPacks.h"
#include "render/Expressions.h"
#include "render/ModelClip.h"
#include "render/Titles.h"
#include "media/DepthMap.h"
#include "media/VideoDepth.h"
#include "media/Exr.h"
#include "media/Faces.h"
#include "media/Matting.h"
#include "media/MotionData.h"
#include "media/Rife.h"
#include "PluginEditorWindow.h"
#include "Theme.h"
#include "audio/PluginEffect.h"
#include "core/ClipAnimation.h"
#include "core/Expression.h"
#include "core/GradeVersions.h"
#include "core/EditOps.h"
#include "core/GraphicsTemplate.h"
#include "core/MaskPath.h"
#include "ColorWheel.h"
#include "ColorWarperEditor.h"
#include "CurveEditor.h"
#include "render/ClipAnalysis.h"
#include "render/ColorSlice.h"
#include "render/ColorSpace.h"
#include "render/Processing.h"
#include "render/Compositor.h"
#include "render/OGraf.h"
#include "render/Restoration.h"
#include "render/FilmScan.h"
#include "render/MeshWarp.h"
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
    connect(state_, &EditorState::maskObjectChanged, this, &InspectorWidget::refreshValues);
    connect(state_, &EditorState::gradeNodeSelected, this, &InspectorWidget::rebuild);
    connect(state_, &EditorState::compositeNodeSelected, this, &InspectorWidget::rebuild);
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
        QString sig = QString("X%1").arg(state_->inspectedChain()) + (sceneReferredWorking(*s) ? QStringLiteral(":scene") : QString());
        for (const auto& e : *chain) sig += QString(":%1%2").arg(e.id).arg(e.enabled ? "+" : "-");
        return sig;
    }
    QString sig = QString("C%1:%2:%3").arg(c->id).arg(QString::fromStdString(c->generator.type)).arg(c->mediaId);
    if (sceneReferredWorking(*s)) sig += QStringLiteral(":scene");  // (effects that work on display values are marked)
    // An OGraf graphic's fields: rebuild when another graphic is chosen.
    if (c->generator.type == "ograf") sig += "|G" + QString::fromStdString(c->generator.s("manifest"));
    // A title's text animations: rebuild when one is added or removed.
    if (c->generator.type == "title")
        for (int k : textAnimators(c->generator)) sig += QString("|A%1").arg(k);
    // A graphic's fields: rebuild when one is published, removed or renamed.
    if (const Sequence* g = graphicOf(state_->project(), *c)) {
        sig += QString("|F%1").arg(g->id);
        for (const TemplateField& f : liveFields(*g))
            sig += QString(":%1/%2/%3").arg(QString::fromStdString(f.id), QString::fromStdString(f.kind), QString::fromStdString(f.label));
    }
    // The OpenEXR layer it shows (a missing one is listed only while chosen).
    sig += "|L" + QString::fromStdString(c->layer);
    // Object, Bézier and Cryptomatte masks have their own controls: rebuild when one is chosen.
    for (const auto& e : c->effects) {
        const long shape = std::lround(e.p("mask.shape", 0));
        sig += QString(":%1%2%3n%4").arg(e.id).arg(e.enabled ? "+" : "-").arg(shape == 3 ? "o" : shape == 5 ? "p" : shape == 7 ? "c" : "").arg(e.node);
        // A film scan's status line: rebuild when its reference or base is taken.
        if (e.type == "perf_lock") sig += QString("r%1").arg(perforationsFromString(e.s("reference"), 1, 1).size());
        if (e.type == "film_negative") sig += QString("b%1").arg(std::lround(e.p("base_from", 0)));
        // A Mesh Warp's pins and a Grid Warp's points: rebuild when a pin is added or removed, or the grid resized.
        if (e.type == "mesh_warp")
            for (int k : meshPins(e)) sig += QString("p%1k%2").arg(k).arg(meshPinKind(e, k));
        if (e.type == "grid_warp") sig += QString("g%1x%2").arg(std::lround(e.p("rows", 0, 3))).arg(std::lround(e.p("cols", 0, 3)));
    }
    // Its colour nodes: rebuild when one is added, picked, renamed or shared, or a shared one's effects change.
    if (!c->grade.empty()) {
        sig += QString("|N%1").arg(state_->selectedGradeNode());
        for (const GradeNode& n : c->grade.nodes) {
            sig += QString(":%1%2%3%4/%5").arg(n.key).arg(QString::fromStdString(n.kind)).arg(n.enabled ? "+" : "-")
                       .arg(QString::fromStdString(n.label)).arg(n.shared);
            for (const std::string& b : n.blend) sig += QString::fromStdString(b) + ',';
            if (const SharedNode* sn = findSharedNode(*s, n.shared))
                for (const auto& e : sn->effects) sig += QString(";%1%2").arg(e.id).arg(e.enabled ? "+" : "-");
        }
    }
    // Its compositing nodes: rebuild when one is added, removed, picked, renamed or switched, or its effects change.
    if (!c->composite.empty()) {
        sig += QString("|K%1").arg(state_->selectedCompositeNode());
        for (const CompositeNode& n : c->composite.nodes) {
            sig += QString(":%1%2%3%4/%5").arg(n.key).arg(QString::fromStdString(n.kind)).arg(n.enabled ? "+" : "-")
                       .arg(QString::fromStdString(n.label)).arg(QString::fromStdString(n.source + n.generator.type));
            for (const auto& e : n.effects) sig += QString(";%1%2").arg(e.id).arg(e.enabled ? "+" : "-");
        }
    }
    // Its colour groups: rebuild when it joins or leaves one, or a group's effects change.
    for (const ColorGroup* g : colorGroupsOf(*s, *c)) {
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
        // (A clip's sound is mixed as it is inside a graphic: only what it shows is published.)
        t.publishClip = kind == TrackKind::Video && effectId != clip.audio.id ? clipId : 0;
        return t;
    };

    if (kind == TrackKind::Video) addGraphicSection(clip);

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
        // A multi-layer OpenEXR render: which of its layers (AOVs) the clip shows.
        const MediaItem* em = clip.mediaId ? state_->project().findMedia(clip.mediaId) : nullptr;
        const auto exr = em && exrAvailable() ? cachedExrInfo(exrFrameFile(em->path, 0)) : nullptr;
        if (exr && (exr->layers.size() > 1 || !exr->hasLayer(clip.layer))) {
            auto* layers = new QComboBox(content_);
            layers->setObjectName(QStringLiteral("exrLayer"));
            layers->setToolTip(tr("The layer of this OpenEXR render the clip shows"));
            for (const ExrLayer& l : exr->layers)
                layers->addItem(l.name.empty() ? tr("RGBA (main)") : QString::fromStdString(l.name), QString::fromStdString(l.name));
            if (!exr->hasLayer(clip.layer)) layers->addItem(tr("%1 (missing)").arg(QString::fromStdString(clip.layer)), QString::fromStdString(clip.layer));
            form->addRow(tr("Layer"), layers);
            connect(layers, &QComboBox::activated, this, [this, clipId, layers](int idx) {
                const std::string layer = layers->itemData(idx).toString().toStdString();
                state_->edit(tr("Choose Layer"), [clipId, layer](Project&, Sequence& s) {
                    Clip* c = edit::clipById(s, clipId);
                    if (!c || c->layer == layer) return false;
                    c->layer = layer;
                    return true;
                });
            });
            refreshers_.push_back([=, this] {
                const Clip* c = clipNow();
                if (!c) return;
                QSignalBlocker b(layers);
                layers->setCurrentIndex(std::max(0, layers->findData(QString::fromStdString(c->layer))));
            });
        }
    }
    refreshers_.push_back([=, this] {
        const Clip* c = clipNow();
        if (!c) return;
        const Sequence* s = state_->sequence();
        if (!name->hasFocus()) name->setText(QString::fromStdString(c->name));
        timing->setText(tr("%1 → %2  (%3)").arg(timecodeString(s, c->start), timecodeString(s, c->end()), durationString(s, c->duration)));
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
            QFormLayout* g = addSection(language::engine(info->displayName));
            addParamRows(g, *info, target(clip.generator.id));
            if (clip.generator.type == "ograf") addOGrafFields(g, clip, target(clip.generator.id));
            if (clip.generator.type == "title") addTitleTextTools(g, clip, target(clip.generator.id));
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
    // ---- Effect stack (in colour nodes, when the clip has them) -------------------------
    if (kind == TrackKind::Video && !clip.grade.empty()) buildGradeNodes(clip, localTime);
    else buildEffectStack(clipId, kind, clip.effects, localTime);
    // ---- Its compositing node graph's picked node -----------------------------------------
    if (kind == TrackKind::Video && !clip.composite.empty()) buildCompositeNode(clip, localTime);
    // ---- Its colour group's grades, before and after its own (core/ColorGroups.h) --------
    const Sequence* seqNow = state_->sequence();
    // (a scene group's around its angle group's, as they run; copies: building can change the project)
    std::vector<std::pair<ColorGroup, int>> shown;
    if (kind == TrackKind::Video && seqNow)
        for (const ColorGroup* g : colorGroupsOf(*seqNow, clip)) shown.push_back({*g, int(colorGroupMembers(*seqNow, g->id, true).size())});
    for (size_t gi = 0; gi < shown.size(); ++gi) {
        const ColorGroup& g = shown[gi].first;
        const int members = shown[gi].second;
        const QString name = QString::fromStdString(g.name);
        const bool inner = gi + 1 == shown.size();  // (the clip's own group; the others are around it)
        for (int stage = 0; stage < 2; ++stage) {
            QFormLayout* f = addSection(stage == 0 ? tr("Group Pre-Clip: %1").arg(name) : tr("Group Post-Clip: %1").arg(name));
            auto* note = new QLabel(stage == 0 ? tr("Runs on each of the group's %n clip(s) before its own effects, to match the shots.", "", members)
                                               : tr("Runs on each of the group's %n clip(s) after its own effects: the group's look.", "", members),
                                    content_);
            note->setObjectName(QString(inner ? "colorGroup%1" : "outerColorGroup%1").arg(stage == 0 ? "Pre" : "Post"));
            note->setWordWrap(true);
            note->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
            f->addRow(note);
            buildEffectStack(stage == 0 ? g.id : g.postId, TrackKind::Video, stage == 0 ? g.pre : g.post, localTime);
        }
    }
}

void InspectorWidget::buildGradeNodes(const Clip& clip, const std::function<FrameTime()>& localTime) {
    const Sequence* s = state_->sequence();
    if (!s) return;
    const Id clipId = clip.id;
    const GradeGraph graph = clip.grade;  // a copy: building can change the project
    const std::vector<Effect> effects = clip.effects;
    const int picked = state_->selectedGradeNode();
    // The nodes in the order they run, then any the output does not reach.
    std::vector<int> keys = gradeOrder(graph);
    for (const GradeNode& n : graph.nodes)
        if (std::find(keys.begin(), keys.end(), n.key) == keys.end()) keys.push_back(n.key);
    auto note = [this](QFormLayout* f, const QString& text, const char* name) {
        auto* l = new QLabel(text, content_);
        l->setObjectName(QString::fromLatin1(name));
        l->setWordWrap(true);
        l->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
        f->addRow(l);
    };
    if (!picked) {
        QFormLayout* f = addSection(tr("Colour Nodes"));
        note(f, tr("%n node(s). Pick one in the Nodes panel to see only its effects.", "", int(graph.nodes.size())), "gradeNodesNote");
    }
    for (int key : keys) {
        if (picked && key != picked) continue;
        const GradeNode* n = findGradeNode(graph, key);
        const QString name = QString::fromStdString(gradeNodeName(*n));
        if (n->kind == "corrector") {
            QFormLayout* f = addSection(n->label.empty() ? name : tr("%1 (Node %2)").arg(name).arg(key));
            if (!n->enabled) note(f, tr("Switched off: it passes its picture on unchanged."), "gradeNodeOff");
            const SharedNode* sn = findSharedNode(*s, n->shared);
            if (sn)
                note(f, tr("Shared node \"%1\": a change here is seen on all %n clip(s) using it.", "", sharedNodeUsers(*s, sn->id))
                            .arg(QString::fromStdString(sn->name)), "gradeNodeShared");
            if (!gradeNodeSettings(clip, key)) {
                // Its window, qualifier, key input and mix: made when first wanted.
                auto* more = new QPushButton(tr("Window, Key and Mix"), content_);
                more->setObjectName(QStringLiteral("gradeNodeSettings"));
                more->setToolTip(tr("Limit this node to a window, a qualifier or its key input, or mix it with its input"));
                connect(more, &QPushButton::clicked, this, [this, clipId, key] {
                    QTimer::singleShot(0, this, [this, clipId, key] {
                        state_->apply(tr("Node Settings"), [clipId, key](Project& p, Sequence& sq) {
                            return edit::setGradeNodeParams(p, sq, clipId, key, {{"mix", 100.0}});
                        });
                    });
                });
                f->addRow(QString(), more);
            }
            if (sn) {
                // Its settings (the clip's own: effects go into the shared node, below).
                buildEffectStack(clipId, TrackKind::Video, effects, localTime, key, false);
                const std::vector<Effect> shared = sn->effects;
                buildEffectStack(sn->id, TrackKind::Video, shared, localTime);
            } else {
                buildEffectStack(clipId, TrackKind::Video, effects, localTime, key);
            }
        } else {
            QFormLayout* f = addSection(name);
            if (n->kind == "parallel") note(f, tr("Each input where its key is (a node's window or qualifier), or their average."), "gradeMixerNote");
            else if (n->kind == "key_mixer") note(f, tr("Joins the keys coming into it."), "gradeMixerNote");
            else note(f, tr("Each input over those before it, with its blend mode, where its key is."), "gradeMixerNote");
            for (size_t i = 0; n->kind == "layer" && i < n->blend.size(); ++i) {
                auto* mode = new QComboBox(content_);
                mode->setObjectName(QStringLiteral("layerBlend%1").arg(i + 1));
                for (const std::string& b : blendModes()) mode->addItem(QString::fromStdString(b));
                mode->setCurrentText(QString::fromStdString(n->blend[i]));
                f->addRow(tr("Input %1:").arg(i + 2), mode);
                connect(mode, &QComboBox::currentTextChanged, this, [this, clipId, key, i](const QString& b) {
                    state_->apply(tr("Layer Blend Mode"), [clipId, key, i, b](Project&, Sequence& sq) {
                        const Clip* c = edit::clipById(sq, clipId);
                        const GradeNode* m = c ? findGradeNode(c->grade, key) : nullptr;
                        if (!m || i >= m->blend.size()) return edit::Result::fail("");
                        GradeNode changed = *m;
                        changed.blend[i] = b.toStdString();
                        return edit::setGradeNode(sq, clipId, changed);
                    });
                });
            }
        }
    }
    // What runs outside the graph, after it.
    if (!picked) {
        addSection(tr("Outside the Nodes"));
        buildEffectStack(clipId, TrackKind::Video, effects, localTime, 0);
    }
}

void InspectorWidget::buildCompositeNode(const Clip& clip, const std::function<FrameTime()>& localTime) {
    const int picked = state_->selectedCompositeNode();
    const CompositeNode* found = findCompositeNode(clip.composite, picked);
    auto note = [this](QFormLayout* f, const QString& text, const char* name) {
        auto* l = new QLabel(text, content_);
        l->setObjectName(QString::fromLatin1(name));
        l->setWordWrap(true);
        l->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
        f->addRow(l);
    };
    if (!found) {
        QFormLayout* f = addSection(tr("Composite"));
        note(f, tr("%n compositing node(s) make this clip's picture. Pick one in the Composite panel to see its settings.", "",
                   int(clip.composite.nodes.size())),
             "compositeNote");
        return;
    }
    const CompositeNode n = *found;  // a copy: building can change the project
    QFormLayout* f = addSection(tr("Composite: %1").arg(QString::fromStdString(compositeNodeName(n))));
    if (!n.enabled) note(f, tr("Switched off: it passes its first input on unchanged."), "compositeNodeOff");
    if (n.kind == "grade" && !n.grade.empty())
        note(f, tr("Grades in its %n colour node(s), as copied from a clip.", "", int(n.grade.nodes.size())), "compositeGradeNote");
    if (n.kind == "input" && n.source == "generator") {
        // The generator's own settings (the title's text, the model's file...).
        if (const EffectInfo* info = findEffectInfo(n.generator.type)) {
            Target t;
            const Id owner = n.id, effectId = n.generator.id;
            t.resolve = [owner, effectId](Sequence& s) -> Effect* { return edit::ownedEffect(s, owner, effectId); };
            t.time = localTime;
            t.origin = [this, owner]() -> FrameTime {
                FrameTime origin = 0;
                if (const Sequence* s = state_->sequence()) edit::effectChain(const_cast<Sequence&>(*s), owner, &origin);
                return origin;
            };
            t.key = QString("p%1:%2").arg(owner).arg(effectId);
            t.owner = owner;
            t.effect = effectId;
            QFormLayout* g = addSection(language::engine(info->displayName));
            addParamRows(g, *info, t);
        }
    }
    // Its settings and effects (a merge, mask or track node keeps only its settings).
    const bool more = n.kind != "merge" && n.kind != "mask" && n.kind != "track";
    buildEffectStack(n.id, TrackKind::Video, n.effects, localTime, -1, more);
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
                                       const std::function<FrameTime()>& localTime, int node, bool addMenu) {
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
        t.effect = effectId;
        // A picture clip's effects can be published (not a track's, bus's or colour group's).
        if (const Sequence* s = state_->sequence(); kind == TrackKind::Video && s && edit::clipById(*s, owner)) t.publishClip = owner;
        return t;
    };
    for (size_t i = 0; i < effects.size(); ++i) {
        const Effect& e = effects[i];
        const EffectInfo* catalog = findEffectInfo(e.type);
        if (!catalog || (node >= 0 && e.node != node)) continue;
        // Plugin effects carry their own parameter list and name.
        EffectInfo shown = *catalog;
        shown.displayName = plugins::effectName(e);
        if (e.type == "grade_node") shown.displayName = tr("Window, Key and Mix").toStdString();  // a colour node's own
        // (as shown: a plug-in's own name and a colour node's as they are, a built-in effect's in the interface's language)
        const QString title = e.type == "plugin" || e.type == "ara" || e.type == "ofx" || e.type == "grade_node"
                                  ? QString::fromStdString(shown.displayName)
                                  : language::engine(shown.displayName);
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
        if (e.type == "plugin" || e.type == "ara") {
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
        QFormLayout* f = addSection(title, tools);
        if (state_->sequence() && sceneReferredWorking(*state_->sequence()) && effectDomain(e.type) == EffectDomain::Display) {
            // (it cannot work on scene light: it gets the picture as the display shows it, and gives it back)
            auto* note = new QLabel(tr("On the displayed picture: graded scene-referred, this effect works through the output "
                                       "transform and back, so it does not see light above the display's peak"), content_);
            note->setObjectName(QStringLiteral("displayDomain"));
            note->setWordWrap(true);
            note->setStyleSheet(QString("color: %1;").arg(theme::kTextDim.name()));
            f->addRow(note);
        }
        if (e.type == "color_correct") addColorWheels(f, target(eid));
        if (e.type == "hdr_palette") addHdrPalette(f, *info, target(eid));
        else if (e.type == "color_slice") addColorSlice(f, *info, target(eid));
        else addParamRows(f, *info, target(eid));
        const bool onClip = state_->sequence() && edit::clipById(*state_->sequence(), owner);
        if ((e.type == "perf_lock" || e.type == "film_negative") && onClip) {
            // What the effect holds to (the perforations' places, the film base), taken from a frame on demand.
            auto* row = new QWidget(content_);
            auto* rh = new QHBoxLayout(row);
            rh->setContentsMargins(0, 0, 0, 0);
            auto* status = new QLabel(row);
            status->setObjectName(QStringLiteral("filmScanStatus"));
            status->setWordWrap(true);
            QPushButton* take = nullptr;
            if (e.type == "perf_lock") {
                const int held = int(perforationsFromString(e.s("reference"), 1, 1).size());
                status->setText(held ? tr("Holding the %n perforation(s) of the reference frame", "", held)
                                     : tr("No reference yet: the frame is left as it is"));
                take = new QPushButton(tr("Set Reference From This Frame"), row);
                take->setObjectName(QStringLiteral("perfReference"));
                take->setToolTip(tr("Hold every frame's perforations where they are on the frame under the playhead"));
            } else {
                status->setText(e.p("base_from", 0) > 0.5 ? tr("Film base set below") : tr("Film base found in each frame's rebate"));
                take = new QPushButton(tr("Take the Base From This Frame"), row);
                take->setObjectName(QStringLiteral("filmBaseFromFrame"));
                take->setToolTip(tr("Set the film base from the rebate of the frame under the playhead, for every frame"));
            }
            rh->addWidget(status, 1);
            rh->addWidget(take);
            f->addRow(QString(), row);
            connect(take, &QPushButton::clicked, this, [this, owner, eid] { QTimer::singleShot(0, this, [this, owner, eid] { takeFromFrame(owner, eid); }); });
        }
        if ((e.type == "stabilize" || e.type == "rolling_shutter" || e.type == "sky_replace") && onClip) {
            // The analysis lives in the effect; it is redone on demand.
            auto* row = new QWidget(content_);
            auto* rh = new QHBoxLayout(row);
            rh->setContentsMargins(0, 0, 0, 0);
            auto* status = new QLabel(row);
            status->setObjectName(QStringLiteral("stabilizeStatus"));
            status->setWordWrap(true);
            CameraMotion cm;
            status->setText(cameraMotionFromString(e.s("motion"), cm) ? tr("Analysed: %n frame(s)", "", int(cm.steps.size()))
                                                                      : tr("Not analysed yet"));
            if (e.type == "stabilize") {
                // Camera Gyro is offered once the analysis has found the camera's motion data and lined it up.
                std::string source;
                double focal = 0;
                const bool gyro = gyroPathSummary(e.s("gyro"), source, focal);
                if (gyro)
                    status->setText(status->text() + tr("; camera motion data: %1, %2° lens")
                                                         .arg(QString::fromStdString(source))
                                                         .arg(2 * std::atan(0.5 / focal) * 180 / M_PI, 0, 'f', 0));
                if (QWidget* section = f->parentWidget())
                    if (auto* method = section->findChild<QComboBox*>(QStringLiteral("choice_method")))
                        if (auto* model = qobject_cast<QStandardItemModel*>(method->model()))
                            if (QStandardItem* item = model->item(3)) {
                                item->setEnabled(gyro);
                                item->setToolTip(gyro ? tr("Turn each frame by the camera's own motion data")
                                                      : tr("Analyze a clip with camera motion data (a GoPro GPMF or CAMM track, or a .gcsv log beside it) first"));
                            }
            }
            auto* analyze = new QPushButton(tr("Analyze"), row);
            analyze->setObjectName(QStringLiteral("analyzeStabilize"));
            analyze->setToolTip(e.type == "sky_replace" ? tr("Measure the camera's movement in this clip, for the new sky to follow")
                                                        : tr("Measure the camera's movement in this clip"));
            rh->addWidget(status, 1);
            rh->addWidget(analyze);
            f->addRow(QString(), row);
            // Deferred: the analysis waits in an event loop, and the Inspector may rebuild meanwhile.
            connect(analyze, &QPushButton::clicked, this,
                    [this, owner, eid] { QTimer::singleShot(0, this, [this, owner, eid] { analyzeStabilize(owner, eid); }); });
            if (e.type == "sky_replace" && depthAvailable() && !depthModel().installed()) {
                // It finds the sky without depth; with it, pale walls and water reaching the sky stay out of it.
                auto* depth = new QPushButton(tr("Find the Sky Better with Depth…"), content_);
                depth->setObjectName(QStringLiteral("skyDepthModel"));
                depth->setToolTip(tr("Get the depth model, so near things that look like sky (a pale wall, water) are kept"));
                f->addRow(QString(), depth);
                connect(depth, &QPushButton::clicked, this, [this, owner, eid] {
                    if (!ensureEffectModel(window(), "depth_map")) return;
                    // Use Depth turned on if it was off; the picture drawn again either way.
                    const Clip* c = state_->sequence() ? edit::clipById(*state_->sequence(), owner) : nullptr;
                    const Effect* now = nullptr;
                    for (size_t i = 0; c && i < c->effects.size(); ++i)
                        if (c->effects[i].id == eid) now = &c->effects[i];
                    auto turnOn = [owner, eid](Project&, Sequence& sq) {
                        Effect* e = edit::ownedEffect(sq, owner, eid);
                        if (!e) return false;
                        if (e->p("use_depth", 0, 1) <= 0.5) e->params["use_depth"] = Param(1.0);
                        return true;
                    };
                    if (now && now->p("use_depth", 0, 1) <= 0.5) state_->edit(tr("Use Depth"), turnOn);
                    else state_->amend(turnOn);
                    state_->message(tr("Sky Replacement now uses depth"), 4000);
                });
            }
        }
        if (e.type == "depth_blur" && onClip) {
            // Click to focus on the Program monitor (app/PaintOverlay); with Focus Point keyed, each click keys it.
            auto* pick = new QPushButton(tr("Pick Focus on the Monitor"), content_);
            pick->setObjectName(QStringLiteral("pickFocus"));
            pick->setCheckable(true);
            pick->setChecked(state_->paintEffect() == eid);
            pick->setToolTip(tr("Click what to focus on in the Program monitor. Key Focus Point X and Y, then click on two "
                                "frames for a focus pull between them. Esc stops."));
            f->addRow(QString(), pick);
            if (std::lround(e.p("follow", localTime())) == 2 && faceSearchAvailable() && !faceModel().installed()) {
                // Nearest Face finds no face (and keeps the Focus Depth) until the model is here.
                auto* get = new QPushButton(tr("Download Face Model…"), content_);
                get->setObjectName(QStringLiteral("getFaceModel"));
                connect(get, &QPushButton::clicked, this, [this] {
                    QTimer::singleShot(0, this, [this] {
                        if (ensureEffectModel(window(), "focus.face")) state_->amend([](Project&, Sequence&) { return true; });
                    });
                });
                f->addRow(tr("Faces:"), get);
            }
            connect(pick, &QPushButton::toggled, this, [this, eid](bool on) {
                if (on) state_->setPaintEffect(eid);
                else if (state_->paintEffect() == eid) state_->setPaintEffect(0);
            });
            // Follow Focus: keys on the Focus Depth that keep the focus on a tracked point, the mask's object or a face.
            auto* follow = new QWidget(content_);
            auto* fh = new QHBoxLayout(follow);
            fh->setContentsMargins(0, 0, 0, 0);
            const struct {
                const char* name;
                QString text, tip;
                FocusTarget target;
            } targets[3] = {{"followPoint", tr("Point"), tr("Track the Focus Point from the playhead through the clip and focus on it"), FocusTarget::Point},
                            {"followObject", tr("Object"), tr("Focus on this effect's Object mask (pick and track it first)"), FocusTarget::Object},
                            {"followFace", tr("Face"), tr("Follow the face nearest the Focus Point through the clip and focus on it"), FocusTarget::Face}};
            for (const auto& t : targets) {
                auto* b = new QPushButton(t.text, follow);
                b->setObjectName(QString::fromLatin1(t.name));
                b->setToolTip(t.tip + tr(". The focus is eased as a focus puller eases it and written as keys."));
                fh->addWidget(b);
                connect(b, &QPushButton::clicked, this, [this, owner, eid, target = t.target] {
                    QTimer::singleShot(0, this, [this, owner, eid, target] { followFocusOn(owner, eid, target); });
                });
            }
            fh->addStretch(1);
            f->addRow(tr("Follow Focus:"), follow);
            connect(state_, &EditorState::paintEffectChanged, pick, [this, pick, eid] {
                const QSignalBlocker block(pick);
                pick->setChecked(state_->paintEffect() == eid);
            });
        }
        if ((e.type == "mesh_warp" || e.type == "grid_warp") && onClip) {
            // Pins are placed and dragged, and grid points dragged, on the Program monitor (app/PaintOverlay).
            const bool mesh = e.type == "mesh_warp";
            auto* row = new QWidget(content_);
            auto* rh = new QHBoxLayout(row);
            rh->setContentsMargins(0, 0, 0, 0);
            auto* arm = new QPushButton(mesh ? tr("Pins on the Monitor") : tr("Points on the Monitor"), row);
            arm->setObjectName(QStringLiteral("warpOnMonitor"));
            arm->setCheckable(true);
            arm->setChecked(state_->paintEffect() == eid);
            arm->setToolTip(mesh ? tr("Click the Program monitor to place a pin of the kind chosen; drag a pin to move it (a key "
                                      "there when it is keyed); Ctrl-click a pin to remove it. Esc stops.")
                                 : tr("Drag the grid's points on the Program monitor (a key there when they are keyed). Esc stops."));
            connect(arm, &QPushButton::toggled, this, [this, eid](bool on) {
                if (on) state_->setPaintEffect(eid);
                else if (state_->paintEffect() == eid) state_->setPaintEffect(0);
            });
            connect(state_, &EditorState::paintEffectChanged, arm, [this, arm, eid] {
                const QSignalBlocker block(arm);
                arm->setChecked(state_->paintEffect() == eid);
            });
            if (mesh) {
                auto* kind = new QComboBox(row);
                kind->setObjectName(QStringLiteral("meshPinKind"));
                kind->addItems({tr("Position Pin"), tr("Stiff Pin"), tr("Bend Pin"), tr("Overlap Pin")});
                kind->setCurrentIndex(state_->meshPinKind());
                kind->setToolTip(tr("What a click on the monitor places: a pin that moves the picture, an area that keeps its "
                                    "shape, a point the picture turns and scales about, or which part is in front where it folds"));
                connect(kind, &QComboBox::currentIndexChanged, this, [this](int i) { state_->setMeshPinKind(i); });
                rh->addWidget(kind);
            }
            rh->addWidget(arm);
            rh->addStretch(1);
            f->addRow(mesh ? tr("Pins:") : tr("Grid:"), row);
            if (mesh && !meshPins(e).empty()) {
                auto* edits = new QWidget(content_);
                auto* eh = new QHBoxLayout(edits);
                eh->setContentsMargins(0, 0, 0, 0);
                auto* which = new QComboBox(edits);
                which->setObjectName(QStringLiteral("meshPinList"));
                static const char* kinds[] = {QT_TR_NOOP("Position"), QT_TR_NOOP("Stiff"), QT_TR_NOOP("Bend"), QT_TR_NOOP("Overlap")};
                for (int k : meshPins(e)) which->addItem(tr("Pin %1 (%2)").arg(k).arg(tr(kinds[meshPinKind(e, k)])), k);
                auto* remove = new QPushButton(tr("Remove"), edits);
                remove->setObjectName(QStringLiteral("removeMeshPin"));
                connect(remove, &QPushButton::clicked, this, [this, owner, eid, which] {
                    const int k = which->currentData().toInt();
                    state_->edit(tr("Remove Pin"), [owner, eid, k](Project&, Sequence& sq) {
                        Effect* ef = edit::ownedEffect(sq, owner, eid);
                        if (!ef || meshPins(*ef).empty()) return false;
                        removeMeshPin(*ef, k);
                        return true;
                    });
                });
                auto* track = new QPushButton(tr("Track Position Pins"), edits);
                track->setObjectName(QStringLiteral("trackMeshPins"));
                track->setToolTip(tr("Key each Position pin to follow what is under it in the footage, tracked from the playhead "
                                     "to both ends of the clip"));
                connect(track, &QPushButton::clicked, this, [this, owner, eid] {
                    QTimer::singleShot(0, this, [this, owner, eid] { trackWarpPins(owner, eid); });
                });
                eh->addWidget(which);
                eh->addWidget(remove);
                eh->addWidget(track);
                eh->addStretch(1);
                f->addRow(QString(), edits);
            }
            if (!mesh) {
                auto* reset = new QPushButton(tr("Reset Points"), content_);
                reset->setObjectName(QStringLiteral("resetGridWarp"));
                connect(reset, &QPushButton::clicked, this, [this, owner, eid] {
                    state_->edit(tr("Reset Grid"), [owner, eid](Project&, Sequence& sq) {
                        Effect* ef = edit::ownedEffect(sq, owner, eid);
                        if (!ef) return false;
                        return std::erase_if(ef->params, [](const auto& kv) {
                                   return kv.first.size() > 3 && kv.first[0] == 'g' && (kv.first.ends_with(".dx") || kv.first.ends_with(".dy"));
                               }) > 0;
                    });
                });
                f->addRow(QString(), reset);
            }
        }
        if ((e.type == "paint" || e.type == "dust_spots") && onClip) {
            // Strokes and spots are drawn on the Program monitor while the effect is armed (app/PaintOverlay).
            const bool dust = e.type == "dust_spots";
            const char* key = dust ? "spots" : "strokes";
            auto* row = new QWidget(content_);
            auto* rv = new QVBoxLayout(row);
            rv->setContentsMargins(0, 0, 0, 0);
            auto* status = new QLabel(row);
            status->setObjectName(QStringLiteral("paintStatus"));
            status->setWordWrap(true);
            const int n = dust ? int(dustSpotsFromString(e.s(key)).size()) : int(paintStrokesFromString(e.s(key)).size());
            status->setText(dust ? tr("%n spot(s) marked. Click dirt on the Program monitor to mark it on that frame; "
                                      "Ctrl-click a spot to remove it.", "", n)
                                 : tr("%n stroke(s). Alt-click the Program monitor where to copy from, then paint; Esc stops.", "", n));
            auto* buttons = new QWidget(row);
            auto* bh = new QHBoxLayout(buttons);
            bh->setContentsMargins(0, 0, 0, 0);
            auto* arm = new QPushButton(dust ? tr("Mark Spots on the Monitor") : tr("Paint on the Monitor"), buttons);
            arm->setObjectName(QStringLiteral("paintOnMonitor"));
            arm->setCheckable(true);
            arm->setChecked(state_->paintEffect() == eid);
            if (dust) {
                // Spots are taken out where a video or picture file is read.
                const Clip* c = edit::clipById(*state_->sequence(), owner);
                const MediaItem* m = c ? state_->project().findMedia(c->mediaId) : nullptr;
                if (!m || (m->kind != MediaKind::Video && m->kind != MediaKind::Image)) {
                    arm->setEnabled(false);
                    arm->setToolTip(tr("Dust spots are taken out of video and picture files, not nested sequences or generators"));
                }
            }
            auto* clearFrame = new QPushButton(tr("Clear This Frame"), buttons);
            clearFrame->setObjectName(QStringLiteral("paintClearFrame"));
            clearFrame->setToolTip(dust ? tr("Remove the spots marked on this frame") : tr("Remove the strokes drawn on this frame"));
            auto* clearAll = new QPushButton(tr("Clear All"), buttons);
            clearAll->setObjectName(QStringLiteral("paintClearAll"));
            bh->addWidget(arm);
            bh->addWidget(clearFrame);
            bh->addWidget(clearAll);
            bh->addStretch(1);
            rv->addWidget(status);
            rv->addWidget(buttons);
            f->addRow(QString(), row);
            connect(arm, &QPushButton::toggled, this, [this, eid](bool on) {
                if (on) state_->setPaintEffect(eid);
                else if (state_->paintEffect() == eid) state_->setPaintEffect(0);
            });
            connect(state_, &EditorState::paintEffectChanged, arm, [this, arm, eid] {
                const QSignalBlocker block(arm);
                arm->setChecked(state_->paintEffect() == eid);
            });
            auto clear = [this, owner, eid, dust, key](bool frameOnly) {
                const Sequence* sq = state_->sequence();
                const Clip* c = sq ? edit::clipById(*sq, owner) : nullptr;
                if (!c || (frameOnly && !c->contains(state_->playhead()))) return;
                const int64_t lt = state_->playhead() - c->start;
                state_->edit(frameOnly ? tr("Clear Frame") : tr("Clear All"), [=](Project&, Sequence& s) {
                    Effect* ef = edit::ownedEffect(s, owner, eid);
                    if (!ef || ef->s(key).empty()) return false;
                    if (!frameOnly) {
                        ef->strings.erase(key);
                        return true;
                    }
                    size_t removed = 0;
                    if (dust) {
                        std::vector<DustSpot> spots = dustSpotsFromString(ef->s(key));
                        removed = std::erase_if(spots, [lt](const DustSpot& d) { return d.frame == lt; });
                        ef->strings[key] = dustSpotsToString(spots);
                    } else {
                        std::vector<PaintStroke> strokes = paintStrokesFromString(ef->s(key));
                        removed = std::erase_if(strokes, [lt](const PaintStroke& p) { return p.frame == lt; });
                        ef->strings[key] = paintStrokesToString(strokes);
                    }
                    return removed > 0;
                });
            };
            connect(clearFrame, &QPushButton::clicked, this, [clear] { clear(true); });
            connect(clearAll, &QPushButton::clicked, this, [clear] { clear(false); });
        }
        if (e.type == "camera_track" && onClip) {
            // The solve lives in the effect; titles are placed from it, and it is exported for 3D programs.
            auto* row = new QWidget(content_);
            auto* rv = new QVBoxLayout(row);
            rv->setContentsMargins(0, 0, 0, 0);
            auto* status = new QLabel(row);
            status->setObjectName(QStringLiteral("cameraTrackStatus"));
            status->setWordWrap(true);
            CameraSolve cs;
            if (cameraSolveFromString(e.s("solve"), cs))
                status->setText(tr("Solved: %1° lens, %2 px error, %n point(s)%3", "", int(cs.points.size()))
                                    .arg(cs.fieldOfView(), 0, 'f', 1)
                                    .arg(cs.error, 0, 'f', 2)
                                    .arg(cs.oriented ? QString() : tr("; no ground found")));
            else
                status->setText(tr("Not solved yet: the camera needs to move (not only turn) over a scene with depth"));
            auto* buttons = new QWidget(row);
            auto* bh = new QHBoxLayout(buttons);
            bh->setContentsMargins(0, 0, 0, 0);
            auto* solve = new QPushButton(tr("Solve"), buttons);
            solve->setObjectName(QStringLiteral("solveCamera"));
            solve->setToolTip(tr("Solve the camera that filmed this clip"));
            auto* place = new QPushButton(tr("Place Title"), buttons);
            place->setObjectName(QStringLiteral("placeSceneTitle"));
            place->setToolTip(tr("Put a title in the scene at the playhead, where Place At points (on the ground, or standing on it)"));
            auto* save = new QPushButton(tr("Export…"), buttons);
            save->setObjectName(QStringLiteral("exportCamera"));
            save->setToolTip(tr("Write the camera as a Nuke .chan file and its points as a .ply beside it"));
            place->setEnabled(cs.oriented);
            save->setEnabled(cs.focal > 0);
            bh->addWidget(solve);
            bh->addWidget(place);
            bh->addWidget(save);
            rv->addWidget(status);
            rv->addWidget(buttons);
            f->insertRow(0, QString(), row);
            connect(solve, &QPushButton::clicked, this,
                    [this, owner, eid] { QTimer::singleShot(0, this, [this, owner, eid] { solveCameraTrack(owner, eid); }); });
            connect(place, &QPushButton::clicked, this, [this, owner, eid] {
                QTimer::singleShot(0, this, [this, owner, eid] {
                    bool ok = false;
                    const QString text = QInputDialog::getText(window(), tr("Place Title"), tr("Text:"), QLineEdit::Normal, tr("Title"), &ok);
                    if (ok && !text.isEmpty()) placeTitleInScene(owner, eid, text);
                });
            });
            connect(save, &QPushButton::clicked, this, [this, cs, owner] {
                QString filter;
                const QString chan = QFileDialog::getSaveFileName(window(), tr("Export Camera"), QStringLiteral("camera.chan"),
                                                                  tr("Nuke camera (*.chan);;USD scene with the placed models (*.usda)"), &filter);
                if (chan.isEmpty()) return;
                std::string err;
                if (chan.endsWith(QLatin1String(".usda"), Qt::CaseInsensitive) || filter.contains(QLatin1String("usda"))) {
                    // The camera, its points and the models placed in the shot, in one USD scene.
                    const Sequence* s = state_->sequence();
                    std::vector<SolvedSceneMesh> models;
                    for (size_t i = 0; s && i < s->videoTracks.size(); ++i)
                        for (const Clip& k : s->videoTracks[i].clips)
                            if (k.id == owner) models = solvedSceneModels(state_->project(), *s, i, k);
                    const QString usd = chan.endsWith(QLatin1String(".usda"), Qt::CaseInsensitive) ? chan : QFileInfo(chan).path() + "/" + QFileInfo(chan).completeBaseName() + ".usda";
                    if (!writeSolveUsda(cs, usd.toStdString(), models, &err)) state_->message(QString::fromStdString(err), 6000);
                    else state_->message(tr("Wrote %1").arg(QFileInfo(usd).fileName()), 4000);
                    return;
                }
                const QString ply = QFileInfo(chan).path() + "/" + QFileInfo(chan).completeBaseName() + ".ply";
                if (!writeNukeChan(cs, chan.toStdString(), 1, &err) || !writePointsPly(cs, ply.toStdString(), &err))
                    state_->message(QString::fromStdString(err), 6000);
                else
                    state_->message(tr("Wrote %1 and %2").arg(QFileInfo(chan).fileName(), QFileInfo(ply).fileName()), 4000);
            });
        }
        if (e.type == "lens_correction" && onClip) {
            // The lens profile it holds, chosen in the Correct Lens dialog.
            auto* row = new QWidget(content_);
            auto* rh = new QHBoxLayout(row);
            rh->setContentsMargins(0, 0, 0, 0);
            auto* name = new QLabel(row);
            name->setObjectName(QStringLiteral("lensName"));
            name->setWordWrap(true);
            const QString cam = QString::fromStdString(e.s("camera")), lensName = QString::fromStdString(e.s("lens"));
            name->setText(e.s("profile").empty() ? tr("No lens chosen") : cam.isEmpty() ? lensName : tr("%1 on %2").arg(lensName, cam));
            auto* choose = new QPushButton(tr("Choose Lens…"), row);
            choose->setObjectName(QStringLiteral("chooseLens"));
            rh->addWidget(name, 1);
            rh->addWidget(choose);
            f->insertRow(0, tr("Lens:"), row);
            connect(choose, &QPushButton::clicked, this,
                    [this, owner] { QTimer::singleShot(0, this, [this, owner] { correctLens(window(), state_, owner); }); });
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
        if (onClip && (needsDepth(e, localTime()) || (e.type == "sky_replace" && e.p("use_depth", localTime(), 1) > 0.5)) && videoDepthAvailable())
            if (const Clip* c = state_->sequence() ? edit::clipById(*state_->sequence(), owner) : nullptr) {
                std::string file;
                double from, to;
                if (clipMediaRange(state_->project(), *state_->sequence(), *c, file, from, to)) {
                    // Depth worked out frame by frame shimmers on video; analysed over the clip it holds still.
                    auto* row = new QWidget(content_);
                    auto* rh = new QHBoxLayout(row);
                    rh->setContentsMargins(0, 0, 0, 0);
                    auto* status = new QLabel(row);
                    status->setObjectName(QStringLiteral("steadyDepthStatus"));
                    status->setWordWrap(true);
                    const bool analysed = clipDepthAnalysed(state_->project(), *state_->sequence(), *c);
                    status->setText(analysed ? tr("Analysed over the clip: steady") : tr("Frame by frame (may shimmer)"));
                    auto* analyze = new QPushButton(analysed ? tr("Analyse Again") : tr("Analyse Depth"), row);
                    analyze->setObjectName(QStringLiteral("analyzeDepth"));
                    analyze->setToolTip(tr("Work out the depth over the whole clip, 32 frames at a time, so it holds still as the camera "
                                           "and the picture move. Kept for the clip's file."));
                    rh->addWidget(status, 1);
                    rh->addWidget(analyze);
                    f->addRow(tr("Steady Depth:"), row);
                    connect(analyze, &QPushButton::clicked, this,
                            [this, owner] { QTimer::singleShot(0, this, [this, owner] { analyzeDepth(owner); }); });
                }
            }
        if (kind == TrackKind::Video && supportsMask(e.type)) {
            // Shape masks and the HSL qualifier; folded away until one is used.
            QFormLayout* mf = addSection(tr("%1 Mask").arg(title), nullptr,
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
            if (onClip && std::lround(e.p("mask.shape", localTime())) == 7) {
                // Objects of a multi-layer OpenEXR render, picked by clicking them in the viewer.
                const Sequence* sq = state_->sequence();
                const Clip* cl = sq ? edit::clipById(*sq, owner) : nullptr;
                const MediaItem* em = cl && cl->mediaId ? state_->project().findMedia(cl->mediaId) : nullptr;
                const auto exr = em && exrAvailable() ? cachedExrInfo(exrFrameFile(em->path, 0)) : nullptr;
                auto* layer = new QComboBox(content_);
                layer->setObjectName(QStringLiteral("cryptomatteLayer"));
                layer->setToolTip(tr("Which Cryptomatte of the render: objects, materials or assets"));
                if (exr)
                    for (const Cryptomatte& cm : exr->cryptomattes) layer->addItem(QString::fromStdString(cm.name), QString::fromStdString(cm.name));
                const std::string chosen = e.s("mask.crypto_layer");
                if (!chosen.empty() && layer->findData(QString::fromStdString(chosen)) < 0)
                    layer->addItem(tr("%1 (missing)").arg(QString::fromStdString(chosen)), QString::fromStdString(chosen));
                layer->setCurrentIndex(std::max(0, layer->findData(QString::fromStdString(chosen))));
                layer->setEnabled(layer->count() > 1);
                mf->addRow(tr("Cryptomatte:"), layer);
                // Kept up with undo, redo and edits from elsewhere ("" is the file's first).
                refreshers_.push_back([this, owner, eid, layer] {
                    const Sequence* s = state_->sequence();
                    const Effect* ef = s ? edit::ownedEffect(const_cast<Sequence&>(*s), owner, eid) : nullptr;
                    if (!ef) return;
                    const QSignalBlocker block(layer);
                    layer->setCurrentIndex(std::max(0, layer->findData(QString::fromStdString(ef->s("mask.crypto_layer")))));
                });
                connect(layer, &QComboBox::activated, this, [this, owner, eid, layer, exr](int idx) {
                    const std::string name = layer->itemData(idx).toString().toStdString();
                    state_->edit(tr("Choose Cryptomatte"), [owner, eid, name, exr](Project&, Sequence& s) {
                        Effect* ef = edit::ownedEffect(s, owner, eid);
                        if (!ef) return false;
                        // (choosing the one shown again, the file's first when none was named, changes nothing)
                        const std::string old = ef->s("mask.crypto_layer");
                        const Cryptomatte* was = exr ? exr->cryptomatte(old) : nullptr;
                        if (old == name || (was && was->name == name)) return false;
                        // Ids belong to one Cryptomatte: picks in another name other things.
                        ef->strings["mask.crypto_layer"] = name;
                        ef->strings["mask.crypto_ids"].clear();
                        return true;
                    });
                });
                auto* status = new QLabel(content_);
                status->setObjectName(QStringLiteral("cryptomatteStatus"));
                status->setWordWrap(true);
                status->setTextInteractionFlags(Qt::TextSelectableByMouse);
                auto* clear = new QToolButton(content_);
                clear->setText(tr("Clear"));
                clear->setObjectName(QStringLiteral("cryptomatteClear"));
                clear->setToolTip(tr("Remove every picked object from the mask"));
                auto refresh = [this, owner, eid, status, clear, exr] {
                    const Sequence* s = state_->sequence();
                    const Effect* ef = s ? edit::ownedEffect(const_cast<Sequence&>(*s), owner, eid) : nullptr;
                    if (!ef) return;
                    if (!exr || exr->cryptomattes.empty()) {
                        status->setText(exrAvailable() ? tr("This clip's media has no Cryptomatte: it needs an OpenEXR render with one.")
                                                       : tr("This build of Montage can't read OpenEXR Cryptomattes."));
                        clear->setEnabled(false);
                        return;
                    }
                    const Cryptomatte* cm = exr->cryptomatte(ef->s("mask.crypto_layer"));
                    QStringList names;
                    for (float id : parseCryptomatteIds(ef->s("mask.crypto_ids"))) {
                        const std::string name = cm ? cm->objectName(id) : std::string();
                        names << QString::fromStdString(name.empty() ? cryptomatteHex(id) : name);
                    }
                    status->setText(names.isEmpty() ? tr("Click objects in the viewer to add them; Ctrl-click (Cmd-click) removes one.")
                                                    : names.join(QStringLiteral(", ")));
                    clear->setEnabled(!names.isEmpty());
                };
                refresh();
                refreshers_.push_back(refresh);
                mf->addRow(tr("Objects:"), status);
                mf->addRow(QString(), clear);
                connect(clear, &QToolButton::clicked, this, [this, owner, eid] {
                    state_->edit(tr("Clear Cryptomatte"), [owner, eid](Project&, Sequence& s) {
                        Effect* ef = edit::ownedEffect(s, owner, eid);
                        if (!ef || ef->s("mask.crypto_ids").empty()) return false;
                        ef->strings["mask.crypto_ids"].clear();
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
                    const ObjectMask* o = ef && ef->object ? &ef->object->object(state_->maskObject(*ef)) : nullptr;
                    if (!o || o->prompts.empty())
                        return tr("Click the object in the viewer. Alt-click what is not part of it, or drag a box around it.");
                    return tr("%n frame(s) clicked", "", int(o->prompts.size())) + QStringLiteral(", ") +
                           tr("%n segmented", "", int(o->frames.size()));
                };
                status->setText(read());
                refreshers_.push_back([status, read] { status->setText(read()); });
                // Several objects in one mask (Premiere's and Resolve's): the one being clicked and tracked, others added
                // to the mask or cut out of it.
                auto* objects = new QWidget(content_);
                auto* oh = new QHBoxLayout(objects);
                oh->setContentsMargins(0, 0, 0, 0);
                auto* list = new QComboBox(objects);
                list->setObjectName(QStringLiteral("objectList"));
                list->setToolTip(tr("The object your clicks and tracking are for"));
                auto* addObject = new QToolButton(objects);
                addObject->setText(tr("+"));
                addObject->setObjectName(QStringLiteral("objectAdd"));
                addObject->setToolTip(tr("Add another object to the mask"));
                auto* cutObject = new QToolButton(objects);
                cutObject->setText(tr("−"));
                cutObject->setObjectName(QStringLiteral("objectSubtract"));
                cutObject->setToolTip(tr("Add an object to cut out of the mask"));
                auto* removeObject = new QToolButton(objects);
                removeObject->setText(tr("Remove"));
                removeObject->setObjectName(QStringLiteral("objectRemove"));
                removeObject->setToolTip(tr("Remove this object from the mask"));
                auto* cutOut = new QCheckBox(tr("Cut Out"), objects);
                cutOut->setObjectName(QStringLiteral("objectCutOut"));
                cutOut->setToolTip(tr("Cut this object out of the mask instead of adding it"));
                oh->addWidget(list, 1);
                for (QWidget* w : {static_cast<QWidget*>(addObject), static_cast<QWidget*>(cutObject), static_cast<QWidget*>(removeObject),
                                   static_cast<QWidget*>(cutOut)})
                    oh->addWidget(w);
                auto fillObjects = [this, owner, eid, list, cutOut, removeObject, addObject, cutObject] {
                    const Sequence* sq = state_->sequence();
                    const Effect* ef = sq ? edit::ownedEffect(const_cast<Sequence&>(*sq), owner, eid) : nullptr;
                    if (!ef) return;
                    const int n = ef->object ? ef->object->count() : 1, current = state_->maskObject(*ef);
                    QStringList names;
                    for (int i = 0; i < n; ++i) {
                        const ObjectMask* o = ef->object ? &ef->object->object(i) : nullptr;
                        QString name = o && !o->name.empty() ? QString::fromStdString(o->name) : tr("Object %1").arg(i + 1);
                        if (o && i > 0 && o->subtract) name += tr(" (cut out)");
                        names << name;
                    }
                    QSignalBlocker block(list), block2(cutOut);
                    QStringList have;
                    for (int i = 0; i < list->count(); ++i) have << list->itemText(i);
                    if (have != names) {
                        list->clear();
                        list->addItems(names);
                    }
                    list->setCurrentIndex(current);
                    cutOut->setChecked(ef->object && current > 0 && ef->object->object(current).subtract);
                    cutOut->setEnabled(current > 0);
                    removeObject->setEnabled(ef->object != nullptr);
                    addObject->setEnabled(ef->object != nullptr);  // (a mask's first object comes from clicking it)
                    cutObject->setEnabled(ef->object != nullptr);
                };
                fillObjects();
                refreshers_.push_back(fillObjects);
                connect(list, qOverload<int>(&QComboBox::activated), this, [this, eid](int i) { state_->setMaskObject(eid, i); });
                for (auto [button, subtract] : {std::pair{addObject, false}, std::pair{cutObject, true}})
                    connect(button, &QToolButton::clicked, this, [this, owner, eid, subtract = subtract] {
                        const bool added = state_->edit(subtract ? tr("Add Object to Cut Out") : tr("Add Object"), [owner, eid, subtract](Project& p, Sequence& s) {
                            Clip* c = edit::clipById(s, owner);
                            Effect* ef = c ? edit::ownedEffect(s, owner, eid) : nullptr;
                            if (!c || !ef) return false;
                            auto o = withNewObject(p, s, *c, *ef, subtract);
                            if (!o) return false;
                            ef->object = o;
                            return true;
                        });
                        // The new object (the last) is the one to click.
                        const Sequence* sq = state_->sequence();
                        const Effect* ef = sq ? edit::ownedEffect(const_cast<Sequence&>(*sq), owner, eid) : nullptr;
                        if (added && ef && ef->object) state_->setMaskObject(eid, ef->object->count() - 1);
                    });
                connect(removeObject, &QToolButton::clicked, this, [this, owner, eid] {
                    const Sequence* sq = state_->sequence();
                    const Effect* now = sq ? edit::ownedEffect(const_cast<Sequence&>(*sq), owner, eid) : nullptr;
                    if (!now) return;
                    const int which = state_->maskObject(*now);
                    const bool removed = state_->edit(tr("Remove Object"), [owner, eid, which](Project&, Sequence& s) {
                        Effect* ef = edit::ownedEffect(s, owner, eid);
                        if (!ef || !ef->object) return false;
                        ef->object = withoutObject(*ef, which);
                        return true;
                    });
                    if (removed) state_->setMaskObject(eid, std::max(0, which - 1));
                });
                connect(cutOut, &QCheckBox::toggled, this, [this, owner, eid](bool on) {
                    const Sequence* sq = state_->sequence();
                    const Effect* now = sq ? edit::ownedEffect(const_cast<Sequence&>(*sq), owner, eid) : nullptr;
                    if (!now) return;
                    const int which = state_->maskObject(*now);
                    state_->edit(on ? tr("Cut Out Object") : tr("Add Object Back"), [owner, eid, on, which](Project&, Sequence& s) {
                        Effect* ef = edit::ownedEffect(s, owner, eid);
                        if (!ef || !ef->object) return false;
                        if (which == 0 || ef->object->object(which).subtract == on) return false;
                        ef->object = withObjectSubtracted(*ef, which, on);
                        return true;
                    });
                });
                mf->addRow(tr("Objects:"), objects);
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
                    const Effect* now = edit::ownedEffect(const_cast<Sequence&>(*sq), owner, eid);
                    if (!now) return;
                    const int which = state_->maskObject(*now);
                    state_->edit(tr("Clear Object Clicks"), [owner, eid, lt, which](Project& p, Sequence& s) {
                        Clip* c = edit::clipById(s, owner);
                        Effect* ef = c ? edit::ownedEffect(s, owner, eid) : nullptr;
                        if (!c || !ef || !ef->object) return false;
                        ef->object = withObjectPrompts(p, s, *c, *ef, lt, {}, which);
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
        // (past the next effect in the same colour node: those in others run elsewhere)
        connect(up, &QToolButton::clicked, this, [=] {
            mutateStack(tr("Reorder Effects"), [](std::vector<Effect>& v, size_t k, Project&) {
                for (size_t j = k; j-- > 0;)
                    if (v[j].node == v[k].node) {
                        // (taken out and put back before it: the effects between keep their places around the graph)
                        Effect moved = std::move(v[k]);
                        v.erase(v.begin() + long(k));
                        v.insert(v.begin() + long(j), std::move(moved));
                        break;
                    }
            });
        });
        connect(down, &QToolButton::clicked, this, [=] {
            mutateStack(tr("Reorder Effects"), [](std::vector<Effect>& v, size_t k, Project&) {
                for (size_t j = k + 1; j < v.size(); ++j)
                    if (v[j].node == v[k].node) {
                        Effect moved = std::move(v[k]);
                        v.erase(v.begin() + long(k));
                        v.insert(v.begin() + long(j), std::move(moved));
                        break;
                    }
            });
        });
        connect(reset, &QToolButton::clicked, this, [=] {
            mutateStack(tr("Reset Effect"), [](std::vector<Effect>& v, size_t k, Project&) {
                if (v[k].type == "plugin") {
                    // Keep the plugin; put its parameters back to their defaults.
                    for (const ParamInfo& pi : effectParams(v[k])) v[k].params[pi.name] = Param(pi.def);
                } else if (v[k].type == "ara") {
                    // Keep the ARA plug-in; its document (analysis and edits) starts afresh.
                    Effect e = makeEffect(v[k].type, v[k].id);
                    e.strings = v[k].strings;
                    e.strings.erase("archive");
                    v[k] = e;
                } else if (v[k].type == "lens_correction") {
                    // Keep the lens and what it was chosen with; put the rest back.
                    Effect e = makeEffect(v[k].type, v[k].id);
                    e.strings = v[k].strings;
                    for (const char* n : {"focal", "aperture", "camera_crop"}) e.params[n] = v[k].params[n];
                    v[k] = e;
                } else {
                    v[k] = makeEffect(v[k].type, v[k].id);
                }
            });
        });
        connect(del, &QToolButton::clicked, this, [=] {
            mutateStack(tr("Remove Effect"), [](std::vector<Effect>& v, size_t k, Project&) { v.erase(v.begin() + long(k)); });
        });
    }
    if (addMenu) addEffectMenu(kind, owner, std::max(0, node));
}

void InspectorWidget::addEffectMenu(TrackKind kind, Id owner, int node) {
    auto* btn = new QPushButton(kind == TrackKind::Video ? tr("Add Video Effect…") : tr("Add Audio Effect…"), content_);
    auto* menu = new QMenu(btn);
    std::map<std::string, QMenu*> groups;
    // A shared node's menu leaves out the effects that run before the grade (they go on a clip, outside its nodes).
    const bool sharedNode = state_->sequence() && findSharedNode(*state_->sequence(), owner);
    for (const EffectInfo* info : effectsInCategory(kind == TrackKind::Video ? EffectCategory::VideoFilter : EffectCategory::AudioFilter)) {
        if (sharedNode && runsBeforeGrade(info->type)) continue;
        QMenu*& g = groups[info->group];
        if (!g) g = menu->addMenu(language::engine(info->group));
        std::string type = info->type;
        QString label = language::engine(info->displayName);
        g->addAction(label, this, [this, owner, type, label, node] {
            if (!ensureEffectModel(window(), type)) return;
            // Bandwidth Extension: the models the clip's cut-off needs.
            if (type == "bandwidth_extension" && state_->sequence())
                if (const Clip* c = edit::clipById(*state_->sequence(), owner);
                    c && !ensureBandwidthModels(window(), state_->project(), *state_->sequence(), *c))
                    return;
            Id added = 0;
            state_->edit(tr("Add %1").arg(label), [owner, type, node, &added](Project& p, Sequence& s) {
                std::vector<Effect>* chain = edit::effectChain(s, owner);
                if (!chain) return false;
                Effect e = makeEffect(p, type);
                added = e.id;
                // Stabilize, Rolling Shutter Repair and Lens Correction reshape the whole frame: they go first, before any
                // filter (outside any colour node).
                if (type == "stabilize" || type == "rolling_shutter" || type == "lens_correction") {
                    chain->insert(chain->begin(), e);
                } else if (node && !runsBeforeGrade(type)) {
                    // Into the colour node, after its own effects (those working on the picture as it comes stay outside,
                    // after the clip's).
                    e.node = node;
                    auto at = chain->end();
                    for (auto it = chain->begin(); it != chain->end(); ++it)
                        if (it->node == node) at = it + 1;
                    chain->insert(at, e);
                } else {
                    chain->push_back(e);
                }
                return true;
            });
            if ((type == "stabilize" || type == "rolling_shutter") && added && state_->sequence() && edit::clipById(*state_->sequence(), owner))
                QTimer::singleShot(0, this, [this, owner, added] { analyzeStabilize(owner, added); });
            // Sky Replacement on footage: the camera's movement measured now, for the new sky to follow.
            if (type == "sky_replace" && added && state_->sequence())
                if (const Clip* c = edit::clipById(*state_->sequence(), owner))
                    if (const MediaItem* m = c->mediaId ? state_->project().findMedia(c->mediaId) : nullptr;
                        m && m->kind == MediaKind::Video && m->hasVideo)
                        QTimer::singleShot(0, this, [this, owner, added] { analyzeStabilize(owner, added); });
            if (type == "lens_correction" && added && state_->sequence() && edit::clipById(*state_->sequence(), owner))
                QTimer::singleShot(0, this, [this, owner] { correctLens(window(), state_, owner); });
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
        type->addItem(language::engine(info->displayName), QString::fromStdString(info->type));
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
    for (const HdrZone& z : hdrZones()) zone->addItem(language::engine(z.label));
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

void InspectorWidget::addColorSlice(QFormLayout* form, const EffectInfo& info, const Target& target) {
    // One vector at a time (Global, the six hues, Skin): its hue, saturation and density.
    auto* vector = new QComboBox(content_);
    vector->setObjectName(QStringLiteral("sliceVector"));
    vector->addItem(tr("Global"));
    for (const HueVector& v : colorSliceVectors()) vector->addItem(language::engine(v.label));
    vector->addItem(tr("Skin"));
    sliceVector_ = std::clamp(sliceVector_, 0, vector->count() - 1);
    vector->setCurrentIndex(sliceVector_);
    connect(vector, &QComboBox::currentIndexChanged, this, [this](int i) {
        sliceVector_ = i;
        QTimer::singleShot(0, this, [this] { rebuild(); });  // not while the combo box is still signalling
    });
    form->addRow(tr("Vector"), vector);
    std::vector<std::string> names{"density", "depth", "saturation", "subtractive", "protect_highlights", "mix"};
    if (sliceVector_ > 0) {
        const bool skin = sliceVector_ > int(colorSliceVectors().size());
        const std::string prefix = skin ? "skin" : colorSliceVectors()[size_t(sliceVector_ - 1)].name;
        names = {prefix + "_hue", prefix + "_saturation", prefix + "_density"};
        if (skin) names.insert(names.end(), {"skin_centre", "skin_width"});
    }
    for (const std::string& n : names)
        for (const ParamInfo& pi : info.params)
            if (pi.name == n) addParamRow(form, pi, target);
}

bool InspectorWidget::ownSetting(const Target& target) const {
    const Sequence* s = state_->sequence();
    const Clip* c = s && target.clip ? edit::clipById(*s, target.clip) : nullptr;
    const Effect* e = c ? target.resolve(const_cast<Sequence&>(*s)) : nullptr;
    if (!e) return false;
    if (e == &c->motion || e == &c->generator || e == &c->audio) return true;
    return std::any_of(c->effects.begin(), c->effects.end(), [e](const Effect& x) { return &x == e; });
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

QString InspectorWidget::publishField(Id clip, Id effect, const std::string& param, const QString& label) {
    QString why;
    state_->edit(tr("Publish %1").arg(label), [&](Project&, Sequence& s) {
        const auto f = fieldFor(s, clip, effect, param, label.toStdString());
        if (!f) {
            why = tr("That setting cannot be published");
            return false;
        }
        const edit::Result r = montage::publishField(s, *f);
        why = QString::fromStdString(r.error);
        return r.ok;
    });
    if (why.isEmpty()) state_->message(tr("\"%1\" is now a field of this graphic: every clip of the sequence can set it").arg(label), 5000);
    return why;
}

void InspectorWidget::editExpression(const Target& target, const std::string& param, const QString& label, double def) {
    const Sequence* seq = state_->sequence();
    const Clip* clip = seq ? edit::clipById(*seq, target.clip) : nullptr;
    const Effect* effect = clip ? target.resolve(const_cast<Sequence&>(*seq)) : nullptr;
    if (!effect) return;
    const auto found = effect->params.find(param);
    const Param base = found == effect->params.end() ? Param(def) : found->second;  // tried with what is typed
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("expressionDialog"));
    dlg.setWindowTitle(tr("Expression: %1").arg(label));
    auto* layout = new QVBoxLayout(&dlg);
    auto* help = new QLabel(tr("A Lua formula for the value at each frame, such as <code>value + wiggle(2, 10)</code>. It can use "
                               "<code>value</code> (the keyed value), <code>time</code>, <code>frame</code>, <code>wiggle(per second, amount)</code>, "
                               "<code>random(lo, hi)</code>, <code>loopOut(\"cycle\")</code>, <code>param(\"clip\", \"setting\")</code>, "
                               "<code>audio(track)</code>, <code>linear</code>, <code>ease</code> and Lua's maths (<code>sin</code>, <code>abs</code>, …)."),
                            &dlg);
    help->setWordWrap(true);
    layout->addWidget(help);
    auto* text = new QPlainTextEdit(QString::fromStdString(base.expr), &dlg);
    text->setObjectName(QStringLiteral("expressionText"));
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setFixedHeight(text->fontMetrics().lineSpacing() * 5 + 12);
    layout->addWidget(text);
    auto* status = new QLabel(&dlg);
    status->setObjectName(QStringLiteral("expressionStatus"));
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    QPushButton* apply = buttons->button(QDialogButtonBox::Ok);
    QPushButton* bake = buttons->addButton(tr("Bake to Keyframes"), QDialogButtonBox::ActionRole);
    bake->setObjectName(QStringLiteral("expressionBake"));
    bake->setToolTip(tr("Replace the expression with keyframes that give the same values (for other applications)"));
    QPushButton* remove = buttons->addButton(tr("Remove"), QDialogButtonBox::ActionRole);
    remove->setObjectName(QStringLiteral("expressionRemove"));
    remove->setEnabled(!base.expr.empty());
    layout->addWidget(buttons);
    enum class Action { Apply, Bake, Remove } action = Action::Apply;
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(bake, &QPushButton::clicked, &dlg, [&] {
        action = Action::Bake;
        dlg.accept();
    });
    connect(remove, &QPushButton::clicked, &dlg, [&] {
        action = Action::Remove;
        dlg.accept();
    });
    // Checked as it is typed: compiled, then run at the playhead.
    auto check = [&, clipId = target.clip, effectId = effect->id, time = target.time] {
        const std::string expr = text->toPlainText().trimmed().toStdString();
        bake->setEnabled(false);
        if (expr.empty()) {
            status->setText(tr("No expression: the keyed value is used."));
            apply->setEnabled(true);
            return;
        }
        std::string why;
        if (!checkExpression(expr, &why)) {
            status->setText(tr("Does not work: %1").arg(QString::fromStdString(why)));
            apply->setEnabled(false);
            return;
        }
        Param trial = base;
        trial.expr = expr;
        if (trial.exprSeed == 0) trial.exprSeed = expressionSeed(clipId, effectId, param);  // (as setExpression will)
        const Sequence* s = state_->sequence();
        ExpressionScope scope(&state_->project(), s, s ? edit::clipById(*s, clipId) : nullptr);
        const FrameTime t = time();
        const double v = evaluateExpression(trial, t, trial.keyedAt(t), &why);
        status->setText(why.empty() ? tr("At the playhead: %1").arg(v, 0, 'g', 6)
                                    : tr("Fails at the playhead (the keyed value is used): %1").arg(QString::fromStdString(why)));
        apply->setEnabled(true);
        bake->setEnabled(why.empty());
    };
    connect(text, &QPlainTextEdit::textChanged, &dlg, check);
    check();
    if (dlg.exec() != QDialog::Accepted) return;
    const std::string expr = action == Action::Remove ? std::string() : text->toPlainText().trimmed().toStdString();
    if (action == Action::Apply && expr == base.expr) return;
    const Id clipId = target.clip, effectId = target.effect;
    QString error;
    state_->edit(action == Action::Bake      ? tr("Bake Expression on %1").arg(label)
                 : expr.empty()              ? tr("Remove Expression on %1").arg(label)
                                             : tr("Expression on %1").arg(label),
                 [clipId, effectId, param, expr, action, &error](Project& p, Sequence& s) {
                     edit::Result r = edit::setExpression(p, s, clipId, effectId, param, expr);
                     if (r.ok && action == Action::Bake) r = edit::bakeExpression(p, s, clipId, effectId, param);
                     if (!r.ok) error = QString::fromStdString(r.error);
                     return r.ok;
                 });
    if (!error.isEmpty()) state_->message(error, 5000);
}

void InspectorWidget::publishParameter(const Target& target, const std::string& param, const QString& label) {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Publish to Graphic"),
                                               tr("Every clip that plays this sequence can set this as a field. Its label:"),
                                               QLineEdit::Normal, label, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (const QString why = publishField(target.publishClip, target.effect, param, name); !why.isEmpty()) state_->message(why, 5000);
}

void InspectorWidget::addGraphicSection(const Clip& clip) {
    const Sequence* tpl = graphicOf(state_->project(), clip);
    if (!tpl) return;
    const std::vector<TemplateField> fields = liveFields(*tpl);
    if (fields.empty()) return;
    QFormLayout* form = addSection(tr("Graphic"));
    const Id clipId = clip.id, tplId = tpl->id;
    // The value shown: the clip's own, else the template's.
    auto valueOf = [this, clipId, tplId](const TemplateField& f) -> QString {
        const Sequence* s = state_->sequence();
        const Clip* c = s ? edit::clipById(*s, clipId) : nullptr;
        const Sequence* t = state_->project().findSequence(tplId);
        return c && t ? QString::fromStdString(fieldValue(*t, *c, f)) : QString();
    };
    auto setValue = [this, clipId, tplId](const TemplateField& f, const QString& v) {
        state_->edit(tr("Change %1").arg(QString::fromStdString(f.label)), [clipId, tplId, f, v](Project& p, Sequence& s) {
            Clip* c = edit::clipById(s, clipId);
            const Sequence* t = p.findSequence(tplId);
            std::string out;
            if (!c || !t || !normalizeFieldValue(p, *t, f, v.toStdString(), out)) return false;
            if (c->fields[f.id] == out) return false;
            c->fields[f.id] = out;
            return true;
        }, QStringLiteral("g%1:%2").arg(clipId).arg(QString::fromStdString(f.id)));
    };
    auto reset = [this, clipId](const TemplateField& f) {
        state_->edit(tr("Reset %1").arg(QString::fromStdString(f.label)), [clipId, f](Project&, Sequence& s) {
            Clip* c = edit::clipById(s, clipId);
            return c && c->fields.erase(f.id) > 0;
        });
    };
    for (const TemplateField& f : fields) {
        auto* row = new QWidget(content_);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(3);
        const QString label = QString::fromStdString(f.label);
        if (f.kind == "text") {
            auto* edit = new QPlainTextEdit(row);
            edit->setObjectName(QStringLiteral("field_") + QString::fromStdString(f.id));
            edit->setFixedHeight(48);
            h->addWidget(edit, 1);
            connect(edit, &QPlainTextEdit::textChanged, this, [edit, setValue, f] { setValue(f, edit->toPlainText()); });
            refreshers_.push_back([edit, valueOf, f] {
                if (edit->hasFocus() || edit->toPlainText() == valueOf(f)) return;
                QSignalBlocker b(edit);
                edit->setPlainText(valueOf(f));
            });
        } else if (f.kind == "color") {
            auto* btn = new QToolButton(row);
            btn->setObjectName(QStringLiteral("field_") + QString::fromStdString(f.id));
            btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            btn->setIconSize(QSize(28, 14));
            h->addWidget(btn, 1);
            connect(btn, &QToolButton::clicked, this, [this, valueOf, clipId, tplId, f, label] {
                const QColor start(valueOf(f));
                QColorDialog dlg(start, this);
                dlg.setWindowTitle(label);
                dlg.setOption(QColorDialog::DontUseNativeDialog);
                // Seen as it is picked; one undo step on OK, and Cancel leaves the clip as it was (its own value or none).
                state_->beginGesture(tr("Change %1").arg(label));
                connect(&dlg, &QColorDialog::currentColorChanged, this, [this, clipId, tplId, f](const QColor& c) {
                    state_->updateGesture([&](Project& p, Sequence& s) {
                        Clip* clip = edit::clipById(s, clipId);
                        const Sequence* t = p.findSequence(tplId);
                        std::string out;
                        if (clip && t && normalizeFieldValue(p, *t, f, c.name().toStdString(), out)) clip->fields[f.id] = out;
                    });
                });
                state_->endGesture(dlg.exec() == QDialog::Accepted);
            });
            refreshers_.push_back([btn, valueOf, f] {
                const QColor c(valueOf(f));
                btn->setIcon(colorIcon(c));
                btn->setText(c.name().toUpper());
            });
        } else if (f.kind == "toggle") {
            auto* cb = new QCheckBox(row);
            cb->setObjectName(QStringLiteral("field_") + QString::fromStdString(f.id));
            h->addWidget(cb, 1);
            connect(cb, &QCheckBox::toggled, this, [setValue, f](bool on) { setValue(f, on ? QStringLiteral("1") : QStringLiteral("0")); });
            refreshers_.push_back([cb, valueOf, f] {
                QSignalBlocker b(cb);
                cb->setChecked(valueOf(f) == QLatin1String("1"));
            });
        } else if (f.kind == "choice" || f.kind == "media") {
            auto* combo = new QComboBox(row);
            combo->setObjectName(QStringLiteral("field_") + QString::fromStdString(f.id));
            if (f.kind == "choice") {
                double lo = 0, hi = 0;
                std::vector<std::string> choices;
                fieldRange(*tpl, f, lo, hi, &choices);
                for (size_t i = 0; i < choices.size(); ++i) combo->addItem(tr(choices[i].c_str()), QString::number(i));
            } else {
                for (const MediaItem& m : state_->project().media)
                    if (m.kind == MediaKind::Image || m.kind == MediaKind::Video) combo->addItem(QString::fromStdString(m.name), QString::number(m.id));
            }
            h->addWidget(combo, 1);
            connect(combo, &QComboBox::activated, this, [combo, setValue, f](int i) { setValue(f, combo->itemData(i).toString()); });
            refreshers_.push_back([combo, valueOf, f] {
                QSignalBlocker b(combo);
                combo->setCurrentIndex(combo->findData(valueOf(f)));
            });
        } else {
            auto* spin = new QDoubleSpinBox(row);
            spin->setObjectName(QStringLiteral("field_") + QString::fromStdString(f.id));
            double lo = 0, hi = 0;
            fieldRange(*tpl, f, lo, hi);
            spin->setRange(lo, hi);
            spin->setDecimals(hi - lo > 100 ? 1 : 3);
            spin->setKeyboardTracking(false);
            h->addWidget(spin, 1);
            connect(spin, &QDoubleSpinBox::valueChanged, this, [setValue, f](double v) { setValue(f, QString::number(v, 'g', 10)); });
            refreshers_.push_back([spin, valueOf, f] {
                QSignalBlocker b(spin);
                spin->setValue(valueOf(f).toDouble());
            });
        }
        auto* undoBtn = smallButton(row, QStringLiteral("↺"), tr("Back to the template's own setting"));
        h->addWidget(undoBtn);
        connect(undoBtn, &QToolButton::clicked, this, [reset, f] { reset(f); });
        form->addRow(label, row);
    }
}

void InspectorWidget::addParamRow(QFormLayout* form, const ParamInfo& pi, const Target& target) {
    auto* row = new QWidget(content_);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(3);
    const std::string name = pi.name;
    const QString label = language::engine(pi.label);
    const QString mergeKey = target.key + ":" + QString::fromStdString(name);
    std::vector<std::string> names = {name};
    if (pi.kind == ParamKind::Color) names = {name + ".r", name + ".g", name + ".b"};

    // Reads the current value(s) from the live project (or, for an insert a mixer pass is writing, what it plays).
    auto read = [this, target, pi](const std::string& n) -> double {
        const Sequence* s = state_->sequence();
        if (!s) return 0;
        if (!target.clip && target.owner)
            if (const auto live = state_->liveParameter(target.owner, target.effect, n)) return *live;
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
    // A track's or bus's insert moved while the mixer writes its automation goes to that pass (held: whether the
    // control is still being dragged, if it can tell).
    const bool toggle = pi.kind == ParamKind::Bool || pi.kind == ParamKind::Choice;
    const double tolerance = std::max(1e-6, (pi.max - pi.min) * 0.001);
    auto write = [this, target, label, mergeKey, read, toggle, tolerance](std::vector<std::pair<std::string, double>> values,
                                                                           std::function<bool()> held = {}) {
        if (!target.clip && target.owner) {
            bool taken = false;
            for (const auto& [n, v] : values)
                taken = state_->captureParameter({target.owner, target.effect, n, v, read(n), tolerance, toggle, held, toggle}) || taken;
            if (taken) return;
        }
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
            combo->setObjectName(QStringLiteral("choice_") + QString::fromStdString(name));
            for (const auto& c : pi.choices) combo->addItem(language::engine(c));
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
            spin->setObjectName(QStringLiteral("param_%1_%2").arg(target.effect).arg(QString::fromStdString(name)));
            int decimals = pi.step >= 1 ? 0 : pi.step >= 0.1 ? 1 : pi.step >= 0.01 ? 2 : pi.step >= 0.001 ? 3 : pi.step >= 0.0001 ? 4 : 5;
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
            // A slider being dragged is held until let go; other changes (typed, stepped, wheeled) count as held briefly.
            connect(spin, &QDoubleSpinBox::valueChanged, this, [write, name, slider = QPointer<QSlider>(slider)](double v) {
                std::function<bool()> held;
                if (slider && slider->isSliderDown()) held = [slider] { return slider && slider->isSliderDown(); };
                write({{name, v}}, held);
            });
            // Double-click the label area resets to default: provided via context menu below.
            spin->setContextMenuPolicy(Qt::CustomContextMenu);
            double def = pi.def;
            connect(spin, &QWidget::customContextMenuRequested, this, [spin, write, name, def, target, label, this](const QPoint& p) {
                QMenu m;
                m.addAction(tr("Reset to Default"), this, [write, name, def] { write({{name, def}}); });
                if (target.clip && target.keyframes && expressionsAvailable() && ownSetting(target))
                    m.addAction(tr("Expression…"), this, [this, target, name, label, def] { editExpression(target, name, label, def); });
                if (target.publishClip) m.addAction(tr("Publish to Graphic…"), this, [this, target, name, label] { publishParameter(target, name, label); });
                m.exec(spin->mapToGlobal(p));
            });
            refreshWidget = [this, spin, slider, read, readParam, name, pi] {
                double v = read(name);
                QSignalBlocker b(spin);
                spin->setValue(v);
                // (Shows what its expression gives; set by the expression, so not edited here.)
                const Param* q = readParam(name);
                const bool driven = q && !q->expr.empty();
                spin->setEnabled(!driven);
                spin->setToolTip(driven ? tr("Set by its expression (=)") : QString());
                if (slider) {
                    QSignalBlocker b2(slider);
                    slider->setValue(int(std::lround((v - pi.min) / (pi.max - pi.min) * 1000.0)));
                    slider->setEnabled(!driven);
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
        // An expression drives it (a clip's own settings; one value each, so not colours).
        QToolButton* formula = nullptr;
        if (target.clip && pi.kind != ParamKind::Color && expressionsAvailable() && ownSetting(target)) {
            formula = smallButton(row, QStringLiteral("="), tr("Expression"));
            formula->setObjectName(QStringLiteral("expression_%1_%2").arg(target.effect).arg(QString::fromStdString(name)));
            formula->setCheckable(true);
            h->addWidget(formula);
            const double def = pi.def;
            connect(formula, &QToolButton::clicked, this, [this, target, name, label, def, formula] {
                formula->setChecked(!formula->isChecked());  // (whether it has one is shown by refreshing, after the dialog)
                editExpression(target, name, label, def);
            });
        }
        connect(watch, &QToolButton::clicked, this, [this, target, names, label](bool on) {
            FrameTime t = target.time();
            state_->edit(on ? tr("Animate %1").arg(label) : tr("Stop Animating %1").arg(label), [target, names, on, t](Project&, Sequence& s) {
                Effect* e = target.resolve(s);
                if (!e) return false;
                for (const auto& n : names) {
                    Param& p = e->params[n];
                    double v = p.keyedAt(t);  // (the keys', not what an expression makes of them)
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
                    else p.addKey(t, p.keyedAt(t));
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
        refreshWidget = [this, inner, watch, key, prev, next, formula, readParam, names, target] {
            inner();
            const Param* p = readParam(names[0]);
            if (formula) {
                // Checked while there is one; red while it fails (the keyed value is used then).
                const std::string expr = p ? p->expr : std::string();
                const Sequence* s = state_->sequence();
                const Effect* e = s && !expr.empty() ? target.resolve(const_cast<Sequence&>(*s)) : nullptr;
                const std::string why = e ? expressionError(target.clip, e->id, names[0]) : std::string();
                QSignalBlocker b0(formula);
                formula->setChecked(!expr.empty());
                formula->setToolTip(expr.empty() ? tr("Expression") : why.empty() ? tr("Expression: %1").arg(QString::fromStdString(expr))
                                                                                  : tr("Expression failed: %1").arg(QString::fromStdString(why)));
                formula->setStyleSheet(why.empty() ? QString() : QStringLiteral("color: #ff6b6b;"));
                formula->setProperty("failing", !why.empty());
            }
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
    // A clip's setting can be published as a field of the graphic its sequence is (core/GraphicsTemplate.h).
    if (target.publishClip) {
        row->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(row, &QWidget::customContextMenuRequested, this, [this, row, target, name, label](const QPoint& p) {
            QMenu m;
            m.addAction(tr("Publish to Graphic…"), this, [this, target, name, label] { publishParameter(target, name, label); });
            m.exec(row->mapToGlobal(p));
        });
    }
    form->addRow(label, row);
    refreshers_.push_back(refreshWidget);
}

void InspectorWidget::addTitleTextTools(QFormLayout* form, const Clip& clip, const Target& target) {
    // Styling a stretch of the text: the selection in the Text box (all of it when nothing is selected).
    auto* row = new QWidget(content_);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    auto* style = new QPushButton(tr("Style Selected Text…"), row);
    style->setObjectName(QStringLiteral("styleSelectedText"));
    style->setToolTip(tr("Give the text selected in the Text box its own font, size, weight, colour, outline, tracking or baseline"));
    auto* clear = new QPushButton(tr("Clear Styles"), row);
    clear->setObjectName(QStringLiteral("clearTextStyles"));
    h->addWidget(style);
    h->addWidget(clear);
    h->addStretch(1);
    form->addRow(QString(), row);
    auto current = [this, target]() -> Effect* {
        const Sequence* s = state_->sequence();
        return s ? target.resolve(const_cast<Sequence&>(*s)) : nullptr;
    };
    connect(clear, &QPushButton::clicked, this, [this, target] {
        state_->edit(tr("Clear Text Styles"), [target](Project&, Sequence& s) {
            Effect* e = target.resolve(s);
            if (!e || e->s("runs").empty()) return false;
            e->strings.erase("runs");
            return true;
        });
    });
    connect(style, &QPushButton::clicked, this, [this, target, current] {
        const Effect* e = current();
        if (!e) return;
        const QString text = QString::fromStdString(e->s("text", "Title"));
        int from = 0, to = int(text.size());
        if (auto* edit = content_->findChild<QPlainTextEdit*>(QStringLiteral("string_text")); edit && edit->textCursor().hasSelection())
            from = edit->textCursor().selectionStart(), to = edit->textCursor().selectionEnd();
        if (to <= from) return;
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Style Selected Text"));
        auto* f = new QFormLayout(&dlg);
        f->addRow(new QLabel(tr("“%1”").arg(text.mid(from, to - from).left(60)), &dlg));
        auto* setFont = new QCheckBox(tr("Font"), &dlg);
        auto* font = new QFontComboBox(&dlg);
        font->setCurrentFont(QFont(QString::fromStdString(e->s("font", "Sans Serif"))));
        f->addRow(setFont, font);
        auto keepSpin = [&dlg](double lo, double hi, double step, const QString& suffix) {
            auto* sp = new QDoubleSpinBox(&dlg);
            sp->setRange(lo, hi);
            sp->setSingleStep(step);
            sp->setSpecialValueText(tr("Unchanged"));
            sp->setValue(lo);
            sp->setSuffix(suffix);
            return sp;
        };
        auto* size = keepSpin(0, 1000, 1, tr(" px"));
        f->addRow(tr("Size"), size);
        auto* bold = new QCheckBox(tr("Bold"), &dlg);
        bold->setTristate(true);
        bold->setCheckState(Qt::PartiallyChecked);
        auto* italic = new QCheckBox(tr("Italic"), &dlg);
        italic->setTristate(true);
        italic->setCheckState(Qt::PartiallyChecked);
        auto* bi = new QWidget(&dlg);
        auto* bih = new QHBoxLayout(bi);
        bih->setContentsMargins(0, 0, 0, 0);
        bih->addWidget(bold);
        bih->addWidget(italic);
        f->addRow(tr("Weight"), bi);
        auto* weight = keepSpin(0, 1000, 50, QString());
        weight->setToolTip(tr("A variable font's weight axis (100 thin to 900 black)"));
        f->addRow(tr("Weight Axis"), weight);
        std::optional<QColor> fill, outlineColor;
        auto colourButton = [&dlg](std::optional<QColor>& c, const QString& title) {
            auto* b = new QPushButton(tr("Unchanged"), &dlg);
            QObject::connect(b, &QPushButton::clicked, &dlg, [b, &c, title, &dlg] {
                const QColor picked = QColorDialog::getColor(c.value_or(Qt::white), &dlg, title);
                if (!picked.isValid()) return;
                c = picked;
                b->setText(picked.name());
                b->setIcon(colorIcon(picked));
            });
            return b;
        };
        f->addRow(tr("Fill"), colourButton(fill, tr("Fill")));
        auto* outline = keepSpin(-0.5, 50, 0.5, tr(" px"));
        outline->setValue(-0.5);
        f->addRow(tr("Outline Width"), outline);
        f->addRow(tr("Outline Color"), colourButton(outlineColor, tr("Outline Color")));
        auto* tracking = keepSpin(-201, 500, 0.5, QString());
        f->addRow(tr("Tracking"), tracking);
        auto* baseline = keepSpin(-1001, 1000, 1, tr(" px"));
        f->addRow(tr("Baseline Shift"), baseline);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        f->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return;
        TextRun r;
        r.from = from, r.len = to - from;
        if (setFont->isChecked()) r.font = font->currentFont().family().toStdString();
        if (size->value() > size->minimum()) r.size = size->value();
        if (bold->checkState() != Qt::PartiallyChecked) r.bold = bold->isChecked() ? 1 : 0;
        if (italic->checkState() != Qt::PartiallyChecked) r.italic = italic->isChecked() ? 1 : 0;
        if (weight->value() > weight->minimum()) r.axes["wght"] = weight->value();
        auto rgb = [](const QColor& c) { return std::array<double, 3>{c.redF(), c.greenF(), c.blueF()}; };
        if (fill) r.color = rgb(*fill);
        if (outlineColor) r.outlineColor = rgb(*outlineColor);
        if (outline->value() > outline->minimum()) r.outline = outline->value();
        if (tracking->value() > tracking->minimum()) r.tracking = tracking->value();
        if (baseline->value() > baseline->minimum()) r.baseline = baseline->value();
        state_->edit(tr("Style Text"), [target, r](Project&, Sequence& s) {
            Effect* ef = target.resolve(s);
            if (!ef) return false;
            const std::vector<TextRun> runs = addTextRun(parseTextRuns(ef->s("runs")), r);
            const std::string json = textRunsJson(runs);
            if (json == ef->s("runs")) return false;
            ef->strings["runs"] = json;
            return true;
        });
    });

    // Text animations: each a range of the text and what it changes there, with its own settings.
    for (int k : textAnimators(clip.generator)) {
        auto* remove = smallButton(nullptr, QStringLiteral("✕"), tr("Remove Text Animation"));
        QFormLayout* section = addSection(tr("Text Animation %1").arg(k), remove);
        connect(remove, &QToolButton::clicked, this, [this, target, k] {
            state_->edit(tr("Remove Text Animation"), [target, k](Project&, Sequence& s) {
                Effect* e = target.resolve(s);
                if (!e) return false;
                const std::string pre = "ta" + std::to_string(k) + ".";
                std::erase_if(e->params, [&](const auto& kv) { return kv.first.rfind(pre, 0) == 0; });
                return true;
            });
        });
        EffectInfo info;
        info.type = "title";
        info.params = textAnimatorParams(k);
        addParamRows(section, info, target);
    }
    auto* add = new QPushButton(tr("Add Text Animation"), content_);
    add->setObjectName(QStringLiteral("addTextAnimation"));
    add->setToolTip(tr("Move, scale, turn, fade, blur or recolour the text a letter, word or line at a time"));
    connect(add, &QPushButton::clicked, this, [this, target] {
        state_->edit(tr("Add Text Animation"), [target](Project&, Sequence& s) {
            Effect* e = target.resolve(s);
            if (!e) return false;
            addTextAnimator(*e);
            return true;
        });
    });
    layout_->addWidget(add);
}

void InspectorWidget::addStringRow(QFormLayout* form, const StringParamInfo& si, const Target& target) {
    const std::string name = si.name;
    const QString label = language::engine(si.label);
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
            // A title's styled runs stay on the characters they were on.
            if (name == "text" && e->type == "title" && !e->s("runs").empty())
                e->strings["runs"] = textRunsJson(shiftTextRuns(parseTextRuns(e->s("runs")), QString::fromStdString(e->s("text", "Title")),
                                                                QString::fromStdString(val)));
            e->strings[name] = val;
            return true;
        }, mergeKey);
    };
    switch (si.kind) {
        case StringKind::MultilineText: {
            auto* edit = new QPlainTextEdit(content_);
            edit->setObjectName(QStringLiteral("string_") + QString::fromStdString(name));
            edit->setFixedHeight(64);
            std::function<void(QMenu*)> more;
            if (target.publishClip)
                more = [this, target, name, label](QMenu* m) {
                    m->addAction(tr("Publish to Graphic…"), this, [this, target, name, label] { publishParameter(target, name, label); });
                };
            enableSpellCheck(edit, state_, [] { return titleSpellingLanguage(); }, more);
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
            const QString none = si.choices.empty() ? tr("None") : language::engine(si.choices.front());
            refreshers_.push_back([this, combo, read, target, none] {
                QSignalBlocker b(combo);
                combo->clear();
                combo->addItem(none, QString());
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
            // (each shown in the interface's language, kept as written)
            for (const auto& c : si.choices) combo->addItem(language::engine(c), QString::fromStdString(c));
            form->addRow(label, combo);
            connect(combo, &QComboBox::activated, this, [combo, write](int i) { write(combo->itemData(i).toString()); });
            refreshers_.push_back([combo, read] {
                QSignalBlocker b(combo);
                combo->setCurrentIndex(combo->findData(read()));
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

void InspectorWidget::addOGrafFields(QFormLayout* form, const Clip& clip, const Target& target) {
    OGrafGraphic g;
    std::string err;
    const std::string manifest = clip.generator.s("manifest");
    auto* about = new QLabel(content_);
    about->setObjectName(QStringLiteral("ografStatus"));
    about->setWordWrap(true);
    form->addRow(QString(), about);
    if (manifest.empty() || !readOGraf(manifest, g, &err)) {
        about->setText(manifest.empty() ? tr("Choose a graphic: its .ograf.json") : QString::fromStdString(err));
        return;
    }
    const Sequence* seq = state_->sequence();
    const std::string failed = seq ? ografFailure(ografClipJob(clip.generator, seq->width, seq->height, seq->fpsValue(), clip.duration)) : std::string();
    if (ografBrowser().empty())
        about->setText(tr("%1. To draw it, install Chrome, Chromium or Edge (or set MONTAGE_CHROMIUM)").arg(QString::fromStdString(g.name)));
    else if (!failed.empty())
        about->setText(tr("%1 cannot be drawn: %2").arg(QString::fromStdString(g.name), QString::fromStdString(failed)));
    else
        about->setText(g.description.empty() ? QString::fromStdString(g.name) : QString::fromStdString(g.name + ": " + g.description));
    // The data: one row per field of its schema, each writing its key into the data's JSON.
    const QJsonObject defaults = QJsonDocument::fromJson(QByteArray::fromStdString(ografDefaultData(g))).object();
    auto read = [this, target, defaults](const QString& key) -> QJsonValue {
        const Sequence* s = state_->sequence();
        const Effect* e = s ? target.resolve(const_cast<Sequence&>(*s)) : nullptr;
        if (!e) return {};
        const QJsonObject data = QJsonDocument::fromJson(QByteArray::fromStdString(e->s("data"))).object();
        return data.contains(key) ? data.value(key) : defaults.value(key);
    };
    for (const OGrafField& f : g.fields) {
        const QString key = QString::fromStdString(f.key);
        const QString label = f.title.empty() ? key : QString::fromStdString(f.title);
        const QString mergeKey = target.key + ":ograf:" + key;
        auto write = [this, target, key, label, mergeKey](const QJsonValue& v) {
            state_->edit(tr("Change %1").arg(label), [target, key, v](Project&, Sequence& s) {
                Effect* e = target.resolve(s);
                if (!e) return false;
                QJsonObject data = QJsonDocument::fromJson(QByteArray::fromStdString(e->s("data"))).object();
                if (data.value(key) == v) return false;
                data[key] = v;
                e->strings["data"] = QJsonDocument(data).toJson(QJsonDocument::Compact).toStdString();
                return true;
            }, mergeKey);
        };
        QWidget* w = nullptr;
        if (!f.choices.empty()) {
            auto* box = new QComboBox(content_);
            for (const std::string& c : f.choices) box->addItem(QString::fromStdString(c));
            connect(box, &QComboBox::currentTextChanged, this, [write](const QString& t) { write(t); });
            refreshers_.push_back([box, read, key] {
                QSignalBlocker b(box);
                box->setCurrentIndex(box->findText(read(key).toString()));  // (none picked when it has no value)
            });
            w = box;
        } else if (f.type == "string" && f.multiline) {
            // Text is written once typing pauses: each new value renders the graphic again.
            auto* edit = new QPlainTextEdit(content_);
            edit->setFixedHeight(64);
            auto* pause = new QTimer(edit);
            pause->setSingleShot(true);
            pause->setInterval(700);
            connect(pause, &QTimer::timeout, this, [edit, write] { write(edit->toPlainText()); });
            connect(edit, &QPlainTextEdit::textChanged, pause, qOverload<>(&QTimer::start));
            refreshers_.push_back([edit, read, key] {
                if (edit->hasFocus() || edit->toPlainText() == read(key).toString()) return;
                QSignalBlocker b(edit);
                edit->setPlainText(read(key).toString());
            });
            w = edit;
        } else if (f.type == "string") {
            auto* line = new QLineEdit(content_);
            auto* pause = new QTimer(line);
            pause->setSingleShot(true);
            pause->setInterval(700);
            connect(pause, &QTimer::timeout, this, [line, write] { write(line->text()); });
            connect(line, &QLineEdit::textEdited, pause, qOverload<>(&QTimer::start));
            connect(line, &QLineEdit::editingFinished, this, [line, pause, write] {
                pause->stop();
                write(line->text());
            });
            refreshers_.push_back([line, read, key] {
                if (!line->hasFocus()) line->setText(read(key).toString());
            });
            w = line;
        } else if (f.type == "number") {
            auto* spin = new QDoubleSpinBox(content_);
            spin->setRange(-1e9, 1e9);
            spin->setDecimals(3);
            spin->setKeyboardTracking(false);  // (a typed number is written when it is finished)
            connect(spin, &QDoubleSpinBox::valueChanged, this, [write](double v) { write(v); });
            refreshers_.push_back([spin, read, key] {
                QSignalBlocker b(spin);
                spin->setValue(read(key).toDouble());
            });
            w = spin;
        } else if (f.type == "integer") {
            auto* spin = new QSpinBox(content_);
            spin->setRange(-1000000000, 1000000000);
            spin->setKeyboardTracking(false);
            connect(spin, &QSpinBox::valueChanged, this, [write](int v) { write(v); });
            refreshers_.push_back([spin, read, key] {
                QSignalBlocker b(spin);
                spin->setValue(read(key).toInt());
            });
            w = spin;
        } else if (f.type == "boolean") {
            auto* check = new QCheckBox(content_);
            connect(check, &QCheckBox::toggled, this, [write](bool v) { write(v); });
            refreshers_.push_back([check, read, key] {
                QSignalBlocker b(check);
                check->setChecked(read(key).toBool());
            });
            w = check;
        } else {
            // An object or a list: its JSON.
            auto* line = new QLineEdit(content_);
            line->setPlaceholderText(tr("JSON"));
            connect(line, &QLineEdit::editingFinished, this, [this, line, write, label] {
                QJsonParseError pe;
                const QJsonDocument d = QJsonDocument::fromJson(("[" + line->text() + "]").toUtf8(), &pe);
                if (pe.error != QJsonParseError::NoError || d.array().size() != 1) {
                    state_->message(tr("%1 is not JSON").arg(label), 4000);
                    return;
                }
                write(d.array().at(0));
            });
            refreshers_.push_back([line, read, key] {
                if (line->hasFocus()) return;
                const QJsonValue v = read(key);
                line->setText(v.isUndefined() || v.isNull() ? QString()
                              : QString::fromUtf8(QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact)).mid(1).chopped(1));
            });
            w = line;
        }
        w->setObjectName("ograf_" + key);
        if (!f.description.empty()) w->setToolTip(QString::fromStdString(f.description));
        form->addRow(label, w);
    }
    // Rendering ahead: otherwise its frames render the first time they are shown.
    auto* row = new QWidget(content_);
    auto* rh = new QHBoxLayout(row);
    rh->setContentsMargins(0, 0, 0, 0);
    auto* render = new QPushButton(tr("Render"), row);
    render->setObjectName(QStringLiteral("renderOGraf"));
    render->setToolTip(tr("Render the graphic's frames now (they are kept, and rendered again only when it changes)"));
    rh->addStretch(1);
    rh->addWidget(render);
    form->addRow(QString(), row);
    const Id clipId = clip.id;
    connect(render, &QPushButton::clicked, this, [this, clipId] { QTimer::singleShot(0, this, [this, clipId] { renderOGrafClip(clipId); }); });
}

void InspectorWidget::renderOGrafClip(Id clip) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    if (!c || c->generator.type != "ograf") return;
    const OGrafJob job = ografClipJob(c->generator, s->width, s->height, s->fpsValue(), c->duration);
    if (ografCached(job)) {
        state_->message(tr("The graphic is rendered"), 3000);
        return;
    }
    if (runAnalysis(tr("Rendering the graphic..."), [job](const auto& progress, const auto* cancel, std::string* err) {
            return prepareOGraf(job, progress, cancel, err);
        }))
        state_->message(tr("Rendered the graphic"), 3000);
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

void InspectorWidget::takeFromFrame(Id clip, Id effect) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    const Effect* e = nullptr;
    if (c)
        for (const Effect& x : c->effects)
            if (x.id == effect) e = &x;
    if (!e) return;
    Effect taken = *e;
    std::string err;
    const bool perfs = e->type == "perf_lock";
    if (!(perfs ? takePerforationReference : takeFilmBase)(state_->project(), *s, *c, taken, state_->playhead(), &err)) {
        state_->message(QString::fromStdString(err), 6000);
        return;
    }
    state_->edit(perfs ? tr("Set Perforation Reference") : tr("Take Film Base"), [clip, effect, taken](Project&, Sequence& sq) {
        Effect* x = edit::ownedEffect(sq, clip, effect);
        if (!x) return false;
        x->strings = taken.strings;
        x->params = taken.params;
        return true;
    });
}

void InspectorWidget::analyzeStabilize(Id clip, Id effect) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    if (!c) return;
    // The analysis reads a copy: editing can go on meanwhile.
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    std::string motion, gyro, gyroNote;
    // (camera motion data is for Stabilize only)
    bool steadies = false;
    for (const Effect& e : c->effects)
        if (e.id == effect) steadies = e.type == "stabilize";
    const bool ok = runAnalysis(tr("Analysing camera movement..."), [&, project, seqId, clip](const auto& progress, const auto* cancel, std::string* err) {
        const Sequence* sq = project->findSequence(seqId);
        const Clip* cl = sq ? edit::clipById(*sq, clip) : nullptr;
        return cl && analyzeClipStabilization(*project, *sq, *cl, motion, progress, cancel, err, steadies ? &gyro : nullptr,
                                              steadies ? &gyroNote : nullptr);
    });
    if (!ok) return;
    // Motion data found switches Stabilize to Camera Gyro (once: a method picked afterwards stays).
    bool usingGyro = false;
    state_->edit(steadies ? tr("Analyze Stabilization") : tr("Analyze Camera Motion"), [clip, effect, motion, gyro, &usingGyro](Project&, Sequence& sq) {
        Effect* e = edit::ownedEffect(sq, clip, effect);
        if (!e) return false;
        e->strings["motion"] = motion;
        if (e->type == "stabilize") {
            const bool had = !e->s("gyro").empty();
            if (gyro.empty()) {
                e->strings.erase("gyro");
                if (std::lround(e->p("method", 0, 2)) == 3) e->params["method"] = Param(2);
            } else {
                e->strings["gyro"] = gyro;
                if (!had) e->params["method"] = Param(3);
                usingGyro = std::lround(e->p("method", 0, 2)) == 3;
            }
        }
        return true;
    });
    if (usingGyro) state_->message(tr("Stabilizing with the camera's motion data"), 5000);
    else if (!gyroNote.empty() && gyroNote != "The clip has no camera motion data")
        state_->message(tr("Camera motion data not used: %1").arg(QString::fromStdString(gyroNote)), 8000);
}

void InspectorWidget::solveCameraTrack(Id clip, Id effect) {
    const Sequence* s = state_->sequence();
    if (!s || !edit::clipById(*s, clip)) return;
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    CameraSolve cs;
    const bool ok = runAnalysis(tr("Solving the camera..."), [&, project, seqId, clip](const auto& progress, const auto* cancel, std::string* err) {
        const Sequence* sq = project->findSequence(seqId);
        const Clip* cl = sq ? edit::clipById(*sq, clip) : nullptr;
        return cl && solveClipCamera(*project, *sq, *cl, {}, cs, [&](double f, const char*) { progress(f); }, cancel, err);
    });
    if (!ok) return;
    const std::string solve = cameraSolveToString(cs);
    state_->edit(tr("Solve Camera"), [clip, effect, solve](Project&, Sequence& sq) {
        Effect* e = edit::ownedEffect(sq, clip, effect);
        if (!e) return false;
        e->strings["solve"] = solve;
        return true;
    });
    state_->message(cs.oriented ? tr("Solved the camera: %1 px error").arg(cs.error, 0, 'f', 2)
                                : tr("Solved the camera, but found no ground to place titles on"),
                    5000);
}

Id InspectorWidget::placeTitleInScene(Id clip, Id effect, const QString& text) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    const Effect* e = c ? edit::ownedEffect(const_cast<Sequence&>(*s), clip, effect) : nullptr;
    CameraSolve cs;
    if (!e || !cameraSolveFromString(e->s("solve"), cs)) {
        state_->message(tr("Solve the camera first"), 5000);
        return 0;
    }
    const FrameTime local = std::clamp<FrameTime>(state_->playhead() - c->start, 0, c->duration - 1);
    std::array<Vec3, 4> corners;
    std::string err;
    if (!sceneRectangleAt(*s, *c, cs, local, e->p("place_x", local, 50) / 100, e->p("place_y", local, 70) / 100,
                          e->p("place_size", local, 40) / 100, double(s->height) / s->width, e->p("place_upright", local) > 0.5,
                          e->p("place_turn", local), corners, &err)) {
        state_->message(QString::fromStdString(err), 6000);
        return 0;
    }
    Id made = 0;
    state_->edit(tr("Place Title in Scene"), [&](Project& p, Sequence& sq) {
        made = placeSceneTitle(p, sq, clip, text.toStdString(), corners, &err);
        return made != 0;
    });
    if (!made) state_->message(QString::fromStdString(err), 6000);
    return made;
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
    const int which = state_->maskObject(*e);
    if (!e->object || e->object->object(which).prompts.empty()) {
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
        result = trackClipObject(*project, *sq, *cl, *ef->object, from, forward, progress, cancel, err, which);
        return result != nullptr;
    });
    if (!ok) return;
    state_->edit(tr("Track Object"), [clip, effect, result](Project&, Sequence& sq) {
        Effect* ef = edit::ownedEffect(sq, clip, effect);
        if (!ef) return false;
        ef->object = result;
        return true;
    });
    state_->message(tr("Tracked the object: %n frame(s) segmented", "", int(result->object(which).frames.size())), 4000);
}

void InspectorWidget::analyzeDepth(Id clip) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    if (!c || !ensureEffectModel(window(), "video_depth")) return;
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    const bool ok = runAnalysis(tr("Analysing depth over the clip..."), [&, project, seqId](const auto& progress, const auto* cancel, std::string* err) {
        const Sequence* sq = project->findSequence(seqId);
        const Clip* cl = sq ? edit::clipById(*sq, clip) : nullptr;
        return cl && analyzeClipDepth(*project, *sq, *cl, 518, [&](double f) { progress(f); return !*cancel; }, err);
    });
    if (!ok) return;
    // Drawn again with it (nothing in the project changed).
    state_->amend([](Project&, Sequence&) { return true; });
    state_->message(tr("Depth analysed over the clip: depth effects on it are steady"), 4000);
}

void InspectorWidget::trackWarpPins(Id clip, Id effect) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    if (!c || c->duration <= 0 || !edit::ownedEffect(const_cast<Sequence&>(*s), clip, effect)) return;
    const FrameTime at = std::clamp<FrameTime>(state_->playhead() - c->start, 0, c->duration - 1);
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    std::map<int, std::pair<Param, Param>> keys;
    const bool ok = runAnalysis(tr("Tracking pins..."), [&, project, seqId](const auto& progress, const auto* cancel, std::string* err) {
        Sequence* sq = const_cast<Sequence*>(project->findSequence(seqId));
        const Clip* cl = sq ? edit::clipById(*sq, clip) : nullptr;
        const Effect* ef = sq ? edit::ownedEffect(*sq, clip, effect) : nullptr;
        return cl && ef && trackMeshPins(*project, *sq, *cl, *ef, at, keys, [&](double f) { progress(f); return !*cancel; }, err);
    });
    if (!ok) return;
    state_->edit(tr("Track Pins"), [clip, effect, keys](Project&, Sequence& sq) {
        Effect* ef = edit::ownedEffect(sq, clip, effect);
        if (!ef) return false;
        for (const auto& [k, xy] : keys) {
            ef->params["pin" + std::to_string(k) + ".to_x"] = xy.first;
            ef->params["pin" + std::to_string(k) + ".to_y"] = xy.second;
        }
        return true;
    });
    state_->message(tr("Tracked %n pin(s)", "", int(keys.size())), 4000);
}

void InspectorWidget::followFocusOn(Id clip, Id effect, FocusTarget target) {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? edit::clipById(*s, clip) : nullptr;
    const Effect* e = c ? edit::ownedEffect(const_cast<Sequence&>(*s), clip, effect) : nullptr;
    if (!c || !e || c->duration <= 0) return;
    if (target == FocusTarget::Face && !ensureEffectModel(window(), "focus.face")) return;
    if (!clipDepthAnalysed(state_->project(), *s, *c) && !ensureEffectModel(window(), "video_depth")) return;
    FollowFocusSettings fs;
    fs.target = target;
    fs.at = std::clamp<FrameTime>(state_->playhead() - c->start, 0, c->duration - 1);
    fs.x = e->p("point_x", fs.at, 0.5), fs.y = e->p("point_y", fs.at, 0.5);
    auto project = std::make_shared<const Project>(state_->project());
    const Id seqId = s->id;
    Param focus;
    const bool ok = runAnalysis(tr("Following focus..."), [&, project, seqId](const auto& progress, const auto* cancel, std::string* err) {
        Sequence* sq = const_cast<Sequence*>(project->findSequence(seqId));
        const Clip* cl = sq ? edit::clipById(*sq, clip) : nullptr;
        const Effect* ef = sq ? edit::ownedEffect(*sq, clip, effect) : nullptr;
        return cl && ef && followFocus(*project, *sq, *cl, *ef, fs, focus, [&](double f) { progress(f); return !*cancel; }, err);
    });
    if (!ok) return;
    // The Focus Depth keyed, and focused on it.
    state_->edit(tr("Follow Focus"), [clip, effect, focus](Project&, Sequence& sq) {
        Effect* ef = edit::ownedEffect(sq, clip, effect);
        if (!ef) return false;
        ef->params["focus"] = focus;
        ef->params["follow"] = Param(0.0);
        return true;
    });
    state_->message(tr("Follow Focus: %n key(s) on Focus Depth", "", int(focus.keys.size())), 4000);
}

}  // namespace montage
