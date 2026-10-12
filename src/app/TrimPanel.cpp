#include "TrimPanel.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "EditorState.h"
#include "core/EditOps.h"
#include "render/TrimPasses.h"

namespace montage {

TrimPanel::TrimPanel(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout;
    targets_ = new QComboBox(this);
    targets_->setObjectName(QStringLiteral("trimTargetShown"));
    targets_->setToolTip(tr("The display the Program monitor shows the cut on, and the one the trims below are for"));
    add_ = new QToolButton(this);
    add_->setObjectName(QStringLiteral("addTrimTarget"));
    add_->setText(QStringLiteral("+"));
    add_->setToolTip(tr("Add a trim target"));
    add_->setPopupMode(QToolButton::InstantPopup);
    auto* presets = new QMenu(add_);
    for (const TrimTarget& t : trimTargetPresets()) {
        const QString id = QString::fromStdString(t.id);
        presets->addAction(QString::fromStdString(t.name), this, [this, id] { addTarget(id); });
    }
    add_->setMenu(presets);
    remove_ = new QToolButton(this);
    remove_->setObjectName(QStringLiteral("removeTrimTarget"));
    remove_->setText(QStringLiteral("−"));
    remove_->setToolTip(tr("Remove the target shown and its trims"));
    top->addWidget(targets_, 1);
    top->addWidget(add_);
    top->addWidget(remove_);
    v->addLayout(top);

    shotLabel_ = new QLabel(this);
    shotLabel_->setObjectName(QStringLiteral("trimShot"));
    shotLabel_->setWordWrap(true);
    v->addWidget(shotLabel_);
    auto* form = new QFormLayout;
    for (const TrimControl& c : trimControls()) {
        auto* box = new QDoubleSpinBox(this);
        box->setObjectName(QStringLiteral("trim_") + QLatin1String(c.name));
        box->setRange(c.min, c.max);
        box->setDecimals(3);
        box->setSingleStep((c.max - c.min) / 100);
        box->setValue(c.def);
        box->setKeyboardTracking(false);  // (typed values count once finished)
        const std::string name = c.name;
        connect(box, &QDoubleSpinBox::valueChanged, this, [this, name](double value) {
            if (!updating_) setControl(QString::fromStdString(name), value);
        });
        form->addRow(tr(c.label), box);
        controls_[c.name] = box;
    }
    v->addLayout(form);
    auto* buttons = new QHBoxLayout;
    key_ = new QToolButton(this);
    key_->setObjectName(QStringLiteral("trimKeyframe"));
    key_->setText(tr("Keyframe"));
    key_->setCheckable(true);
    key_->setToolTip(tr("Changes add keyframes at the playhead (a control with keyframes always does)"));
    buttons->addWidget(key_);
    reset_ = new QPushButton(tr("Reset"), this);
    reset_->setObjectName(QStringLiteral("resetTrim"));
    reset_->setToolTip(tr("Take the shot's trim for this target away"));
    copy_ = new QPushButton(tr("Copy to Selected"), this);
    copy_->setObjectName(QStringLiteral("copyTrim"));
    copy_->setToolTip(tr("Put this shot's trim for this target on the selected clips"));
    buttons->addWidget(reset_);
    buttons->addWidget(copy_);
    v->addLayout(buttons);
    queue_ = new QPushButton(tr("Queue Passes…"), this);
    queue_->setObjectName(QStringLiteral("queueTrimPasses"));
    queue_->setToolTip(tr("Render every target into a folder (the render queue): HDR ones as 10-bit HEVC with HDR10+, SDR as H.264"));
    v->addWidget(queue_);
    v->addStretch(1);

    connect(targets_, &QComboBox::currentIndexChanged, this, [this] {
        if (updating_) return;
        refresh();
        emit shownChanged(shown());
    });
    connect(remove_, &QToolButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Remove Trim Target"), tr("Remove %1 and every shot's trim for it?").arg(targets_->currentText())) ==
            QMessageBox::Yes)
            removeShown();
    });
    connect(reset_, &QPushButton::clicked, this, [this] { resetShot(); });
    connect(copy_, &QPushButton::clicked, this, [this] { copyToSelection(); });
    connect(queue_, &QPushButton::clicked, this, &TrimPanel::queueRequested);
    for (auto sig : {&EditorState::projectChanged, &EditorState::sequenceSwitched}) connect(state_, sig, this, &TrimPanel::refresh);
    connect(state_, &EditorState::playheadChanged, this, [this] { refreshShot(); });
    refresh();
}

QString TrimPanel::shown() const { return targets_->currentData().toString(); }

void TrimPanel::showTarget(const QString& target) {
    const int i = targets_->findData(target);
    targets_->setCurrentIndex(std::max(0, i));
}

