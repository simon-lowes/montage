// Montage — the Sequence Index (like Premiere's): every clip and marker in the
// sequence as rows of a table, with its colour label, track, timing, source and
// effects. Type to filter (any column matches, so a colour's name finds its markers); click a header to sort; double-click a
// row to select the clip and move the playhead to it; edit a name in place.
#pragma once

#include <QWidget>
#include <vector>

#include "core/Model.h"

class QLineEdit;
class QTableWidget;
class QLabel;

namespace montage {

class EditorState;

class SequenceIndexPanel : public QWidget {
    Q_OBJECT
public:
    explicit SequenceIndexPanel(EditorState* state, QWidget* parent = nullptr);

    void setFilter(const QString& text);
    int rowCount() const;                // rows shown (after the filter)
    QString cell(int row, int column) const;
    void activate(int row);              // as a double-click
    bool rename(int row, const QString& name);  // a clip or marker, as one undo step

    enum Column { Name, Kind, Color, Track, Start, End, Duration, SourceIn, Media, Effects, Columns };

private:
    struct Row {
        bool marker = false;
        Id clip = 0;
        int markerIndex = -1;
        FrameTime start = 0;
        int color = 0;  // the clip's or marker's colour label, shown as a swatch
    };
    void rebuild();
    void applyFilter();

    EditorState* state_;
    QLineEdit* filter_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* count_ = nullptr;
    std::vector<Row> rows_;  // by the table's item data, not position (sorting moves rows)
    bool building_ = false;
};

}  // namespace montage
