// Montage — File › Project Manager…: a copy of the project and its media in a new folder (render/ProjectManager.h).
#pragma once

#include <QDialog>

#include "render/ProjectManager.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;

namespace montage {

class EditorState;

class ProjectManagerDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProjectManagerDialog(EditorState* state, QWidget* parent = nullptr);
    ConsolidateOptions options() const;

private:
    EditorState* state_;
    QLineEdit* folder_;
    QLineEdit* name_;
    QComboBox* sequences_;
    QComboBox* mode_;
    QDoubleSpinBox* handles_;
    QComboBox* codec_;
    QCheckBox* keepUnused_;
};

}  // namespace montage
