#include "ProjectLock.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSysInfo>
#include <QUuid>
#include <mutex>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace montage {

namespace {

std::mutex nameMutex;
std::string userName;

const std::string& processToken() {
    static const std::string token = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    return token;
}

bool processAlive(qint64 pid) {
    if (pid <= 0) return false;
#ifdef Q_OS_WIN
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

QByteArray encode(const LockOwner& o) {
    const QJsonObject j{{"user", QString::fromStdString(o.user)},
                        {"host", QString::fromStdString(o.host)},
                        {"app", QString::fromStdString(o.app)},
                        {"pid", double(o.pid)},
                        {"token", QString::fromStdString(o.token)},
                        {"since", o.since.toString(Qt::ISODateWithMs)},
                        {"heartbeat", o.heartbeat.toString(Qt::ISODateWithMs)}};
    return QJsonDocument(j).toJson(QJsonDocument::Indented);
}

bool decode(const QByteArray& bytes, LockOwner& o) {
    const QJsonObject j = QJsonDocument::fromJson(bytes).object();
    if (j.isEmpty()) return false;
    o.user = j.value("user").toString().toStdString();
    o.host = j.value("host").toString().toStdString();
    o.app = j.value("app").toString().toStdString();
    o.pid = qint64(j.value("pid").toDouble());
    o.token = j.value("token").toString().toStdString();
    o.since = QDateTime::fromString(j.value("since").toString(), Qt::ISODateWithMs);
    o.heartbeat = QDateTime::fromString(j.value("heartbeat").toString(), Qt::ISODateWithMs);
    return true;
}

bool sameHost(const LockOwner& a, const LockOwner& b) { return QString::fromStdString(a.host).compare(QString::fromStdString(b.host), Qt::CaseInsensitive) == 0; }

// This process's lock: by its token (older locks without one: by machine and process).
bool isMine(const LockOwner& o, const LockOwner& me) { return o.token.empty() ? sameHost(o, me) && o.pid == me.pid : o.token == me.token; }

}  // namespace

std::string LockOwner::describe() const {
    const std::string who = user.empty() ? std::string("Someone") : user;
    return host.empty() ? who : who + " on " + host;
}

std::string lockPathFor(const std::string& project) {
    const QFileInfo fi(QString::fromStdString(project));
    return (fi.absolutePath() + "/." + fi.fileName() + ".lock").toStdString();
}

void setLockUserName(const std::string& name) {
    std::lock_guard lock(nameMutex);
    userName = name;
}

LockOwner currentLockOwner() {
    LockOwner o;
    {
        std::lock_guard lock(nameMutex);
        o.user = userName;
    }
    if (o.user.empty()) {
        QString u = qEnvironmentVariable("USER");
        if (u.isEmpty()) u = qEnvironmentVariable("USERNAME");
        o.user = u.toStdString();
    }
    o.host = QSysInfo::machineHostName().toStdString();
    o.app = "Montage";
    o.pid = QCoreApplication::applicationPid();
    o.token = processToken();
    o.since = o.heartbeat = QDateTime::currentDateTimeUtc();
    return o;
}

LockStatus projectLockStatus(const std::string& project) {
    LockStatus st;
    QFile f(QString::fromStdString(lockPathFor(project)));
    if (!f.exists()) return st;
    if (!f.open(QIODevice::ReadOnly)) {
        st.state = LockState::Theirs;  // there, but unreadable: not ours to break
        return st;
    }
    const QByteArray bytes = f.readAll();
    f.close();
    if (!decode(bytes, st.owner)) {
        // Half written by someone starting up just now, or damaged: damaged only once it is old.
        const QDateTime written = QFileInfo(f).lastModified().toUTC();
        st.state = written.isValid() && written.secsTo(QDateTime::currentDateTimeUtc()) > kLockStaleSeconds ? LockState::Stale : LockState::Theirs;
        return st;
    }
    const LockOwner me = currentLockOwner();
    if (isMine(st.owner, me)) {
        st.state = LockState::Mine;
        return st;
    }
    if (sameHost(st.owner, me) && !processAlive(st.owner.pid)) {
        st.state = LockState::Stale;
        return st;
    }
    // Heard from lately (by its own clock, or the file's time on the shared drive, whichever is later)? A heartbeat
    // from the future (a clock running ahead) is not believed.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QDateTime last = st.owner.heartbeat;
    if (last.isValid() && last.secsTo(now) < -60) last = QDateTime();
    const QDateTime touched = QFileInfo(f).lastModified().toUTC();
    if (!last.isValid() || (touched.isValid() && touched > last)) last = touched;
    st.state = last.isValid() && last.secsTo(QDateTime::currentDateTimeUtc()) > kLockStaleSeconds ? LockState::Stale : LockState::Theirs;
    return st;
}

LockResult acquireProjectLock(const std::string& project, LockOwner* holder) {
    const QString path = QString::fromStdString(lockPathFor(project));
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LockStatus st = projectLockStatus(project);
        if (st.state == LockState::Mine) {
            refreshProjectLock(project);
            return LockResult::Acquired;
        }
        if (st.state == LockState::Theirs) {
            if (holder) *holder = st.owner;
            return LockResult::HeldByOther;
        }
        if (st.state == LockState::Stale) {
            // Taken over by moving it aside under a name of this process's own: of two editors taking it over at
            // once only one move succeeds. What was moved must be the stale lock that was read, not a fresh one
            // someone wrote meanwhile (that one is put back).
            const QString aside = path + "." + QString::fromStdString(processToken()) + ".stale";
            QFile::remove(aside);
            if (!QFile::rename(path, aside)) continue;  // someone else got there first: look again
            QByteArray bytes;
            if (QFile moved(aside); moved.open(QIODevice::ReadOnly)) bytes = moved.readAll();
            LockOwner was;
            const bool parsed = decode(bytes, was);
            const bool damaged = st.owner.host.empty() && st.owner.token.empty() && st.owner.pid == 0;  // judged stale unreadable
            const bool same = damaged ? !parsed
                                      : parsed && was.token == st.owner.token && was.pid == st.owner.pid && was.host == st.owner.host &&
                                            was.heartbeat == st.owner.heartbeat;
            if (!same) {
                QFile::rename(aside, path);
                continue;
            }
            QFile::remove(aside);
        }
        // Created only if it is not there, so of two editors opening at once one gets it.
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            const QByteArray bytes = encode(currentLockOwner());
            const bool ok = f.write(bytes) == bytes.size() && f.flush();
            f.close();
            if (!ok) {
                QFile::remove(path);
                return LockResult::Unavailable;
            }
            return LockResult::Acquired;
        }
        if (!QFileInfo::exists(path)) return LockResult::Unavailable;  // the folder cannot be written to
    }
    const LockStatus st = projectLockStatus(project);
    if (holder) *holder = st.owner;
    return st.state == LockState::Mine ? LockResult::Acquired : LockResult::HeldByOther;
}

