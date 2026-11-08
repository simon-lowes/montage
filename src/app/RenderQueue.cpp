#include "RenderQueue.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtConcurrent>
#include <algorithm>

#include "Settings.h"
#include "core/ProjectIO.h"
#include "render/BackgroundJobs.h"
#include "render/GpuCompositor.h"
#include "render/RenderFarm.h"

namespace montage {

namespace {

const QString kOwner = QStringLiteral("render-queue");

const char* statusName(RenderQueue::Status s) {
    switch (s) {
        case RenderQueue::Status::Waiting: return "waiting";
        case RenderQueue::Status::Rendering: return "running";
        case RenderQueue::Status::Done: return "done";
        case RenderQueue::Status::Failed: return "failed";
        case RenderQueue::Status::Cancelled: return "cancelled";
    }
    return "waiting";
}

}  // namespace

bool exportsOnGpu() { return appSettings().value("playback/gpuRendering", true).toBool() && gpu::available(); }

RenderQueue::RenderQueue(QObject* parent, const QString& folder) : QObject(parent), atOnce_(jobs::atOnce()), folder_(folder) {
    poll_.setInterval(200);
    connect(&poll_, &QTimer::timeout, this, &RenderQueue::poll);
    if (!folder_.isEmpty()) load();
}

RenderQueue::~RenderQueue() { suspend(); }

void RenderQueue::suspend() {
    // (what renders now is stopped, but stays "running" in the store, so it is rendered again next time, as after a
    // crash; its half-written file is written over then)
    poll_.stop();
    for (auto& r : runs_) {
        QObject::disconnect(&r->watcher, nullptr, this, nullptr);
        r->cancel = true;
    }
    for (auto& r : runs_) r->watcher.waitForFinished();
    runs_.clear();
}

void RenderQueue::load() {
    const std::vector<jobs::Record> records = jobs::all(kOwner, folder_);
    bool wasRunning = false;
    {
        QFile f(folder_ + QStringLiteral("/render-queue.json"));
        if (f.open(QIODevice::ReadOnly)) wasRunning = QJsonDocument::fromJson(f.readAll()).object().value("running").toBool();
    }
    for (const jobs::Record& r : records) {
        // (ids past every job in the store, another Montage's too, so no job of this queue is written over one of its)
        nextId_ = std::max(nextId_, r.data.value("id").toInt() + 1);
        if (jobs::runningElsewhere(r, folder_)) continue;  // (another Montage's queue holds it)
        Job j;
        j.id = r.data.value("id").toInt();
        if (j.id <= 0) continue;
        j.name = r.title;
        j.preset = r.data.value("preset").toString();
        j.sequence = r.data.value("sequence").toString().toULongLong();
        j.stems = r.data.value("stems").toInt();
        std::string err;
        if (!exportSettingsFromJson(r.data.value("settings").toObject(), j.settings, &err)) continue;
        QFile pf(jobs::jobFolder(r.id, folder_) + QStringLiteral("/project.json"));
        if (!pf.open(QIODevice::ReadOnly)) continue;
        auto project = std::make_shared<Project>();
        if (!projectFromJson(pf.readAll().toStdString(), *project, &err)) continue;
        j.project = project;
        j.error = r.error;
        j.encoder = r.data.value("encoder").toString();
        j.finished = QDateTime::fromString(r.data.value("finished").toString(), Qt::ISODate);
        j.fileExisted = r.data.value("fileExisted").toBool();
        j.fileModified = QDateTime::fromString(r.data.value("fileModified").toString(), Qt::ISODateWithMs);
        const QString st = r.status;
        if (st == QLatin1String("done")) j.status = Status::Done, j.progress = 1;
        else if (st == QLatin1String("failed")) j.status = Status::Failed;
        else if (st == QLatin1String("cancelled")) j.status = Status::Cancelled;
        else j.status = Status::Waiting;
        // A render under way when Montage stopped (or crashed) waits to be rendered again from its start.
        if (st == QLatin1String("running") || st == QLatin1String("interrupted")) {
            j.resumed = true;
            j.progress = 0;
            wasRunning = true;
        }
        nextId_ = std::max(nextId_, j.id + 1);
        jobs_.push_back(std::move(j));
        store(jobs_.back());  // (held by this queue now: another Montage leaves it alone)
    }
    std::sort(jobs_.begin(), jobs_.end(), [](const Job& a, const Job& b) { return a.id < b.id; });
    if (wasRunning && std::any_of(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.status == Status::Waiting; }))
        QTimer::singleShot(0, this, [this] { start(); });
}

