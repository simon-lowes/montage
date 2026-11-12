// Montage — background jobs kept on disk: the render queue's exports and MCP's background calls (analyses, renders,
// transcodes and the rest) each write a record of themselves, so they are listed (montage_job) and taken up again after
// Montage is restarted or after a crash.
//
// The store is a folder (MONTAGE_JOBS_DIR, else "Montage/jobs" in the user's data folder): one folder a job, named by
// its id, holding job.json (what it is, its state and progress, the process running it, and its owner's own data) and
// whatever else its owner keeps with it (the render queue: the project as it was when the job was added), and
// "alive/<token>", touched by each process keeping jobs there. A job a process was running is "interrupted" once that
// process is gone.
#pragma once

#include <QJsonObject>
#include <QString>
#include <cstdint>
#include <string>
#include <vector>

namespace montage::jobs {

struct Record {
    QString id;      // its folder's name: "queue-3", "mcp-job-12"
    QString owner;   // "render-queue", "mcp"
    QString kind;    // "export", or the MCP tool's name
    QString title;
    QString status = QStringLiteral("waiting");  // waiting, running, done, failed, cancelled (and interrupted, as read)
    double progress = 0;
    QString step, error;
    qint64 pid = 0;   // the process running it (while running)
    QString process;  // that process's token (core/ProjectLock.h's), so a reused pid is not taken for it
    QString created, updated;  // ISO 8601, UTC
    int order = 0;             // its place among its owner's jobs
    QJsonObject data;          // its owner's own
    bool finished() const;
};

// The store's folder (made when first written). `folder` empty: the default above.
QString storeFolder();
QString jobFolder(const QString& id, const QString& folder = {});

// Written whole (a crash leaves the previous version); `updated` stamped. False when it cannot be written.
bool save(Record r, const QString& folder = {});
bool load(const QString& id, Record& r, const QString& folder = {});
// Every job in the store (`owner` empty: anyone's), by owner and order. A running job whose process is gone reads as
// "interrupted".
std::vector<Record> all(const QString& owner = {}, const QString& folder = {});
bool remove(const QString& id, const QString& folder = {});
// `owner`'s finished jobs but the `keep` most lately updated removed, and any job folder a day old without a readable
// record. Returns how many.
int prune(const QString& owner, int keep, const QString& folder = {});

// This process, as a record's pid and process name it.
qint64 thisProcess();
QString thisProcessToken();
// Whether the record says this process runs it, or another that is still running (its pid alive, and the store's
// "alive" file of its token touched in the last 30 seconds: each process keeping jobs touches its own every 5).
bool runningHere(const Record& r);
bool runningElsewhere(const Record& r, const QString& folder = {});

// How many background jobs run at once (each kind its own: exports in the render queue, MCP's background calls):
// Settings' "jobs/at_once", 2 unless set (1 to 16).
int atOnce();
void setAtOnce(int n);

}  // namespace montage::jobs
