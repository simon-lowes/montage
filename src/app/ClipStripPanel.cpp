#include "ClipStripPanel.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <QDropEvent>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTreeWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <tuple>

#include "EditorState.h"
#include "core/ColorGroups.h"
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

constexpr int kRowKind = Qt::UserRole + 1;  // 0 a clip, 1 a group, 2 Not Grouped

// The group rows, which take clips and groups dropped on them.
class GroupTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    std::function<bool(const std::vector<Id>&, Id)> dropped;

protected:
    void dropEvent(QDropEvent* e) override {
        QTreeWidgetItem* at = itemAt(e->position().toPoint());
        // (onto a clip: into its group)
        while (at && at->data(0, kRowKind).toInt() == 0) at = at->parent();
        std::vector<Id> moved;
        for (QTreeWidgetItem* item : selectedItems())
            if (item->data(0, kRowKind).toInt() != 2) moved.push_back(Id(item->data(0, Qt::UserRole).toULongLong()));
        e->setDropAction(Qt::IgnoreAction);  // (the rows are listed again from the project)
        e->accept();
        if (at && dropped) dropped(moved, at->data(0, kRowKind).toInt() == 1 ? Id(at->data(0, Qt::UserRole).toULongLong()) : 0);
    }
};

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
    groups_ = new QToolButton(this);
    groups_->setObjectName(QStringLiteral("clipGroups"));
    groups_->setText(tr("Groups"));
    groups_->setCheckable(true);
    groups_->setToolTip(tr("Show the colour groups as rows: scenes, the angles in them, and their clips"));
    top->addWidget(new QLabel(tr("Clips"), this));
    top->addStretch(1);
    top->addWidget(groups_);
    top->addWidget(lightbox_);
    v->addLayout(top);
    // Group Shots and what it is done with.
    groupBar_ = new QWidget(this);
    auto* bar = new QHBoxLayout(groupBar_);
    bar->setContentsMargins(0, 0, 0, 0);
    auto* run = new QPushButton(tr("Group Shots"), groupBar_);
    run->setObjectName(QStringLiteral("groupShots"));
    run->setToolTip(tr("Group the shots by camera angle and scene (groups made by hand and locked ones stay)"));
    sensitivity_ = new QSlider(Qt::Horizontal, groupBar_);
    sensitivity_->setObjectName(QStringLiteral("groupSensitivity"));
    sensitivity_->setRange(0, 100);
    sensitivity_->setValue(50);
    sensitivity_->setToolTip(tr("Sensitivity: higher makes more, closer angle groups"));
    bar->addWidget(run);
    bar->addWidget(new QLabel(tr("Sensitivity"), groupBar_));
    bar->addWidget(sensitivity_, 1);
    for (const auto& [name, text, tip] : {std::tuple{"splitGroup", tr("Split"), tr("The selected clips into a group of their own")},
                                          std::tuple{"joinGroups", tr("Join"), tr("The selected groups into the first of them")},
                                          std::tuple{"lockGroup", tr("Lock"), tr("Lock the selected groups (Group Shots leaves them as they are)")},
                                          std::tuple{"unlockGroup", tr("Unlock"), tr("Unlock the selected groups")}}) {
        auto* b = new QToolButton(groupBar_);
        b->setObjectName(QString::fromLatin1(name));
        b->setText(text);
        b->setToolTip(tip);
        bar->addWidget(b);
        const QString which = QString::fromLatin1(name);
        connect(b, &QToolButton::clicked, this, [this, which] {
            if (which == "splitGroup") splitSelected();
            else if (which == "joinGroups") joinSelected();
            else lockSelected(which == "lockGroup");
        });
    }
    connect(run, &QPushButton::clicked, this, [this] { emit groupShotsRequested(sensitivity()); });
    v->addWidget(groupBar_);
    auto* tree = new GroupTree(this);
    tree_ = tree;
    tree_->setObjectName(QStringLiteral("colorGroupTree"));
    tree_->setHeaderHidden(true);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->setDragDropMode(QAbstractItemView::InternalMove);
    tree_->setDefaultDropAction(Qt::MoveAction);
    tree->dropped = [this](const std::vector<Id>& items, Id group) { return moveInto(items, group); };
    connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        const Sequence* s = state_->sequence();
        if (!s || item->data(0, kRowKind).toInt() != 0) return;
        const Clip* c = edit::clipById(*s, Id(item->data(0, Qt::UserRole).toULongLong()));
        if (!c) return;
        state_->setSelection({c->id}, false);
        if (!c->contains(state_->playhead())) state_->setPlayhead(c->start);
    });
    v->addWidget(tree_, 1);
    connect(groups_, &QToolButton::toggled, this, [this](bool on) { setGroupsShown(on); });
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("clipStrip"));
    list_->setViewMode(QListView::IconMode);
    list_->setMovement(QListView::Static);
    list_->setWordWrap(true);
    list_->setUniformItemSizes(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    v->addWidget(list_, 1);
    setLightbox(false);
    setGroupsShown(false);

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

void ClipStripPanel::setGroupsShown(bool on) {
    {
        const QSignalBlocker b(groups_);
        groups_->setChecked(on);
    }
    list_->setVisible(!on);
    lightbox_->setEnabled(!on);
    groupBar_->setVisible(on);
    tree_->setVisible(on);
    if (on) rebuildGroups();
}

bool ClipStripPanel::groupsShown() const { return groups_->isChecked(); }

double ClipStripPanel::sensitivity() const { return sensitivity_->value() / 100.0; }

void ClipStripPanel::rebuildGroups() {
    const Sequence* s = state_->sequence();
    // (the rows open and selected stay so)
    std::set<std::pair<int, qulonglong>> selected;
    for (QTreeWidgetItem* item : tree_->selectedItems()) selected.insert({item->data(0, kRowKind).toInt(), item->data(0, Qt::UserRole).toULongLong()});
    const QSignalBlocker b(tree_);
    tree_->clear();
    if (!s) return;
    std::map<Id, int> numbers;
    int n = 0;
    for (const Listed& c : pictureClips(*s)) numbers[c.id] = ++n;
    auto clipRow = [&](QTreeWidgetItem* parent, Id id) {
        const Clip* c = edit::clipById(*s, id);
        if (!c) return;
        auto* item = new QTreeWidgetItem(parent, {QStringLiteral("%1  %2  %3").arg(numbers[id]).arg(QString::fromStdString(formatTimecode(c->start, s->fps)),
                                                                                                     QString::fromStdString(c->name))});
        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(id));
        item->setData(0, kRowKind, 0);
        item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled);
        item->setSelected(selected.count({0, qulonglong(id)}) > 0);
    };
    // Each group's shots and those of the groups inside it, and its own, in timeline order.
    std::map<Id, int> shotCount;
    std::map<Id, std::vector<std::pair<FrameTime, Id>>> own;
    for (const Track& t : s->videoTracks)
        for (const Clip& c : t.clips) {
            for (const ColorGroup* g : colorGroupsOf(*s, c)) ++shotCount[g->id];
            if (c.colorGroup) own[c.colorGroup].push_back({c.start, c.id});
        }
    for (auto& [g, clips] : own) std::sort(clips.begin(), clips.end());
    std::function<void(QTreeWidgetItem*, const ColorGroup&)> groupRow = [&](QTreeWidgetItem* parent, const ColorGroup& g) {
        const int shots = shotCount[g.id];
        const QString kind = g.kind == "scene" ? tr("scene") : g.kind == "angle" ? tr("angle") : tr("by hand");
        const QString text = tr("%1  (%2, %n shot(s))", "", shots).arg(QString::fromStdString(g.name), kind) + (g.locked ? tr("  locked") : QString());
        auto* item = parent ? new QTreeWidgetItem(parent, {text}) : new QTreeWidgetItem(tree_, {text});
        item->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(g.id));
        item->setData(0, kRowKind, 1);
        QFont f = item->font(0);
        f.setBold(true);
        item->setFont(0, f);
        item->setSelected(selected.count({1, qulonglong(g.id)}) > 0);
        for (const ColorGroup& inner : s->colorGroups)
            if (inner.parent == g.id && inner.id != g.id) groupRow(item, inner);
        for (const auto& [at, c] : own[g.id]) clipRow(item, c);
    };
    for (const ColorGroup& g : s->colorGroups)
        if (!findColorGroup(*s, g.parent)) groupRow(nullptr, g);
    auto* loose = new QTreeWidgetItem(tree_, {tr("Not grouped")});
    loose->setData(0, kRowKind, 2);
    loose->setData(0, Qt::UserRole, QVariant::fromValue<qulonglong>(0));
    loose->setFlags(loose->flags() & ~Qt::ItemIsDragEnabled);
    for (const Listed& c : pictureClips(*s))
        if (const Clip* clip = edit::clipById(*s, c.id); clip && !colorGroupOf(*s, *clip)) clipRow(loose, c.id);
    tree_->expandAll();
}

