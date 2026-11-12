#include "DialogJobs.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtConcurrent>
#include <algorithm>

#include "core/ProjectIO.h"

namespace montage {

namespace {

const QString kOwner = QStringLiteral("app");
const QString kConsolidate = QStringLiteral("consolidate");
constexpr int kKept = 100;  // finished jobs kept in the store
constexpr int kTries = 2;   // how often one cut short is taken up again

QJsonObject optionsJson(const ConsolidateOptions& o) {
    QJsonArray sequences;
    for (Id id : o.sequences) sequences.append(QString::number(id));
    return QJsonObject{{"folder", QString::fromStdString(o.folder)}, {"name", QString::fromStdString(o.name)},
                       {"sequences", sequences},                     {"trim", o.trim},
                       {"handles", o.handles},                       {"codec", QString::fromStdString(o.codec)},
                       {"keepUnused", o.keepUnused}};
}

ConsolidateOptions optionsFromJson(const QJsonObject& j) {
    ConsolidateOptions o;
    o.folder = j.value("folder").toString().toStdString();
    o.name = j.value("name").toString(QStringLiteral("Project")).toStdString();
    for (const QJsonValue& v : j.value("sequences").toArray()) o.sequences.push_back(Id(v.toString().toULongLong()));
    o.trim = j.value("trim").toBool();
    o.handles = j.value("handles").toDouble(1.0);
    o.codec = j.value("codec").toString(QStringLiteral("prores_ks")).toStdString();
    o.keepUnused = j.value("keepUnused").toBool();
    return o;
}

}  // namespace

DialogJobs::DialogJobs(QObject* parent, const QString& folder) : QObject(parent), folder_(folder) {
    poll_.setInterval(250);
    connect(&poll_, &QTimer::timeout, this, [this] {
        if (!run_) return;
        setProgress(run_->id, run_->done->load());
        if (cancelAsked(run_->id)) *run_->cancel = true;
    });
    if (!folder_.isEmpty()) jobs::prune(kOwner, kKept, folder_);
}

DialogJobs::~DialogJobs() { suspend(); }

void DialogJobs::save(const QString& id, const std::function<void(jobs::Record&)>& change) {
    if (id.isEmpty()) return;
    std::lock_guard l(m_);
    const auto it = records_.find(id);
    if (it == records_.end()) return;
    change(it->second);
    jobs::save(it->second, folder_);
}

QString DialogJobs::begin(const QString& kind, const QString& title) {
    if (folder_.isEmpty() || !QDir().mkpath(folder_)) return {};
    // (an id after every one the store has, so that they are listed in the order they were begun, and one no other
    // Montage takes at the same moment: the folder is made here, or the next one is tried)
    int from = 1;
    for (const QString& name : QDir(folder_).entryList({QStringLiteral("app-*")}, QDir::Dirs | QDir::NoDotAndDotDot))
        from = std::max(from, name.mid(4).toInt() + 1);
    QString id;
    int order = 0;
    for (int n = from; n < from + 1000; ++n) {
        const QString tryId = QStringLiteral("app-%1").arg(n);
        if (!QDir(folder_).mkdir(tryId)) continue;
        id = tryId;
        order = n;
        break;
    }
    if (id.isEmpty()) return {};
    jobs::Record r;
    r.id = id;
    r.owner = kOwner;
    r.kind = kind;
    r.title = title;
    r.status = QStringLiteral("running");
    r.order = order;
    r.pid = jobs::thisProcess();
    r.process = jobs::thisProcessToken();
    std::lock_guard l(m_);
    if (!jobs::save(r, folder_)) return {};
    records_[id] = r;
    return id;
}

QString DialogJobs::beginConsolidate(const Project& p, const ConsolidateOptions& o, const QString& title) {
    const QString id = begin(kConsolidate, title);
    if (id.isEmpty()) return id;
    // The project as it is now, kept with the job: taken up again, it copies this, not what is open then.
    QSaveFile f(jobs::jobFolder(id, folder_) + QStringLiteral("/project.json"));
    const bool kept = f.open(QIODevice::WriteOnly) && f.write(QByteArray::fromStdString(projectToJson(p))) >= 0 && f.commit();
    save(id, [&](jobs::Record& r) {
        r.data["options"] = optionsJson(o);
        r.data["output"] = QString::fromStdString(o.folder);
        r.data["resumable"] = kept;
    });
    return id;
}

ConsolidateOptions DialogJobs::tracked(const QString& id, ConsolidateOptions o) {
    if (id.isEmpty()) return o;
    o.began = [this, id](const std::string& file) {
        save(id, [&](jobs::Record& r) {
            QJsonArray files = r.data.value("files").toArray();
            const QString name = QString::fromStdString(file);
            if (!files.contains(name)) files.append(name);
            r.data["files"] = files;
        });
    };
    return o;
}

void DialogJobs::setProgress(const QString& id, double fraction) {
    if (id.isEmpty()) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    {
        std::lock_guard l(m_);
        const auto it = records_.find(id);
        if (it == records_.end() || fraction <= it->second.progress) return;
        qint64& last = written_[id];
        if (now - last < 1000) return;
        last = now;
    }
    save(id, [&](jobs::Record& r) { r.progress = std::clamp(fraction, 0.0, 1.0); });
}

void DialogJobs::end(const QString& id, bool ok, bool cancelled, const QString& error) {
    if (id.isEmpty()) return;
    save(id, [&](jobs::Record& r) {
        r.status = cancelled ? QStringLiteral("cancelled") : ok ? QStringLiteral("done") : QStringLiteral("failed");
        if (ok && !cancelled) r.progress = 1;
        r.error = cancelled ? QString() : error;
        r.pid = 0;
        r.process.clear();
    });
    QFile::remove(jobs::jobFolder(id, folder_) + QStringLiteral("/cancel"));
    // (its project is needed no longer)
    QFile::remove(jobs::jobFolder(id, folder_) + QStringLiteral("/project.json"));
    std::lock_guard l(m_);
    records_.erase(id);
    written_.erase(id);
}

bool DialogJobs::cancelAsked(const QString& id) const {
    return !id.isEmpty() && QFileInfo::exists(jobs::jobFolder(id, folder_) + QStringLiteral("/cancel"));
}

int DialogJobs::resume() {
    if (folder_.isEmpty()) return 0;
    int taken = 0;
    for (jobs::Record r : jobs::all(kOwner, folder_)) {
        // (one running here is a dialog's own; one another Montage runs is its)
        if (r.status != QLatin1String("interrupted") || jobs::runningHere(r) || jobs::runningElsewhere(r, folder_)) continue;
        const QString project = jobs::jobFolder(r.id, folder_) + QStringLiteral("/project.json");
        auto giveUp = [&](const QString& why) {
            r.status = QStringLiteral("failed");
            r.error = why;
            r.pid = 0;
            r.process.clear();
            jobs::save(r, folder_);
            QFile::remove(project);
        };
        if (r.kind != kConsolidate || !r.data.value("resumable").toBool()) {
            giveUp(tr("Montage stopped while this ran"));
            continue;
        }
        const int tries = r.data.value("tries").toInt();
        if (tries >= kTries) {
            giveUp(tr("Montage stopped %n time(s) while this ran: it was not taken up again", nullptr, tries + 1));
            continue;
        }
        auto run = std::make_unique<Run>();
        auto loaded = std::make_shared<Project>();
        QFile pf(project);
        std::string err;
        if (!pf.open(QIODevice::ReadOnly) || !projectFromJson(pf.readAll().toStdString(), *loaded, &err)) {
            giveUp(tr("The project kept with it could not be read"));
            continue;
        }
        pf.close();
        run->id = r.id;
        run->project = loaded;
        run->options = optionsFromJson(r.data.value("options").toObject());
        // (the files the run before had begun are this job's own: they are written again, not beside themselves)
        for (const QJsonValue& v : r.data.value("files").toArray()) run->options.replace.push_back(v.toString().toStdString());
        // Held by this Montage from now on (unless another one, started with this, took it meanwhile).
        if (jobs::Record now; !jobs::load(r.id, now, folder_) || now.status != QLatin1String("interrupted")) continue;
        r.status = QStringLiteral("running");
        r.progress = 0;
        r.error.clear();
        r.pid = jobs::thisProcess();
        r.process = jobs::thisProcessToken();
        r.data["tries"] = tries + 1;
        {
            std::lock_guard l(m_);
            if (!jobs::save(r, folder_)) continue;
            records_[r.id] = r;
        }
        run->options = tracked(r.id, run->options);
        waiting_.push_back(std::move(run));
        ++taken;
    }
    if (taken && !run_) QTimer::singleShot(0, this, &DialogJobs::startNext);
    return taken;
}

void DialogJobs::startNext() {
    if (run_ || waiting_.empty()) return;
    run_ = std::move(waiting_.front());
    waiting_.pop_front();
    QString title;
    {
        std::lock_guard l(m_);
        if (const auto it = records_.find(run_->id); it != records_.end()) title = it->second.title;
    }
    emit resumed(title);
    connect(&run_->watcher, &QFutureWatcher<bool>::finished, this, &DialogJobs::runEnded);
    // (everything the thread touches is shared with it, so it may outlive this object's wait in suspend())
    run_->watcher.setFuture(QtConcurrent::run([project = run_->project, options = run_->options, result = run_->result, error = run_->error,
                                               done = run_->done, cancel = run_->cancel] {
        return consolidateProject(*project, options, result.get(), [done](double f) { *done = f; }, cancel.get(), error.get());
    }));
    poll_.start();
}

void DialogJobs::runEnded() {
    if (!run_) return;
    poll_.stop();
    const std::unique_ptr<Run> run = std::move(run_);
    const bool cancelled = run->cancel->load(), ok = run->watcher.result() && !cancelled;
    QString title;
    {
        std::lock_guard l(m_);
        if (const auto it = records_.find(run->id); it != records_.end()) title = it->second.title;
    }
    const QString error = QString::fromStdString(*run->error);
    end(run->id, ok, cancelled, error);
    QString text;
    if (ok) {
        text = tr("Taken up again after Montage stopped: the project was copied to %1").arg(QString::fromStdString(run->result->projectPath));
        if (!run->result->missing.empty()) text += tr(", %n file(s) missing", nullptr, int(run->result->missing.size()));
    } else if (cancelled) {
        text = tr("Stopped: %1").arg(title);
    } else {
        text = tr("Taken up again after Montage stopped, but it failed: %1").arg(error);
    }
    emit finished(run->id, ok, text);
    startNext();
}

void DialogJobs::suspend() {
    poll_.stop();
    // (what runs now stops where it is and stays "running" in the store, as after a crash, without counting as a try)
    auto leave = [this](const QString& id) {
        save(id, [](jobs::Record& r) { r.data["tries"] = std::max(0, r.data.value("tries").toInt() - 1); });
        std::lock_guard l(m_);
        records_.erase(id);
        written_.erase(id);
    };
    if (run_) {
        QObject::disconnect(&run_->watcher, nullptr, this, nullptr);
        *run_->cancel = true;
        run_->watcher.waitForFinished();
        leave(run_->id);
        run_.reset();
    }
    for (const auto& w : waiting_) leave(w->id);
    waiting_.clear();
}

}  // namespace montage