RefreshResult refreshProjectLockState(const std::string& project) {
    const QString path = QString::fromStdString(lockPathFor(project));
    QFile in(path);
    if (!in.exists()) {
        // Gone (removed by hand, or the drive came back without it): written again unless someone has meanwhile.
        if (!QFileInfo(path).absoluteDir().exists()) return RefreshResult::Unknown;
        QFile f(path);
        if (f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            f.write(encode(currentLockOwner()));
            f.close();
            return RefreshResult::Held;
        }
        return projectLockStatus(project).state == LockState::Mine ? RefreshResult::Held : RefreshResult::Unknown;
    }
    if (!in.open(QIODevice::ReadOnly)) return RefreshResult::Unknown;
    LockOwner owner;
    const bool read = decode(in.readAll(), owner);
    in.close();
    if (!read) return RefreshResult::Unknown;
    if (!isMine(owner, currentLockOwner())) return RefreshResult::Lost;
    owner.heartbeat = QDateTime::currentDateTimeUtc();
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(encode(owner));
        f.commit();
    }
    return RefreshResult::Held;
}

bool refreshProjectLock(const std::string& project) { return refreshProjectLockState(project) != RefreshResult::Lost; }

void releaseProjectLock(const std::string& project) {
    if (projectLockStatus(project).state == LockState::Mine) QFile::remove(QString::fromStdString(lockPathFor(project)));
}

}  // namespace montage
