#include "EditorState.h"

#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QDirIterator>
#include <QDateTime>
#include <QTimer>
#include <QStandardPaths>
#include <QtConcurrent>
#include <algorithm>
#include <limits>
#include <set>
#include <utility>

#include "core/MediaLog.h"
#include "core/ProjectIO.h"
#include "media/Decoder.h"
#include "media/MediaPool.h"
#include "media/ImageSequence.h"
#include "media/Relink.h"
#include "Settings.h"
#include "ThumbnailCache.h"

namespace montage {

EditorState::EditorState(QObject* parent) : QObject(parent), project_(makeDefaultProject()) {
    // Clips playing chosen channels of a file need those decoded too (the whole file is decoded on import).
    connect(this, &EditorState::projectChanged, this, &EditorState::startClipAudioDecodes);
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
    // Changed media files are reloaded once they have been quiet for a moment (editors save in several writes).
    watcher_ = new QFileSystemWatcher(this);
    reloadTimer_ = new QTimer(this);
    reloadTimer_->setSingleShot(true);
    reloadTimer_->setInterval(400);
    connect(watcher_, &QFileSystemWatcher::fileChanged, this, [this](const QString& path) {
        if (!changedFiles_.contains(path)) changedFiles_ << path;
        reloadTimer_->start();
    });
    connect(reloadTimer_, &QTimer::timeout, this, [this] {
        const QStringList paths = std::exchange(changedFiles_, {});
        reloadChangedMedia(paths);
    });
    connect(this, &EditorState::projectChanged, this, &EditorState::watchMediaFiles);
    // Watch folders: a scan a moment after anything in them changes.
    folderWatcher_ = new QFileSystemWatcher(this);
    folderTimer_ = new QTimer(this);
    folderTimer_->setSingleShot(true);
    folderTimer_->setInterval(1100);
    connect(folderWatcher_, &QFileSystemWatcher::directoryChanged, this, [this] { folderTimer_->start(); });
    connect(folderTimer_, &QTimer::timeout, this, [this] { scanWatchFolders(); });
    connect(this, &EditorState::projectChanged, this, &EditorState::watchFoldersChanged);
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

void EditorState::watchMediaFiles() {
    // Decoders stay open only on this project's files (and proxies); a closed project's files are let go.
    std::set<std::string> open;
    for (const MediaItem& m : project_.media) {
        if (!m.path.empty()) open.insert(m.path);
        if (!m.proxyPath.empty()) open.insert(m.proxyPath);
    }
    MediaPool::instance().setOpenFiles(std::move(open));
    QStringList wanted;
    for (const MediaItem& m : project_.media)
        if (!m.path.empty() && m.kind != MediaKind::Sequence && !m.subclipOf) {
            const QString p = QString::fromStdString(mediaFileOnDisk(m.path));  // a Photoshop layer's file, a sequence's first frame
            if (!wanted.contains(p) && QFileInfo::exists(p)) wanted << p;
        }
    const QStringList watched = watcher_->files();
    QStringList gone, added;
    for (const QString& p : watched)
        if (!wanted.contains(p)) gone << p;
    for (const QString& p : wanted)
        if (!watched.contains(p)) added << p;
    if (!gone.isEmpty()) watcher_->removePaths(gone);
    if (!added.isEmpty()) watcher_->addPaths(added);
}

void EditorState::reloadChangedMedia(const QStringList& paths) {
    QStringList names;
    std::vector<Id> reloaded;
    for (const QString& path : paths) {
        if (!QFileInfo::exists(path)) continue;  // gone (or being replaced): offline until it is back
        if (!watcher_->files().contains(path)) watcher_->addPath(path);  // replaced by a new file: watch that
        // The media in that file: the file itself, or each of its layers.
        std::set<std::string> mediaPaths;
        for (const MediaItem& m : project_.media)
            if (!m.path.empty() && mediaFileOnDisk(m.path) == path.toStdString()) mediaPaths.insert(m.path);
        for (const std::string& p : mediaPaths) {
            MediaPool::instance().forget(p);
            ThumbnailCache::instance().forget(QString::fromStdString(p));
            MediaItem fresh;
            if (!probeMedia(p, fresh)) continue;
            for (MediaItem& m : project_.media) {
                if (m.path != p) continue;
                m.width = fresh.width, m.height = fresh.height, m.fps = fresh.fps;
                m.hasVideo = fresh.hasVideo, m.hasAudio = fresh.hasAudio;
                m.sampleRate = fresh.sampleRate, m.channels = fresh.channels, m.audioStreams = fresh.audioStreams;
                m.videoCodec = fresh.videoCodec, m.audioCodec = fresh.audioCodec;
                if (!m.subclipOf) {
                    m.duration = fresh.duration;
                    names << QString::fromStdString(m.name);
                }
                reloaded.push_back(m.id);
            }
            for (const MediaItem& m : project_.media)
                if (m.path == p && !m.subclipOf) startAudioDecode(m);
            for (auto it = clipAudioRequested_.begin(); it != clipAudioRequested_.end();)
                it = audioKeyFile(it->first) == p ? clipAudioRequested_.erase(it) : std::next(it);
        }
    }
    offlineChecked_.clear();
    if (reloaded.empty()) return;
    for (Id id : reloaded) emit mediaFileChanged(id);
    emit projectChanged();
    message(tr("Reloaded %1, changed on disk").arg(names.join(QStringLiteral(", "))), 4000);
}

bool EditorState::isMediaOffline(Id media) const {
    const MediaItem* m = media ? project_.findMedia(media) : nullptr;
    if (!m || m->kind == MediaKind::Sequence || m->path.empty()) return false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto& [offline, when] = offlineChecked_[m->path];
    if (when == 0 || now - when > 2000) {
        offline = isOffline(*m);
        when = now;
    }
    return offline;
}

void EditorState::startClipAudioDecodes() {
    const Sequence* s = sequence();
    const int rate = s ? s->sampleRate : 48000;
    for (const Sequence& seq : project_.sequences)
        for (const Track& t : seq.audioTracks)
            for (const Clip& c : t.clips) {
                if (c.channels.empty()) continue;
                const MediaItem* m = project_.findMedia(c.mediaId);
                if (!m || !m->hasAudio || m->path.empty()) continue;
                std::string key = audioKey(m->path, c.channels);
                if (!clipAudioRequested_.insert({key, rate}).second) continue;
                (void)QtConcurrent::run([key, rate] { MediaPool::instance().audio(key, rate); });
            }
}

void EditorState::startAudioDecode(const MediaItem& m) {
    if (!m.hasAudio || m.path.empty() || m.kind == MediaKind::Image) return;
    const Sequence* s = sequence();
    int rate = s ? s->sampleRate : 48000;
    std::string path = m.path;
    (void)QtConcurrent::run([path, rate] { MediaPool::instance().audio(path, rate); });
}

Id EditorState::importImageSequence(const QString& frame, Rational fps, QString* error) {
    ImageSequence seq;
    if (!detectImageSequence(frame.toStdString(), seq)) {
        if (error) *error = tr("%1 is not part of a numbered sequence").arg(QFileInfo(frame).fileName());
        return 0;
    }
    seq.fps = fps.valid() ? fps : Rational{24, 1};
    MediaItem m;
    std::string err;
    if (!probeMedia(imageSequencePath(seq), m, &err)) {
        if (error) *error = QString::fromStdString(err);
        return 0;
    }
    Id id = 0;
    edit(tr("Import %1").arg(QString::fromStdString(m.name)), [&](Project& p, Sequence&) {
        m.id = id = p.newId();
        p.media.push_back(m);
        return true;
    });
    return id;
}

namespace {

bool importableMedia(const QString& file) {
    static const QStringList exts = {"mp4", "mov", "mkv",  "avi",  "webm", "m4v", "mxf", "mts", "m2ts", "ts",  "mpg", "mpeg", "wmv",
                                     "flv", "gif", "wav",  "mp3",  "aac",  "m4a", "flac", "ogg", "opus", "aif", "aiff", "png", "jpg",
                                     "jpeg", "tif", "tiff", "bmp", "webp", "exr", "dpx", "svg", "psd",  "psb", "heic", "dng", "cr2",
                                     "cr3", "nef", "arw",  "raf",  "rw2",  "orf"};
    return exts.contains(QFileInfo(file).suffix().toLower());
}

}  // namespace

void EditorState::watchFoldersChanged() {
    QStringList wanted;
    for (const std::string& f : project_.watchFolders) {
        // The folder and its subfolders, so files landing anywhere in it are noticed.
        const QString root = QString::fromStdString(f);
        if (!QFileInfo(root).isDir()) continue;
        wanted << root;
        QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) wanted << it.next();
    }
    const QStringList watched = folderWatcher_->directories();
    QStringList gone, added;
    for (const QString& d : watched)
        if (!wanted.contains(d)) gone << d;
    for (const QString& d : wanted)
        if (!watched.contains(d)) added << d;
    if (!gone.isEmpty()) folderWatcher_->removePaths(gone);
    if (!added.isEmpty()) folderWatcher_->addPaths(added);
}

bool EditorState::addWatchFolder(const QString& folder) {
    const QFileInfo fi(folder);
    if (!fi.isDir()) return false;
    const std::string path = QDir::cleanPath(fi.absoluteFilePath()).toStdString();
    if (std::find(project_.watchFolders.begin(), project_.watchFolders.end(), path) != project_.watchFolders.end()) return false;
    if (!edit(tr("Watch Folder"), [&](Project& p, Sequence&) {
            p.watchFolders.push_back(path);
            return true;
        }))
        return false;
    scanWatchFolders();
    return true;
}

bool EditorState::removeWatchFolder(const QString& folder) {
    const std::string path = QDir::cleanPath(QFileInfo(folder).absoluteFilePath()).toStdString();
    return edit(tr("Stop Watching Folder"), [&](Project& p, Sequence&) {
        const auto before = p.watchFolders.size();
        std::erase(p.watchFolders, path);
        return p.watchFolders.size() != before;
    });
}

std::vector<Id> EditorState::scanWatchFolders() {
    std::vector<Id> ids;
    // What the project already has: files, image sequences' runs, Photoshop layers' files.
    std::set<std::string> have, runs;
    for (const MediaItem& m : project_.media) {
        if (m.path.empty()) continue;
        have.insert(m.path);
        have.insert(mediaFileOnDisk(m.path));
        if (ImageSequence seq; parseImageSequencePath(m.path, seq)) runs.insert(seq.pattern);
    }
    bool waiting = false;
    const QDateTime now = QDateTime::currentDateTime();
    for (const std::string& folder : std::vector<std::string>(project_.watchFolders)) {
        QStringList fresh;
        QDirIterator it(QString::fromStdString(folder), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QFileInfo fi(it.next());
            if (fi.fileName().startsWith(QLatin1Char('.')) || !importableMedia(fi.fileName())) continue;
            const std::string abs = QDir::cleanPath(fi.absoluteFilePath()).toStdString();
            if (have.count(abs) || watchSkipped_.count(abs)) continue;
            if (ImageSequence seq; isFrameFormat(abs) && detectImageSequence(abs, seq) && runs.count(seq.pattern)) continue;
            if (fi.lastModified().msecsTo(now) < 1000) {  // still being written
                waiting = true;
                continue;
            }
            fresh << fi.absoluteFilePath();
        }
        if (fresh.isEmpty()) continue;
        fresh.sort();
        const QString bin = tr("Watch Folder - %1").arg(QFileInfo(QString::fromStdString(folder)).fileName());
        QStringList errors;
        const std::vector<Id> got = importFiles(fresh, &errors, bin);
        ids.insert(ids.end(), got.begin(), got.end());
        // Files that did not come in are not tried again.
        std::set<std::string> now2;
        for (const MediaItem& m : project_.media) now2.insert(mediaFileOnDisk(m.path));
        for (const QString& f : fresh)
            if (!now2.count(f.toStdString())) watchSkipped_.insert(f.toStdString());
    }
    if (waiting) folderTimer_->start();
    if (!ids.empty()) message(tr("Imported %n file(s) from a watch folder", "", int(ids.size())), 5000);
    return ids;
}

std::vector<Id> EditorState::importPsd(const QString& path, PsdImport mode, QString* error) {
    std::vector<Id> ids;
    std::string err;
    const std::string file = QFileInfo(path).absoluteFilePath().toStdString();
    const bool ok = edit(tr("Import %1").arg(QFileInfo(path).fileName()), [&](Project& p, Sequence&) {
        ids = montage::importPsd(p, file, mode, 5, &err);
        return !ids.empty();
    });
    if (!ok) {
        if (error) *error = QString::fromStdString(err.empty() ? "The Photoshop file could not be read" : err);
        return {};
    }
    return ids;
}

std::vector<Id> EditorState::importFiles(const QStringList& paths, QStringList* errors, const QString& bin) {
    std::vector<MediaItem> items;
    // Numbered frames in render formats, two or more of a run chosen: one image sequence each.
    std::map<std::string, std::pair<ImageSequence, int>> runs;
    if (appSettings().value(QStringLiteral("import/imageSequences"), true).toBool())
        for (const QString& path : paths)
            if (ImageSequence seq; isFrameFormat(path.toStdString()) && detectImageSequence(QFileInfo(path).absoluteFilePath().toStdString(), seq)) {
                auto& run = runs[seq.pattern];
                run.first = seq;
                ++run.second;
            }
    std::set<std::string> made;
    std::vector<Id> fromFolders;  // what folders brought in, each a step of its own
    const Rational rate = sequence() && sequence()->fps.valid() ? sequence()->fps : Rational{24, 1};
    for (const QString& path : paths) {
        QFileInfo fi(path);
        if (ImageSequence seq; !fi.isDir() && isFrameFormat(path.toStdString()) &&
                               detectImageSequence(fi.absoluteFilePath().toStdString(), seq) && runs.count(seq.pattern) &&
                               runs[seq.pattern].second >= 2) {
            if (!made.insert(seq.pattern).second) continue;  // already made from another of its frames
            double dngRate = 0;
            seq.fps = cinemaDng(path.toStdString(), &dngRate) && dngRate > 0 ? rateFor(dngRate) : rate;  // CinemaDNG says its rate
            MediaItem m;
            std::string err;
            if (!probeMedia(imageSequencePath(seq), m, &err)) {
                if (errors) *errors << QString::fromStdString(err);
                continue;
            }
            m.bin = bin.toStdString();
            items.push_back(m);
            continue;
        }
        // A layered Photoshop file, in layers or as a sequence when so preferred (a step of its own).
        if (const QString how = appSettings().value(QStringLiteral("import/psd"), QStringLiteral("merged")).toString();
            !fi.isDir() && isPsdFile(path.toStdString()) && how != QLatin1String("merged")) {
            QString err;
            const auto layers = importPsd(path, how == QLatin1String("sequence") ? PsdImport::Sequence : PsdImport::Layers, &err);
            if (layers.empty() && errors) *errors << err;
            fromFolders.insert(fromFolders.end(), layers.begin(), layers.end());
            continue;
        }
        if (fi.isDir()) {
            QStringList children;
            for (const QFileInfo& c : QDir(path).entryInfoList(QDir::Files, QDir::Name)) children << c.absoluteFilePath();
            const auto inFolder = importFiles(children, errors, bin.isEmpty() ? fi.fileName() : bin + "/" + fi.fileName());
            fromFolders.insert(fromFolders.end(), inFolder.begin(), inFolder.end());
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
    std::vector<Id> ids = fromFolders;
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
    const Id removedSource = sourceMedia_;
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
        // Its subclips go with it.
        p.media.erase(std::remove_if(p.media.begin(), p.media.end(), [id](const MediaItem& x) { return x.id == id || x.subclipOf == id; }),
                      p.media.end());
        return true;
    });
    if (ok && removedSource == id) setSourceMedia(0);
    return ok;
}

void EditorState::setSourceMedia(Id id) {
    sourceMedia_ = id;
    sourceIn_ = sourceOut_ = -1;
    if (const MediaItem* m = project_.findMedia(id); m && m->subclipOf && project_.findMedia(m->subclipOf)) {
        const double fps = sequence() ? sequence()->fpsValue() : 30.0;
        sourceMedia_ = m->subclipOf;
        sourceIn_ = FrameTime(std::floor(m->subclipIn * fps + 1e-6));
        sourceOut_ = std::max(sourceIn_, FrameTime(std::ceil(m->subclipOut * fps - 1e-6)) - 1);
    }
    emit sourceChanged();
}

Id EditorState::makeSubclip(Id media, FrameTime in, FrameTime out, const QString& name) {
    const double fps = sequence() ? sequence()->fpsValue() : 30.0;
    auto sub = montage::makeSubclip(project_, media, double(in) / fps, double(out + 1) / fps, name.toStdString());
    if (!sub) return 0;
    Id id = 0;
    edit(tr("Make Subclip"), [&](Project& p, Sequence&) {
        sub->id = id = p.newId();
        p.media.push_back(*sub);
        return true;
    });
    return id;
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

bool EditorState::insertFromSource(bool overwriteMode) { return sourceEdit(overwriteMode ? SourceEdit::Overwrite : SourceEdit::Insert); }

bool EditorState::sourceEdit(SourceEdit mode) {
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
    const bool insertMode = mode == SourceEdit::Insert || mode == SourceEdit::SmartInsert;
    // Ripple Overwrite replaces the clip under the playhead on the target video track (else the audio one).
    Id replaced = 0;
    if (mode == SourceEdit::RippleOverwrite) {
        const Clip* c = edit::clipAt(*s, vt, s->playhead);
        if (!c) c = edit::clipAt(*s, at_, s->playhead);
        if (!c) {
            message(tr("Put the playhead over the clip to replace"));
            return false;
        }
        replaced = c->id;
    } else if (mode == SourceEdit::SmartInsert) {
        at = edit::nearestEdit(*s, vt, s->playhead);
    } else if (mode == SourceEdit::Append) {
        at = s->duration();
    }
    std::vector<Id> created;
    bool matched = false;
    const QString label = mode == SourceEdit::Overwrite         ? tr("Overwrite")
                          : mode == SourceEdit::Append          ? tr("Append at End")
                          : mode == SourceEdit::PlaceOnTop      ? tr("Place on Top")
                          : mode == SourceEdit::RippleOverwrite ? tr("Ripple Overwrite")
                          : mode == SourceEdit::SmartInsert     ? tr("Smart Insert")
                                                                : tr("Insert");
    bool ok = apply(label, [&](Project& p, Sequence& sq) {
        matched = edit::matchSequenceToMedia(sq, *m);
        edit::Result r = mode == SourceEdit::PlaceOnTop        ? edit::placeOnTop(p, sq, m->id, at, in, out, vt, at_)
                         : mode == SourceEdit::RippleOverwrite ? edit::rippleOverwrite(p, sq, replaced, m->id, in, out, vt, at_)
                                                               : edit::placeMedia(p, sq, m->id, at, in, out, vt, at_, insertMode);
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
    // The last project's files are let go (idle decoders keep them open, which on Windows stops their folder being
    // moved or renamed, as when a card is relinked).
    MediaPool::instance().clear();
    history_.clear();
    savedRevision_ = history_.revision();
    path_ = path;
    selection_.clear();
    selectedTransition_ = 0;
    inspectedChain_ = 0;
    sourceMedia_ = 0;
    sourceIn_ = sourceOut_ = -1;
    targetVideo_ = targetAudio_ = 0;
    offlineChecked_.clear();
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
    MediaPool::instance().clear();  // let the last project's files go
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
