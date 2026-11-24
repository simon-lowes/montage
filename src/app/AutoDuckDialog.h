// Montage — Clip › Auto Duck Music…: the selected audio clips dip under
// the dialogue on the chosen tracks (media/AutoDuck.h).
#pragma once

#include <QDialog>
#include <vector>

#include "core/Model.h"
#include "media/AutoDuck.h"

class QCheckBox;
class QDoubleSpinBox;
class QListWidget;

namespace montage {

class EditorState;

class AutoDuckDialog : public QDialog {
    Q_OBJECT
public:
    AutoDuckDialog(EditorState* state, const std::vector<Id>& music, QWidget* parent = nullptr);

    DuckOptions options() const;
    std::vector<int> dialogueTracks() const;

    bool keepUpToDate() const;

    // Finds the speech, as a background job (DialogJobs.h), and writes the music clips' volume keyframes as one undo
    // step. Returns how many clips changed, or -1 (nothing to do, cancelled, or left to run in the background: the
    // keyframes are then written when it ends). `keep`: the duck is kept up to date (media/AutoDuck.h): made again
    // when the dialogue under it changes.
    static int apply(EditorState* state, const std::vector<Id>& music, const std::vector<int>& tracks, const DuckOptions& o,
                     QWidget* parent, bool keep = false);

private:
    QListWidget* tracks_;
    QDoubleSpinBox* amount_;
    QDoubleSpinBox* fadeDown_;
    QDoubleSpinBox* fadeUp_;
    QDoubleSpinBox* threshold_;
    QCheckBox* transcripts_;
    QCheckBox* keep_;
};

}  // namespace montage
