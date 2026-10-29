// Montage — Sequence › VFX Shots: the shots pulled for visual effects (render/VfxShots.h), each with its status, note,
// the version its clip shows and what is wrong with it; the returns folder looked through for new versions (and
// watched while the project is open), versions stepped up and down, the shot list exported and compositing scripts
// written.
#pragma once

#include <QDialog>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QTreeWidget;

namespace montage {

class EditorState;

class VfxShotsDialog : public QDialog {
    Q_OBJECT
public:
    VfxShotsDialog(EditorState* state, QWidget* parent = nullptr);
    void refresh();
    // The shots picked in the list (all of them when none are), by name.
    std::vector<std::string> chosenShots() const;
    // Shows `version` (0 the plate, -1 the latest) on the chosen shots, or steps them (`step` +1 / -1); one undo step.
    bool showVersion(int version, int step = 0);
    bool setStatus(int status, const QString* note = nullptr);
    bool writeScripts(QString* error = nullptr);
    bool exportList(const QString& path, QString* error = nullptr);
    void setReturnsFolder(const QString& folder);

private:
    EditorState* state_;
    QLabel* returns_;
    QLineEdit* pattern_;
    QTreeWidget* list_;
    QComboBox* status_;
    QLineEdit* note_;
    QLabel* message_;
};

}  // namespace montage