void RenderQueue::store(const Job& j, bool withProject) {
    if (folder_.isEmpty()) return;
    const QString id = storeId(j.id);
    if (withProject) {
        if (!QDir().mkpath(jobs::jobFolder(id, folder_))) return;
        QSaveFile f(jobs::jobFolder(id, folder_) + QStringLiteral("/project.json"));
        if (!f.open(QIODevice::WriteOnly)) return;
        f.write(QByteArray::fromStdString(projectToJson(*j.project)));
        if (!f.commit()) return;
    }
    jobs::Record r;
    jobs::load(id, r, folder_);  // (its created time kept)
    r.id = id;
    r.owner = kOwner;
    r.kind = QStringLiteral("export");
    r.title = j.name;
    r.status = QString::fromLatin1(statusName(j.status));
    r.progress = j.progress;
    r.step = j.status == Status::Rendering ? tr("Rendering") : QString();
    r.error = j.error;
    r.order = j.id;
    r.pid = jobs::thisProcess();  // (the queue holding it: another Montage's queue leaves it alone)
    r.process = jobs::thisProcessToken();
    r.data = QJsonObject{{"id", j.id},
                         {"preset", j.preset},
                         {"sequence", QString::number(j.sequence)},
                         {"stems", j.stems},
                         {"settings", exportSettingsToJson(j.settings)},
                         {"output", QString::fromStdString(j.settings.path)},
                         {"encoder", j.encoder},
                         {"finished", j.finished.toString(Qt::ISODate)},
                         {"fileExisted", j.fileExisted},
                         {"fileModified", j.fileModified.toString(Qt::ISODateWithMs)}};
    jobs::save(r, folder_);
}

void RenderQueue::storeQueue() {
    if (folder_.isEmpty() || !QDir().mkpath(folder_)) return;
    QSaveFile f(folder_ + QStringLiteral("/render-queue.json"));
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(QJsonObject{{"running", running_}}).toJson(QJsonDocument::Compact));
    f.commit();
}

int RenderQueue::add(const QString& name, const QString& preset, const Project& project, Id sequence, const ExportSettings& settings,
                     int stems) {
    Job j;
    // (not one another Montage sharing the store has taken since)
    if (!folder_.isEmpty())
        while (QFileInfo::exists(jobs::jobFolder(storeId(nextId_), folder_))) ++nextId_;
    j.id = nextId_++;
    j.name = name;
    j.preset = preset;
    j.project = std::make_shared<const Project>(project);
    j.sequence = sequence;
    j.settings = settings;
    j.stems = stems;
    jobs_.push_back(std::move(j));
    store(jobs_.back(), true);
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
    storeQueue();
    emit changed();
    next();
}

void RenderQueue::stop() {
    running_ = false;
    storeQueue();
    for (auto& r : runs_) r->cancel = true;
    emit changed();
}

bool RenderQueue::cancel(int id) {
    Job* j = find(id);
    if (!j) return false;
    if (j->status == Status::Waiting) {
        j->status = Status::Cancelled;
        j->error = tr("Cancelled");
        store(*j);
        emit changed();
        return true;
    }
    for (auto& r : runs_)
        if (r->id == id) {
            r->cancel = true;
            return true;
        }
    return false;
}

void RenderQueue::setAtOnce(int n) {
    atOnce_ = std::clamp(n, 1, 16);
    next();
}

void RenderQueue::waitForIdle() {
    while (!runs_.empty()) {
        Run* r = runs_.front().get();
        r->watcher.waitForFinished();
        finished(r);  // (which may start the next while the queue runs)
    }
}

bool RenderQueue::remove(int id) {
    const Job* j = job(id);
    if (!j || j->status == Status::Rendering) return false;
    std::erase_if(jobs_, [id](const Job& j) { return j.id == id; });
    if (!folder_.isEmpty()) jobs::remove(storeId(id), folder_);
    emit changed();
    return true;
}

void RenderQueue::clearFinished() {
    std::vector<int> gone;
    for (const Job& j : jobs_)
        if (j.status == Status::Done) gone.push_back(j.id);
    for (int id : gone) {
        std::erase_if(jobs_, [id](const Job& j) { return j.id == id; });
        if (!folder_.isEmpty()) jobs::remove(storeId(id), folder_);
    }
    if (!gone.empty()) emit changed();
}

bool RenderQueue::retry(int id) {
    Job* j = find(id);
    if (!j || j->status == Status::Waiting || j->status == Status::Rendering) return false;
    j->status = Status::Waiting;
    j->progress = 0;
    j->error.clear();
    j->resumed = false;
    store(*j);
    emit changed();
    if (running_) next();
    return true;
}

