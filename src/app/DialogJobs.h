// Montage — the jobs the app's own dialogs run (the analyses behind a progress dialog, the Project Manager's copies
// and transcodes), kept in the job store (render/BackgroundJobs.h) as the render queue's exports and MCP's background
// calls are: each is there while it runs and after it ends, so montage_job lists it and can ask it to stop, and one cut
// short by a crash or a kill reads as interrupted.
//
// A consolidation is kept with the project as it was and its settings, as the render queue keeps an export. The next
// Montage to start takes it up again by itself: in the background, from its start, writing the files the first run had
// begun under the same names. One that was cut short twice is given up (it may be what stops Montage).
//
// A task is a job of a kind registered by name (registerKind): its work runs off the interface thread from the project
// as it was and the task's own input, both kept with the job, and leaves its result in the job's folder
// (result.json, and whatever files and finished parts it writes there). The dialog that started it may watch it behind
// a progress dialog or leave it to run; when it ends, its result is applied as one undo step, unless the clips it was
// made for have moved meanwhile: then it is offered with a note of what moved, to be applied from the jobs list. A task
// cut short by a crash or a kill is taken up again by the next Montage to start, passing over the parts already
// finished.
#pragma once

#include <QFutureWatcher>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "core/Model.h"
#include "render/BackgroundJobs.h"
#include "render/ProjectManager.h"

class QWidget;

namespace montage {

class EditorState;

// What a task's work is given, off the interface thread.
struct TaskRun {
    std::shared_ptr<const Project> project;  // as it was when the task began
    QJsonObject input;
    QString folder;                          // the job's own folder ("" when nothing is kept)
    std::function<void(double)> progress;    // how far it is, 0 to 1 (a paused task waits in here)
    const std::atomic<bool>* cancel = nullptr;
    QJsonObject result;                      // kept as result.json when it ends well
    std::string error;
    // A part finished by an earlier run of this job (kept in its folder), so that it is not worked out again.
    bool part(const QString& name, QJsonValue* value) const;
    void keepPart(const QString& name, const QJsonValue& value) const;
    bool cancelled() const { return cancel && cancel->load(); }
};

struct TaskKind {
    // The work, off the interface thread: false when it failed (`error`) or was cancelled.
    std::function<bool(TaskRun&)> run;
    // On the interface thread once it has ended well: makes its edit as one undo step and returns what it did
    // ("message": what to say in the status bar; the rest is the kind's own). May be empty (a result only shown).
    std::function<QJsonObject(EditorState*, const QJsonObject& input, const QJsonObject& result)> apply;
};

struct TaskOutcome {
    enum How { Done, Failed, Cancelled, Background };  // Background: left to run, its dialog closed
    How how = Failed;
    QString id;
    QJsonObject result, applied;
    QString error;
    bool done() const { return how == Done; }
};

class DialogJobs : public QObject {
    Q_OBJECT
public:
    // `folder`: the store (the window's is jobs::storeFolder()). Empty: nothing is kept.
    explicit DialogJobs(QObject* parent = nullptr, const QString& folder = {});
    ~DialogJobs() override;

    // A job a dialog starts now, recorded as running in this process. Returns its id in the store, empty when nothing
    // is kept.
    QString begin(const QString& kind, const QString& title);
    // The Project Manager's, with the project and its settings, so that it can be taken up again.
    QString beginConsolidate(const Project& p, const ConsolidateOptions& o, const QString& title);
    // The settings to run that job with: every file it begins is noted in its record (from any thread).
    ConsolidateOptions tracked(const QString& id, ConsolidateOptions o);
    // How far it is, 0 to 1 (written at most once a second).
    void setProgress(const QString& id, double fraction);
    void end(const QString& id, bool ok, bool cancelled, const QString& error = {});
    // Whether the store was asked to stop it (montage_job's cancel).
    bool cancelAsked(const QString& id) const;

    // Takes up the consolidations in the store that were cut short and that no other Montage holds, one after the
    // other in the background. Returns how many were taken up. (Other jobs cut short are marked as failed: they are
    // not run again.)
    int resume();
    bool running() const { return bool(run_) || !waiting_.empty(); }
    // Stops what is taken up, leaving it in the store to be taken up again (as the window's closing does).
    void suspend();

