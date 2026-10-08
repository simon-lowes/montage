// Montage — Sequence › Compare with Sequence (Resolve 21.1's timeline comparison): what changed between another
// version of the cut and this one, as a list (added, removed, trimmed, moved and changed clips, in time order).
// Double-click a row to go to it; Add as Markers puts a coloured marker on this sequence for each change.
#pragma once

#include <QDialog>
#include <vector>

#include "core/TimelineCompare.h"

class QTableWidget;
class QLabel;

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

private:
    EditorState* state_;
    std::vector<TimelineChange> changes_;
    QTableWidget* table_ = nullptr;
    QLabel* summary_ = nullptr;
};

}  // namespace montage