void RenderQueue::next() {
    while (running_ && int(runs_.size()) < atOnce_) {
        // (the next waiting, unless a job rendering now writes the same file: that one waits for it)
        // (paths compared as written, made absolute: QFileInfo's == takes two files not there yet for one)
        const auto where = [](const Job& job) { return QDir::cleanPath(QFileInfo(QString::fromStdString(job.settings.path)).absoluteFilePath()); };
        Job* j = nullptr;
        for (Job& candidate : jobs_) {
            if (candidate.status != Status::Waiting) continue;
            const bool sameFile = std::any_of(runs_.begin(), runs_.end(), [&](const std::unique_ptr<Run>& r) {
                const Job* other = find(r->id);
                return other && where(*other) == where(candidate);
            });
            if (sameFile) continue;
            j = &candidate;
            break;
        }
        if (!j) break;
        if (cancelAsked(*j)) {  // (by montage_job while it waited)
            j->status = Status::Cancelled;
            j->error = tr("Cancelled");
            j->finished = QDateTime::currentDateTime();
            store(*j);
            continue;
        }
        // (a job rendered again after Montage stopped keeps what was there before its first start)
        if (!j->resumed) {
            const QFileInfo out(QString::fromStdString(j->settings.path));
            j->fileExisted = out.exists();
            j->fileModified = j->fileExisted ? out.lastModified() : QDateTime();
        }
        j->status = Status::Rendering;
        j->progress = 0;
        store(*j);
        auto run = std::make_unique<Run>();
        run->id = j->id;
        Run* r = run.get();
        runs_.push_back(std::move(run));
        connect(&r->watcher, &QFutureWatcher<Result>::finished, this, [this, r] { finished(r); });
        const std::shared_ptr<const Project> project = j->project;
        const Id seq = j->sequence;
        ExportSettings settings = j->settings;
        settings.gpu = exportsOnGpu();  // (what it cannot draw on the CPU)
        const int stems = j->stems;
        std::atomic<bool>* cancel = &r->cancel;
        std::atomic<double>* progress = &r->progress;
        r->watcher.setFuture(QtConcurrent::run([project, seq, settings, stems, cancel, progress]() -> Result {
            Result res;
            res.ok = renderExport(*project, seq, settings, stems, [progress](double f, FrameTime) { *progress = f; }, cancel, &res.error,
                                  &res.encoder, &res.keptUnsigned);
            return res;
        }));
    }
    if (!runs_.empty() && !poll_.isActive()) poll_.start();
    if (running_ && runs_.empty()) {
        // Nothing left to render.
        running_ = false;
        storeQueue();
    }
    emit changed();
}

bool RenderQueue::cancelAsked(const Job& j) {
    if (folder_.isEmpty()) return false;
    const QString file = jobs::jobFolder(storeId(j.id), folder_) + QStringLiteral("/cancel");
    if (!QFileInfo::exists(file)) return false;
    QFile::remove(file);
    return true;
}

void RenderQueue::poll() {
    // (progress written to the store every second; a cancel asked for there, by montage_job, heard here)
    const bool write = !stored_.isValid() || stored_.msecsTo(QDateTime::currentDateTimeUtc()) >= 1000;
    for (auto& r : runs_) {
        Job* j = find(r->id);
        if (!j) continue;
        j->progress = r->progress;
        if (cancelAsked(*j)) r->cancel = true;
        if (write) store(*j);
    }
    if (write) stored_ = QDateTime::currentDateTimeUtc();
    if (!runs_.empty()) emit changed();
}

void RenderQueue::finished(Run* run) {
    spent_.clear();  // (runs finished before: their watchers' signals have long returned)
    const auto it = std::find_if(runs_.begin(), runs_.end(), [run](const auto& r) { return r.get() == run; });
    if (it == runs_.end()) return;
    std::unique_ptr<Run> hold = std::move(*it);
    runs_.erase(it);
    if (runs_.empty()) poll_.stop();
    const Result r = run->watcher.result();
    const int id = run->id;
    const bool cancelled = run->cancel.load();
    if (Job* j = find(id)) {
        j->finished = QDateTime::currentDateTime();
        j->encoder = QString::fromStdString(r.encoder);
        if (r.ok) {
            j->status = Status::Done;
            j->progress = 1;
        } else {
            j->status = cancelled ? Status::Cancelled : Status::Failed;
            j->error = cancelled ? tr("Cancelled") : QString::fromStdString(r.error);
            // No half-written files, but nothing the render did not write (and not a whole one only left unsigned).
            const QFileInfo out(QString::fromStdString(j->settings.path));
            if (!r.keptUnsigned && out.exists() && (!j->fileExisted || out.lastModified() != j->fileModified))
                QFile::remove(out.filePath());
        }
        store(*j);
    }
    emit changed();
    emit jobFinished(id, r.ok);
    // (the watcher is let go once its own signal has returned)
    QObject::disconnect(&hold->watcher, nullptr, this, nullptr);
    spent_.push_back(std::move(hold));
    next();
}

}  // namespace montage
