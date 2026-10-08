// Montage — the Keyframes panel: every animated parameter of the selected
// clip as a row of keyframes over the clip's length, like the timeline in
// Premiere's Effect Controls. Click or drag a box to select keys, drag them
// to retime (all selected rows move together), Delete removes them, the
// context menu sets Linear, Hold, Smooth or Bezier and eases keys in and
// out, double-click adds a key, and a click on the ruler moves the playhead.
// Graph shows the values as curves instead (After Effects' value graph):
// drag keys in time and value, and a Bezier key's handles to shape the
// curve (Alt breaks the two handles apart).
#pragma once

#include <QWidget>
#include <map>
#include <set>
#include <tuple>
#include <vector>

#include "core/KeyframeEdit.h"
#include "core/Model.h"

class QToolButton;

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
    bool easeSelected(bool in, bool out);
    // After the last keyframe, for the parameters of the selected keys: hold, loop, ping-pong or offset.
    bool setRepeat(Repeat repeat);
    // Copy the selected keys, and paste the copied ones at the playhead into this clip (or another one): the same
    // parameters, the earliest copied key at the playhead. Ctrl+C / Ctrl+V while the panel has focus.
    bool copySelected();
    int pasteAtPlayhead();
    static bool hasCopiedKeys();

    // The value graph.
    void setGraph(bool on);
    bool graph() const { return graph_; }
    // Shows one row's curve alone (with its values on the axis), or all of them (-1).
    void setGraphRow(int row);
    int graphRow() const { return graphRow_; }
    // Where a key, or one of its handles, is drawn in the graph.
    QPointF graphPoint(int row, FrameTime t) const;
    QPointF handlePoint(int row, FrameTime t, bool out) const;
    // The value at a height in the graph, for a row.
    double valueAtY(int row, double y) const;

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    QSize sizeHint() const override { return {520, 200}; }

private:
    void rebuild();
    const Clip* currentClip() const;
    QRect lane(int row) const;
    int rowAt(int y) const;
    double xForFrame(FrameTime t) const;
    FrameTime frameAtX(int x) const;
    bool keyAt(const QPoint& pos, Key& key) const;
    // The keys with those a Mask Path row stands for (every coordinate of the path).
    std::set<Key> linked(const std::set<Key>& keys) const;
    FrameTime clampShift(FrameTime delta) const;
    double xForFrameD(double t) const;
    double frameAtXD(double x) const;  // unclamped, in fractions of a frame
    void paintGraph(QPainter& p, const Clip& c);
    std::vector<int> graphRows() const;
    QRect plot() const;
    std::pair<double, double> graphRange(int row) const;  // the value range drawn (frozen while dragging)
    double yForValue(int row, double v) const;
    bool graphKeyAt(const QPoint& pos, Key& key, int& row) const;
    bool handleAt(const QPoint& pos, Key& key, int& row, bool& out) const;

    EditorState* state_;
    Id clip_ = 0;
    std::vector<Row> rows_;
    std::set<Key> selection_;
    // Mouse: dragging keys, a selection box, or scrubbing the ruler.
    enum class Drag { None, Keys, Box, Scrub, GraphKey, Handle } drag_ = Drag::None;
    QPoint press_;
    QRect box_;
    std::set<Key> dragStart_;
    FrameTime dragDelta_ = 0;
    bool dragging_ = false;
    // Graph.
    QToolButton* graphButton_ = nullptr;
    bool graph_ = false;
    int graphRow_ = -1;
    Key dragKey_;
    int dragRow_ = -1;
    bool dragOut_ = true;
    std::map<int, std::pair<double, double>> frozen_;  // ranges held while dragging
};

}  // namespace montage
