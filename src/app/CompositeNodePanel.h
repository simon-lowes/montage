// Montage — the Composite panel: the selected picture clip's compositing node graph (core/CompositeGraph.h) as a node
// editor draws it, its output on the right becoming the clip's picture. Inputs (the clip, a
// bin item, another track, a title, a 3D model or a colour) are joined by merges and changed by transform, mask, key,
// track, effect and grade nodes. Click a node to see its settings and effects in the Inspector; drag it to move it;
// drag from an output's dot to an input's to link them (right-click a link to take it away); Delete removes the picked
// node; double-click renames it. The toolbar adds nodes after the picked one (an input as a new layer merged over
// it), switches one off (Ctrl+D), and saves the graph as a preset or applies one. Each change is one undo step.
#pragma once

#include <QPointF>
#include <QString>
#include <QWidget>

#include "core/CompositeGraph.h"

class QToolButton;
class QMenu;

namespace montage {

class EditorState;
class CompositeNodeCanvas;

class CompositeNodePanel : public QWidget {
    Q_OBJECT
public:
    explicit CompositeNodePanel(EditorState* state, QWidget* parent = nullptr);

    // The clip shown (the Inspector's picture clip), or 0.
    Id clip() const;
    int selectedNode() const;
    void selectNode(int key);
    // The edits, on the shown clip, each one undo step; a new node is picked.
    bool addNode(const QString& kind);
    // A new input (`source` "clip", "media" with `media`, "track" with `track`, "generator" with `generator`'s type)
    // merged over the picked node: as the next layer of a picked merge, else through a new merge after it.
    bool addInput(const QString& source, Id media = 0, int track = -1, const QString& generator = {});
    bool removeSelected();
    bool connectNodes(const CompositeLink& link);
    bool disconnectInput(int to, int input);
    bool setSelectedEnabled(bool on);
    bool toggleSelected();
    bool renameSelected(const QString& label);
    bool moveNode(int key, double x, double y);
    bool setSelectedBlend(const QString& mode);
    bool clearGraph();
    // Presets: the clip's graph saved as a file in the presets folder (or `path`), and one applied to the clip.
    static QString presetsFolder();
    bool savePreset(const QString& name, const QString& path = {});
    bool applyPreset(const QString& path);

    // Where things are drawn in the canvas, for tests and the mouse: a node's centre (-1: the output), and a port.
    QPointF nodeCentre(int key) const;
    QPointF portPoint(int node, bool output, int input) const;
    QWidget* canvas() const;

private:
    void sync();
    EditorState* state_;
    CompositeNodeCanvas* canvas_ = nullptr;
    QToolButton* input_ = nullptr;
    QToolButton* merge_ = nullptr;
    QToolButton* transform_ = nullptr;
    QToolButton* mask_ = nullptr;
    QToolButton* key_ = nullptr;
    QToolButton* track_ = nullptr;
    QToolButton* effect_ = nullptr;
    QToolButton* grade_ = nullptr;
    QToolButton* remove_ = nullptr;
    QToolButton* presets_ = nullptr;
    QMenu* inputMenu_ = nullptr;
    QMenu* presetMenu_ = nullptr;
    friend class CompositeNodeCanvas;
};

}  // namespace montage
