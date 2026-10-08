// Montage — the Keyframes panel: every animated parameter of the selected
// clip as a row of keyframes over the clip's length, like the timeline in
// Premiere's Effect Controls. Click or drag a box to select keys, drag them
// to retime (all selected rows move together), Delete removes them, the
// context menu sets Linear, Hold or Smooth, double-click adds a key, and a
// click on the ruler moves the playhead.
#pragma once

#include <QWidget>
#include <set>
#include <tuple>
#include <vector>

#include "core/KeyframeEdit.h"
#include "core/Model.h"

namespace montage {

class EditorState;

class KeyframePanel : public QWidget {
    Q_OBJECT
public:
    explicit KeyframePanel(EditorState* state, QWidget* parent = nullptr);

    struct Row {
        QString label;
        ParamAddress address;
    };
    struct Key {
        ParamAddress address;
        FrameTime t;
        bool operator<(const Key& o) const { return std::tie(address, t) < std::tie(o.address, o.t); }
        bool operator==(const Key& o) const = default;
    };

    Id clip() const { return clip_; }
    const std::vector<Row>& rows() const { return rows_; }
    const std::set<Key>& selection() const { return selection_; }
    void select(const std::set<Key>& keys);
    // Where a key is drawn, and the frame at a position (for tests and the mouse).
    QPoint keyPoint(int row, FrameTime t) const;
    // Edits on the selection, each one undo step.
    bool deleteSelected();
    bool setInterpolation(Interp interp);
    bool shiftSelected(FrameTime delta);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;
    QSize sizeHint() const override { return {520, 200}; }

private:
    void rebuild();
    const Clip* currentClip() const;
    QRect lane(int row) const;
    int rowAt(int y) const;
    double xForFrame(FrameTime t) const;
    FrameTime frameAtX(int x) const;
    bool keyAt(const QPoint& pos, Key& key) const;
    FrameTime clampShift(FrameTime delta) const;

    EditorState* state_;
    Id clip_ = 0;
    std::vector<Row> rows_;
    std::set<Key> selection_;
    // Mouse: dragging keys, a selection box, or scrubbing the ruler.
    enum class Drag { None, Keys, Box, Scrub } drag_ = Drag::None;
    QPoint press_;
    QRect box_;
    std::set<Key> dragStart_;
    FrameTime dragDelta_ = 0;
    bool dragging_ = false;
};

}  // namespace montage
