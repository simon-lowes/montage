// Montage — File › Encode Folders…: folders whose files are each encoded with an export preset into an output folder
// once they stop growing (render/EncodeFolders.h), as background jobs of the window (app/DialogJobs.h), so the jobs
// list shows them and they can be stopped. Kept in the settings and taken up again when Montage starts; what was
// encoded before is not encoded again.
#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <map>
#include <memory>
#include <vector>

#include "render/EncodeFolders.h"

class QTimer;
class QWidget;

namespace montage {

class DialogJobs;
class EditorState;

class EncodeFolderJobs : public QObject {
    Q_OBJECT
public:
    // The window's own (made, and its folders looked at, the first time it is asked for).
    static EncodeFolderJobs* of(QWidget* window, EditorState* state);
    ~EncodeFolderJobs() override;

    // The folders (as saved in the settings), read again; each looked at every second.
    void reloadFolders();
    const std::vector<EncodeFolder>& folders() const { return folders_; }
    // How long a file has to stay unchanged before it is encoded (3 s; less for tests).
    void setSettleMs(qint64 ms);
    // Looks at the folders now (as the timer does), starting the encodes of what is ready. Returns how many began.
    int scan();
    // Encodes begun since this object was made, and those running or waiting now.
    int started() const { return started_; }
    int pending() const { return int(pending_.size()); }

signals:
    void encoded(const QString& input, const QString& output);
    void failed(const QString& input, const QString& error);

private:
    EncodeFolderJobs(QWidget* window, EditorState* state);
    // The scan of a folder, by its input folder, preset and output folder (two may share an input or an output).
    EncodeFolderScan* scanFor(const EncodeFolder& folder) const;
    QString titleFor(const QString& path) const;
    QPointer<QWidget> window_;
    QPointer<EditorState> state_;
    QTimer* timer_ = nullptr;
    qint64 settleMs_ = 3000;
    std::vector<EncodeFolder> folders_;
    std::vector<std::unique_ptr<EncodeFolderScan>> scans_;
    struct Pending {
        EncodeFolder folder;
        QString file, output;
    };
    std::map<QString, Pending> pending_;  // by task id
    int started_ = 0;
};

// The dialog: each folder with its preset and output folder, added and removed. Saved when closed with OK.
void showEncodeFoldersDialog(QWidget* window, EditorState* state);

}  // namespace montage
