// Montage — encode folders: each file that lands in a folder encoded with an export preset into an output folder,
// once it has stopped growing, and once only.
//
// A file is ready once its size and modification time have stayed the same for `settleMs` (a copy over a network, a
// recorder or a download still writing it is waited for), and it is not hidden or a partial download (".part",
// ".crdownload", ".download", ".tmp"). What was encoded is kept in a ledger in the output folder
// (".montage-encoded.json": each file's path, the preset, its size and time and what it became), so after a restart
// nothing finished is encoded again, while a new file of the same name (another size or time) is. Folders that share
// an output folder share its ledger, each keeping its own files apart from the others' (one input with two presets,
// or two inputs with files of one name); a ledger written before kept bare names, which are read as before. The app
// runs the encodes as background jobs (app/EncodeFoldersDialog.h); `montage-cli encode-folder` runs them one after
// the other.
#pragma once

#include <QString>
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace montage {

struct EncodeFolder {
    QString input;   // the folder watched (not its subfolders)
    QString preset;  // an export preset's name (render/Exporter.h)
    QString output;  // where the encodes go, as "<file's name>.<preset's extension>"
    bool operator==(const EncodeFolder&) const = default;
};

class EncodeFolderScan {
public:
    explicit EncodeFolderScan(EncodeFolder folder, qint64 settleMs = 3000);
    const EncodeFolder& folder() const { return folder_; }

    // Looked at at `now` (milliseconds, any clock): the files that have stopped growing and are neither encoded nor
    // taken (begun and not yet ended), by name. Each is given once: taken until marked done or let go.
    std::vector<QString> ready(qint64 now);
    // Whether the ledger has the file as it is now (its size and time) encoded.
    bool encoded(const QString& file) const;
    // The file's encode written to `output`: kept in the ledger (written at once). False when it cannot be written.
    bool markDone(const QString& file, const QString& output);
    // An encode that failed or was stopped: the file is looked at again once it changes.
    void letGo(const QString& file);
    // Where the file's encode goes: its name with the preset's extension, numbered when another file has it, or
    // another encode is writing it now (`busy`: those encodes' outputs).
    QString outputFor(const QString& file, const std::vector<QString>& busy = {}) const;
    // The ledger read again (another process wrote it).
    void reload();
    // How many of this folder's files (with its preset) the ledger has.
    int encodedCount() const;
    // How many files the last look found still growing (not yet ready).
    int waiting() const { return waiting_; }

private:
    struct Seen {
        qint64 size = -1, modified = 0, since = 0;
    };
    struct Done {
        qint64 size = 0, modified = 0;
        QString output;
        QString input, preset;  // the file's path and the preset ("" for a ledger's entry of before, by bare name)
    };
    EncodeFolder folder_;
    qint64 settleMs_;
    int waiting_ = 0;
    std::map<QString, Seen> seen_;
    std::map<QString, Done> ledger_;  // by key(): this folder's and every other's that shares the output folder
    std::map<QString, Seen> taken_;   // as it was when taken
    std::map<QString, Seen> failed_;  // as it was when it failed: tried again once it changes
    QString ledgerPath() const;
    QString key(const QString& file) const;  // a file's entry: its path and the preset
    // The file's entry: its own, else one by its bare name from a ledger of before that has it as it is now.
    const Done* entryFor(const QString& file) const;
    bool isOutput(const QString& path) const;  // an encode written here (when the output folder is the input one)
};

// Whether a file name is one an encode folder passes over (hidden, or a partial download).
bool encodeFolderSkips(const QString& name);

// Encodes one file with a preset: the file as a sequence of its own (its picture size and rate, or the preset's
// defaults for a sound or a still), the whole of it, to `output`. False with why.
bool encodeFileWithPreset(const QString& input, const QString& preset, const QString& output, std::string* error = nullptr,
                          const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

// The encode folders kept in the settings ("encode/folders").
std::vector<EncodeFolder> savedEncodeFolders();
void saveEncodeFolders(const std::vector<EncodeFolder>& folders);

}  // namespace montage
