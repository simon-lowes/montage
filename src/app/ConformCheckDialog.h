// Montage — Sequence › Conform Check: the editor's reference movie (the offline cut as signed off) laid against this
// sequence, each shot compared with it (render/ConformCheck.h). The table lists each shot's verdict (matches,
// reframed, slipped, another take, missing, no match) with what was found; double-click a row to go to it; Apply Fix puts the
// selected shots right (Apply All every one that can be), Add as Markers marks those that differ, each one undo step.
// Any other change to the project (an edit, an undo) drops the results, to be checked again.
#pragma once

#include <QDialog>
#include <vector>

#include "render/ConformCheck.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

namespace montage {

class EditorState;

class ConformCheckDialog : public QDialog {
    Q_OBJECT
public:
    explicit ConformCheckDialog(EditorState* state, QWidget* parent = nullptr);

    void setReference(Id media);
    void setReferenceStart(FrameTime t);
    // Runs the check (with a progress dialog); false when cancelled or the reference cannot be read.
    bool runCheck();
    const std::vector<ConformShot>& shots() const { return shots_; }
    void activate(int row);       // the playhead to the shot, its clip selected
    int applyFixes(bool all);     // the selected rows' fixes (all: every fixable shot), one undo step; how many
    int addMarkers();             // a marker for each shot that differs, one undo step; how many
    static int labelFor(ConformVerdict v);

private:
    void updateButtons();
    void forget();  // the results dropped: the sequence changed since the check (an edit, an undo)

    EditorState* state_;
    Id sequence_ = 0;
    std::vector<ConformShot> shots_;
    std::vector<bool> fixed_;
    bool editing_ = false;  // our own edit (its change does not make the results stale)
    bool marked_ = false;   // markers added for these results
    QComboBox* reference_ = nullptr;
    QLineEdit* start_ = nullptr;
    QLabel* summary_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* applyAllButton_ = nullptr;
    QPushButton* markersButton_ = nullptr;
};

}  // namespace montage
