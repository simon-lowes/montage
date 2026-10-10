#include "RenderQueue.h"

#include <QFile>
#include <QFileInfo>
#include <QtConcurrent>
#include <algorithm>

namespace montage {

RenderQueue::RenderQueue(QObject* parent) : QObject(parent) {
    connect(&watcher_, &QFutureWatcher<Result>::finished, this, &RenderQueue::finished);
    poll_.setInterval(200);
    connect(&poll_, &QTimer::timeout, this, &RenderQueue::poll);
}

RenderQueue::~RenderQueue() {
    cancel_ = true;
    watcher_.waitForFinished();
}

int RenderQueue::add(const QString& name, const QString& preset, const Project& project, Id sequence, const ExportSettings& settings,
                     int stems) {
    Job j;
    j.id = nextId_++;
    j.name = name;
    j.preset = preset;
    j.project = std::make_shared<const Project>(project);
    j.sequence = sequence;
    j.settings = settings;
    j.stems = stems;
    jobs_.push_back(std::move(j));
    emit changed();
    return jobs_.back().id;
}

const RenderQueue::Job* RenderQueue::job(int id) const {
    for (const Job& j : jobs_)
        if (j.id == id) return &j;
    return nullptr;
}

RenderQueue::Job* RenderQueue::find(int id) {
    for (Job& j : jobs_)
        if (j.id == id) return &j;
    return nullptr;
}

void RenderQueue::start() {
    if (running_) return;
    running_ = true;
    emit changed();
    if (!current_) next();
}

void RenderQueue::stop() {
    running_ = false;
    if (current_) cancel_ = true;
    emit changed();
}

void RenderQueue::waitForIdle() {
    watcher_.waitForFinished();
    if (current_) finished();
}

bool RenderQueue::remove(int id) {
    if (id == current_) return false;
    const auto n = std::erase_if(jobs_, [id](const Job& j) { return j.id == id; });
    if (n) emit changed();
    return n > 0;
}

void RenderQueue::clearFinished() {
    const auto n = std::erase_if(jobs_, [](const Job& j) { return j.status == Status::Done; });
    if (n) emit changed();
}

bool RenderQueue::retry(int id) {
    Job* j = find(id);
    if (!j || j->status == Status::Waiting || j->status == Status::Rendering) return false;
    j->status = Status::Waiting;
    j->progress = 0;
    j->error.clear();
    emit changed();
    if (running_ && !current_) next();
    return true;
}

void RenderQueue::next() {
    if (!running_ || current_) return;
    Job* j = nullptr;
    for (Job& candidate : jobs_)
        if (candidate.status == Status::Waiting) {
            j = &candidate;
            break;
        }
    if (!j) {
        running_ = false;
        emit changed();
        return;
    }
    current_ = j->id;
    const QFileInfo out(QString::fromStdString(j->settings.path));
    j->fileExisted = out.exists();
    j->fileModified = j->fileExisted ? out.lastModified() : QDateTime();
    j->status = Status::Rendering;
    j->progress = 0;
    cancel_ = false;
    progress_ = 0;
    poll_.start();
    emit changed();
    const std::shared_ptr<const Project> project = j->project;
    const Id seq = j->sequence;
    const ExportSettings settings = j->settings;
    const int stems = j->stems;
    std::atomic<bool>* cancel = &cancel_;
    std::atomic<double>* progress = &progress_;
    watcher_.setFuture(QtConcurrent::run([project, seq, settings, stems, cancel, progress]() -> Result {
        Result r;
        const Sequence* s = project->findSequence(seq);
        if (!s) {
            r.error = "The sequence no longer exists";
            return r;
        }
        try {
            const double share = stems ? 0.5 : 1.0;  // the stems take the second half of the bar
            r.ok = exportSequence(*project, *s, settings, [progress, share](double f, FrameTime) { *progress = f * share; }, cancel,
                                  &r.error, &r.encoder);
            if (r.ok && stems)
                r.ok = exportStems(*project, *s, settings, stems, nullptr,
                                   [progress](double f, FrameTime) { *progress = 0.5 + f * 0.5; }, cancel, &r.error);
        } catch (const std::exception& e) {
            r.error = e.what();
        }
        return r;
    }));
}

void RenderQueue::poll() {
    if (Job* j = find(current_)) {
        j->progress = progress_;
        emit changed();
    }
}

void RenderQueue::finished() {
    if (!current_) return;
    poll_.stop();
    const Result r = watcher_.result();
    const int id = current_;
    current_ = 0;
    const bool cancelled = cancel_.load();
    if (Job* j = find(id)) {
        j->finished = QDateTime::currentDateTime();
        j->encoder = QString::fromStdString(r.encoder);
        if (r.ok) {
            j->status = Status::Done;
            j->progress = 1;
        } else {
            j->status = cancelled ? Status::Cancelled : Status::Failed;
            j->error = cancelled ? tr("Cancelled") : QString::fromStdString(r.error);
            // No half-written files, but nothing the render did not write.
            const QFileInfo out(QString::fromStdString(j->settings.path));
            if (out.exists() && (!j->fileExisted || out.lastModified() != j->fileModified)) QFile::remove(out.filePath());
        }
    }
    emit changed();
    emit jobFinished(id, r.ok);
    next();
}

}  // namespace montage
