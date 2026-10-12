#include "GalleryPanel.h"

#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "EditorState.h"
#include "core/EditOps.h"
#include "render/Gallery.h"

namespace montage {

GalleryPanel::GalleryPanel(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout;
    albums_ = new QComboBox(this);
    albums_->setObjectName(QStringLiteral("galleryAlbum"));
    albums_->setToolTip(tr("Album"));
    auto* newAlbum = new QToolButton(this);
    newAlbum->setText(QStringLiteral("+"));
    newAlbum->setToolTip(tr("New Album…"));
    top->addWidget(albums_, 1);
    top->addWidget(newAlbum);
    v->addLayout(top);

    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("galleryStills"));
    list_->setViewMode(QListView::IconMode);
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setIconSize(QSize(160, 90));
    list_->setGridSize(QSize(176, 128));
    list_->setWordWrap(true);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    v->addWidget(list_, 1);

    auto* buttons = new QHBoxLayout;
    auto* grabButton = new QPushButton(tr("Grab Still"), this);
    grabButton->setObjectName(QStringLiteral("grabStill"));
    grabButton->setToolTip(tr("Keep the Program monitor's picture with the grade of the clip it shows"));
    wipe_ = new QPushButton(tr("Wipe"), this);
    wipe_->setObjectName(QStringLiteral("wipeStill"));
    wipe_->setCheckable(true);
    wipe_->setToolTip(tr("Compare the Program monitor with the still (drag the divider); double-click a still to do the same"));
    auto* apply = new QPushButton(tr("Apply Grade"), this);
    apply->setObjectName(QStringLiteral("applyStill"));
    apply->setToolTip(tr("Put the still's grade on the selected clips (or the clip at the playhead), in place of theirs"));
    buttons->addWidget(grabButton);
    buttons->addWidget(wipe_);
    buttons->addWidget(apply);
    v->addLayout(buttons);

    connect(albums_, &QComboBox::currentTextChanged, this, [this] { refresh(); });
    connect(newAlbum, &QToolButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("New Album"), tr("Name:"), QLineEdit::Normal, QString(), &ok);
        if (ok && !name.trimmed().isEmpty()) addAlbum(name.trimmed());
    });
    connect(grabButton, &QPushButton::clicked, this, [this] { grab(); });
    connect(apply, &QPushButton::clicked, this, [this] { applyToSelection(); });
    connect(wipe_, &QPushButton::toggled, this, [this](bool on) { wipe(on); });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] {
        // (Another still while wiping: wipe against that one.)
        wipe(!(wipe_->isChecked() && wiping_ == selectedStill()));
    });
    connect(list_, &QListWidget::currentItemChanged, this, [this] {
        if (wipe_->isChecked() && !selectedStill().isEmpty() && selectedStill() != wiping_) wipe(true);
    });
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        if (!list_->itemAt(at)) return;
        QMenu menu(this);
        menu.addAction(tr("Apply Grade"), this, [this] { applyToSelection(); });
        menu.addAction(tr("Rename…"), this, [this] {
            GalleryStill s;
            if (!still(selectedStill(), s)) return;
            bool ok = false;
            const QString label = QInputDialog::getText(this, tr("Rename Still"), tr("Label:"), QLineEdit::Normal, QString::fromStdString(s.label), &ok);
            if (ok && !label.trimmed().isEmpty() && renameStill(folder().toStdString(), s, label.trimmed().toStdString())) refresh();
        });
        menu.addAction(tr("Delete…"), this, [this] {
            if (QMessageBox::question(this, tr("Delete Still"), tr("Delete this still and its grade from the gallery?")) == QMessageBox::Yes)
                removeSelected();
        });
        menu.exec(list_->viewport()->mapToGlobal(at));
    });
    // Another project, another gallery.
    shownFolder_ = folder();
    connect(state_, &EditorState::fileStateChanged, this, [this] {
        if (shownFolder_ == folder()) return;
        shownFolder_ = folder();
        if (wipe_->isChecked()) wipe(false);
        refresh();
    });
    refresh();
}

QString GalleryPanel::folder() const { return QString::fromStdString(galleryFolder(state_->filePath().toStdString())); }

QString GalleryPanel::album() const { return albums_->currentText().isEmpty() ? QString(kDefaultAlbum) : albums_->currentText(); }

void GalleryPanel::setAlbum(const QString& name) {
    refresh();
    albums_->setCurrentIndex(std::max(0, albums_->findText(name)));
}

