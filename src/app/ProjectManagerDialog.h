// Montage — File › Project Manager…: a copy of the project and its media in a new folder (render/ProjectManager.h),
// for archiving, or for finishing: the parts used in their own formats or transcoded to movies or image sequences,
// with presets kept in the project, the user's library and a facility folder.
#pragma once

#include <QDialog>

#include "render/ProjectManager.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QSpinBox;

namespace montage {

class EditorState;

class ProjectManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProjectManagerDialog(EditorState* state, QWidget* parent = nullptr);
    ConsolidateOptions options() const;
    // The dialog's choices set from options (a preset's).
    void setChoices(const ConsolidateOptions& o);
    // The user's library of Consolidate presets: a folder of <name>.json files in Montage's data folder.
    static QString userPresetFolder();
    // The presets as the dialog has them (the project's, the user's and the facility's).
    ConsolidatePresets presets() const;
    // The compare view's text for a preset at its narrowest level: what it sets differently from the levels above.
    QString compareText(const QString& preset) const;

private:
    void loadPresets();
    void update();
    EditorState* state_;
    QLineEdit* folder_;
    QLineEdit* name_;
    QComboBox* sequences_;
    QComboBox* preset_;
    QComboBox* mode_;
    QComboBox* handleUnit_;
    QDoubleSpinBox* handles_;
    QSpinBox* handleFrames_;
    QLineEdit* handleTimecode_;
    QComboBox* codec_;
    QComboBox* frames_;
    QComboBox* colour_;
    QLineEdit* names_;
    QSpinBox* depth_;
    QCheckBox* leaveOut_;
    QCheckBox* bake_;
    QCheckBox* trimSound_;
    QCheckBox* keepUnused_;
};

}  // namespace montage
