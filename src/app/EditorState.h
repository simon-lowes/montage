// Montage — application state shared by every panel: the project, undo
// history, selection, playhead, source-monitor clip and track targeting.
#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "core/EditOps.h"
#include "core/History.h"
#include "core/Model.h"

class QFileSystemWatcher;
class QTimer;

namespace montage {

class EditorState : public QObject {
    Q_OBJECT
public:
    explicit EditorState(QObject* parent = nullptr);
    ~EditorState() override;

    // ---- Project access ---------------------------------------------------
    const Project& project() const { return project_; }
    const Sequence* sequence() const { return project_.active(); }
    // Mutable access for panels that call notifyChanged() themselves
    // (only inside an edit() callback or a gesture).
    Project& mutableProject() { return project_; }

    // Runs an undoable edit. `fn` mutates the project; return false to
    // abandon it (nothing is recorded). Edits sharing a non-empty mergeKey
    // within ~1.5 s collapse into one undo step (slider drags, typing).
    bool edit(const QString& label, const std::function<bool(Project&, Sequence&)>& fn,
              const QString& mergeKey = QString());
    // Changes the project without a new undo step, for results derived from the
    // latest edit (a segmentation that finished in the background): undoing
    // that edit takes them away with it.
    bool amend(const std::function<bool(Project&, Sequence&)>& fn);
    // Convenience for EditOps that return a Result; reports errors to the status bar.
    bool apply(const QString& label, const std::function<edit::Result(Project&, Sequence&)>& fn);

    // Interactive gestures (drag to move / trim...). Every update re-applies
    // `fn` to the state captured at begin, so the result depends only on the
    // total drag distance. end(true) records one undo step if anything changed.
    void beginGesture(const QString& label);
    void updateGesture(const std::function<void(Project&, Sequence&)>& fn);
    void endGesture(bool commit = true);
    bool inGesture() const { return gesture_.has_value(); }
    // The active sequence as it was when the current gesture began (or the
    // live one outside gestures): drag maths must be relative to this.
    const Sequence* gestureBase() const { return gesture_ ? gesture_->origin.active() : sequence(); }

    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }
    QString undoText() const { return QString::fromStdString(history_.undoLabel()); }
    QString redoText() const { return QString::fromStdString(history_.redoLabel()); }

    // ---- Selection --------------------------------------------------------
    const std::vector<Id>& selectedClips() const { return selection_; }
    bool isSelected(Id clip) const;
    void setSelection(std::vector<Id> clips, bool expandLinked = true);
    void clearSelection();
    Id selectedTransition() const { return selectedTransition_; }
    void selectTransition(Id id);
    // An effect chain shown in the Inspector instead of a clip: an audio
    // track's inserts, a bus, or the master (the sequence's id); 0 = none.
    // Selecting clips or a transition clears it.
    Id inspectedChain() const { return inspectedChain_; }
    void inspectChain(Id owner);
    // The single clip shown in the inspector (first selected), or nullptr.
    const Clip* primaryClip() const;

    // ---- Playhead (stored in the active sequence) ------------------------
    FrameTime playhead() const;
    void setPlayhead(FrameTime t);  // clamped at 0; does not create undo steps
    void setInPoint(FrameTime t);   // -1 clears
    void setOutPoint(FrameTime t);

    // ---- Track targeting for source edits ---------------------------------
    int targetVideoTrack() const { return targetVideo_; }
    int targetAudioTrack() const { return targetAudio_; }
    void setTargetVideoTrack(int i);
    void setTargetAudioTrack(int i);
    bool snapping() const { return snapping_; }
    void setSnapping(bool on);

    // ---- Media ------------------------------------------------------------
    // Probes and adds files; returns ids of the new media. Errors are
    // appended to `errors`. Starts background audio decoding for waveforms.
    std::vector<Id> importFiles(const QStringList& paths, QStringList* errors = nullptr, const QString& bin = QString());
    bool removeMedia(Id id, QString* error = nullptr);

