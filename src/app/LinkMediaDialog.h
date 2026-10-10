// Montage — Link Media (Premiere's Link Media, Resolve's Relink Clips, Final Cut's Relink Files): the media whose
// files are gone, listed with where they were. Locate one file and the rest are looked for beside it, or search a
// folder for all of them; each relink is one undo step. Shown when a project opens with media offline, and from
// File › Link Media. It closes by itself once everything is found.
#pragma once

#include <QDialog>
#include <vector>

#include "core/Model.h"

class QCheckBox;
class QLabel;
class QTableWidget;

namespace montage {

class EditorState;

class LinkMediaDialog : public QDialog {
    Q_OBJECT
public:
    explicit LinkMediaDialog(EditorState* state, QWidget* parent = nullptr);

    const std::vector<Id>& offline() const { return offline_; }  // as listed
    // Relinks the item to `file`; with Relink Others on, then looks for the rest in its folder and the one above.
    // Returns how many were relinked (0 with a message if the file does not match).
    int locate(Id id, const QString& file);
    int searchFolder(const QString& folder);  // how many were found there
    void refresh();

private:
    EditorState* state_;
    QTableWidget* table_ = nullptr;
    QCheckBox* others_ = nullptr;
    QLabel* status_ = nullptr;
    std::vector<Id> offline_;
};

}  // namespace montage
