// Montage — File › Merge Project: another copy of this project (an assistant's, a colourist's, the work done on another
// computer) brought into this one from the version both started from (core/ProjectMerge.h). The copy's own record of
// that version is used, or this project's, or a project file chosen as the base. The list shows every change with the
// side that made it and every conflict with Mine, Theirs or (for clips that overlap) Both; double-click one to go to
// it. Merge brings it all in as one undo step.
#pragma once

#include <QDialog>
#include <map>
#include <string>

#include "core/ProjectMerge.h"

class QLabel;
class QPushButton;
class QTableWidget;

namespace montage {

class EditorState;

class MergeDialog : public QDialog {
    Q_OBJECT
public:
    explicit MergeDialog(EditorState* state, QWidget* parent = nullptr);

    // Their copy, and its merge base unless one was chosen. False (the reason shown) when it cannot be read or no
    // base is known.
    bool setTheirs(const QString& path);
    // A project file as the version both started from.
    bool setBase(const QString& path);

    const MergeResult& result() const { return result_; }
    enum Column { Side, Kind, What, Field, Choice, Columns };
    int rowCount() const;
    QString cell(int row, int column) const;
    bool isConflict(int row) const;
    // Settles conflict `row` (merged again at once).
    void choose(int row, MergeSide side);
    // Goes to what row `row` is about: its sequence, its clip selected and the playhead on it.
    void activate(int row);
    // Brings their copy in, as one undo step. False when there is nothing to merge.
    bool merge();
    QString summary() const;

private:
    void remerge();
    void fill();

    EditorState* state_;
    Project theirs_, previewed_;  // (previewed_: this project as the merge shown was made from)
    MergeBase base_;
    bool haveTheirs_ = false, haveBase_ = false;
    QString theirsPath_, basePath_;
    std::map<std::string, MergeSide> choices_;
    MergeResult result_;
    QLabel* theirsLabel_ = nullptr;
    QLabel* baseLabel_ = nullptr;
    QLabel* summary_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* mergeButton_ = nullptr;
};

}  // namespace montage