std::vector<Id> ClipStripPanel::selectedRows(int kind) const {
    std::vector<Id> out;
    // (in the order listed)
    for (QTreeWidgetItemIterator it(tree_); *it; ++it)
        if ((*it)->isSelected() && (*it)->data(0, kRowKind).toInt() == kind) out.push_back(Id((*it)->data(0, Qt::UserRole).toULongLong()));
    return out;
}

bool ClipStripPanel::splitSelected() {
    const std::vector<Id> clips = selectedRows(0);
    if (clips.empty()) return false;
    return state_->edit(tr("Split Colour Group"), [clips](Project& p, Sequence& s) { return edit::splitColorGroup(p, s, clips).ok; });
}

bool ClipStripPanel::joinSelected() {
    const std::vector<Id> groups = selectedRows(1);
    if (groups.size() < 2) return false;
    return state_->edit(tr("Join Colour Groups"), [groups](Project&, Sequence& s) {
        return edit::joinColorGroups(s, groups.front(), std::vector<Id>(groups.begin() + 1, groups.end())).ok;
    });
}

bool ClipStripPanel::lockSelected(bool locked) {
    const std::vector<Id> groups = selectedRows(1);
    if (groups.empty()) return false;
    return state_->edit(locked ? tr("Lock Colour Group") : tr("Unlock Colour Group"), [groups, locked](Project&, Sequence& s) {
        bool any = false;
        for (Id g : groups) any = edit::setColorGroupLocked(s, g, locked).ok || any;
        return any;
    });
}

