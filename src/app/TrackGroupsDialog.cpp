#include "TrackGroupsDialog.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "EditorState.h"
#include "Theme.h"
#include "core/TrackGroups.h"

namespace montage {

namespace {

int trackKey(TrackRef r) { return (r.kind == TrackKind::Video ? 0 : 1) * 100000 + r.index; }
TrackRef trackFromKey(int key) { return {key >= 100000 ? TrackKind::Audio : TrackKind::Video, key % 100000}; }

QIcon swatch(const QColor& c) {
    QPixmap px(14, 14);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setPen(theme::kBorder);
    p.setBrush(c.isValid() ? c : QColor(Qt::transparent));
    p.drawRect(0, 0, 13, 13);
    return QIcon(px);
}

}  // namespace

TrackGroupsDialog::TrackGroupsDialog(EditorState* state, QWidget* parent) : QDialog(parent), state_(state) {
    setWindowTitle(tr("Track Groups"));
    setObjectName(QStringLiteral("trackGroupsDialog"));
    auto* lay = new QHBoxLayout(this);

    // The groups, with buttons to make, rename and delete one.
    auto* left = new QVBoxLayout;
    auto* groupsLabel = new QLabel(tr("&Groups:"), this);
    groups_ = new QListWidget(this);
    groups_->setObjectName(QStringLiteral("trackGroupList"));
    groups_->setAccessibleName(tr("Track groups"));
    groupsLabel->setBuddy(groups_);
    left->addWidget(groupsLabel);
    left->addWidget(groups_, 1);
    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("&New…"), this);
    add->setObjectName(QStringLiteral("newTrackGroup"));
    add->setToolTip(tr("Make a group of the tracks ticked on the right, or of none to tick them after"));
    rename_ = new QPushButton(tr("&Rename…"), this);
    rename_->setObjectName(QStringLiteral("renameTrackGroup"));
    remove_ = new QPushButton(tr("&Delete"), this);
    remove_->setObjectName(QStringLiteral("deleteTrackGroup"));
    remove_->setToolTip(tr("The group goes; its tracks stay"));
    buttons->addWidget(add);
    buttons->addWidget(rename_);
    buttons->addWidget(remove_);
    left->addLayout(buttons);
    lay->addLayout(left, 1);

    // What a change on one of its tracks does to the others.
    auto* middle = new QVBoxLayout;
    auto* box = new QGroupBox(tr("A change on one track reaches the others for"), this);
    auto* switches = new QVBoxLayout(box);
    const std::pair<const char*, QString> names[] = {
        {"groupEditing", tr("&Selection and editing (cut, trim, move, delete, fades)")},
        {"groupMute", tr("&Mute and solo")},
        {"groupVolume", tr("&Volume (relative)")},
        {"groupPan", tr("&Pan (relative)")},
        {"groupAutomation", tr("&Automation modes and writes")},
        {"groupRecord", tr("Record &arming")},
        {"groupComp", tr("&Comping, auditions, ratings and Best Take")},
    };
    for (const auto& [name, text] : names) {
        auto* c = new QCheckBox(text, box);
        c->setObjectName(QString::fromLatin1(name));
        switches->addWidget(c);
        switches_.push_back(c);
        connect(c, &QCheckBox::toggled, this, &TrackGroupsDialog::switchesChanged);
    }
    auto* suspended = new QCheckBox(tr("S&uspended (each track acts alone)"), this);
    suspended->setObjectName(QStringLiteral("groupSuspended"));
    suspended->setToolTip(tr("Sequence › Suspend Track Groups (Ctrl+Shift+G) suspends every group at once"));
    switches_.push_back(suspended);
    connect(suspended, &QCheckBox::toggled, this, &TrackGroupsDialog::switchesChanged);
    middle->addWidget(box);
    middle->addWidget(suspended);
    middle->addStretch(1);
    lay->addLayout(middle, 1);

    // The tracks: ticked when in the group shown; and the highlighted track's colour.
    auto* right = new QVBoxLayout;
    auto* tracksLabel = new QLabel(tr("&Tracks in the group:"), this);
    tracks_ = new QListWidget(this);
    tracks_->setObjectName(QStringLiteral("trackGroupTracks"));
    tracks_->setAccessibleName(tr("Tracks in the group"));
    tracksLabel->setBuddy(tracks_);
    right->addWidget(tracksLabel);
    right->addWidget(tracks_, 1);
    auto* form = new QFormLayout;
    color_ = new QComboBox(this);
    color_->setObjectName(QStringLiteral("trackColor"));
    form->addRow(tr("Track c&olour:"), color_);
    colorClips_ = new QCheckBox(tr("Colour its c&lips"), this);
    colorClips_->setObjectName(QStringLiteral("trackColorClips"));
    colorClips_->setToolTip(tr("Clips without a label of their own are drawn in the track's colour"));
    form->addRow(QString(), colorClips_);
    right->addLayout(form);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    right->addWidget(close);
    lay->addLayout(right, 1);

