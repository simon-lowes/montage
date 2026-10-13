#include "ClipStripPanel.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>

#include "EditorState.h"
#include "core/EditOps.h"
#include "core/GradeGraph.h"
#include "core/History.h"
#include "core/ProjectIO.h"
#include "render/Compositor.h"

namespace montage {

namespace {

constexpr int kThumbWidth = 256;  // drawn once, shown smaller in the strip

struct Listed {
    Id id = 0;
    int track = 0;
    FrameTime start = 0, duration = 0;
    std::string name;
};

// The picture clips in timeline order (then bottom track first).
std::vector<Listed> pictureClips(const Sequence& s) {
    std::vector<Listed> out;
    for (int i = 0; i < int(s.videoTracks.size()); ++i)
        for (const Clip& c : s.videoTracks[size_t(i)].clips)
            if (!(c.isGenerator() && (c.generator.type == "adjustment" || c.generator.type == "audio_viz")))
                out.push_back({c.id, i, c.start, c.duration, c.name});
    std::stable_sort(out.begin(), out.end(), [](const Listed& a, const Listed& b) { return a.start != b.start ? a.start < b.start : a.track < b.track; });
    return out;
}

}  // namespace

ClipStripPanel::ClipStripPanel(EditorState* state, QWidget* parent) : QWidget(parent), state_(state) {
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    auto* top = new QHBoxLayout;
    lightbox_ = new QToolButton(this);
    lightbox_->setObjectName(QStringLiteral("lightbox"));
    lightbox_->setText(tr("Lightbox"));
    lightbox_->setCheckable(true);
    lightbox_->setToolTip(tr("Show the clips as a grid of larger thumbnails"));
    top->addWidget(new QLabel(tr("Clips"), this));
    top->addStretch(1);
    top->addWidget(lightbox_);
    v->addLayout(top);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("clipStrip"));
    list_->setViewMode(QListView::IconMode);
    list_->setMovement(QListView::Static);
    list_->setWordWrap(true);
    list_->setUniformItemSizes(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    v->addWidget(list_, 1);
    setLightbox(false);

    auto* later = new QTimer(this);
    later->setSingleShot(true);
    later->setInterval(400);
    connect(later, &QTimer::timeout, this, &ClipStripPanel::rebuild);
    for (auto sig : {&EditorState::projectChanged, &EditorState::sequenceSwitched})
        connect(state_, sig, later, qOverload<>(&QTimer::start));
    connect(state_, &EditorState::mediaFileChanged, this, [this, later] {  // (the same file, its picture changed)
        thumbs_.clear();
        later->start();
    });
    connect(state_, &EditorState::playheadChanged, this, [this] { markCurrent(); });
    connect(lightbox_, &QToolButton::toggled, this, [this](bool on) { setLightbox(on); });
    connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const Id id = Id(item->data(Qt::UserRole).toULongLong());
        const Sequence* s = state_->sequence();
        const Clip* c = s ? edit::clipById(*s, id) : nullptr;
        if (!c) return;
        state_->setSelection({id}, false);
        if (!c->contains(state_->playhead())) state_->setPlayhead(c->start);
    });
    rebuild();
}

void ClipStripPanel::setLightbox(bool on) {
    {
        const QSignalBlocker b(lightbox_);
        lightbox_->setChecked(on);
    }
    // The strip: one row, scrolled across; the lightbox: rows that fill the panel.
    list_->setFlow(QListView::LeftToRight);
    list_->setWrapping(on);
    list_->setResizeMode(on ? QListView::Adjust : QListView::Fixed);
    list_->setHorizontalScrollBarPolicy(on ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    const QSize icon = on ? QSize(kThumbWidth, kThumbWidth * 9 / 16) : QSize(128, 72);
    list_->setIconSize(icon);
    list_->setGridSize(QSize(icon.width() + 16, icon.height() + 44));
    list_->doItemsLayout();
}

bool ClipStripPanel::lightbox() const { return lightbox_->isChecked(); }

std::vector<Id> ClipStripPanel::clips() const {
    std::vector<Id> out;
    for (int i = 0; i < list_->count(); ++i) out.push_back(Id(list_->item(i)->data(Qt::UserRole).toULongLong()));
    return out;
}

void ClipStripPanel::rebuild() {
    const Sequence* s = state_->sequence();
    const QSignalBlocker b(list_);
    list_->clear();
    keys_.clear();
    if (!s) return;
    int n = 0;
    QPixmap blank(kThumbWidth, kThumbWidth * 9 / 16);
    blank.fill(QColor(40, 40, 40));
    for (const Listed& c : pictureClips(*s)) {
        const Clip* clip = edit::clipById(*s, c.id);
        // What the thumbnail shows: the clip (as saved), its media's file, the shared nodes it shows, its track, the
        // sequence's size and colour.
        QCryptographicHash h(QCryptographicHash::Sha1);
        h.addData(QByteArray::fromStdString(clipToJsonString(*clip)));
        if (const MediaItem* m = state_->project().findMedia(clip->mediaId))
            h.addData(QByteArray::fromStdString("|" + m->path + "|" + m->proxyPath + "|" + m->colorOverride));
        for (const SharedNode& n : sharedNodesUsed(*s, *clip))
            for (const Effect& e : n.effects) h.addData(QByteArray::fromStdString(effectToJsonString(e)));
        h.addData(QStringLiteral("|%1|%2|%3x%4|%5").arg(s->id).arg(c.track).arg(s->width).arg(s->height).arg(QString::fromStdString(s->colorSpace)).toUtf8());
        const QString key = QString::fromLatin1(h.result().toHex());
        keys_.insert(c.id, key);
        const auto thumb = thumbs_.constFind(key);
        auto* item = new QListWidgetItem(QIcon(thumb != thumbs_.constEnd() ? QPixmap::fromImage(*thumb) : blank),
                                         QStringLiteral("%1  V%2  %3\n%4").arg(++n).arg(c.track + 1)
                                             .arg(QString::fromStdString(formatTimecode(c.start, s->fps)), QString::fromStdString(c.name)),
                                         list_);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(c.id));
        item->setToolTip(QString::fromStdString(c.name));
    }
    // Only what is listed is kept.
    QHash<QString, QImage> kept;
    for (const QString& k : keys_)
        if (thumbs_.contains(k)) kept.insert(k, thumbs_.value(k));
    thumbs_ = std::move(kept);
    markCurrent();
    requestThumbnails();
}

