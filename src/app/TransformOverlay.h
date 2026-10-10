// Montage — on-screen transform in the Program monitor (Premiere's direct manipulation, Resolve's transform
// overlay, Final Cut's Transform tool): the selected picture clip's box is drawn over the picture. Drag inside it
// to move the clip, a corner to scale it, an edge to stretch it, just outside a corner to turn it (Shift: 15°
// steps). Moving snaps the box's centre and edges to the frame's centre and edges, with guides (Ctrl/Cmd: no
// snapping). Clicking a clip's picture selects it (the one on top). Animated settings are keyed at the playhead.
// An animated position shows its motion path (After Effects' and Premiere's): a dot a frame, so their spacing shows
// the speed, and a square at each position keyframe, which can be dragged to move that keyframe.
// One undo step a drag. Mask and Corner Pin handles (MaskOverlay) come first.
#pragma once

#include <QObject>
#include <QPointF>
#include <array>
#include <utility>
#include <vector>

#include "core/Model.h"

class QPainter;
class QRectF;

namespace montage {

class EditorState;
class ViewerWidget;

class TransformOverlay : public QObject {
    Q_OBJECT
public:
    TransformOverlay(EditorState* state, ViewerWidget* viewer);

    // The clip whose box is shown: the primary selected picture clip on screen at the playhead (0 = none).
    Id target() const;
    // The box's corners in the widget (top left, top right, bottom right, bottom left, cropped) and its anchor.
    bool box(Id clip, std::array<QPointF, 4>& corners, QPointF& anchor) const;
    // While moving: the frame lines the box is snapped to (for the guides).
    bool snappedX() const { return snapX_ >= 0; }
    bool snappedY() const { return snapY_ >= 0; }
    // An animated position's path in the widget, a point a frame (or fewer on long clips), and its keyframes (clip
    // frames and where they sit). False if the position is not animated.
    bool motionPath(Id clip, std::vector<QPointF>& path, std::vector<std::pair<FrameTime, QPointF>>& keys) const;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    enum class Grab { None, Move, Corner, EdgeX, EdgeY, Rotate, PathKey };
    void paint(QPainter& p, const QRectF& r) const;
    Grab hit(Id clip, const QPointF& pos, int& corner) const;
    // The topmost picture clip whose box holds the point, at the playhead.
    Id clipAt(const QPointF& pos) const;
    QPointF toSequence(const QPointF& widget) const;
    void drag(const QPointF& pos, Qt::KeyboardModifiers mods);

    EditorState* state_;
    ViewerWidget* viewer_;
    Grab grab_ = Grab::None;
    Id clip_ = 0;
    QPointF press_;                    // sequence pixels
    QPointF anchor_;                   // sequence pixels, at the press
    std::array<QPointF, 4> corners_;   // sequence pixels, at the press
    double posX_ = 0, posY_ = 0, scale_ = 100, scaleX_ = 100, scaleY_ = 100, rotation_ = 0;
    double snapX_ = -1, snapY_ = -1;  // sequence x / y of the guide shown, -1 = none
    FrameTime pathKey_ = 0;           // the dragged position keyframe (clip frames)
};

}  // namespace montage
