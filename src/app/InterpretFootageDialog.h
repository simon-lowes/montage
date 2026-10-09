// Montage — Interpret Footage (Premiere's dialog of the same name, Resolve's Clip Attributes, Final Cut's Conform
// Speed and anamorphic override): how the selected videos and stills are read, for every clip made from them: the
// frame rate they play at (with their sound at the new speed or keeping its pitch), their pixel aspect ratio, their
// alpha and their field order. From the media bin's context menu.
#pragma once

#include <QDialog>

#include "core/Interpretation.h"
#include "core/Model.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QRadioButton;

namespace montage {

class InterpretFootageDialog : public QDialog {
    Q_OBJECT
public:
    // Starts from how `first` is read; `count` items will take the choices.
    InterpretFootageDialog(const MediaItem& first, int count, QWidget* parent = nullptr);
    // The choices (fps 0 = the file's own rate).
    Interpretation interpretation() const;

private:
    QRadioButton* fileRate_ = nullptr;
    QRadioButton* assumeRate_ = nullptr;
    QComboBox* rate_ = nullptr;
    QCheckBox* keepPitch_ = nullptr;
    QRadioButton* filePar_ = nullptr;
    QRadioButton* conformPar_ = nullptr;
    QComboBox* parPreset_ = nullptr;
    QDoubleSpinBox* par_ = nullptr;
    QComboBox* alpha_ = nullptr;
    QComboBox* fields_ = nullptr;
};

}  // namespace montage