    connect(add, &QPushButton::clicked, this, &TrackGroupsDialog::promptNewGroup);
    connect(rename_, &QPushButton::clicked, this, &TrackGroupsDialog::renameGroup);
    connect(remove_, &QPushButton::clicked, this, &TrackGroupsDialog::deleteGroup);
    connect(groups_, &QListWidget::currentRowChanged, this, [this] {
        if (!updating_) refresh();
    });
    connect(tracks_, &QListWidget::currentRowChanged, this, [this] {
        if (!updating_) refresh();
    });
    connect(tracks_, &QListWidget::itemChanged, this, [this](QListWidgetItem* it) {
        if (!updating_) memberToggled(tracks_->row(it));
    });
    connect(color_, &QComboBox::activated, this, &TrackGroupsDialog::colorChosen);
    connect(colorClips_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_) return;
        bool ok = false;
        const TrackRef r = currentTrack(&ok);
        if (ok) state_->apply(on ? tr("Colour Track's Clips") : tr("Plain Track Clips"), [=](Project&, Sequence& s) { return edit::setTrackColorClips(s, {r}, on); });
    });
    connect(state_, &EditorState::projectChanged, this, &TrackGroupsDialog::refresh);
    refresh();
    if (tracks_->count()) tracks_->setCurrentRow(0);
}

QCheckBox* TrackGroupsDialog::switchBox(const QString& name) const {
    for (QCheckBox* c : switches_)
        if (c->objectName() == name) return c;
    return nullptr;
}

Id TrackGroupsDialog::currentGroup() const {
    const QListWidgetItem* it = groups_->currentItem();
    return it ? Id(it->data(Qt::UserRole).toULongLong()) : 0;
}

TrackRef TrackGroupsDialog::currentTrack(bool* ok) const {
    const QListWidgetItem* it = tracks_->currentItem();
    *ok = it != nullptr;
    return it ? trackFromKey(it->data(Qt::UserRole).toInt()) : TrackRef{};
}

void TrackGroupsDialog::refresh() {
    const Sequence* s = state_->sequence();
    updating_ = true;
    const Id was = currentGroup();
    const int trackRow = tracks_->currentRow();
    groups_->clear();
    tracks_->clear();
    if (s) {
        for (const TrackGroup& g : s->trackGroups) {
            auto* it = new QListWidgetItem(QString::fromStdString(g.name) + (g.suspended ? tr(" (suspended)") : QString()), groups_);
            it->setData(Qt::UserRole, qulonglong(g.id));
            if (g.id == was) groups_->setCurrentItem(it);
        }
        if (!groups_->currentItem() && groups_->count()) groups_->setCurrentRow(groups_->count() - 1);
    }
    const Id group = currentGroup();
    const TrackGroup* g = s ? findTrackGroup(*s, group) : nullptr;
    if (s)
        for (TrackRef r : allTracks(*s)) {
            const Track* t = trackAt(*s, r);
            const QString id = (r.kind == TrackKind::Video ? QStringLiteral("V") : QStringLiteral("A")) + QString::number(r.index + 1);
            QString text = t->name.empty() || QString::fromStdString(t->name) == id ? id : id + QStringLiteral("  ") + QString::fromStdString(t->name);
            if (t->group && t->group != group)
                if (const TrackGroup* other = findTrackGroup(*s, t->group)) text += tr("  (in %1)").arg(QString::fromStdString(other->name));
            auto* it = new QListWidgetItem(swatch(t->color.empty() ? QColor() : QColor(QString::fromStdString(t->color))), text, tracks_);
            it->setData(Qt::UserRole, trackKey(r));
            if (g) {
                it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
                it->setCheckState(t->group == group ? Qt::Checked : Qt::Unchecked);
            }
        }
    tracks_->setCurrentRow(std::clamp(trackRow, 0, std::max(0, tracks_->count() - 1)));
    const bool values[] = {g && g->editing, g && g->mute, g && g->volume, g && g->pan, g && g->automation, g && g->record, g && g->comp, g && g->suspended};
    for (size_t k = 0; k < switches_.size(); ++k) {
        const QSignalBlocker block(switches_[k]);
        switches_[k]->setChecked(values[k]);
        switches_[k]->setEnabled(g != nullptr);
    }
    rename_->setEnabled(g != nullptr);
    remove_->setEnabled(g != nullptr);
    // The colour of the highlighted track: none, one of the label colours, or its own.
    bool ok = false;
    const TrackRef r = currentTrack(&ok);
    const Track* t = s && ok ? trackAt(*s, r) : nullptr;
    color_->clear();
    color_->addItem(tr("None"), QString());
    for (int k = 1; k < theme::labelCount(); ++k)
        color_->addItem(swatch(theme::labelColor(k)), tr(theme::labelName(k)), theme::labelColor(k).name());
    const QString current = t ? QString::fromStdString(t->color) : QString();
    int at = color_->findData(current);
    if (at < 0) {
        color_->addItem(swatch(QColor(current)), tr("Custom (%1)").arg(current), current);
        at = color_->count() - 1;
    }
    color_->addItem(tr("Custom…"), QStringLiteral("custom"));
    color_->setCurrentIndex(at);
    color_->setEnabled(t != nullptr);
    {
        const QSignalBlocker block(colorClips_);
        colorClips_->setChecked(t && t->colorClips);
        colorClips_->setEnabled(t != nullptr);
    }
    updating_ = false;
}

