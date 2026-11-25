#include "DialogJobs.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLockFile>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QThread>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent>
#include <algorithm>

#include "EditorState.h"
#include "core/EditOps.h"
#include "core/ProjectIO.h"

namespace montage {

namespace {

const QString kOwner = QStringLiteral("app");
const QString kConsolidate = QStringLiteral("consolidate");
constexpr int kKept = 100;  // finished jobs kept in the store
constexpr int kTries = 2;   // how often one cut short is taken up again

// The threads tasks run on: a pool of their own, so a task paused (which waits on its thread) holds none of the
// threads the rest of Montage shares (peaking, thumbnails, waveforms). Never deleted: a task's thread may outlive
// this object, as suspend() lets it.
QThreadPool* taskThreads() {
    static QThreadPool* pool = [] {
        auto* p = new QThreadPool;
        p->setMaxThreadCount(64);
        return p;
    }();
    return pool;
}

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

std::mutex& kindsLock() {
    static std::mutex m;
    return m;
}
std::map<QString, TaskKind>& kinds() {
    static std::map<QString, TaskKind> k;
    return k;
}
bool kindOf(const QString& name, TaskKind* out) {
    std::lock_guard l(kindsLock());
    const auto it = kinds().find(name);
    if (it == kinds().end()) return false;
    if (out) *out = it->second;
    return true;
}

QString partFile(const QString& folder, const QString& name) {
    QString safe;
    for (const QChar c : name) safe += c.isLetterOrNumber() || c == QLatin1Char('-') ? c : QLatin1Char('_');
    return folder + QStringLiteral("/part-") + safe + QStringLiteral(".json");
}

bool writeJson(const QString& path, const QJsonObject& o) {
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(QJsonDocument(o).toJson(QJsonDocument::Compact)) >= 0 && f.commit();
}

bool readJson(const QString& path, QJsonObject* o) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QJsonParseError err;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !d.isObject()) return false;
    *o = d.object();
    return true;
}

// A clip and the track it is on, in any sequence (`sequence` 0) or in that one.
struct Found {
    const Clip* clip = nullptr;
    Id track = 0;
};
Found findClip(const Project& p, Id sequence, Id clip) {
    for (const Sequence& s : p.sequences) {
        if (sequence && s.id != sequence) continue;
        for (const std::vector<Track>* tracks : {&s.videoTracks, &s.audioTracks})
            for (const Track& t : *tracks)
                for (const Clip& c : t.clips)
                    if (c.id == clip) return {&c, t.id};
    }
    return {};
}

}  // namespace

bool TaskRun::part(const QString& name, QJsonValue* value) const {
    QJsonObject o;
    if (folder.isEmpty() || !readJson(partFile(folder, name), &o) || !o.contains(QStringLiteral("v"))) return false;
    if (value) *value = o.value(QStringLiteral("v"));
    return true;
}

void TaskRun::keepPart(const QString& name, const QJsonValue& value) const {
    if (!folder.isEmpty()) writeJson(partFile(folder, name), QJsonObject{{QStringLiteral("v"), value}});
}

struct DialogJobs::Shared {
    std::shared_ptr<const Project> project;
    QJsonObject input;
    QString folder;
    std::atomic<double> done{0};
    std::atomic<bool> cancel{false}, paused{false};
    double pauseAt = -1;  // pauses by itself once this far (MONTAGE_JOBS_PAUSE_AT: the tests' way to stop one part-way)
    QJsonObject result;
    std::string error;
};

void registerDialogTaskKinds();  // (DialogTasks.cpp)