bool ClipStripPanel::moveInto(const std::vector<Id>& items, Id group) {
    const Sequence* now = state_->sequence();
    if (!now || items.empty()) return false;
    std::vector<Id> clips, groups;
    for (Id id : items) (findColorGroup(*now, id) ? groups : clips).push_back(id);
    return state_->edit(tr("Move into Colour Group"), [clips, groups, group](Project&, Sequence& s) {
        bool any = false;
        if (!clips.empty()) any = (group ? edit::addToColorGroup(s, clips, group) : edit::removeFromColorGroup(s, clips)).ok || any;
        for (Id g : groups) any = edit::setColorGroupParent(s, g, group).ok || any;
        return any;
    });
}

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
            h.addData(QByteArray::fromStdString("|" + m->path + "|" + m->proxyPath + "|" + m->colorOverride + "|" + m->sdrExpand));
        for (const SharedNode& n : sharedNodesUsed(*s, *clip))
            for (const Effect& e : n.effects) h.addData(QByteArray::fromStdString(effectToJsonString(e)));
        for (const ColorGroup* g : colorGroupsOf(*s, *clip))  // (its groups' grades)
            for (const auto* chain : {&g->pre, &g->post})
                for (const Effect& e : *chain) h.addData(QByteArray::fromStdString("|g" + effectToJsonString(e)));
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
    if (groupsShown()) rebuildGroups();
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