bool TrackGroupsDialog::newGroup(const QString& name) {
    const Sequence* s = state_->sequence();
    if (!s) return false;
    // Of the tracks ticked (while another group is shown), else the highlighted one.
    std::vector<TrackRef> tracks;
    for (int i = 0; i < tracks_->count(); ++i)
        if (tracks_->item(i)->checkState() == Qt::Checked) tracks.push_back(trackFromKey(tracks_->item(i)->data(Qt::UserRole).toInt()));
    bool ok = false;
    if (tracks.empty())
        if (const TrackRef r = currentTrack(&ok); ok) tracks.push_back(r);
    Id made = 0;
    if (!state_->apply(tr("New Track Group"), [&](Project& p, Sequence& sq) {
            edit::Result r = edit::createTrackGroup(p, sq, name.trimmed().toStdString(), tracks);
            if (r.ok) made = r.created.front();
            return r;
        }))
        return false;
    for (int i = 0; i < groups_->count(); ++i)
        if (Id(groups_->item(i)->data(Qt::UserRole).toULongLong()) == made) groups_->setCurrentRow(i);
    return true;
}

void TrackGroupsDialog::promptNewGroup() {
    bool ok = false;
    const Sequence* s = state_->sequence();
    const QString name = QInputDialog::getText(this, tr("New Track Group"), tr("Group name:"), QLineEdit::Normal,
                                               tr("Group %1").arg(s ? s->trackGroups.size() + 1 : 1), &ok);
    if (ok && !name.trimmed().isEmpty()) newGroup(name);
}

void TrackGroupsDialog::renameGroup() {
    const Id group = currentGroup();
    const Sequence* s = state_->sequence();
    const TrackGroup* g = s ? findTrackGroup(*s, group) : nullptr;
    if (!g) return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename Track Group"), tr("Group name:"), QLineEdit::Normal, QString::fromStdString(g->name), &ok);
    if (ok && !name.trimmed().isEmpty())
        state_->apply(tr("Rename Track Group"), [=](Project&, Sequence& sq) { return edit::renameTrackGroup(sq, group, name.trimmed().toStdString()); });
}

void TrackGroupsDialog::deleteGroup() {
    const Id group = currentGroup();
    if (group) state_->apply(tr("Delete Track Group"), [=](Project&, Sequence& sq) { return edit::removeTrackGroup(sq, group); });
}

void TrackGroupsDialog::switchesChanged() {
    if (updating_) return;
    const Id group = currentGroup();
    if (!group) return;
    TrackGroup g;
    g.editing = switches_[0]->isChecked(), g.mute = switches_[1]->isChecked(), g.volume = switches_[2]->isChecked();
    g.pan = switches_[3]->isChecked(), g.automation = switches_[4]->isChecked(), g.record = switches_[5]->isChecked();
    g.comp = switches_[6]->isChecked(), g.suspended = switches_[7]->isChecked();
    state_->apply(tr("Track Group Switches"), [=](Project&, Sequence& sq) { return edit::setTrackGroupSwitches(sq, group, g); });
}

void TrackGroupsDialog::memberToggled(int row) {
    const Id group = currentGroup();
    QListWidgetItem* it = tracks_->item(row);
    if (!group || !it) return;
    const TrackRef r = trackFromKey(it->data(Qt::UserRole).toInt());
    const bool in = it->checkState() == Qt::Checked;
    state_->apply(in ? tr("Add Track to Group") : tr("Remove Track from Group"),
                  [=](Project&, Sequence& sq) { return edit::setTrackGroup(sq, {r}, in ? group : Id(0)); });
}

void TrackGroupsDialog::colorChosen(int index) {
    bool ok = false;
    const TrackRef r = currentTrack(&ok);
    if (!ok || index < 0) return;
    QString color = color_->itemData(index).toString();
    if (color == QStringLiteral("custom")) {
        const Sequence* s = state_->sequence();
        const Track* t = s ? trackAt(*s, r) : nullptr;
        const QColor c = QColorDialog::getColor(t && !t->color.empty() ? QColor(QString::fromStdString(t->color)) : theme::kAccent, this, tr("Track Colour"));
        if (!c.isValid()) {
            refresh();
            return;
        }
        color = c.name();
    }
    if (!state_->apply(tr("Track Colour"), [=](Project&, Sequence& sq) { return edit::setTrackColor(sq, {r}, color.toStdString()); })) refresh();
}

}  // namespace montage
