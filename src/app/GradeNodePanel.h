// Montage — the Nodes panel: the selected picture clip's colour nodes (core/GradeGraph.h) as Resolve's node editor
// draws them. The clip's picture comes in on the left and the output leaves on the right; correctors, parallel and
// layer mixers and key mixers between them, joined by picture (green) and key (blue) links. Click a node to see only
// its effects in the Inspector; drag it to move it; drag from an output's dot to an input's to link them (right-click
// a link to take it away); Delete removes the picked node. The toolbar and the context menu add nodes after the picked
// one (Alt+S serial, Alt+P parallel, Alt+L layer, Alt+O outside), switch one off (Ctrl+D), share it with other clips,
// use a shared one, and flatten the graph back into an effect stack. Each change is one undo step.
#pragma once

#include <QPointF>
#include <QWidget>

#include "core/GradeGraph.h"

class QToolButton;
class QMenu;

namespace montage {

class EditorState;
class GradeNodeCanvas;

class GradeNodePanel : public QWidget {
    Q_OBJECT
public:
    explicit GradeNodePanel(EditorState* state, QWidget* parent = nullptr);

    // The clip shown (the Inspector's picture clip), or 0.
    Id clip() const;
    int selectedNode() const;
    void selectNode(int key);
    // The edits, on the shown clip, each one undo step; a new node is picked.
    bool convert();
    bool addNode(edit::NodeHow how);
    bool addKeyMixer();  // the picked corrector's key input fed by a key mixer joining its current key
    bool removeSelected();
    bool connectNodes(const GradeLink& link);
    bool disconnectInput(int to, int input);
    bool setSelectedEnabled(bool on);
    bool toggleSelected();
    bool renameSelected(const QString& label);
    bool moveNode(int key, double x, double y);
    bool shareSelected(const QString& name);
    bool useShared(Id shared);
    bool unshareSelected();
    bool flatten();

    // Where things are drawn in the canvas, for tests and the mouse: a node's centre, and a port (output or input
    // `input`; `key`: a corrector's key output).
    QPointF nodeCentre(int key) const;
    QPointF portPoint(int node, bool output, int input, bool key = false) const;
    QWidget* canvas() const;

private:
    void sync();
    EditorState* state_;
    GradeNodeCanvas* canvas_ = nullptr;
    QToolButton* convert_ = nullptr;
    QToolButton* serial_ = nullptr;
    QToolButton* parallel_ = nullptr;
    QToolButton* layer_ = nullptr;
    QToolButton* outside_ = nullptr;
    QToolButton* remove_ = nullptr;
    QToolButton* share_ = nullptr;
    QToolButton* shared_ = nullptr;  // Use Shared, a menu of the sequence's shared nodes
    QToolButton* flatten_ = nullptr;
    QMenu* sharedMenu_ = nullptr;
    friend class GradeNodeCanvas;
};

}  // namespace montage
