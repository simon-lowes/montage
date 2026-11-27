// Montage — Edit in Another Application: a still opened in the picture editor chosen for stills, and the clips using
// it updated each time it is saved there.
//
// The editor works on a copy of the still (in "Edited Stills" beside the project, or in the app's data folder for a
// project not yet saved), so the original is never written over. Each time the copy is saved, once it has been quiet
// for a moment and reads as a picture, it is kept as a numbered version ("Logo v2.png") and the still's media points
// at that version as one undo step: undo shows the picture from before, and the still keeps its name. A layered file
// is read as Montage reads it anyway (a layer's media keeps its layer).
#pragma once

#include <QObject>
#include <QString>
#include <functional>
#include <map>
#include <set>

#include "core/Model.h"

class QFileSystemWatcher;
class QMenu;
class QTimer;
class QWidget;

namespace montage {

class EditorState;

class StillEditor : public QObject {
    Q_OBJECT
public:
    // The state's own (made the first time it is asked for).
    static StillEditor* of(EditorState* state);

    // The program stills are edited in, chosen once and kept in the settings ("editing/stillProgram"); "" for none yet.
    static QString program();
    static void setProgram(const QString& path);
    // Whether the media is a still that can be edited elsewhere: a picture from a file, not a numbered sequence.
    bool canEdit(Id media) const;
    // Opens the still in the program (the copy made first, or the one already being edited); false, with why, when
    // it is not a still, no program is chosen, the copy cannot be made or the program does not start.
    bool edit(Id media, QString* why = nullptr);
    // The copy being edited for the media ("" for none).
    QString workingCopy(Id media) const;
    // The menu items for a still: Edit in Another Application… (asking for the program the first time) and Choose
    // Application for Stills….
    static void addActions(QMenu& menu, EditorState* state, Id media, QWidget* parent);
    // What starts the program with the file, for tests (empty: the program itself, detached).
    static void setLauncherForTesting(std::function<bool(const QString& program, const QString& file)> launch);

    // Looks at the copies saved since (the watcher's notice, after a quiet moment; public for tests). Returns how many
    // stills were updated.
    int takeSaved();

signals:
    void updated(Id media);  // a saved copy is now the still's picture

private:
    explicit StillEditor(EditorState* state);
    EditorState* state_;
    QFileSystemWatcher* watcher_;
    QTimer* quiet_;
    // A working copy's still: its media in the project open when the copy was made (that project's generation and
    // file; a saved copy changes only that project, open again, never another that has the same media id).
    struct Copy {
        Id media = 0;
        uint64_t generation = 0;
        QString project;
    };
    std::map<QString, Copy> copies_;  // working copy -> its still
    bool ours(const Copy& c) const;    // whether the copy's project is the one open
    std::set<QString> saved_;       // copies written to since last looked at
};

}  // namespace montage