void ClipStripPanel::requestThumbnails() {
    const Sequence* s = state_->sequence();
    struct Job {
        QString key;
        int track = 0;
        FrameTime at = 0;
    };
    std::vector<Job> jobs;
    if (s)
        for (const Listed& c : pictureClips(*s)) {
            const QString key = keys_.value(c.id);
            if (!key.isEmpty() && !thumbs_.contains(key) &&
                std::none_of(jobs.begin(), jobs.end(), [&](const Job& j) { return j.key == key; }))
                jobs.push_back({key, c.track, c.start + c.duration / 2});
        }
    const int generation = ++*generation_;
    pending_ = int(jobs.size());
    if (jobs.empty()) return;
    // Drawn from a copy, away from the interface; a newer listing makes these stale.
    auto project = std::make_shared<const Project>(state_->project());
    const Id seq = s->id;
    QPointer<ClipStripPanel> self(this);
    std::weak_ptr<std::atomic<int>> current = generation_;
    (void)QtConcurrent::run([project, seq, jobs, generation, self, current] {
        const Sequence* sq = project->findSequence(seq);
        for (const Job& j : jobs) {
            if (const auto g = current.lock(); !g || g->load() != generation || !sq) return;
            RenderOptions o;
            o.scale = std::min(1.0, double(kThumbWidth) / std::max(1, sq->width));
            o.displaySpace = "rec709";
            o.soloVideoTrack = j.track;
            const Image img = renderProgramFrame(*project, *sq, j.at, o);
            QImage q(img.width, img.height, QImage::Format_RGBA8888);
            if (img.width > 0) toRgba8(img, q.bits(), size_t(q.bytesPerLine()));
            QMetaObject::invokeMethod(
                qApp, [self, current, generation, key = j.key, q] {
                    const auto g = current.lock();
                    if (!self || !g || g->load() != generation) return;
                    self->thumbs_.insert(key, q);
                    self->pending_ = std::max(0, self->pending_ - 1);
                    for (int i = 0; i < self->list_->count(); ++i) {
                        QListWidgetItem* item = self->list_->item(i);
                        if (self->keys_.value(Id(item->data(Qt::UserRole).toULongLong())) == key) item->setIcon(QIcon(QPixmap::fromImage(q)));
                    }
                },
                Qt::QueuedConnection);
        }
    });
}

void ClipStripPanel::markCurrent() {
    const Sequence* s = state_->sequence();
    if (!s) return;
    // The top picture clip at the playhead (not a track matte's hidden source).
    Id current = 0;
    const std::vector<bool> mattes = hiddenMatteTracks(*s, state_->playhead());
    for (int i = int(s->videoTracks.size()) - 1; i >= 0 && !current; --i)
        if (!mattes[size_t(i)])
            if (const Clip* c = edit::clipAt(*s, TrackRef{TrackKind::Video, i}, state_->playhead()); c && keys_.contains(c->id)) current = c->id;
    const QSignalBlocker b(list_);
    for (int i = 0; i < list_->count(); ++i)
        if (Id(list_->item(i)->data(Qt::UserRole).toULongLong()) == current) {
            list_->setCurrentRow(i);
            list_->scrollToItem(list_->item(i));
            return;
        }
    list_->clearSelection();
}

}  // namespace montage
