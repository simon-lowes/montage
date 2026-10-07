// Montage — application state shared by every panel: the project, undo
// history, selection, playhead, source-monitor clip and track targeting.
#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>
#include <vector>

#include "core/EditOps.h"
#include "core/History.h"
#include "core/Model.h"

namespace montage {

class EditorState : public QObject {
    Q_OBJECT
public:
    explicit EditorState(QObject* parent = nullptr);

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
    // Convenience for EditOps that return a Result; reports errors to the status bar.
    bool apply(const QString& label, const std::function<edit::Result(Project&, Sequence&)>& fn);

    // Interactive gestures (drag to move / trim...). Every update re-applies
    // `fn` to the state captured at begin, so the result depends only on the
    // total drag distance. end(true) records one undo step if anything changed.
    void beginGesture(const QString& label);
    void updateGesture(const std::function<void(Project&, Sequence&)>& fn);
    void endGesture(bool commit = true);
    bool inGesture() const { return gesture_.has_value(); }

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
    void setSourceMedia(Id id);
    FrameTime sourceIn() const { return sourceIn_; }
    FrameTime sourceOut() const { return sourceOut_; }
    void setSourceIn(FrameTime t);
    void setSourceOut(FrameTime t);
    // Three-point edits from the source monitor into the timeline at the
    // playhead (or the sequence In point), on the targeted tracks.
    bool insertFromSource(bool overwriteMode);

    // ---- Sequences ----------------------------------------------------------
    void setActiveSequence(Id id);
    Id newSequence(const QString& name, int w, int h, Rational fps);

    // ---- File -----------------------------------------------------------------
    QString filePath() const { return path_; }
    bool isModified() const { return history_.revision() != savedRevision_; }
    bool save(const QString& path, QString* error = nullptr);
    bool open(const QString& path, QString* error = nullptr);
    void newProject();
    // Writes an autosave copy next to the project (or in the app data dir).
    void autosave();

    // Emits projectChanged() after a direct mutation of mutableProject().
    void notifyChanged() { emit projectChanged(); }
    void message(const QString& text, int timeoutMs = 4000) { emit statusMessage(text, timeoutMs); }

signals:
    void projectChanged();            // anything in the project changed
    void selectionChanged();
    void playheadChanged(montage::FrameTime t);
    void sequenceSwitched();          // a different sequence became active / project replaced
    void historyChanged();
    void sourceChanged();             // source monitor media or marks changed
    void mediaReady(const QString& path);  // audio peaks / thumbnails available
    void statusMessage(const QString& text, int timeoutMs);
    void fileStateChanged();          // path or modified flag changed

private:
    void pruneSelection();
    void startAudioDecode(const MediaItem& m);

    Project project_;
    History history_;
    uint64_t savedRevision_ = 0;
    QString path_;
    std::vector<Id> selection_;
    Id selectedTransition_ = 0;
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
