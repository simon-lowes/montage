// Montage — sharing a project between editors (Avid's bin locking, Premiere's Productions, Resolve's collaboration
// locks): whoever opens a project to edit it holds its lock, a small file beside it (".<name>.lock") naming them,
// their machine and process, when they opened it and when they were last heard from. Anyone else opening it gets it
// read-only until the lock is let go. A lock whose holder has stopped refreshing it (a crash, a machine switched off)
// goes stale after a few minutes, or at once when its process is gone from this machine, and can be taken over. Locks
// live on the project's own drive, so they work across a shared volume (NAS, SAN, a synced folder).
#pragma once

#include <QDateTime>
#include <QString>
#include <string>

namespace montage {

struct LockOwner {
    std::string user, host, app;
    qint64 pid = 0;
    QDateTime since, heartbeat;  // UTC
    // "Sam on edit-bay-2", for messages.
    std::string describe() const;
};

enum class LockState {
    Free,    // no lock
    Mine,    // held by this process
    Theirs,  // held by someone else who is still there
    Stale,   // held by someone no longer there: can be taken over
};

struct LockStatus {
    LockState state = LockState::Free;
    LockOwner owner;  // when there is a lock
};

// Seconds without a heartbeat after which a lock is stale.
constexpr int kLockStaleSeconds = 180;

// The lock file of a project.
std::string lockPathFor(const std::string& project);
// This process as a lock holder (the user's name from the system, or as set).
LockOwner currentLockOwner();
void setLockUserName(const std::string& name);  // "" = the system's
LockStatus projectLockStatus(const std::string& project);

enum class LockResult {
    Acquired,     // held by this process now
    HeldByOther,  // someone else has it (`holder` says who)
    Unavailable,  // no lock can be written there (a read-only folder): the project opens unlocked
};
// Takes the lock (a stale one is taken over). Writing it is atomic, so of two editors opening at once one wins.
LockResult acquireProjectLock(const std::string& project, LockOwner* holder = nullptr);
// Renews this process's heartbeat; false when the lock is no longer ours (taken over after this machine slept).
bool refreshProjectLock(const std::string& project);
// Lets go of the lock if this process holds it.
void releaseProjectLock(const std::string& project);

}  // namespace montage
