// Montage — Playback › Video Output › Monitor Calibration…: each display's calibration LUT (app/DisplayCalibration.h)
// for its SDR pictures and its HDR viewer, the reference output's, a switch to bypass them all, and the network patch
// generator (app/PatchGenerator.h) that calibration software measures a display through.
#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace montage {

class PatchGenerator;
class ReferenceOutputController;

class MonitorCalibrationDialog : public QDialog {
    Q_OBJECT
public:
    MonitorCalibrationDialog(PatchGenerator* generator, ReferenceOutputController* output, QWidget* parent = nullptr);

private:
    void fillLuts();
    void fillTargets();
    void refreshPatch();
    void addLutRow(int row, const QString& label, const QString& target, const QString& tip);
    PatchGenerator* generator_;
    ReferenceOutputController* output_;
    QGridLayout* luts_ = nullptr;
    QCheckBox* bypass_ = nullptr;
    QLineEdit* host_ = nullptr;
    QSpinBox* port_ = nullptr;
    QComboBox* target_ = nullptr;
    QCheckBox* through_ = nullptr;
    QPushButton* startStop_ = nullptr;
    QLabel* status_ = nullptr;
};

}  // namespace montage
