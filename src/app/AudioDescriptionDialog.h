// Montage — Sequence › Audio Description: the gaps between the dialogue where a description fits, the descriptions
// written into them (each with how well it fits, at a describer's pace), voiced by the speech generator onto an AD
// track as clips of the role "Description", the programme ducked under them, and whether they are heard while
// working (core/AudioDescription.h). Exports add the described stream (Export › Audio description).
#pragma once

#include <QDialog>
#include <vector>

#include "core/AudioDescription.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QTableWidget;

namespace montage {

class EditorState;

class AudioDescriptionDialog : public QDialog {
    Q_OBJECT
public:
    AudioDescriptionDialog(EditorState* state, QWidget* parent = nullptr);

    // Finds the gaps in the dialogue (the audio tracks other than the AD track) and lists them with the descriptions.
    int findGaps(QString* error = nullptr);
    // A description from the playhead: to the next line of dialogue, at most three seconds.
    void addAtPlayhead();
    // Voices the descriptions onto the AD track (replacing what was voiced before). Returns the clips made.
    int voice(QString* error = nullptr);
    // Ducks every other clip under the voiced descriptions. Returns how many clips changed.
    int duck();
    // Whether descriptions are heard while working (they always go to the described stream of an export).
    void setHear(bool on);
    int rows() const;

private:
    void refresh();
    void cellEdited(int row, int column);
    int adTrack() const;  // the audio track named "AD", -1 if none

    EditorState* state_;
    QTableWidget* table_;
    QDoubleSpinBox* minGap_;
    QSpinBox* pace_;
    QComboBox* voice_;
    QDoubleSpinBox* duckDb_;
    QCheckBox* hear_;
    QLabel* status_;
    std::vector<DescriptionGap> gaps_;
    bool refreshing_ = false;
};

}  // namespace montage
