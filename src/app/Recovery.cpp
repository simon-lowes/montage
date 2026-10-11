#include "Recovery.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

#include "EditorState.h"
#include "audio/Plugins.h"
#include "core/ProjectIO.h"

namespace montage {

namespace {
const char* kRecoveryName = "recovery.montage";
const char* kInfoName = "session.json";

QString projectNameFor(const EditorState* state) {
    if (!state->filePath().isEmpty()) return QFileInfo(state->filePath()).completeBaseName();
    const std::string& name = state->project().name;
    return name.empty() ? QStringLiteral("Untitled") : QString::fromStdString(name);
}
}  // namespace

RecoveryManager::RecoveryManager(EditorState* state, const QString& baseDir, QObject* parent)
    : QObject(parent), state_(state), base_(baseDir) {
    if (base_.isEmpty()) base_ = qEnvironmentVariable("MONTAGE_RECOVERY_DIR");
    if (base_.isEmpty()) base_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/recovery";
    QDir().mkpath(base_ + "/sessions");
    // Live save shortly after editing stops, so a crash loses at most a second or so.
    live_.setSingleShot(true);
    live_.setInterval(1000);
    connect(&live_, &QTimer::timeout, this, &RecoveryManager::saveNow);
    connect(state_, &EditorState::historyChanged, this, [this] {
        if (!lock_) return;
        dirtySinceSnapshot_ = true;
        live_.start();
    });
    connect(state_, &EditorState::fileStateChanged, this, [this] {
        if (lock_) live_.start();
    });
}

RecoveryManager::~RecoveryManager() {
    // Destructors only run on a normal shutdown.
    endSession();
}

std::vector<RecoveryManager::Session> RecoveryManager::crashedSessions() {
    std::vector<Session> out;
    const QFileInfoList dirs = QDir(base_ + "/sessions").entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& d : dirs) {
        const QString dir = d.absoluteFilePath();
        if (dir == sessionDir_) continue;
        QLockFile lock(dir + "/lock");
        // Only a dead owner makes a lock stale; a long-running session never does.
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0)) continue;  // another editor is using it
        Session s;
        s.dir = dir;
        QFile f(dir + "/" + kInfoName);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            s.projectPath = o.value("project").toString();
            s.projectName = o.value("name").toString();
            s.lastSave = QDateTime::fromString(o.value("saved").toString(), Qt::ISODate);
            s.pluginsUsed = o.value("pluginsUsed").toBool();
            if (o.value("modified").toBool() && QFileInfo::exists(dir + "/" + kRecoveryName)) s.recoveryFile = dir + "/" + kRecoveryName;
        }
        lock.unlock();
        if (s.lastSave.isNull()) s.lastSave = d.lastModified();
        if (s.recoveryFile.isEmpty() && !s.pluginsUsed) {
            QDir(dir).removeRecursively();  // crashed with nothing to recover or report
            continue;
        }
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const Session& a, const Session& b) { return a.lastSave > b.lastSave; });
    return out;
}

bool RecoveryManager::recover(const Session& s, QString* error) {
    if (s.recoveryFile.isEmpty()) {
        if (error) *error = tr("There are no unsaved changes to recover.");
        return false;
    }
    if (!state_->recover(s.recoveryFile, s.projectPath, error)) return false;
    discard(s);
    return true;
}

void RecoveryManager::discard(const Session& s) { QDir(s.dir).removeRecursively(); }

void RecoveryManager::beginSession() {
    if (lock_) return;
    sessionDir_ = base_ + "/sessions/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(sessionDir_);
    lock_ = std::make_unique<QLockFile>(sessionDir_ + "/lock");
    lock_->setStaleLockTime(0);
    lock_->tryLock(0);
    lastSnapshot_ = QDateTime::currentDateTimeUtc();
    writeSessionInfo();
}

void RecoveryManager::endSession() {
    live_.stop();
    if (!lock_) return;
    lock_->unlock();
    lock_.reset();
    QDir(sessionDir_).removeRecursively();
    sessionDir_.clear();
}

void RecoveryManager::abandonSession() {
    live_.stop();
    if (!lock_) return;
    lock_->unlock();
    lock_.reset();
    sessionDir_.clear();
}

void RecoveryManager::saveNow() {
    live_.stop();
    if (!lock_) return;
    const QString file = sessionDir_ + "/" + kRecoveryName;
    if (state_->isModified()) saveProject(state_->project(), file.toStdString());
    else QFile::remove(file);
    writeSessionInfo();
    if (dirtySinceSnapshot_ && lastSnapshot_.msecsTo(QDateTime::currentDateTimeUtc()) >= snapshotInterval_) takeSnapshot();
}

void RecoveryManager::writeSessionInfo() {
    QSaveFile f(sessionDir_ + "/" + kInfoName);
    if (!f.open(QIODevice::WriteOnly)) return;
    const QJsonObject o{{"project", state_->filePath()},
                        {"name", projectNameFor(state_)},
                        {"modified", state_->isModified()},
                        {"saved", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                        {"pluginsUsed", plugins::instancesCreated() > 0},
                        {"pid", QCoreApplication::applicationPid()}};
    f.write(QJsonDocument(o).toJson());
    f.commit();
}

QString RecoveryManager::snapshotDir(const QString& projectName) const {
    QString safe = projectName;
    safe.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    return base_ + "/snapshots/" + (safe.isEmpty() ? QStringLiteral("Untitled") : safe);
}

QStringList RecoveryManager::snapshots(const QString& projectName) const {
    QDir dir(snapshotDir(projectName));
    QStringList files = dir.entryList({"*.montage"}, QDir::Files, QDir::Name | QDir::Reversed);
    for (QString& f : files) f = dir.filePath(f);
    return files;
}

void RecoveryManager::takeSnapshot() {
    const QString name = projectNameFor(state_);
    const QString dir = snapshotDir(name);
    QDir().mkpath(dir);
    const QDateTime now = QDateTime::currentDateTime();
    const QString file = dir + "/" + name + " " + now.toString("yyyy-MM-dd HH-mm-ss") + ".montage";
    lastSnapshot_ = now.toUTC();
    dirtySinceSnapshot_ = false;
    if (!QFileInfo::exists(file)) saveProject(state_->project(), file.toStdString());
    const QStringList all = snapshots(name);
    for (int i = maxSnapshots_; i < all.size(); ++i) QFile::remove(all[i]);
}

}  // namespace montage
