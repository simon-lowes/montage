// Montage — crash safety: live recovery saves, rolling snapshots and
// detection of sessions that did not exit cleanly.
//
// Each running editor owns a session folder under the recovery directory:
//   lock            QLockFile held for the whole session
//   session.json    project path, whether it has unsaved changes, plugin use
//   recovery.montage  the project as of the last edit (debounced live save)
// A clean exit deletes the folder. A folder whose lock can be taken at startup
// belongs to a session that crashed (or was killed).
//
// Snapshots: every few minutes of editing, a timestamped copy goes to
// snapshots/<project>/, keeping the newest N, like Premiere's Auto-Save folder.
#pragma once

#include <QDateTime>
#include <QLockFile>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <memory>
#include <vector>

namespace montage {

class EditorState;

class RecoveryManager : public QObject {
    Q_OBJECT
public:
    struct Session {
        QString dir;           // session folder
        QString projectPath;   // empty for an untitled project
        QString projectName;
        QString recoveryFile;  // empty if nothing unsaved
        QDateTime lastSave;
        bool pluginsUsed = false;
    };

    // `baseDir` empty = the user's app data folder (or $MONTAGE_RECOVERY_DIR).
    explicit RecoveryManager(EditorState* state, const QString& baseDir = QString(), QObject* parent = nullptr);
    ~RecoveryManager() override;

    QString baseDir() const { return base_; }
    // Sessions that ended without a clean exit, newest first. Call before
    // beginSession() at startup; their locks are taken over (and released).
    std::vector<Session> crashedSessions();
    // Loads a crashed session's unsaved work into the editor, keeping the
    // original project path and marking it modified. Removes the session.
    bool recover(const Session& s, QString* error = nullptr);
    void discard(const Session& s);

    void beginSession();
    // Clean exit: the session folder (and its recovery copy) is removed.
    void endSession();
    // Simulates a crash for tests: releases the lock and leaves the folder.
    void abandonSession();

    void setLiveSaveDelay(int ms) { live_.setInterval(ms); }
    void setSnapshotInterval(int ms) { snapshotInterval_ = ms; }
    void setMaxSnapshots(int n) { maxSnapshots_ = n; }
    QString snapshotDir(const QString& projectName) const;
    QStringList snapshots(const QString& projectName) const;  // newest first
    // Writes the live recovery copy now (and a snapshot if one is due).
    void saveNow();

private:
    void writeSessionInfo();
    void takeSnapshot();

    EditorState* state_;
    QString base_;
    QString sessionDir_;
    std::unique_ptr<QLockFile> lock_;
    QTimer live_;
    int snapshotInterval_ = 5 * 60 * 1000;
    int maxSnapshots_ = 20;
    QDateTime lastSnapshot_;
    bool dirtySinceSnapshot_ = false;
};

}  // namespace montage
