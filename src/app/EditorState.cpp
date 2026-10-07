#include "EditorState.h"

#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtConcurrent>
#include <algorithm>

#include "core/ProjectIO.h"
#include "media/Decoder.h"
#include "media/MediaPool.h"

namespace montage {

EditorState::EditorState(QObject* parent) : QObject(parent), project_(makeDefaultProject()) {
    // Decodes finish on worker threads, possibly after this object is gone:
    // hop to the application object and re-check before emitting.
    QPointer<EditorState> self(this);
    MediaPool::instance().setReadyCallback([self](const std::string& path) {
        QCoreApplication* app = QCoreApplication::instance();
        if (!app) return;
        QString p = QString::fromStdString(path);
        QMetaObject::invokeMethod(app, [self, p] {
            if (self) emit self->mediaReady(p);
        }, Qt::QueuedConnection);
    });
    savedRevision_ = history_.revision();
}

EditorState::~EditorState() { MediaPool::instance().setReadyCallback(nullptr); }

QString timecodeString(const Sequence* s, FrameTime t) {
    return QString::fromStdString(formatTimecode(t, s ? s->fps : Rational{30, 1}));
}

// ---------------------------------------------------------------------------
// Editing

bool EditorState::edit(const QString& label, const std::function<bool(Project&, Sequence&)>& fn,
                       const QString& mergeKey) {
    if (gesture_) endGesture(true);
    Sequence* s = project_.active();
    if (!s) return false;
    Project before = project_;
    if (!fn(project_, *project_.active())) {
        project_ = std::move(before);
        return false;
    }
    if (project_ == before) return true;
    QDateTime now = QDateTime::currentDateTimeUtc();
    bool merge = !mergeKey.isEmpty() && mergeKey == lastMergeKey_ && lastMergeTime_.isValid() &&
                 lastMergeTime_.msecsTo(now) < 1500 && history_.canUndo();
    if (merge) history_.touch();
    else history_.push(label.toStdString(), before);
    lastMergeKey_ = mergeKey;
    lastMergeTime_ = now;
    pruneSelection();
    emit projectChanged();
    emit historyChanged();
    emit fileStateChanged();
    return true;
}

bool EditorState::amend(const std::function<bool(Project&, Sequence&)>& fn) {
    if (gesture_) endGesture(true);
    Sequence* s = project_.active();
    if (!s) return false;
    Project before = project_;
    if (!fn(project_, *project_.active())) {
        project_ = std::move(before);
        return false;
    }
    if (project_ == before) return true;
    history_.touch();
    emit projectChanged();
    emit fileStateChanged();
    return true;
}

bool EditorState::apply(const QString& label, const std::function<edit::Result(Project&, Sequence&)>& fn) {
    QString err;
    bool ok = edit(label, [&](Project& p, Sequence& s) {
        edit::Result r = fn(p, s);
        if (!r.ok) err = QString::fromStdString(r.error);
        return r.ok;
    });
    if (!ok && !err.isEmpty()) message(err);
    return ok;
}

void EditorState::beginGesture(const QString& label) {
    if (gesture_) endGesture(true);
    gesture_ = Gesture{label, project_, false};
    lastMergeKey_.clear();
}

void EditorState::updateGesture(const std::function<void(Project&, Sequence&)>& fn) {
    if (!gesture_) return;
    project_ = gesture_->origin;
    if (Sequence* s = project_.active()) fn(project_, *s);
    gesture_->changed = !(project_ == gesture_->origin);
    emit projectChanged();
}

void EditorState::endGesture(bool commit) {
    if (!gesture_) return;
    Gesture g = std::move(*gesture_);
    gesture_.reset();
    if (commit && g.changed) {
        history_.push(g.label.toStdString(), g.origin);
        pruneSelection();
        emit historyChanged();
        emit fileStateChanged();
    } else if (!commit) {
        project_ = std::move(g.origin);
    }
    emit projectChanged();
}

void EditorState::undo() {
    if (gesture_) endGesture(false);
    FrameTime ph = playhead();
    if (!history_.undo(project_)) return;
    if (Sequence* s = project_.active()) s->playhead = ph;
    lastMergeKey_.clear();
    pruneSelection();
    emit projectChanged();
    emit historyChanged();
    emit fileStateChanged();
    emit sourceChanged();
}

void EditorState::redo() {
    if (gesture_) endGesture(false);
    FrameTime ph = playhead();
    if (!history_.redo(project_)) return;
    if (Sequence* s = project_.active()) s->playhead = ph;
    lastMergeKey_.clear();
    pruneSelection();
    emit projectChanged();
    emit historyChanged();
    emit fileStateChanged();
}

// ---------------------------------------------------------------------------
// Selection

bool EditorState::isSelected(Id clip) const {
    return std::find(selection_.begin(), selection_.end(), clip) != selection_.end();
}

void EditorState::setSelection(std::vector<Id> clips, bool expandLinked) {
    if (expandLinked && sequence()) clips = edit::expandLinks(*sequence(), clips);
    if (clips == selection_ && selectedTransition_ == 0 && (clips.empty() || !inspectedChain_)) return;
    selection_ = std::move(clips);
    selectedTransition_ = 0;
    if (!selection_.empty()) inspectedChain_ = 0;
    emit selectionChanged();
}

void EditorState::clearSelection() {
    if (selection_.empty() && selectedTransition_ == 0) return;
    selection_.clear();
    selectedTransition_ = 0;
    emit selectionChanged();
}

void EditorState::selectTransition(Id id) {
    selection_.clear();
    selectedTransition_ = id;
    inspectedChain_ = 0;
    emit selectionChanged();
}

void EditorState::inspectChain(Id owner) {
    selection_.clear();
    selectedTransition_ = 0;
    inspectedChain_ = owner;
    emit selectionChanged();
}

const Clip* EditorState::primaryClip() const {
    const Sequence* s = sequence();
    if (!s || selection_.empty()) return nullptr;
    // Prefer a video clip when a linked A/V pair is selected.
    const Clip* first = nullptr;
    for (Id id : selection_) {
        auto loc = edit::locate(*s, id);
        if (!loc) continue;
        const Clip* c = &trackAt(*s, loc->track)->clips[loc->index];
        if (loc->track.kind == TrackKind::Video) return c;
        if (!first) first = c;
    }
    return first;
}

void EditorState::pruneSelection() {
    const Sequence* s = sequence();
    size_t before = selection_.size();
    Id tr = selectedTransition_;
    if (!s) {
        selection_.clear();
        selectedTransition_ = 0;
    } else {
        selection_.erase(std::remove_if(selection_.begin(), selection_.end(),
                                        [&](Id id) { return !edit::clipById(*s, id); }),
                         selection_.end());
        if (selectedTransition_ && !edit::transitionById(const_cast<Sequence&>(*s), selectedTransition_))
            selectedTransition_ = 0;
    }
    const Id chain = inspectedChain_;
    if (inspectedChain_ && (!s || !edit::effectChain(const_cast<Sequence&>(*s), inspectedChain_))) inspectedChain_ = 0;
    if (selection_.size() != before || tr != selectedTransition_ || chain != inspectedChain_) emit selectionChanged();
}

// ---------------------------------------------------------------------------
// Playhead & marks

FrameTime EditorState::playhead() const {
    const Sequence* s = sequence();
    return s ? s->playhead : 0;
}

void EditorState::setPlayhead(FrameTime t) {
    Sequence* s = project_.active();
    if (!s) return;
    t = std::max<FrameTime>(0, t);
    if (s->playhead == t) return;
    s->playhead = t;
    emit playheadChanged(t);
}

void EditorState::setInPoint(FrameTime t) {
    Sequence* s = project_.active();
    if (!s) return;
    s->inPoint = t;
    if (t >= 0 && s->outPoint >= 0 && s->outPoint <= t) s->outPoint = -1;
    history_.touch();
    emit projectChanged();
    emit fileStateChanged();
}

void EditorState::setOutPoint(FrameTime t) {
    Sequence* s = project_.active();
    if (!s) return;
    s->outPoint = t;
    if (t >= 0 && s->inPoint >= 0 && s->inPoint >= t) s->inPoint = -1;
    history_.touch();
    emit projectChanged();
    emit fileStateChanged();
}

void EditorState::setTargetVideoTrack(int i) {
    targetVideo_ = std::max(0, i);
    emit projectChanged();
}

void EditorState::setTargetAudioTrack(int i) {
    targetAudio_ = std::max(0, i);
    emit projectChanged();
}

void EditorState::setSnapping(bool on) {
    snapping_ = on;
    message(on ? tr("Snapping on") : tr("Snapping off"), 1500);
}

// ---------------------------------------------------------------------------
// Media

void EditorState::startAudioDecode(const MediaItem& m) {
    if (!m.hasAudio || m.path.empty() || m.kind == MediaKind::Image) return;
    const Sequence* s = sequence();
    int rate = s ? s->sampleRate : 48000;
    std::string path = m.path;
    (void)QtConcurrent::run([path, rate] { MediaPool::instance().audio(path, rate); });
}

std::vector<Id> EditorState::importFiles(const QStringList& paths, QStringList* errors, const QString& bin) {
    std::vector<MediaItem> items;
    for (const QString& path : paths) {
        QFileInfo fi(path);
        if (fi.isDir()) {
            QStringList children;
            for (const QFileInfo& c : QDir(path).entryInfoList(QDir::Files, QDir::Name)) children << c.absoluteFilePath();
            auto ids = importFiles(children, errors, bin.isEmpty() ? fi.fileName() : bin + "/" + fi.fileName());
            (void)ids;
            continue;
        }
        MediaItem m;
        std::string err;
        if (!probeMedia(fi.absoluteFilePath().toStdString(), m, &err)) {
            if (errors) *errors << QString::fromStdString(err);
            continue;
        }
        m.bin = bin.toStdString();
        items.push_back(m);
    }
    std::vector<Id> ids;
    if (items.empty()) return ids;
    edit(items.size() == 1 ? tr("Import %1").arg(QString::fromStdString(items[0].name)) : tr("Import %n Files", "", int(items.size())),
         [&](Project& p, Sequence&) {
             for (auto& m : items) {
                 m.id = p.newId();
                 ids.push_back(m.id);
                 p.media.push_back(m);
             }
             return true;
         });
    for (const auto& m : items) startAudioDecode(m);
    return ids;
}

bool EditorState::removeMedia(Id id, QString* error) {
    const MediaItem* m = project_.findMedia(id);
    if (!m) return false;
    if (m->kind == MediaKind::Sequence && m->sequenceId == project_.activeSequence) {
        if (error) *error = tr("Cannot remove the open sequence");
        return false;
    }
    QString name = QString::fromStdString(m->name);
    bool ok = edit(tr("Remove %1").arg(name), [id](Project& p, Sequence&) {
        for (auto& s : p.sequences) {
            std::vector<Id> uses;
            for (auto* list : {&s.videoTracks, &s.audioTracks})
                for (auto& t : *list)
                    for (auto& c : t.clips)
                        if (c.mediaId == id) uses.push_back(c.id);
            if (!uses.empty()) edit::removeClips(p, s, uses, false);
        }
        const MediaItem* mi = p.findMedia(id);
        if (mi && mi->kind == MediaKind::Sequence) {
            Id sid = mi->sequenceId;
            p.sequences.erase(std::remove_if(p.sequences.begin(), p.sequences.end(),
                                             [sid](const Sequence& s) { return s.id == sid; }),
                              p.sequences.end());
        }
        p.media.erase(std::remove_if(p.media.begin(), p.media.end(), [id](const MediaItem& x) { return x.id == id; }),
                      p.media.end());
        return true;
    });
    if (ok && sourceMedia_ == id) setSourceMedia(0);
    return ok;
}

void EditorState::setSourceMedia(Id id) {
    sourceMedia_ = id;
    sourceIn_ = sourceOut_ = -1;
    emit sourceChanged();
}

void EditorState::setSourceIn(FrameTime t) {
    sourceIn_ = t;
    if (t >= 0 && sourceOut_ >= 0 && sourceOut_ <= t) sourceOut_ = -1;
    emit sourceChanged();
}

void EditorState::setSourceOut(FrameTime t) {
    sourceOut_ = t;
    if (t >= 0 && sourceIn_ >= 0 && sourceIn_ >= t) sourceIn_ = -1;
    emit sourceChanged();
}

bool EditorState::insertFromSource(bool overwriteMode) {
    const MediaItem* m = project_.findMedia(sourceMedia_);
    const Sequence* s = sequence();
    if (!m || !s) {
        message(tr("Load a clip in the Source monitor first"));
        return false;
    }
    if (m->kind == MediaKind::Sequence && m->sequenceId == s->id) {
        message(tr("A sequence cannot be edited into itself"));
        return false;
    }
    FrameTime at = s->inPoint >= 0 ? s->inPoint : s->playhead;
    double in = sourceIn_ >= 0 ? double(sourceIn_) : 0.0;
    double out = sourceOut_ >= 0 ? double(sourceOut_ + 1) : -1.0;
    TrackRef vt{TrackKind::Video, std::min(targetVideo_, int(s->videoTracks.size()) - 1)};
    TrackRef at_{TrackKind::Audio, std::min(targetAudio_, int(s->audioTracks.size()) - 1)};
    std::vector<Id> created;
    bool matched = false;
    bool ok = apply(overwriteMode ? tr("Overwrite") : tr("Insert"), [&](Project& p, Sequence& sq) {
        matched = edit::matchSequenceToMedia(sq, *m);
        auto r = edit::placeMedia(p, sq, m->id, at, in, out, vt, at_, !overwriteMode);
        created = r.created;
        return r;
    });
    if (ok && !created.empty()) {
        if (const Clip* c = edit::clipById(*sequence(), created.front())) setPlayhead(c->end());
        setSelection(created);
        if (matched) message(tr("Sequence settings changed to match %1").arg(QString::fromStdString(m->name)), 6000);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Sequences

void EditorState::setActiveSequence(Id id) {
    if (!project_.findSequence(id) || id == project_.activeSequence) return;
    if (gesture_) endGesture(true);
    project_.activeSequence = id;
    selection_.clear();
    selectedTransition_ = 0;
    inspectedChain_ = 0;
    history_.touch();
    emit sequenceSwitched();
    emit selectionChanged();
    emit projectChanged();
}

Id EditorState::newSequence(const QString& name, int w, int h, Rational fps) {
    Id sid = 0;
    edit(tr("New Sequence"), [&](Project& p, Sequence&) {
        Sequence s = makeSequence(p, name.toStdString(), w, h, fps);
        sid = s.id;
        p.sequences.push_back(s);
        MediaItem m;
        m.id = p.newId();
        m.kind = MediaKind::Sequence;
        m.name = name.toStdString();
        m.sequenceId = sid;
        m.hasVideo = m.hasAudio = true;
        m.width = w;
        m.height = h;
        m.fps = fps;
        p.media.push_back(m);
        return true;
    });
    if (sid) setActiveSequence(sid);
    return sid;
}

// ---------------------------------------------------------------------------
// Files

bool EditorState::save(const QString& path, QString* error) {
    if (gesture_) endGesture(true);
    Project p = project_;
    if (p.name.empty() || p.name == "Untitled") p.name = QFileInfo(path).completeBaseName().toStdString();
    std::string err;
    if (!saveProject(p, path.toStdString(), &err)) {
        if (error) *error = QString::fromStdString(err);
        return false;
    }
    project_.name = p.name;
    path_ = path;
    savedRevision_ = history_.revision();
    QFile::remove(path + ".autosave");
    emit fileStateChanged();
    return true;
}

bool EditorState::open(const QString& path, QString* error) {
    Project p;
    std::string err;
    if (!loadProject(path.toStdString(), p, &err)) {
        if (error) *error = QString::fromStdString(err);
        return false;
    }
    if (gesture_) gesture_.reset();
    project_ = std::move(p);
    history_.clear();
    savedRevision_ = history_.revision();
    path_ = path;
    selection_.clear();
    selectedTransition_ = 0;
    inspectedChain_ = 0;
    sourceMedia_ = 0;
    sourceIn_ = sourceOut_ = -1;
    targetVideo_ = targetAudio_ = 0;
    for (const auto& m : project_.media) startAudioDecode(m);
    emit sequenceSwitched();
    emit selectionChanged();
    emit sourceChanged();
    emit projectChanged();
    emit historyChanged();
    emit fileStateChanged();
    return true;
}

void EditorState::newProject() {
    if (gesture_) gesture_.reset();
    project_ = makeDefaultProject();
    history_.clear();
    savedRevision_ = history_.revision();
    path_.clear();
    selection_.clear();
    selectedTransition_ = 0;
    sourceMedia_ = 0;
    sourceIn_ = sourceOut_ = -1;
    targetVideo_ = targetAudio_ = 0;
    emit sequenceSwitched();
    emit selectionChanged();
    emit sourceChanged();
    emit projectChanged();
    emit historyChanged();
    emit fileStateChanged();
}

bool EditorState::recover(const QString& copy, const QString& originalPath, QString* error) {
    if (!open(copy, error)) return false;
    path_ = originalPath;
    savedRevision_ = ~uint64_t(0);  // never equal to a real revision: unsaved until saved
    emit fileStateChanged();
    return true;
}

}  // namespace montage
