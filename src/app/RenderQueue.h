// Montage — the render queue: exports added from the Export dialog wait in a list and render in the background while
// editing goes on, several at once up to a set number (Settings' jobs/at_once), each with its own progress and cancel.
// Each job renders the project as it was when it was added.
//
// The Montage window's queue is kept on disk (render/BackgroundJobs.h: a folder a job, with the project as it was), so
// after a restart, or a crash, its jobs are there again and a render that was under way when it stopped is rendered
// again from the start (the queue starting by itself if it was running). MCP's montage_job lists its jobs too, and
// can cancel one (the queue hears it within a fifth of a second).
#pragma once

#include <QDateTime>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <list>
#include <memory>
#include <vector>

#include "core/Model.h"
#include "render/Exporter.h"

namespace montage {

// Whether exports draw their frames on the GPU: while GPU Rendering is on (Playback menu) and there is one.
bool exportsOnGpu();

class RenderQueue : public QObject {
    Q_OBJECT
public:
    enum class Status { Waiting, Rendering, Done, Failed, Cancelled };
    struct Job {
        int id = 0;
        QString name;  // sequence and preset
        QString preset;
        std::shared_ptr<const Project> project;
        Id sequence = 0;
        ExportSettings settings;
        int stems = 0;  // after the export: 1 a WAV per audio track, 2 per bus
        Status status = Status::Waiting;
        double progress = 0;
        QString error;
        QString encoder;
        QDateTime finished;
        // The output file before rendering, so a failed render only removes what it wrote.
        bool fileExisted = false;
        QDateTime fileModified;
        bool resumed = false;  // taken up again after Montage stopped while it rendered
    };

    // `folder`: where the queue is kept (render/BackgroundJobs.h's store: jobs::storeFolder() for the window's); empty,
    // only in memory. A kept queue is read back at once, and starts if it was running when it was last left.
    explicit RenderQueue(QObject* parent = nullptr, const QString& folder = {});
    ~RenderQueue() override;

    // Adds a job (a copy of `project` is kept); returns its id.
    int add(const QString& name, const QString& preset, const Project& project, Id sequence, const ExportSettings& settings,
            int stems = 0);
    const std::vector<Job>& jobs() const { return jobs_; }
    const Job* job(int id) const;

    // Renders the waiting jobs in order, up to atOnce() of them at a time, until none is left (or stop()).
    void start();
    // Cancels the jobs rendering now and stops the queue; waiting jobs stay.
    void stop();
    // Cancels one job: one rendering stops (the others go on), one waiting is set aside. False for one already finished.
    bool cancel(int id);
    bool running() const { return running_; }
    // How many render at once (1 to 16; jobs::atOnce() unless set).
    int atOnce() const { return atOnce_; }
    void setAtOnce(int n);
    int rendering() const { return int(runs_.size()); }
    // Waits for the jobs rendering now (after stop()).
    void waitForIdle();
    // On quitting: the renders under way stopped, but kept as they were (rendering, the queue running), so a kept queue
    // renders them again next time as after a crash. (The destructor does the same.)
    void suspend();
    bool remove(int id);  // not while it renders
    void clearFinished();
    // Puts a finished, failed or cancelled job back to waiting.
    bool retry(int id);
    // The job's id in the store (render/BackgroundJobs.h), as montage_job names it.
    static QString storeId(int id) { return QStringLiteral("queue-%1").arg(id); }

signals:
    void changed();
    void jobFinished(int id, bool ok);

private:
    struct Result {
        bool ok = false;
        bool keptUnsigned = false;  // failed signing; the export is kept
        std::string error, encoder;
    };
    struct Run {
        int id = 0;
        std::atomic<bool> cancel{false};
        std::atomic<double> progress{0};
        QFutureWatcher<Result> watcher;
    };
    void next();
    void finished(Run* run);
    void poll();
    bool cancelAsked(const Job& j);  // a cancel file left in its folder (taken away)
    Job* find(int id);
    void load();
    void store(const Job& j, bool withProject = false);
    void storeQueue();

    std::vector<Job> jobs_;
    int nextId_ = 1;
    bool running_ = false;
    int atOnce_ = 1;
    QString folder_;
    std::list<std::unique_ptr<Run>> runs_;
    std::vector<std::unique_ptr<Run>> spent_;
    QTimer poll_;
    QDateTime stored_;  // when the rendering jobs' progress was last written
};

}  // namespace montage
