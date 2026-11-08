// Montage — Sequence › Quality Check…: the delivery checks of render/QualityCheck.h over the sequence (or In to
// Out), listed with their times. Double-click a problem to go to it; Add Markers marks each on the timeline.
#pragma once

#include <QDialog>
#include <vector>

#include "render/QualityCheck.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace montage {

class EditorState;

class QualityCheckDialog : public QDialog {
    Q_OBJECT
public:
    explicit QualityCheckDialog(EditorState* state, QWidget* parent = nullptr);

    QcSettings settings() const;
    // Runs the checks in the background (with progress) and lists what they find. False if cancelled.
    bool runCheck();
    const std::vector<QcIssue>& issues() const { return issues_; }
    // Red markers for the problems, as one undo step (earlier QC markers are replaced).
    int addMarkers();
    // Moves the playhead to the problem in `row`.
    void activate(int row);
    // The last check's Compare Encode (ok false when no encode was compared).
    const EncodeComparison& comparison() const { return comparison_; }
    // Writes it as <base>.pdf and <base>.csv. False (with the reason shown) if it cannot.
    bool saveQualityReport(const QString& base);

signals:
    // A check compared an encode with the sequence (its id) as it was at that history revision.
    void compared(Id sequence, uint64_t revision);

private:
    EditorState* state_;
    QComboBox* range_;
    QCheckBox* flashing_;
    QCheckBox* levels_;
    QCheckBox* black_;
    QDoubleSpinBox* blackSeconds_;
    QCheckBox* freeze_;
    QDoubleSpinBox* freezeSeconds_;
    QCheckBox* focus_ = nullptr;
    QCheckBox* silence_;
    QDoubleSpinBox* silenceSeconds_;
    QCheckBox* clipping_;
    QCheckBox* phase_;
    QCheckBox* dcOffset_;
    QCheckBox* spelling_ = nullptr;
    QCheckBox* uncaptioned_ = nullptr;
    QDoubleSpinBox* uncaptionedSeconds_ = nullptr;
    QCheckBox* sync_ = nullptr;
    QCheckBox* intelligibility_ = nullptr;
    QDoubleSpinBox* intelligibilitySeconds_ = nullptr;
    QComboBox* intelligibilityPreset_ = nullptr;
    QComboBox* loudness_;
    QLineEdit* compareFile_ = nullptr;
    QLineEdit* hrdFile_ = nullptr;
    QComboBox* qualityPreset_ = nullptr;
    QDoubleSpinBox* minPsnr_ = nullptr;
    QDoubleSpinBox* maxDeltaItp_ = nullptr;
    QDoubleSpinBox* minVmaf_ = nullptr;
    QCheckBox* vmaf_ = nullptr;
    QPushButton* report_ = nullptr;
    EncodeComparison comparison_;
    QTableWidget* table_;
    QLabel* summary_;
    QPushButton* markers_ = nullptr;
    std::vector<QcIssue> issues_;
};

}  // namespace montage
