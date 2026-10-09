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

private:
    EditorState* state_;
    QComboBox* range_;
    QCheckBox* flashing_;
    QCheckBox* levels_;
    QCheckBox* black_;
    QDoubleSpinBox* blackSeconds_;
    QCheckBox* freeze_;
    QDoubleSpinBox* freezeSeconds_;
    QCheckBox* silence_;
    QDoubleSpinBox* silenceSeconds_;
    QCheckBox* clipping_;
    QCheckBox* spelling_ = nullptr;
    QComboBox* loudness_;
    QTableWidget* table_;
    QLabel* summary_;
    QPushButton* markers_ = nullptr;
    std::vector<QcIssue> issues_;
};

}  // namespace montage