    // The media item loaded in the source monitor and its marks
    // (frames at the active sequence's rate; -1 = unset).
    Id sourceMedia() const { return sourceMedia_; }
    // A subclip opens its media with In and Out around its range.
    void setSourceMedia(Id id);
    FrameTime sourceIn() const { return sourceIn_; }
    FrameTime sourceOut() const { return sourceOut_; }
    void setSourceIn(FrameTime t);
    void setSourceOut(FrameTime t);
    // Three-point edits from the source monitor into the timeline at the
    // playhead (or the sequence In point), on the targeted tracks.
    bool insertFromSource(bool overwriteMode);
    // Saves a range of a media item (frames of the active sequence, Out
    // inclusive) as a subclip in the bin; returns its id, or 0.
    Id makeSubclip(Id media, FrameTime in, FrameTime out, const QString& name = QString());

    // ---- Sequences ----------------------------------------------------------
    void setActiveSequence(Id id);
    Id newSequence(const QString& name, int w, int h, Rational fps);

    // ---- File -----------------------------------------------------------------
    QString filePath() const { return path_; }
    bool isModified() const { return history_.revision() != savedRevision_; }
    bool save(const QString& path, QString* error = nullptr);
    bool open(const QString& path, QString* error = nullptr);
    void newProject();
    // Opens a recovered copy as if it were `originalPath` (empty = untitled),
    // marked modified so the user decides whether to save it.
    bool recover(const QString& copy, const QString& originalPath, QString* error = nullptr);

    // Emits projectChanged() after a direct mutation of mutableProject().
    void notifyChanged() { emit projectChanged(); }
    void message(const QString& text, int timeoutMs = 4000) { emit statusMessage(text, timeoutMs); }
    // Decodes the media's audio (and its waveform peaks) in the background; mediaReady follows.
    void startAudioDecode(const MediaItem& m);
    // Whether the media's file is missing (media/Relink.h), each file checked at most every two seconds, since
    // painting asks often. Nested sequences and generators are never offline.
    bool isMediaOffline(Id media) const;
    void recheckOffline() { offlineChecked_.clear(); }  // after files moved or were relinked
    // Media files changed on disk (a still re-saved, a graphic re-rendered) are reloaded by themselves, as Premiere
    // and Resolve do: their cached frames, thumbnails and sound are dropped and their details re-read.
    void reloadChangedMedia(const QStringList& paths);

signals:
    void projectChanged();            // anything in the project changed
    void selectionChanged();
    void playheadChanged(montage::FrameTime t);
    void sequenceSwitched();          // a different sequence became active / project replaced
    void historyChanged();
    void sourceChanged();             // source monitor media or marks changed
    void mediaReady(const QString& path);  // audio peaks / thumbnails available
    void mediaFileChanged(montage::Id media);  // its file changed on disk and was reloaded
    void statusMessage(const QString& text, int timeoutMs);
    void fileStateChanged();          // path or modified flag changed

private:
    void pruneSelection();
    void watchMediaFiles();  // keeps the watcher on the project's media files
    QFileSystemWatcher* watcher_ = nullptr;
    QTimer* reloadTimer_ = nullptr;
    QStringList changedFiles_;
    mutable std::map<std::string, std::pair<bool, qint64>> offlineChecked_;  // path -> offline, when checked (ms)

    Project project_;
    History history_;
    uint64_t savedRevision_ = 0;
    QString path_;
    std::vector<Id> selection_;
    Id selectedTransition_ = 0;
    Id inspectedChain_ = 0;
    int targetVideo_ = 0;
    int targetAudio_ = 0;
    bool snapping_ = true;
    Id sourceMedia_ = 0;
    FrameTime sourceIn_ = -1;
    FrameTime sourceOut_ = -1;
    QString lastMergeKey_;
    QDateTime lastMergeTime_;

    struct Gesture {
        QString label;
        Project origin;
        bool changed = false;
    };
    std::optional<Gesture> gesture_;
};

// Formats a frame count using the active sequence's timecode.
QString timecodeString(const Sequence* s, FrameTime t);

}  // namespace montage