    // --- Tasks ---
    static void registerKind(const QString& kind, TaskKind k);
    static bool hasKind(const QString& kind);
    // The job store of the window `w` is in, else one shared by the dialogs that have no window (it keeps nothing on
    // disk: its tasks run and are applied, and are gone with Montage).
    static DialogJobs* of(QWidget* w);
    // Where results are applied (the window's editor state): a task taken up again has no dialog to say.
    void setState(EditorState* state);
    // Starts a task now. `input` is the kind's own; its "clips" (ids as strings) are the clips the result is made
    // for and "sequence" the sequence it edits: moved or changed while it ran, the result is offered and not applied.
    // Returns its id ("" for a kind not registered).
    QString start(EditorState* state, const QString& kind, const QString& title, const QJsonObject& input);
    // The same on `project` (not the state's own as it is now).
    QString startOn(const Project& project, EditorState* state, const QString& kind, const QString& title, const QJsonObject& input);
    // Waits for it behind a progress dialog with Cancel and Run in Background. Done: its result was applied (as
    // `applied` says). Background: the dialog was closed and the task goes on; its result is applied when it ends.
    TaskOutcome watch(const QString& id, QWidget* parent);
    TaskOutcome run(EditorState* state, const QString& kind, const QString& title, const QJsonObject& input, QWidget* parent) {
        return watch(start(state, kind, title, input), parent);
    }
    // The tasks running or waiting here, in the order they run.
    struct TaskInfo {
        QString id, kind, title;
        double progress = 0;
        bool running = false, paused = false;
    };
    std::vector<TaskInfo> tasks() const;
    void setPaused(const QString& id, bool paused);  // (it waits at its next report of progress)
    void stop(const QString& id);
    bool moveUp(const QString& id);                  // a waiting one, before the one ahead of it
    // A result kept and not applied because its clips had moved: the note of what moved ("" when none is offered).
    QString offeredNote(const QString& id) const;
    // Applies it now, as one undo step, onto the project as it is. False when there is none or it cannot be applied.
    bool applyOffered(const QString& id, QString* said = nullptr);
    // The result a finished task of this kind left in the store, the latest whose input `matches` (null: any). False
    // when there is none.
    bool latestResult(const QString& kind, const std::function<bool(const QJsonObject& input)>& matches, QJsonObject* result,
                      QJsonObject* input = nullptr) const;
    // What a task's result names of the project that is no longer as it was: "" when nothing it was made for moved.
    static QString movedNote(const Project& then, const Project& now, const QJsonObject& input);
    bool busy() const { return running() || !tasks_.empty(); }

signals:
    void resumed(const QString& title);
    // A job taken up again, or a task nobody watched, has ended: `text` says how, for the status bar.
    void finished(const QString& id, bool ok, const QString& text);
    void taskEnded(const QString& id);
    // A task's result was kept and not applied: `note` says what moved.
    void offered(const QString& id, const QString& title, const QString& note);
    void tasksChanged();

private:
    struct Run {
        QString id;
        std::shared_ptr<const Project> project;
        ConsolidateOptions options;
        std::shared_ptr<ConsolidateResult> result = std::make_shared<ConsolidateResult>();
        std::shared_ptr<std::string> error = std::make_shared<std::string>();
        std::shared_ptr<std::atomic<double>> done = std::make_shared<std::atomic<double>>(0.0);
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
        QFutureWatcher<bool> watcher;
    };
    struct Shared;  // what a task's thread and this object both hold
    struct Task {
        QString id, kind, title;
        QJsonObject input;
        QPointer<EditorState> state;
        std::shared_ptr<Shared> shared;
        QFutureWatcher<bool> watcher;
        bool started = false, watched = false, resumed = false, saidPaused = false;
    };
    struct Offered {
        QString kind;
        QJsonObject input, result;
    };
    void save(const QString& id, const std::function<void(jobs::Record&)>& change);
    void startNext();
    void runEnded();
    Task* task(const QString& id) const;
    void startTask(Task& t);
    void startWaitingTasks();
    void taskFinished(const QString& id);
    QJsonObject applyResult(Task& t, const QJsonObject& result, QString* note);
    EditorState* stateFor(const Task& t) const;

    QString folder_;
    mutable std::mutex m_;                    // (records are written from the dialogs' threads too)
    std::map<QString, jobs::Record> records_;  // those running here
    std::map<QString, qint64> written_;        // when each one's progress was last written
    std::deque<std::unique_ptr<Run>> waiting_;
    std::unique_ptr<Run> run_;
    QTimer poll_;
    std::deque<std::unique_ptr<Task>> tasks_;      // running and waiting, in order
    std::map<QString, TaskOutcome> outcomes_;      // of watched tasks that ended, until their watcher takes them
    std::map<QString, Offered> offeredHere_;       // results kept and not applied, where nothing is kept on disk
    QPointer<EditorState> state_;
    int memoryIds_ = 0;
};

}  // namespace montage
