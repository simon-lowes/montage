#include "MediaBinWidget.h"

#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QSettings>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "EditorState.h"
#include "Theme.h"
#include "ThumbnailCache.h"

namespace montage {

namespace {
constexpr int kThumbW = 128;
constexpr int kThumbH = 72;

QString kindLabel(const MediaItem& m) {
    switch (m.kind) {
        case MediaKind::Video: return m.hasAudio ? QObject::tr("Video + Audio") : QObject::tr("Video");
        case MediaKind::Audio: return QObject::tr("Audio");
        case MediaKind::Image: return QObject::tr("Still Image");
        case MediaKind::Sequence: return QObject::tr("Sequence");
    }
    return {};
}

// Placeholder tile for media without a picture (audio, sequences).
QPixmap tile(const QString& text, const QColor& color) {
    QPixmap pm(kThumbW, kThumbH);
    pm.fill(color.darker(220));
    QPainter p(&pm);
    p.setPen(color.lighter(150));
    QFont f = p.font();
    f.setPointSize(9);
    f.setBold(true);
    p.setFont(f);
    p.drawText(pm.rect(), Qt::AlignCenter, text);
    return pm;
}
}  // namespace

MediaList::MediaList(QWidget* parent) : QListWidget(parent) {
    setViewMode(QListView::IconMode);
    setIconSize(QSize(kThumbW, kThumbH));
    setGridSize(QSize(kThumbW + 16, kThumbH + 40));
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(false);
    setDragDropMode(QAbstractItemView::DragDrop);
    setWordWrap(true);
    setUniformItemSizes(true);
    setTextElideMode(Qt::ElideMiddle);
}

QStringList MediaList::mimeTypes() const { return {"application/x-montage-media", "text/uri-list"}; }

QMimeData* MediaList::mimeData(const QList<QListWidgetItem*>& items) const {
    QStringList ids;
    for (auto* it : items) ids << QString::number(it->data(Qt::UserRole).toULongLong());
    auto* m = new QMimeData;
    m->setData("application/x-montage-media", ids.join(',').toUtf8());
    return m;
}

void MediaList::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    else QListWidget::dragEnterEvent(e);
}

void MediaList::dragMoveEvent(QDragMoveEvent* e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    else e->ignore();
}

void MediaList::dropEvent(QDropEvent* e) {
    if (!e->mimeData()->hasUrls()) {
        e->ignore();
        return;
    }
    QStringList files;
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile()) files << u.toLocalFile();
    emit filesDropped(files);
    e->acceptProposedAction();
}

MediaBinWidget::MediaBinWidget(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(2);
    auto* bar = new QHBoxLayout;
    auto addButton = [&](const QString& text, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        bar->addWidget(b);
        return b;
    };
    auto* importBtn = addButton(tr("Import"), tr("Import media files (Ctrl+I)"));
    auto* titleBtn = addButton(tr("Title"), tr("New title at the playhead"));
    auto* seqBtn = addButton(tr("Sequence"), tr("New sequence"));
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Search media"));
    search_->setClearButtonEnabled(true);
    bar->addWidget(search_, 1);
    lay->addLayout(bar);
    list_ = new MediaList(this);
    lay->addWidget(list_, 1);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(importBtn, &QToolButton::clicked, this, &MediaBinWidget::importDialog);
    connect(titleBtn, &QToolButton::clicked, this, &MediaBinWidget::newTitleRequested);
    connect(seqBtn, &QToolButton::clicked, this, &MediaBinWidget::newSequenceRequested);
    connect(search_, &QLineEdit::textChanged, this, &MediaBinWidget::rebuild);
    connect(list_, &MediaList::filesDropped, this, [this](const QStringList& files) {
        QStringList errors;
        state_->importFiles(files, &errors);
        if (!errors.isEmpty()) QMessageBox::warning(this, tr("Import"), errors.join("\n"));
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        Id id = it->data(Qt::UserRole).toULongLong();
        const MediaItem* m = state_->project().findMedia(id);
        if (m && m->kind == MediaKind::Sequence) state_->setActiveSequence(m->sequenceId);
        else emit openInSource(id);
    });
    connect(list_, &QWidget::customContextMenuRequested, this, &MediaBinWidget::showContextMenu);
    connect(state_, &EditorState::projectChanged, this, &MediaBinWidget::rebuild);
    connect(state_, &EditorState::sequenceSwitched, this, &MediaBinWidget::rebuild);
    connect(&ThumbnailCache::instance(), &ThumbnailCache::ready, this, &MediaBinWidget::refreshThumbnails);
    rebuild();
}

