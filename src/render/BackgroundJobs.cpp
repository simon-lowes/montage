#include "BackgroundJobs.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QThread>
#include <QSettings>
#include <QStandardPaths>
#include <QFileInfo>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "core/ProjectLock.h"

namespace montage::jobs {

namespace {

QString folderOr(const QString& folder) { return folder.isEmpty() ? storeFolder() : folder; }

// While a process keeps jobs in a store, it touches "alive/<its token>" there every few seconds (a thread of its own,
// from its first save to its end), so a process that has the pid of one that ran a job, after a restart, is not taken
// for it: a job is held elsewhere only while its process lives and its file is fresh.
constexpr int kBeatSeconds = 5, kStaleSeconds = 30;
QString aliveFile(const QString& folder, const QString& token) { return folder + QStringLiteral("/alive/") + token; }

class Heartbeat {
public:
    ~Heartbeat() {
        {
            std::lock_guard l(m_);
            stop_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
        for (const QString& f : folders_) QFile::remove(aliveFile(f, token_));
    }
    void beatIn(const QString& folder) {
        std::lock_guard l(m_);
        if (stop_ || std::find(folders_.begin(), folders_.end(), folder) != folders_.end()) return;
        if (token_.isEmpty()) token_ = thisProcessToken();
        folders_.push_back(folder);
        touch(folder);
        if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    }

private:
    void touch(const QString& folder) const {
        QDir().mkpath(folder + QStringLiteral("/alive"));
        QFile f(aliveFile(folder, token_));
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QByteArray::number(thisProcess()));
    }
    void run() {
        std::unique_lock l(m_);
        while (!wake_.wait_for(l, std::chrono::seconds(kBeatSeconds), [this] { return stop_; }))
            for (const QString& f : folders_) touch(f);
    }
    std::mutex m_;
    std::condition_variable wake_;
    std::vector<QString> folders_;
    std::thread thread_;
    QString token_;  // (kept: this process's token may be gone by the time this is)
    bool stop_ = false;
};

Heartbeat& heartbeat() {
    static Heartbeat h;
    return h;
}

bool beating(const QString& folder, const QString& token) {
    if (token.isEmpty() || token.contains(QLatin1Char('/')) || token.contains(QLatin1Char('\\')) || token.startsWith(QLatin1Char('.'))) return false;
    const QFileInfo f(aliveFile(folderOr(folder), token));
    return f.exists() && f.lastModified().secsTo(QDateTime::currentDateTime()) < kStaleSeconds;
}

QJsonObject toJson(const Record& r) {
    return QJsonObject{{"id", r.id},           {"owner", r.owner},     {"kind", r.kind},       {"title", r.title},
                       {"status", r.status},   {"progress", r.progress}, {"step", r.step},     {"error", r.error},
                       {"pid", double(r.pid)}, {"process", r.process}, {"created", r.created}, {"updated", r.updated},
                       {"order", r.order},     {"data", r.data},       {"version", 1}};
}

Record fromJson(const QJsonObject& o) {
    Record r;
    r.id = o.value("id").toString();
    r.owner = o.value("owner").toString();
    r.kind = o.value("kind").toString();
    r.title = o.value("title").toString();
    r.status = o.value("status").toString(QStringLiteral("waiting"));
    r.progress = o.value("progress").toDouble();
    r.step = o.value("step").toString();
    r.error = o.value("error").toString();
    r.pid = qint64(o.value("pid").toDouble());
    r.process = o.value("process").toString();
    r.created = o.value("created").toString();
    r.updated = o.value("updated").toString();
    r.order = o.value("order").toInt();
    r.data = o.value("data").toObject();
    return r;
}

}  // namespace

bool Record::finished() const {
    return status == QLatin1String("done") || status == QLatin1String("failed") || status == QLatin1String("cancelled");
}

QString storeFolder() {
    const QString env = qEnvironmentVariable("MONTAGE_JOBS_DIR");
    if (!env.isEmpty()) return env;
    // (the generic folder, not the app's: Montage, montage-cli and its MCP server share it whatever they are called)
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/Montage/jobs");
}

QString jobFolder(const QString& id, const QString& folder) { return folderOr(folder) + QLatin1Char('/') + id; }

bool save(Record r, const QString& folder) {
    if (r.id.isEmpty() || r.id.contains(QLatin1Char('/')) || r.id.contains(QLatin1Char('\\')) || r.id.startsWith(QLatin1Char('.'))) return false;
    const QString dir = jobFolder(r.id, folder);
    if (!QDir().mkpath(dir)) return false;
    if (r.process == thisProcessToken()) heartbeat().beatIn(folderOr(folder));
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    if (r.created.isEmpty()) r.created = now;
    r.updated = now;
    const QByteArray text = QJsonDocument(toJson(r)).toJson(QJsonDocument::Indented);
    // (tried again for up to two seconds when only putting it in place failed: on Windows a record cannot be replaced
    // while another reader has it open, as a job list or a second Montage reading the store does for a moment. A write
    // that fails, a full disk say, is not tried again: the GUI saves its running jobs every second.)
    for (int attempt = 0; attempt < 400; ++attempt) {
        QSaveFile f(dir + QStringLiteral("/job.json"));
        if (!f.open(QIODevice::WriteOnly)) return false;
        if (f.write(text) != text.size()) return false;
        if (f.commit()) return true;
#ifdef Q_OS_WIN
        if (f.error() != QFileDevice::RenameError) return false;
#else
        return false;
#endif
        QThread::msleep(5);
    }
    return false;
}

bool load(const QString& id, Record& r, const QString& folder) {
    QFile f(jobFolder(id, folder) + QStringLiteral("/job.json"));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    r = fromJson(doc.object());
    if (r.id.isEmpty()) r.id = id;
    if (r.status == QLatin1String("running") && !runningHere(r) && !runningElsewhere(r, folder)) r.status = QStringLiteral("interrupted");
    return true;
}

std::vector<Record> all(const QString& owner, const QString& folder) {
    std::vector<Record> out;
    const QDir dir(folderOr(folder));
    for (const QString& id : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (id == QLatin1String("alive")) continue;
        Record r;
        if (load(id, r, folder) && (owner.isEmpty() || r.owner == owner)) out.push_back(std::move(r));
    }
    std::sort(out.begin(), out.end(), [](const Record& a, const Record& b) { return a.owner != b.owner ? a.owner < b.owner : a.order < b.order; });
    return out;
}

bool remove(const QString& id, const QString& folder) {
    if (id.isEmpty() || id.contains(QLatin1Char('/')) || id.contains(QLatin1Char('\\')) || id.startsWith(QLatin1Char('.'))) return false;
    QDir dir(jobFolder(id, folder));
    return !dir.exists() || dir.removeRecursively();
}

int prune(const QString& owner, int keep, const QString& folder) {
    // Folders left without a record (a crash as one was made) or with one that was read but is damaged are never
    // listed: those a day old go too. One whose folder or record cannot be read (no permission, a file another
    // process holds, a read error) is left alone: what could not be read is never taken for damaged.
    int n = 0;
    const QDir dir(folderOr(folder));
    const QDateTime dayAgo = QDateTime::currentDateTimeUtc().addDays(-1);
    for (const QFileInfo& d : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (d.fileName() == QLatin1String("alive") || d.lastModified().toUTC() > dayAgo) continue;
        if (!d.isReadable() || !d.isExecutable()) continue;  // (its contents cannot be seen)
        const QFileInfo record(d.filePath() + QStringLiteral("/job.json"));
        if (record.exists() && record.lastModified().toUTC() > dayAgo) continue;
        if (record.exists()) {
            QFile f(record.filePath());
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QByteArray bytes = f.readAll();
            if (f.error() != QFileDevice::NoError || bytes.size() < f.size()) continue;
            if (QJsonDocument::fromJson(bytes).isObject()) continue;  // (a record: listed, and pruned below if done)
        }
        n += remove(d.fileName(), folder) ? 1 : 0;
    }
    std::vector<Record> done;
    for (Record& r : all(owner, folder))
        if (r.finished() || r.status == QLatin1String("resumed")) done.push_back(std::move(r));
    if (int(done.size()) <= keep) return n;
    std::stable_sort(done.begin(), done.end(), [](const Record& a, const Record& b) { return a.updated < b.updated; });
    for (size_t i = 0; i + size_t(std::max(0, keep)) < done.size(); ++i) n += remove(done[i].id, folder) ? 1 : 0;
    return n;
}

qint64 thisProcess() { return QCoreApplication::applicationPid(); }
QString thisProcessToken() { return QString::fromStdString(currentLockOwner().token); }

bool runningHere(const Record& r) { return r.pid == thisProcess() && r.process == thisProcessToken(); }
bool runningElsewhere(const Record& r, const QString& folder) {
    return !runningHere(r) && r.process != thisProcessToken() && processAlive(r.pid) && beating(folder, r.process);
}

int atOnce() {
    const int n = QSettings(QStringLiteral("Montage"), QStringLiteral("Montage")).value(QStringLiteral("jobs/at_once"), 2).toInt();
    return std::clamp(n, 1, 16);
}

void setAtOnce(int n) { QSettings(QStringLiteral("Montage"), QStringLiteral("Montage")).setValue(QStringLiteral("jobs/at_once"), std::clamp(n, 1, 16)); }

}  // namespace montage::jobs