DialogJobs::DialogJobs(QObject* parent, const QString& folder) : QObject(parent), folder_(folder) {
    registerDialogTaskKinds();
    poll_.setInterval(250);
    connect(&poll_, &QTimer::timeout, this, [this] {
        if (run_) {
            setProgress(run_->id, run_->done->load());
            if (cancelAsked(run_->id)) *run_->cancel = true;
        }
        bool any = false;
        for (const auto& t : tasks_) {
            if (!t->started) continue;
            any = true;
            setProgress(t->id, t->shared->done.load());
            if (!folder_.isEmpty() && cancelAsked(t->id)) t->shared->cancel = true;
            // (one that waits says so in the store, with how far it is)
            if (const bool paused = t->shared->paused.load(); paused != t->saidPaused) {
                t->saidPaused = paused;
                save(t->id, [&](jobs::Record& r) {
                    r.step = paused ? QStringLiteral("paused") : QString();
                    r.progress = std::clamp(t->shared->done.load(), r.progress, 1.0);
                });
            }
        }
        if (any) emit tasksChanged();
        if (!run_ && !any) poll_.stop();
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
        const bool isTask = r.data.value("task").toBool() && hasKind(r.kind);
        if ((r.kind != kConsolidate && !isTask) || !r.data.value("resumable").toBool()) {
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
        // Held by this Montage from now on, unless another one, started with this, took it meanwhile: looked at again
        // and written under a lock on the job's folder, so of two that find it interrupted only one takes it.
        QLockFile taking(jobs::jobFolder(r.id, folder_) + QStringLiteral("/resume.lock"));
        if (!taking.tryLock(0)) continue;
        if (jobs::Record now; !jobs::load(r.id, now, folder_) || now.status != QLatin1String("interrupted")) continue;
        r.status = QStringLiteral("running");
        r.progress = 0;
        r.step.clear();
        r.error.clear();
        r.pid = jobs::thisProcess();
        r.process = jobs::thisProcessToken();
        r.data["tries"] = tries + 1;
        {
            std::lock_guard l(m_);
            if (!jobs::save(r, folder_)) continue;
            records_[r.id] = r;
        }
        ++taken;
        if (isTask) {
            // A task goes on in its own folder: the parts it had finished are passed over.
            auto t = std::make_unique<Task>();
            t->id = r.id;
            t->kind = r.kind;
            t->title = r.title;
            t->input = r.data.value("input").toObject();
            t->project = markOf(r.data);
            t->resumed = true;
            t->shared = std::make_shared<Shared>();
            t->shared->project = loaded;
            t->shared->input = t->input;
            t->shared->folder = jobs::jobFolder(r.id, folder_);
            tasks_.push_back(std::move(t));
            continue;
        }
        run->options = tracked(r.id, run->options);
        waiting_.push_back(std::move(run));
    }
    if (!waiting_.empty() && !run_) QTimer::singleShot(0, this, &DialogJobs::startNext);
    if (taken) QTimer::singleShot(0, this, &DialogJobs::startWaitingTasks);
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
        if (run->result->passedOver > 0) text += tr(", %n file(s) already whole left as they were", nullptr, run->result->passedOver);
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
    // Tasks stop where they are too, and are taken up from there by the next Montage.
    for (const auto& t : tasks_) {
        QObject::disconnect(&t->watcher, nullptr, this, nullptr);
        t->shared->cancel = true;
    }
    std::deque<std::unique_ptr<Task>> stopped = std::move(tasks_);
    tasks_.clear();
    for (const auto& t : stopped) {
        if (t->started) t->watcher.waitForFinished();
        leave(t->id);
    }
    for (const auto& t : stopped) emit taskEnded(t->id);  // (whoever watches one stops waiting)
}

// --- Tasks ---

void DialogJobs::registerKind(const QString& kind, TaskKind k) {
    std::lock_guard l(kindsLock());
    kinds()[kind] = std::move(k);
}

bool DialogJobs::hasKind(const QString& kind) { return kindOf(kind, nullptr); }

DialogJobs* DialogJobs::of(QWidget* w) {
    for (; w; w = w->parentWidget())
        if (auto* jobs = w->findChild<DialogJobs*>(QString(), Qt::FindDirectChildrenOnly)) return jobs;
    static QPointer<DialogJobs> shared;
    if (!shared) shared = new DialogJobs(QCoreApplication::instance());
    return shared;
}

void DialogJobs::setState(EditorState* state) {
    state_ = state;
    if (!state) return;
    // A duck kept up to date that could not follow an edit (sound not heard yet): the sound is listened to in the
    // background, once, and the ducks are made again.
    connect(state, &EditorState::ducksUnheard, this, [this] {
        for (const auto& t : tasks_)
            if (t->kind == QLatin1String("auto-duck") && t->input.value("refresh").toBool()) return;
        const Sequence* s = state_ ? state_->sequence() : nullptr;
        if (!s) return;
        start(state_, QStringLiteral("auto-duck"), tr("Listening for dialogue…"),
              QJsonObject{{"sequence", QString::number(s->id)}, {"refresh", true}, {"revision", QString::number(state_->history().revision())}});
    }, Qt::QueuedConnection);
}

EditorState* DialogJobs::stateFor(const Task& t) const { return t.state ? t.state.data() : state_.data(); }

DialogJobs::Task* DialogJobs::task(const QString& id) const {
    for (const auto& t : tasks_)
        if (t->id == id) return t.get();
    return nullptr;
}

QString DialogJobs::start(EditorState* state, const QString& kind, const QString& title, const QJsonObject& input) {
    return startOn(state ? state->project() : Project(), state, kind, title, input);
}

DialogJobs::ProjectMark DialogJobs::markOf(const EditorState* state) {
    ProjectMark m;
    if (!state) return m;
    m.known = m.thisSession = true;
    m.path = state->filePath();
    m.generation = state->projectGeneration();
    return m;
}

DialogJobs::ProjectMark DialogJobs::markOf(const QJsonObject& data) {
    ProjectMark m;
    m.known = data.contains(QStringLiteral("project"));
    m.path = data.value(QStringLiteral("project")).toString();
    return m;
}

QString DialogJobs::notTheProject(const ProjectMark& mark, const EditorState* state) const {
    if (!mark.known) return {};
    if (!state) return tr("no project is open");
    if (mark.thisSession && state->projectGeneration() == mark.generation) return {};
    const QString now = state->filePath();
    if (!mark.path.isEmpty() && !now.isEmpty()) {
        const QFileInfo a(mark.path), b(now);
        if (a.exists() && b.exists() ? a.canonicalFilePath() == b.canonicalFilePath() : QDir::cleanPath(a.absoluteFilePath()) == QDir::cleanPath(b.absoluteFilePath()))
            return {};
    }
    // (an untitled project, taken up after Montage stopped: an untitled one, as a recovered copy opens)
    if (!mark.thisSession && mark.path.isEmpty() && now.isEmpty()) return {};
    return mark.path.isEmpty() ? tr("another project is open (it was begun on an untitled one)")
                               : tr("another project is open (it was begun on %1)").arg(QFileInfo(mark.path).fileName());
}

QString DialogJobs::startOn(const Project& project, EditorState* state, const QString& kind, const QString& title, const QJsonObject& input) {
    if (!hasKind(kind)) return {};
    auto t = std::make_unique<Task>();
    t->kind = kind;
    t->title = title;
    t->input = input;
    t->state = state;
    t->project = markOf(state ? state : state_.data());
    t->shared = std::make_shared<Shared>();
    t->shared->project = std::make_shared<const Project>(project);
    t->shared->input = input;
    if (const QByteArray at = qgetenv("MONTAGE_JOBS_PAUSE_AT"); !at.isEmpty()) t->shared->pauseAt = at.toDouble();
    t->id = begin(kind, title);
    if (t->id.isEmpty()) {
        t->id = QStringLiteral("mem-%1").arg(++memoryIds_);  // (nothing is kept: it lives here only)
    } else {
        // The project as it is now and what was asked, kept with the job: taken up again, it works on these.
        t->shared->folder = jobs::jobFolder(t->id, folder_);
        QSaveFile f(t->shared->folder + QStringLiteral("/project.json"));
        const bool kept = f.open(QIODevice::WriteOnly) && f.write(QByteArray::fromStdString(projectToJson(project))) >= 0 && f.commit();
        save(t->id, [&](jobs::Record& r) {
            r.data["task"] = true;
            r.data["input"] = input;
            r.data["resumable"] = kept;
            if (t->project.known) r.data["project"] = t->project.path;
        });
    }
    const QString id = t->id;
    marks_[id] = t->project;
    tasks_.push_back(std::move(t));
    startTask(*tasks_.back());
    emit tasksChanged();
    return id;
}

void DialogJobs::startTask(Task& t) {
    TaskKind kind;
    if (t.started || !kindOf(t.kind, &kind)) return;
    t.started = true;
    const QString id = t.id;
    connect(&t.watcher, &QFutureWatcher<bool>::finished, this, [this, id] { taskFinished(id); });
    // (everything the thread touches is shared with it, so it may outlive this object's wait in suspend())
    t.watcher.setFuture(QtConcurrent::run(taskThreads(), [sh = t.shared, run = kind.run] {
        TaskRun r;
        r.project = sh->project;
        r.input = sh->input;
        r.folder = sh->folder;
        r.cancel = &sh->cancel;
        r.progress = [sh](double f) {
            sh->done = std::clamp(f, 0.0, 1.0);
            if (sh->pauseAt >= 0 && f >= sh->pauseAt) {
                sh->pauseAt = -1;
                sh->paused = true;
            }
            while (sh->paused.load() && !sh->cancel.load()) QThread::msleep(20);
        };
        r.progress(0);
        // (stopped by hand, it ends well only where it made what it was for and stopped only what came after, a
        // package's check, as its result's "stopped" says)
        bool ok = run && !sh->cancel.load() && run(r) && (!sh->cancel.load() || r.result.value("stopped").toBool());
        sh->error = r.error;
        if (ok) {
            sh->result = r.result;
            // (the result is in the job's folder before its record says it is done)
            if (!sh->folder.isEmpty() && !writeJson(sh->folder + QStringLiteral("/result.json"), r.result)) {
                sh->error = "The result could not be kept in the job's folder";
                ok = false;
            }
        }
        return ok;
    }));
    if (!poll_.isActive()) poll_.start();
}

void DialogJobs::startWaitingTasks() {
    // (tasks a dialog starts run at once; those taken up again share the places background jobs have)
    int running = 0;
    for (const auto& t : tasks_) running += t->started ? 1 : 0;
    for (const auto& t : tasks_) {
        if (t->started) continue;
        if (running >= jobs::atOnce()) break;
        emit resumed(t->title);
        startTask(*t);
        ++running;
    }
    emit tasksChanged();
}

QString DialogJobs::movedNote(const Project& then, const Project& now, const QJsonObject& input) {
    const Id sequence = Id(input.value("sequence").toString().toULongLong());
    if (sequence) {
        if (!now.findSequence(sequence)) return tr("its sequence is gone");
        if (now.activeSequence != sequence) return tr("another sequence is open");
    }
    QStringList moved;
    for (const QJsonValue& v : input.value("clips").toArray()) {
        const Id id = Id(v.toString().toULongLong());
        const Found was = findClip(then, sequence, id), is = findClip(now, sequence, id);
        if (!was.clip) continue;
        const QString name = QString::fromStdString(was.clip->name);
        if (!is.clip) {
            moved << tr("“%1” is gone").arg(name);
        } else if (was.clip->start != is.clip->start || was.clip->duration != is.clip->duration || was.clip->sourceIn != is.clip->sourceIn ||
                   was.clip->mediaId != is.clip->mediaId || was.clip->speed != is.clip->speed || was.track != is.track) {
            moved << tr("“%1” was moved").arg(name);
        }
    }
    return moved.join(QStringLiteral(", "));
}

QJsonObject DialogJobs::applyResult(Task& t, const QJsonObject& result, QString* note) {
    TaskKind kind;
    if (!kindOf(t.kind, &kind) || !kind.apply) return {};
    EditorState* state = stateFor(t);
    // Never to another project than the one it was begun on (one opened since, whose ids may be the same).
    if (const QString other = notTheProject(t.project, state); !other.isEmpty()) {
        if (note) *note = other;
        return {};
    }
    if (!t.watched) {
        // Nobody watched, so the project may have changed under it: what it was made for must still be where it was.
        QString moved = state ? movedNote(*t.shared->project, state->project(), t.input) : tr("no project is open");
        if (moved.isEmpty() && kind.review) moved = tr("it waits to be looked over");
        if (!moved.isEmpty()) {
            if (note) *note = moved;
            return {};
        }
    }
    return state ? kind.apply(state, t.input, result) : QJsonObject();
}

void DialogJobs::taskFinished(const QString& id) {
    Task* t = task(id);
    if (!t) return;
    // (a task that ended well is done even when it was stopped as it ended: what it made is kept)
    const bool ok = t->watcher.result(), cancelled = !ok && t->shared->cancel.load();
    TaskOutcome out;
    out.id = id;
    out.how = cancelled ? TaskOutcome::Cancelled : ok ? TaskOutcome::Done : TaskOutcome::Failed;
    out.error = QString::fromStdString(t->shared->error);
    QString text, note;
    if (ok) {
        out.result = t->shared->result;
        const QPointer<DialogJobs> alive(this);
        out.applied = applyResult(*t, out.result, &note);
        // (a result looked over in a dialog of its own, as Auto Mix's is, runs an event loop: Montage may have quit
        // meanwhile, taking this with it)
        if (!alive || !(t = task(id))) return;
        text = out.applied.value("message").toString();
        if (!note.isEmpty()) {
            // Kept, not applied: offered from the jobs list.
            save(id, [&](jobs::Record& r) { r.data["offered"] = note; });
            if (folder_.isEmpty()) offeredHere_[id] = {t->kind, t->title, t->input, out.result};
            text = tr("%1 has finished, but %2: its result was kept and not applied. Apply it from the Render Queue panel's task list.")
                       .arg(t->title, note);
        } else if (text.isEmpty()) {
            text = tr("Finished: %1").arg(t->title);
        }
    } else if (cancelled) {
        text = tr("Stopped: %1").arg(t->title);
    } else {
        text = t->resumed ? tr("Taken up again after Montage stopped, but it failed: %1").arg(out.error) : out.error;
    }
    if (!folder_.isEmpty()) end(id, ok, cancelled, out.error);
    if (TaskKind kind; !ok && kindOf(t->kind, &kind) && kind.discard) kind.discard(t->input, t->shared->folder);
    const bool watched = t->watched;
    const QString title = t->title, kindName = t->kind;
    const QJsonObject input = t->input;
    for (auto it = tasks_.begin(); it != tasks_.end(); ++it)
        if (it->get() == t) {
            // (its watcher is in the middle of telling us: it goes once we are back in the event loop)
            QTimer::singleShot(0, this, [dead = std::shared_ptr<Task>(std::move(*it))] {});
            tasks_.erase(it);
            break;
        }
    if (watched) outcomes_[id] = out;
    emit taskEnded(id);
    if (!note.isEmpty()) emit offered(id, title, note);
    if (!watched && !text.isEmpty()) emit finished(id, ok, text);
    if (!watched && ok && note.isEmpty()) emit taskDone(id, kindName, input, out.result);
    emit tasksChanged();
    startWaitingTasks();
}

TaskOutcome DialogJobs::watch(const QString& id, QWidget* parent) {
    TaskOutcome out;
    out.id = id;
    Task* t = task(id);
    if (t) {
        t->watched = true;
        QDialog dlg(parent);
        dlg.setObjectName(QStringLiteral("jobProgress"));
        dlg.setWindowTitle(tr("Working"));
        dlg.setWindowModality(Qt::WindowModal);
        auto* lay = new QVBoxLayout(&dlg);
        auto* label = new QLabel(t->title, &dlg);
        label->setWordWrap(true);
        auto* bar = new QProgressBar(&dlg);
        bar->setRange(0, 1000);
        bar->setMinimumWidth(320);
        auto* background = new QPushButton(tr("Run in Background"), &dlg);
        background->setObjectName(QStringLiteral("jobBackground"));
        background->setToolTip(tr("Close this and carry on editing: the job goes on, is listed with the render queue, and its result is "
                                  "applied when it ends"));
        auto* cancel = new QPushButton(tr("Cancel"), &dlg);
        cancel->setObjectName(QStringLiteral("jobCancel"));
        auto* row = new QHBoxLayout;
        row->addStretch(1);
        row->addWidget(background);
        row->addWidget(cancel);
        lay->addWidget(label);
        lay->addWidget(bar);
        lay->addLayout(row);
        QEventLoop loop;
        bool left = false;
        const auto ended = connect(this, &DialogJobs::taskEnded, &loop, [&](const QString& which) {
            if (which == id) loop.quit();
        });
        connect(background, &QPushButton::clicked, &loop, [&] {
            left = true;
            if (Task* x = task(id)) x->watched = false;
            loop.quit();
        });
        connect(cancel, &QPushButton::clicked, &loop, [&] { stop(id); });
        connect(&dlg, &QDialog::rejected, &loop, [&] { stop(id); });  // (closed: as Cancel)
        QTimer tick;
        connect(&tick, &QTimer::timeout, &loop, [&] {
            if (const Task* x = task(id)) bar->setValue(int(x->shared->done.load() * 1000));
        });
        tick.start(100);
        QTimer::singleShot(400, &dlg, [&dlg] { dlg.show(); });  // (a short job shows nothing)
        loop.exec();
        disconnect(ended);
        dlg.hide();
        if (left) {
            out.how = TaskOutcome::Background;
            return out;
        }
    }
    if (const auto it = outcomes_.find(id); it != outcomes_.end()) {
        out = it->second;
        outcomes_.erase(it);
    } else {
        out.how = t ? TaskOutcome::Cancelled : TaskOutcome::Failed;  // (stopped with Montage, or never begun)
    }
    return out;
}

std::vector<DialogJobs::TaskInfo> DialogJobs::tasks() const {
    std::vector<TaskInfo> out;
    for (const auto& t : tasks_)
        out.push_back({t->id, t->kind, t->title, t->shared->done.load(), t->started, t->shared->paused.load(), {}});
    return out;
}

void DialogJobs::setPaused(const QString& id, bool paused) {
    if (Task* t = task(id)) t->shared->paused = paused;
    emit tasksChanged();
}

void DialogJobs::stop(const QString& id) {
    Task* t = task(id);
    if (!t) return;
    t->shared->cancel = true;
    if (t->started) return;  // (it ends by itself)
    // One still waiting its turn is set aside at once.
    if (!folder_.isEmpty()) end(id, false, true);
    for (auto it = tasks_.begin(); it != tasks_.end(); ++it)
        if (it->get() == t) {
            tasks_.erase(it);
            break;
        }
    emit taskEnded(id);
    emit tasksChanged();
}

bool DialogJobs::moveUp(const QString& id) {
    for (size_t i = 1; i < tasks_.size(); ++i)
        if (tasks_[i]->id == id && !tasks_[i]->started && !tasks_[i - 1]->started) {
            std::swap(tasks_[i], tasks_[i - 1]);
            emit tasksChanged();
            return true;
        }
    return false;
}

QString DialogJobs::offeredNote(const QString& id) const {
    if (folder_.isEmpty()) return offeredHere_.count(id) ? tr("the project changed while it ran") : QString();
    jobs::Record r;
    return jobs::load(id, r, folder_) ? r.data.value("offered").toString() : QString();
}

bool DialogJobs::applyOffered(const QString& id, QString* said) {
    QJsonObject input, result;
    QString kindName;
    jobs::Record r;
    if (folder_.isEmpty()) {
        const auto it = offeredHere_.find(id);
        if (it == offeredHere_.end()) return false;
        kindName = it->second.kind, input = it->second.input, result = it->second.result;
    } else {
        if (!jobs::load(id, r, folder_) || r.data.value("offered").toString().isEmpty() ||
            !readJson(jobs::jobFolder(id, folder_) + QStringLiteral("/result.json"), &result))
            return false;
        input = r.data.value("input").toObject();
        kindName = r.kind;
    }
    TaskKind kind;
    if (!state_ || !kindOf(kindName, &kind) || !kind.apply) return false;
    // (to the project it was begun on only: in this session, that one, else the file it was in)
    const auto mark = marks_.find(id);
    if (const QString other = notTheProject(mark != marks_.end() ? mark->second : markOf(r.data), state_); !other.isEmpty()) {
        if (said) *said = tr("It was not applied: %1.").arg(other);
        return false;
    }
    const QPointer<DialogJobs> alive(this);
    const QJsonObject applied = kind.apply(state_, input, result);
    if (said) *said = applied.value("message").toString();
    if (!alive || applied.value("failed").toBool()) return false;  // (Montage may have quit in a dialog the result is looked over in)
    if (folder_.isEmpty()) {
        offeredHere_.erase(id);
    } else {
        r.data.remove("offered");
        r.data["applied"] = true;
        jobs::save(r, folder_);
    }
    emit tasksChanged();
    return true;
}

std::vector<DialogJobs::TaskInfo> DialogJobs::offeredTasks() const {
    std::vector<TaskInfo> out;
    if (folder_.isEmpty()) {
        for (const auto& [id, o] : offeredHere_) out.push_back({id, o.kind, o.title, 1.0, false, false, tr("the project changed while it ran")});
        return out;
    }
    std::vector<jobs::Record> all = jobs::all(kOwner, folder_);
    std::sort(all.begin(), all.end(), [](const jobs::Record& a, const jobs::Record& b) { return a.order > b.order; });
    for (const jobs::Record& r : all)
        if (const QString note = r.data.value("offered").toString(); !note.isEmpty() && r.status == QLatin1String("done"))
            out.push_back({r.id, r.kind, r.title, 1.0, false, false, note});
    return out;
}

void DialogJobs::discardOffered(const QString& id) {
    if (folder_.isEmpty()) {
        offeredHere_.erase(id);
    } else if (jobs::Record r; jobs::load(id, r, folder_) && r.data.contains("offered")) {
        r.data.remove("offered");
        jobs::save(r, folder_);
    }
    emit tasksChanged();
}

bool DialogJobs::latestResult(const QString& kind, const std::function<bool(const QJsonObject&)>& matches, QJsonObject* result,
                              QJsonObject* input) const {
    if (folder_.isEmpty()) return false;
    std::vector<jobs::Record> all = jobs::all(kOwner, folder_);
    std::sort(all.begin(), all.end(), [](const jobs::Record& a, const jobs::Record& b) { return a.order > b.order; });
    for (const jobs::Record& r : all) {
        if (r.kind != kind || r.status != QLatin1String("done")) continue;
        const QJsonObject in = r.data.value("input").toObject();
        if (matches && !matches(in)) continue;
        QJsonObject found;
        if (!readJson(jobs::jobFolder(r.id, folder_) + QStringLiteral("/result.json"), &found)) continue;
        if (result) *result = found;
        if (input) *input = in;
        return true;
    }
    return false;
}

}  // namespace montage
