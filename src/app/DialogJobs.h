// Montage — the jobs the app's own dialogs run (the analyses behind a progress dialog, the Project Manager's copies
// and transcodes), kept in the job store (render/BackgroundJobs.h) as the render queue's exports and MCP's background
// calls are: each is there while it runs and after it ends, so montage_job lists it and can ask it to stop, and one cut
// short by a crash or a kill reads as interrupted.
//
// A consolidation is kept with the project as it was and its settings, as the render queue keeps an export. The next
// Montage to start takes it up again by itself: in the background, from its start, writing the files the first run had
// begun under the same names. One that was cut short twice is given up (it may be what stops Montage).
#pragma once

#include <QFutureWatcher>
#include <QObject>
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

namespace montage {

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

signals:
    void resumed(const QString& title);
    // A job taken up again has ended: `text` says how, for the status bar.
    void finished(const QString& id, bool ok, const QString& text);

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
    void save(const QString& id, const std::function<void(jobs::Record&)>& change);
    void startNext();
    void runEnded();

    QString folder_;
    mutable std::mutex m_;                    // (records are written from the dialogs' threads too)
    std::map<QString, jobs::Record> records_;  // those running here
    std::map<QString, qint64> written_;        // when each one's progress was last written
    std::deque<std::unique_ptr<Run>> waiting_;
    std::unique_ptr<Run> run_;
    QTimer poll_;
};

}  // namespace montage