std::vector<Id> MediaBinWidget::selectedMedia() const {
    std::vector<Id> ids;
    for (auto* it : list_->selectedItems()) ids.push_back(it->data(Qt::UserRole).toULongLong());
    return ids;
}

void MediaBinWidget::importDialog() {
    QSettings settings("Montage", "Montage");
    QString dir = settings.value("lastImportDir").toString();
    QStringList files = QFileDialog::getOpenFileNames(
        this, tr("Import Media"), dir,
        tr("Media (*.mp4 *.mov *.mkv *.avi *.webm *.m4v *.mxf *.mts *.m2ts *.ts *.mpg *.mpeg *.wmv *.flv *.gif "
           "*.wav *.mp3 *.aac *.m4a *.flac *.ogg *.opus *.aif *.aiff *.png *.jpg *.jpeg *.tif *.tiff *.bmp *.webp *.exr);;All files (*)"));
    if (files.isEmpty()) return;
    settings.setValue("lastImportDir", QFileInfo(files.first()).absolutePath());
    QStringList errors;
    state_->importFiles(files, &errors);
    if (!errors.isEmpty()) QMessageBox::warning(this, tr("Import"), errors.join("\n"));
}

void MediaBinWidget::rebuild() {
    std::vector<Id> keep = selectedMedia();
    const Project& p = state_->project();
    QString filter = search_->text().trimmed();
    // Skip the rebuild if the visible set is unchanged (keeps scroll position and selection).
    QStringList signature;
    for (const auto& m : p.media)
        if (filter.isEmpty() || QString::fromStdString(m.name).contains(filter, Qt::CaseInsensitive))
            signature << QString::number(m.id) + QString::fromStdString(m.name);
    if (list_->property("signature").toStringList() == signature) {
        refreshThumbnails();
        return;
    }
    list_->setProperty("signature", signature);
    list_->clear();
    for (const auto& m : p.media) {
        QString name = QString::fromStdString(m.name);
        if (!filter.isEmpty() && !name.contains(filter, Qt::CaseInsensitive)) continue;
        auto* it = new QListWidgetItem(name, list_);
        it->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(m.id));
        QString dur = m.duration > 0 ? QString::fromStdString(formatTimecode(FrameTime(m.duration * (m.fps.valid() ? m.fps.toDouble() : 30)),
                                                                              m.fps.valid() ? m.fps : Rational{30, 1}))
                                     : QString();
        QString tip = QString("<b>%1</b><br>%2").arg(name.toHtmlEscaped(), kindLabel(m));
        if (m.hasVideo && m.width > 0) tip += QString("<br>%1×%2 @ %3 fps, %4").arg(m.width).arg(m.height).arg(m.fps.toDouble(), 0, 'f', 3).arg(QString::fromStdString(m.videoCodec));
        if (m.hasAudio && m.sampleRate > 0) tip += QString("<br>%1 Hz, %2 ch, %3").arg(m.sampleRate).arg(m.channels).arg(QString::fromStdString(m.audioCodec));
        if (!dur.isEmpty()) tip += "<br>" + dur;
        if (!m.path.empty()) tip += "<br><i>" + QString::fromStdString(m.path).toHtmlEscaped() + "</i>";
        it->setToolTip(tip);
        if (std::find(keep.begin(), keep.end(), m.id) != keep.end()) it->setSelected(true);
    }
    refreshThumbnails();
}