void GalleryPanel::refresh() {
    const QString picked = selectedStill(), keep = album();
    {
        const QSignalBlocker b(albums_);
        albums_->clear();
        for (const std::string& a : galleryAlbums(folder().toStdString())) albums_->addItem(QString::fromStdString(a));
        albums_->setCurrentIndex(std::max(0, albums_->findText(keep)));
    }
    const QSignalBlocker b(list_);
    list_->clear();
    for (const GalleryStill& s : galleryStills(folder().toStdString(), album().toStdString())) {
        QPixmap pm(QString::fromStdString(s.picture));
        if (!pm.isNull()) pm = pm.scaled(list_->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        auto* item = new QListWidgetItem(QIcon(pm), QString::fromStdString(s.label + "  " + s.clip), list_);
        item->setData(Qt::UserRole, QString::fromStdString(s.id));
        item->setToolTip(tr("%1 at %2 in %3\nGrabbed %4\n%n grade effect(s)", "", int(s.effects.size()))
                             .arg(QString::fromStdString(s.clip), QString::fromStdString(s.timecode), QString::fromStdString(s.sequence),
                                  QString::fromStdString(s.created).left(16).replace('T', ' ')));
        if (item->data(Qt::UserRole).toString() == picked) list_->setCurrentItem(item);
    }
}

QString GalleryPanel::selectedStill() const {
    const QListWidgetItem* item = list_->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void GalleryPanel::selectStill(const QString& id) {
    for (int i = 0; i < list_->count(); ++i)
        if (list_->item(i)->data(Qt::UserRole).toString() == id) list_->setCurrentRow(i);
}

bool GalleryPanel::still(const QString& id, GalleryStill& out) const {
    return !id.isEmpty() && findStill(folder().toStdString(), album().toStdString(), id.toStdString(), out);
}

bool GalleryPanel::grab() {
    const Sequence* s = state_->sequence();
    if (!s) return false;
    GalleryStill g;
    std::string err;
    if (!grabStill(state_->project(), *s, state_->playhead(), folder().toStdString(), album().toStdString(), g, &err)) {
        state_->message(QString::fromStdString(err), 5000);
        return false;
    }
    refresh();
    selectStill(QString::fromStdString(g.id));
    state_->message(tr("Grabbed still %1 of %2").arg(QString::fromStdString(g.label), QString::fromStdString(g.clip)), 3000);
    return true;
}

bool GalleryPanel::applyToSelection() {
    GalleryStill g;
    if (!still(selectedStill(), g)) {
        state_->message(tr("Pick a still to apply"), 4000);
        return false;
    }
    const Sequence* s = state_->sequence();
    if (!s) return false;
    // The selected picture clips, else the top one at the playhead.
    std::vector<Id> clips;
    for (Id id : state_->selectedClips())
        if (const auto loc = edit::locate(*s, id); loc && loc->track.kind == TrackKind::Video) clips.push_back(id);
    if (clips.empty())
        for (int i = int(s->videoTracks.size()) - 1; i >= 0 && clips.empty(); --i)
            if (const Clip* c = edit::clipAt(*s, TrackRef{TrackKind::Video, i}, state_->playhead())) clips.push_back(c->id);
    if (clips.empty()) {
        state_->message(tr("Select the clips to grade"), 4000);
        return false;
    }
    return state_->apply(tr("Apply Still Grade"), [g, clips](Project& p, Sequence& sq) { return edit::applyStillGrade(p, sq, g, clips); });
}

bool GalleryPanel::wipe(bool on) {
    GalleryStill g;
    if (on && !still(selectedStill(), g)) {
        state_->message(tr("Pick a still to wipe against"), 4000);
        on = false;
    }
    {
        const QSignalBlocker b(wipe_);
        wipe_->setChecked(on);
    }
    if (!on) {
        const bool was = !wiping_.isEmpty();
        wiping_.clear();
        if (was) emit compareCleared();
        return false;
    }
    const QImage picture(QString::fromStdString(g.picture));
    if (picture.isNull()) {
        state_->message(tr("The still's picture is missing"), 4000);
        return wipe(false);
    }
    wiping_ = QString::fromStdString(g.id);
    emit compareRequested(picture.convertToFormat(QImage::Format_RGBA8888), tr("Still %1: %2").arg(QString::fromStdString(g.label), QString::fromStdString(g.clip)));
    return true;
}

bool GalleryPanel::removeSelected() {
    GalleryStill g;
    if (!still(selectedStill(), g)) return false;
    if (wiping_ == QString::fromStdString(g.id)) wipe(false);
    const bool ok = removeStill(folder().toStdString(), g);
    refresh();
    return ok;
}

bool GalleryPanel::addAlbum(const QString& name) {
    std::string err;
    if (!montage::addAlbum(folder().toStdString(), name.toStdString(), &err)) {
        state_->message(QString::fromStdString(err), 4000);
        return false;
    }
    setAlbum(name);
    return true;
}

void GalleryPanel::wipeEnded() {
    const QSignalBlocker b(wipe_);
    wipe_->setChecked(false);
    wiping_.clear();
}

}  // namespace montage
