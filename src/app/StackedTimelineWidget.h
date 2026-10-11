// Montage — the stacked timeline (Premiere's stacked timelines, Resolve's stacked timeline tabs): a second sequence,
// picked from a list, shown read-only so parts of it can go into the active one. Click clips to select them (Shift or
// Ctrl adds), click the ruler to move its own playhead, I and O mark a range (X clears it); drag the selection, or the
// marked range from an empty part of the tracks, onto the timeline (Ctrl inserts). Swap makes it the active sequence and
// stacks the one it replaces.
#pragma once

#include <QWidget>
#include <vector>

#include "core/EditOps.h"
#include "core/Model.h"

class QComboBox;
class QLabel;
class QMimeData;
class QPushButton;

namespace montage {

class EditorState;
class StackedTimelineCanvas;

constexpr const char* kSequenceClipsMime = "application/x-montage-clips";  // "sequence;clip,clip,…"
constexpr const char* kSequenceRangeMime = "application/x-montage-range";  // "sequence;in;out" (out exclusive, -1 = end)

QMimeData* sequenceClipsMime(Id sequence, const std::vector<Id>& clips);
QMimeData* sequenceRangeMime(Id sequence, FrameTime in, FrameTime out);
bool hasSequencePart(const QMimeData* mime);
// The part of a sequence a drag carries, its clips timed from 0; false if it carries none or the sequence is gone.
bool sequencePartFromMime(const Project& p, const QMimeData* mime, Id* sequence, edit::SequenceRange* part);

class StackedTimelineWidget : public QWidget {
    Q_OBJECT
public:
    explicit StackedTimelineWidget(EditorState* state, QWidget* parent = nullptr);

    Id sequenceId() const { return seq_; }
    void setSequence(Id id);  // any sequence but the active one; 0 = the first such
    const std::vector<Id>& selection() const { return selection_; }
    void setSelection(const std::vector<Id>& clips);
    FrameTime playhead() const { return playhead_; }
    void setPlayhead(FrameTime t);
    FrameTime markIn() const { return in_; }    // -1 = none
    FrameTime markOut() const { return out_; }  // inclusive, -1 = none
    void setMarks(FrameTime in, FrameTime out);
    // What a drag takes: the selected clips, else the marked range (from 0 or to the end where a mark is missing).
    QMimeData* dragData() const;
    bool swap();
    QComboBox* picker() const { return picker_; }
    QWidget* canvas() const;

signals:
    void changed();

private:
    friend class StackedTimelineCanvas;
    const Sequence* sequence() const;
    void refresh();
    void updateInfo();

    EditorState* state_;
    QComboBox* picker_ = nullptr;
    QPushButton* swap_ = nullptr;
    QLabel* info_ = nullptr;
    StackedTimelineCanvas* canvas_ = nullptr;
    Id seq_ = 0;
    std::vector<Id> selection_;
    FrameTime playhead_ = 0, in_ = -1, out_ = -1;
    bool filling_ = false;
};

}  // namespace montage