Id TrimPanel::shot() const {
    const Sequence* s = state_->sequence();
    const Clip* c = s ? trimShot(*s, state_->playhead()) : nullptr;
    return c ? c->id : 0;
}

void TrimPanel::refresh() {
    const Sequence* s = state_->sequence();
    const QString was = shown();
    updating_ = true;
    targets_->clear();
    targets_->addItem(tr("Master (as graded)"), QString());
    if (s)
        for (const TrimTarget& t : s->trimTargets) targets_->addItem(QString::fromStdString(t.name), QString::fromStdString(t.id));
    const int at = targets_->findData(was);
    targets_->setCurrentIndex(std::max(0, at));
    updating_ = false;
    refreshShot();
    if (at < 0 && !was.isEmpty()) emit shownChanged(QString());  // (its target was removed: the master shows again)
}

void TrimPanel::refreshShot() {
    const Sequence* s = state_->sequence();
    updating_ = true;
    const std::string target = shown().toStdString();
    const Clip* c = s ? trimShot(*s, state_->playhead()) : nullptr;
    const bool editing = s && c && !target.empty();
    shotLabel_->setText(target.empty() ? tr("The master shows: pick a target to trim for")
                        : c        ? tr("Shot: %1").arg(QString::fromStdString(c->name))
                                   : tr("No shot at the playhead"));
    const auto trim = editing ? c->trims.find(target) : decltype(c->trims)::const_iterator{};
    const bool has = editing && trim != c->trims.end();
    for (const TrimControl& k : trimControls()) {
        QDoubleSpinBox* box = controls_[k.name];
        box->setEnabled(editing);
        if (!box->hasFocus())  // (not under someone typing)
            box->setValue(has ? trim->second.p(k.name, state_->playhead() - c->start, k.def) : k.def);
        const auto p = has ? trim->second.params.find(k.name) : decltype(trim->second.params)::const_iterator{};
        box->setToolTip(has && p != trim->second.params.end() && p->second.animated() ? tr("Keyframed: set at the playhead") : QString());
    }
    remove_->setEnabled(!target.empty());
    reset_->setEnabled(has);
    copy_->setEnabled(editing);
    queue_->setEnabled(s && !s->trimTargets.empty());
    updating_ = false;
}

bool TrimPanel::addTarget(const QString& preset) {
    const auto& all = trimTargetPresets();
    const auto it = std::find_if(all.begin(), all.end(), [&](const TrimTarget& t) { return t.id == preset.toStdString(); });
    if (it == all.end() || !state_->sequence()) return false;
    const size_t before = state_->sequence()->trimTargets.size();
    const TrimTarget target = *it;
    if (!state_->apply(tr("Add Trim Target"), [target](Project&, Sequence& s) { return edit::addTrimTarget(s, target); })) return false;
    showTarget(QString::fromStdString(state_->sequence()->trimTargets.at(before).id));
    return true;
}

bool TrimPanel::removeShown() {
    const std::string target = shown().toStdString();
    if (target.empty()) return false;
    return state_->apply(tr("Remove Trim Target"), [target](Project&, Sequence& s) { return edit::removeTrimTarget(s, target); });
}

bool TrimPanel::setControl(const QString& name, double value) {
    const std::string target = shown().toStdString(), control = name.toStdString();
    const Id clip = shot();
    if (target.empty() || !clip) return false;
    const FrameTime at = state_->playhead();
    QString why;
    const bool ok = state_->edit(
        tr("Trim"),
        [&](Project&, Sequence& s) {
            const edit::Result r = edit::setTrim(s, clip, target, {{control, value}}, at, key_->isChecked());
            why = QString::fromStdString(r.error);
            return r.ok;
        },
        QStringLiteral("trim:%1:%2:%3").arg(clip).arg(shown(), name));
    if (!ok && !why.isEmpty()) state_->message(why, 4000);
    return ok;
}

bool TrimPanel::resetShot() {
    const std::string target = shown().toStdString();
    const Id clip = shot();
    if (target.empty() || !clip) return false;
    return state_->apply(tr("Reset Trim"), [clip, target](Project&, Sequence& s) { return edit::resetTrims(s, {clip}, target); });
}

bool TrimPanel::copyToSelection() {
    const std::string target = shown().toStdString();
    const Id clip = shot();
    if (target.empty() || !clip) return false;
    std::vector<Id> to;
    for (Id id : state_->selectedClips())
        if (id != clip) to.push_back(id);
    if (to.empty()) {
        state_->message(tr("Select the clips to copy the trim onto"), 4000);
        return false;
    }
    return state_->apply(tr("Copy Trim"), [clip, to, target](Project&, Sequence& s) { return edit::copyTrim(s, clip, to, target); });
}

}  // namespace montage
