// Montage — application state shared by every panel: the project, undo
// history, selection, playhead, source-monitor clip and track targeting.
#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <set>
#include <optional>
#include <vector>

#include "core/EditOps.h"
#include "core/History.h"
#include "media/Psd.h"
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
    // Edits with merge key `key` join one undo step however far apart they come (a script's run), until it is
    // cleared (""), the editor makes an edit of their own, or one is undone.
    void setUndoGroup(const QString& key) { undoGroup_ = key; }
    QString undoGroup() const { return undoGroup_; }
    // The merge key of the latest undo step's edits, while nothing else has been done since ("" otherwise).
    QString lastMergeKey() const { return lastMergeKey_; }
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
    // Automation passes (MixerPanel): while the mixer records one, a move of an insert parameter on a track or bus it
    // writes (from the Inspector or a plugin's editor) is offered to it first. True if it took the move: the move then
    // plays, and is written into the parameter's keys when the pass ends, instead of changing the project now.
    struct ParameterMove {
        Id owner = 0, effect = 0;
        std::string param;
        double value = 0, before = 0;  // the new value, and the one shown before it
        double tolerance = 0.001;      // how far thinning may move the written keys
        bool toggle = false;           // a switch, a choice or a stepped parameter: its keys hold
        std::function<bool()> held;    // the control still held (a slider or a plugin's knob being dragged); may be empty
        bool whole = false;            // its values are whole numbers (an on/off or a choice index)
    };
    void setParameterCapture(std::function<bool(const ParameterMove&)> fn) { parameterCapture_ = std::move(fn); }
    bool captureParameter(const ParameterMove& move) const { return parameterCapture_ && parameterCapture_(move); }
    // The value a pass plays for an insert parameter it is writing (what its Inspector row and plugin editor show),
    // or none.
    using LiveParameter = std::function<std::optional<double>(Id owner, Id effect, const std::string& param)>;
    void setLiveParameter(LiveParameter fn) { liveParameter_ = std::move(fn); }
    std::optional<double> liveParameter(Id owner, Id effect, const std::string& param) const {
        return liveParameter_ ? liveParameter_(owner, effect, param) : std::nullopt;
    }
    // Counts projects opened, reloaded or made new: what was taken from one project can tell it is gone.
    uint64_t projectGeneration() const { return projectGeneration_; }
    // The active sequence as it was when the current gesture began (or the
    // live one outside gestures): drag maths must be relative to this.
    const Sequence* gestureBase() const { return gesture_ ? gesture_->origin.active() : sequence(); }

    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    const History& history() const { return history_; }
    bool canRedo() const { return history_.canRedo(); }
    QString undoText() const { return QString::fromStdString(history_.undoLabel()); }
    QString redoText() const { return QString::fromStdString(history_.redoLabel()); }

    // ---- Selection --------------------------------------------------------
    const std::vector<Id>& selectedClips() const { return selection_; }
    bool isSelected(Id clip) const;
    void setSelection(std::vector<Id> clips, bool expandLinked = true);
    // The clip the selection was made from (the first given to setSelection, not a linked partner it brought along)
    // while it is still selected, else the first selected; 0 with none. What the keyboard and a screen reader are on.
    Id focusClip() const;
    void clearSelection();
    Id selectedTransition() const { return selectedTransition_; }
    void selectTransition(Id id);
    // An effect chain shown in the Inspector instead of a clip: an audio
    // track's inserts, a bus, or the master (the sequence's id); 0 = none.
    // Selecting clips or a transition clears it.
    Id inspectedChain() const { return inspectedChain_; }
    void inspectChain(Id owner);
    // The object being picked in an effect's object mask (0 the first), clamped to the objects it has. Which one is a
    // choice of what to work on, not a change to the project: it makes no undo step and works in a read-only project.
    int maskObject(const Effect& e) const;
    void setMaskObject(Id effect, int which);
    // The Paint or Dust Spot Removal effect whose strokes or spots are being marked on the Program monitor (0: none). Also a
    // choice of what to work on, not an edit.
    Id paintEffect() const { return paintEffect_; }
    void setPaintEffect(Id effect);
    // The kind of pin a click on the monitor places on an armed Mesh Warp (core/Effects.h meshPinKind's).
    int meshPinKind() const { return meshPinKind_; }
    void setMeshPinKind(int kind) { meshPinKind_ = kind; }
    // The single clip shown in the inspector (first selected), or nullptr.
    const Clip* primaryClip() const;
    // The colour node picked in the Nodes panel (core/GradeGraph.h): its key in the graph of the clip shown in the
    // Inspector, or 0 (none picked: the Inspector shows every node). A choice of what to work on, not an edit.
    int selectedGradeNode() const;
    void selectGradeNode(int key);

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
    // A numbered image sequence (media/ImageSequence.h) from any of its frames, played at `fps`; one undo step.
    // Importing two or more frames of a run in a render format (EXR, DPX, PNG, TIFF...) with importFiles makes one
    // too, unless Preferences turn it off ("import/imageSequences").
    Id importImageSequence(const QString& frame, Rational fps, QString* error = nullptr);
    // A Photoshop file brought in merged (one still), as a still per layer, or as a sequence of its layers
    // (media/Psd.h), in one undo step: the ids made, the sequence's media item last; none with `error`. importFiles
    // brings PSDs in the way the "import/psd" preference says (merged, layers or sequence; merged by default).
    std::vector<Id> importPsd(const QString& path, PsdImport mode, QString* error = nullptr);
    // Watch folders (Premiere's watch folder import, for cards being offloaded or renders landing): media files that
    // arrive in them (subfolders too) are imported into a bin named for the folder once they have settled (not written
    // to for a second); what is there when a folder is added comes in too. Saved with the project; each change one undo
    // step, each import one too.
    bool addWatchFolder(const QString& folder);
    bool removeWatchFolder(const QString& folder);
    std::vector<Id> scanWatchFolders();  // imports what has arrived and settled; the ids
    bool removeMedia(Id id, QString* error = nullptr);

    // The media item loaded in the source monitor and its marks
    // (frames at the active sequence's rate; -1 = unset).
    Id sourceMedia() const { return sourceMedia_; }
    // A subclip opens its media with In and Out around its range.
    void setSourceMedia(Id id);
    FrameTime sourceIn() const { return sourceIn_; }
    // Premiere's "insert and overwrite sequences as nests or individual clips": a sequence edited from the Source monitor
    // or dropped on the timeline comes in as its own clips (edit::placeSequenceClips) rather than one nested clip.
    bool sequencesAsClips() const { return sequencesAsClips_; }
    void setSequencesAsClips(bool on);
    FrameTime sourceOut() const { return sourceOut_; }
    void setSourceIn(FrameTime t);
    void setSourceOut(FrameTime t);
    // Three-point edits from the source monitor into the timeline at the
    // playhead (or the sequence In point), on the targeted tracks.
    bool insertFromSource(bool overwriteMode);
    // Source edits beyond insert and overwrite (Resolve's Cut and Edit pages, Final Cut's): the Source monitor's In to
    // Out appended at the end of the sequence, placed on top (the first free tracks above the targets at the
    // playhead), in place of the clip under the playhead with the difference rippled (Ripple Overwrite), or inserted
    // at the edit nearest the playhead (Smart Insert). Each one undo step.
    enum class SourceEdit { Insert, Overwrite, Append, PlaceOnTop, RippleOverwrite, SmartInsert };
    bool sourceEdit(SourceEdit mode);
    // Resolve's Video Only and Audio Only: source edits (and sequences edited as their clips) bring only the picture
    // or only the sound. Both on is the usual; setting one alone turns the other off.
    bool sourceVideo() const { return sourceVideo_; }
    bool sourceAudio() const { return sourceAudio_; }
    void setSourceKinds(bool video, bool audio);
    // Saves a range of a media item (frames of the active sequence, Out
    // inclusive) as a subclip in the bin; returns its id, or 0.
    Id makeSubclip(Id media, FrameTime in, FrameTime out, const QString& name = QString());
    // A stretch of a sound library file ([first, first + count) samples at `rate`) on audio track `track` (-1: the
    // target) from frame `at`, to the sample (edit::placeSoundRange). The file comes into the "Sound Effects" bin
    // first (once); with the "soundLibrary/copyToProject" preference and a saved project, a copy of it in the "Sound
    // Effects" folder beside the project does. Returns the clips made (none on failure, with `error`).
    std::vector<Id> placeSound(const QString& path, int64_t first, int64_t count, int rate, FrameTime at, int track = -1,
                               bool insertMode = false, QString* error = nullptr);

    // ---- Sequences ----------------------------------------------------------
    void setActiveSequence(Id id);
    Id newSequence(const QString& name, int w, int h, Rational fps);

    // ---- File -----------------------------------------------------------------
    QString filePath() const { return path_; }
    bool isModified() const { return history_.revision() != savedRevision_; }
    // Saving to the project's own path needs it not to be read-only (and the lock still to be this editor's); saving
    // to another path (Save As) takes that file's lock, so the copy is this editor's to edit.
    bool save(const QString& path, QString* error = nullptr);
    // Shared projects (core/ProjectLock.h): Auto takes the project's lock, or opens it read-only when someone else is
    // editing it; Edit refuses then (a stale lock is taken over either way); ReadOnly opens it without the lock.
    enum class Access { Auto, Edit, ReadOnly };
    bool open(const QString& path, QString* error = nullptr, Access access = Access::Auto);
    void newProject();
    // A project opened read-only: nothing can change it (edits are refused with a message saying who is editing it),
    // it reloads by itself when its editor saves, and once they let it go it can be taken to edit.
    bool readOnly() const { return readOnly_; }
    QString lockHolder() const { return lockHolder_; }  // who is editing a read-only project ("" once it is free)
    bool holdsLock() const { return holdsLock_; }
    bool canTakeEdit() const;
    // Takes the lock of a read-only project whose editor has let it go, reloading it from disk first. Changes made
    // here before it became read-only (taken over while this machine slept) are not thrown away unless asked.
    bool takeEdit(QString* error = nullptr, bool discardChanges = false);
    // Renews this editor's lock (every half minute); for a read-only project, reloads it when it was saved (unless
    // it holds changes of this editor's not yet saved) and notices when its lock is let go. Runs every few seconds.
    void checkSharedState(bool renewNow = false);  // renewNow: the lock renewed now, not on the half minute
    // Opens a recovered copy as if it were `originalPath` (empty = untitled),
    // marked modified so the user decides whether to save it.
    bool recover(const QString& copy, const QString& originalPath, QString* error = nullptr);

    // Emits projectChanged() after a direct mutation of mutableProject().
    void notifyChanged() { emit projectChanged(); }
    void message(const QString& text, int timeoutMs = 4000) { emit statusMessage(text, timeoutMs); }
    // Decodes the media's audio (and its waveform peaks) in the background; mediaReady follows.
    void startAudioDecode(const MediaItem& m);
    // Reads and checks the Content Credentials in the media's file (media/ContentCredentials.h) in the background,
    // setting its "c2pa" metadata to what they say ("" when it has none); projectChanged follows a change.
    void startCredentialRead(const MediaItem& m);
    // The same for the channels clips have chosen (Clip::channels), each once.
    void startClipAudioDecodes();
    // Whether the media's file is missing (media/Relink.h), each file checked at most every two seconds, since
    // painting asks often. Nested sequences and generators are never offline.
    bool isMediaOffline(Id media) const;
    void recheckOffline() { offlineChecked_.clear(); }  // after files moved or were relinked
    // Media files changed on disk (a still re-saved, a graphic re-rendered) are reloaded by themselves, as Premiere
    // and Resolve do: their cached frames, thumbnails and sound are dropped and their details re-read.
    void reloadChangedMedia(const QStringList& paths);
    // Growing media (Avid's and Premiere's growing files, Resolve's editing while recording): a file that is still
    // being written (a recorder, a capture card, a render still going) is followed while it grows: its length,
    // pictures and sound are read again every growingRefreshMs (0: only once it stops), off the window's thread and
    // without waiting for it to go quiet, and it is reloaded in full (thumbnails too) once it has stopped growing. A
    // file counts as growing once written for over two seconds without a pause of a second and a half. What is read
    // is a fact about the file, not an edit: undo keeps it, and the project is marked changed.
    bool isMediaGrowing(Id media) const;
    QStringList growingFiles() const;  // the files followed as growing now
    int growingRefreshMs() const { return growingRefreshMs_; }
    void setGrowingRefreshMs(int ms);
    // Lengths read again now: of the growing files, or with none seen growing, of every media file (one written on a
    // network share from another computer is not noticed by itself). Done when growingRefreshed is emitted.
    void refreshGrowingMedia();
    // A media file written to (the watcher's notice; public for tests) and the growing files looked at (every second
    // while any file is being written), by the clock (setClock for tests; milliseconds).
    void noteMediaFileWritten(const QString& path);
    void checkGrowingMedia();
    void setClock(std::function<qint64()> clock);  // (empty: the real clock again)