void MediaBinWidget::refreshThumbnails() {
    const Project& p = state_->project();
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem* it = list_->item(i);
        const MediaItem* m = p.findMedia(it->data(Qt::UserRole).toULongLong());
        if (!m) continue;
        if (m->kind == MediaKind::Audio) {
            if (it->icon().isNull()) it->setIcon(tile(tr("AUDIO"), theme::kAudioClip));
            continue;
        }
        if (m->kind == MediaKind::Sequence) {
            if (it->icon().isNull()) it->setIcon(tile(tr("SEQUENCE"), theme::kCompoundClip));
            continue;
        }
        if (it->data(Qt::UserRole + 1).toBool()) continue;  // already has its real thumbnail
        double aspect = m->width > 0 && m->height > 0 ? double(m->width) / m->height : 16.0 / 9;
        int w = kThumbW, h = int(kThumbW / aspect);
        if (h > kThumbH) {
            h = kThumbH;
            w = int(kThumbH * aspect);
        }
        double t = m->kind == MediaKind::Video ? std::min(1.0, m->duration * 0.1) : 0.0;
        QImage img = ThumbnailCache::instance().get(QString::fromStdString(m->path), t, std::max(2, w), std::max(2, h));
        if (img.isNull()) {
            if (it->icon().isNull()) it->setIcon(tile(QString(), theme::kVideoClip));
            continue;
        }
        QPixmap pm(kThumbW, kThumbH);
        pm.fill(Qt::black);
        QPainter pa(&pm);
        pa.drawImage(QPoint((kThumbW - img.width()) / 2, (kThumbH - img.height()) / 2), img);
        it->setIcon(pm);
        it->setData(Qt::UserRole + 1, true);
    }
}

void MediaBinWidget::showContextMenu(const QPoint& pos) {
    auto ids = selectedMedia();
    QMenu menu(this);
    if (ids.size() == 1) {
        Id id = ids.front();
        const MediaItem* m = state_->project().findMedia(id);
        if (m && m->kind == MediaKind::Sequence) {
            menu.addAction(tr("Open Sequence"), this, [this, m] { state_->setActiveSequence(m->sequenceId); });
        } else {
            menu.addAction(tr("Open in Source Monitor"), this, [this, id] { emit openInSource(id); });
        }
        if (m && !m->path.empty())
            menu.addAction(tr("Reveal in File Manager"), this, [m] {
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(QString::fromStdString(m->path)).absolutePath()));
            });
        menu.addAction(tr("Rename..."), this, [this, id, m] {
            bool ok = false;
            QString name = QInputDialog::getText(this, tr("Rename"), tr("Name:"), QLineEdit::Normal,
                                                 QString::fromStdString(m->name), &ok);
            if (!ok || name.isEmpty()) return;
            state_->edit(tr("Rename Media"), [id, name](Project& p, Sequence&) {
                MediaItem* mi = p.findMedia(id);
                if (!mi) return false;
                mi->name = name.toStdString();
                if (mi->kind == MediaKind::Sequence)
                    if (Sequence* s = p.findSequence(mi->sequenceId)) s->name = mi->name;
                return true;
            });
        });
    }
    if (!ids.empty()) {
        menu.addSeparator();
        menu.addAction(tr("Remove from Project"), this, [this, ids] {
            for (Id id : ids) {
                QString err;
                if (!state_->removeMedia(id, &err) && !err.isEmpty()) state_->message(err);
            }
        });
    }
    menu.addSeparator();
    menu.addAction(tr("Import..."), this, &MediaBinWidget::importDialog);
    menu.exec(list_->viewport()->mapToGlobal(pos));
}

}  // namespace montage
