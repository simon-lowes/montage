// Montage — Sequence › Compare with Sequence (Resolve 21.1's timeline comparison, Avid's Change List tool): what changed
// between another version of the cut and this one. The Clips tab lists clips added, removed, trimmed, moved and changed,
// in time order; double-click a row to go to it; Add as Markers puts a coloured marker on this sequence for each change.
// The Change List tab follows the picture frame by frame: the stretches of the old cut kept (in place or moved), the
// new material and what was taken out. It exports as a change EDL or CSV for sound and VFX departments, and Re-conform
// rebuilds a sequence cut to the old version (a mix, a grade, effects and titles) to play against this one.
#pragma once

#include <QDialog>
#include <vector>

#include "core/Reconform.h"
#include "core/TimelineCompare.h"

class QTableWidget;
class QLabel;
class QTabWidget;

namespace montage {

class EditorState;

class CompareDialog : public QDialog {
    Q_OBJECT
public:
    // `before` is the older version; the comparison is against the active sequence.
    CompareDialog(EditorState* state, Id before, QWidget* parent = nullptr);

    const std::vector<TimelineChange>& changes() const { return changes_; }
    int rowCount() const;
    QString cell(int row, int column) const;
    void activate(int row);  // the playhead to the change, its clip selected
    int addMarkers();        // one undo step; returns how many

    enum Column { Change, Clip, Track, Timecode, Length, Details, Columns };
    static int labelFor(ChangeKind k);  // the colour label a change's marker gets

    // The picture's change list.
    const CutChanges& cuts() const { return cuts_; }
    int cutRowCount() const;
    QString cutCell(int row, int column) const;
    void activateCut(int row);  // the playhead to where it is in this version
    enum CutColumn { CutNumber, CutKind, CutShot, CutOldIn, CutOldOut, CutNewIn, CutNewOut, CutLength, CutShift, CutColumns };
    static int labelFor(CutEventKind k);
    // A change EDL (.edl) or a CSV (anything else); false (with a message) when it cannot be written.
    bool exportChangeList(const QString& path);
    // A new sequence: `source` (cut to the older version) conformed to this one, made active; one undo step. 0 if not.
    Id reconform(Id source, const ReconformOptions& options = {});
    void showTab(int index);

private:
    void exportChangeListDialog();
    void reconformDialog();

    EditorState* state_;
    Id before_ = 0, after_ = 0;
    std::vector<TimelineChange> changes_;
    CutChanges cuts_;
    QTabWidget* tabs_ = nullptr;
    QTableWidget* table_ = nullptr;
    QTableWidget* cutTable_ = nullptr;
    QLabel* summary_ = nullptr;
};

}  // namespace montage