signals:
    void projectChanged();            // anything in the project changed
    void selectionChanged();
    void maskObjectChanged();         // a different object chosen in an object mask
    void paintEffectChanged();        // painting on the monitor started, stopped or moved to another effect
    void gradeNodeSelected();         // a different colour node picked
    void playheadChanged(montage::FrameTime t);
    void sequenceSwitched();          // a different sequence became active / project replaced
    void historyChanged();
    // An edit made, undone or redone: its name ("Insert", "Undo Insert", "Redo Insert"), to announce.
    void edited(const QString& what);
    void sourceChanged();             // source monitor media or marks changed
    void sequencesAsClipsChanged(bool on);
    void sourceKindsChanged();
    void mediaReady(const QString& path);  // audio peaks / thumbnails available
    void mediaFileChanged(montage::Id media);  // its file changed on disk and was reloaded
    void growingRefreshed(bool changed);       // lengths read again (refreshGrowingMedia, or a growing file's turn)
    void statusMessage(const QString& text, int timeoutMs);
    void fileStateChanged();          // path or modified flag changed
    void lockStateChanged();          // read-only, who holds the lock, or whether it is free changed

private:
    std::function<bool(const ParameterMove&)> parameterCapture_;
    LiveParameter liveParameter_;
    uint64_t projectGeneration_ = 0;
    void pruneSelection();
    bool refuseReadOnly();     // true, with a message, when the project cannot be changed
    void releaseLock();        // lets go of the current project's lock, if held
    bool reloadFromDisk();     // a read-only project, read again (keeping what is shown)
    bool readOnly_ = false;
    bool holdsLock_ = false;
    bool lockFree_ = false;     // a read-only project's lock has been let go
    QString lockHolder_;
    QDateTime diskTime_;        // the project file's time when last read or written
    QDateTime lastRefresh_;     // when the lock was last renewed
    QTimer* lockTimer_ = nullptr;
    void watchMediaFiles();  // keeps the watcher on the project's media files
    QFileSystemWatcher* watcher_ = nullptr;
    QFileSystemWatcher* folderWatcher_ = nullptr;  // the watch folders
    QTimer* folderTimer_ = nullptr;                 // a scan once things settle
    std::set<std::string> watchSkipped_;            // arrived but could not be imported: not tried again
    void watchFoldersChanged();
    QTimer* reloadTimer_ = nullptr;
    QStringList changedFiles_;
    // Files being written (growing media): when writing began and was last seen, and whether it counts as growing.
    struct Growth {
        qint64 first = 0, last = 0, refreshed = 0;
        bool growing = false;
    };
    std::map<QString, Growth> growth_;
    QTimer* growthTimer_ = nullptr;
    int growingRefreshMs_ = 15000;
    std::function<qint64()> clock_;
    // Reads their media's length and streams again on a worker (paths already being read are skipped), then applies
    // what changed (no reload) and emits growingRefreshed.
    void refreshLengths(const QStringList& paths);
    std::map<std::string, int> credentialReads_;  // per media path, credential reads begun (only the latest is applied)
    std::set<QString> probing_;
    std::map<QString, int> reloads_;  // per file, how often reloaded (a read begun before a reload is not applied)
    // Sets what probing `path` gave on its media, in the project and every state undo, redo or a gesture's start can
    // bring back, marking the project changed; whether the project changed.
    bool applyProbedFacts(const std::string& path, const MediaItem& fresh);
    mutable std::map<std::string, std::pair<bool, qint64>> offlineChecked_;  // path -> offline, when checked (ms)
    std::set<std::pair<std::string, int>> clipAudioRequested_;  // audio keys and rates already decoding

    Project project_;
    History history_;
    uint64_t savedRevision_ = 0;
    QString path_;
    std::vector<Id> selection_;
    Id primary_ = 0;
    Id selectedTransition_ = 0;
    Id inspectedChain_ = 0;
    std::map<Id, int> maskObjects_;  // effect -> object being picked
    Id paintEffect_ = 0;
    int meshPinKind_ = 0;
    Id gradeNodeClip_ = 0;           // the clip whose colour node is picked, and the node
    int gradeNodeKey_ = 0;
    int targetVideo_ = 0;
    int targetAudio_ = 0;
    bool snapping_ = true;
    Id sourceMedia_ = 0;
    bool sequencesAsClips_ = false;
    bool sourceVideo_ = true, sourceAudio_ = true;
    FrameTime sourceIn_ = -1;
    FrameTime sourceOut_ = -1;
    QString lastMergeKey_, undoGroup_;
    QDateTime lastMergeTime_;

    struct Gesture {
        QString label;
        Project origin;
        bool changed = false;
    };
    std::optional<Gesture> gesture_;
};

// What the timecode readouts show: timecode, or feet and frames (core/FilmCount.h; in the sequence's film gauge, 35 mm
// 4-perf where it has none) with 0+00 at the timeline's first frame or at its first picture (the earliest video clip,
// so a leader before it counts back from zero). Kept in the app's settings.
enum class Counter { Timecode, Feet, FeetFromPicture };
Counter counterDisplay();
void setCounterDisplay(Counter c);
// A frame of sequence `s` as the readouts show it.
QString timecodeString(const Sequence* s, FrameTime t);
// A length of time (feet and frames from zero whichever the counter).
QString durationString(const Sequence* s, FrameTime d);
// What a readout shows (feet and frames while the counter shows them; else timecode, frames or seconds as
// parseTimecode reads them), as a frame of the sequence, or a length where `duration`.
bool parseCounterString(const Sequence* s, const QString& text, FrameTime& t, bool duration = false);

}  // namespace montage
