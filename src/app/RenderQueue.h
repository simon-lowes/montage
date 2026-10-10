// Montage — the render queue (as Adobe Media Encoder's and Resolve's): exports
// added from the Export dialog wait in a list and render one after another in
// the background while editing goes on. Each job renders the project as it
// was when it was added.
#pragma once

#include <QDateTime>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <atomic>
#include <memory>
#include <vector>

#include "core/Model.h"
#include "render/Exporter.h"

namespace montage {

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
    };

    explicit RenderQueue(QObject* parent = nullptr);
    ~RenderQueue() override;

    // Adds a job (a copy of `project` is kept); returns its id.
    int add(const QString& name, const QString& preset, const Project& project, Id sequence, const ExportSettings& settings,
            int stems = 0);
    const std::vector<Job>& jobs() const { return jobs_; }
    const Job* job(int id) const;

    // Renders the waiting jobs in order until none is left (or stop()).
    void start();
    // Cancels the job rendering now and stops the queue; waiting jobs stay.
    void stop();
    bool running() const { return running_; }
    // Waits for the job rendering now (after stop(), on quitting).
    void waitForIdle();
    bool remove(int id);  // not while it renders
    void clearFinished();
    // Puts a finished, failed or cancelled job back to waiting.
    bool retry(int id);

signals:
    void changed();
    void jobFinished(int id, bool ok);

private:
    void next();
    void finished();
    void poll();
    Job* find(int id);

    std::vector<Job> jobs_;
    int nextId_ = 1;
    bool running_ = false;
    int current_ = 0;
    std::atomic<bool> cancel_{false};
    std::atomic<double> progress_{0};
    struct Result {
        bool ok = false;
        std::string error, encoder;
    };
    QFutureWatcher<Result> watcher_;
    QTimer poll_;
};

}  // namespace montage
