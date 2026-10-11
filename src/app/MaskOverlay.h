// Montage — on-screen mask editing in the Program monitor: the ellipse,
// rectangle and gradient masks of the selected clip's effects are drawn over
// the picture, and can be moved by dragging inside them, resized with their
// handles and turned with the handle above them. Bézier masks are drawn here:
// click to place corners and drag to pull out curves, then click the first
// point to close the path. After that drag points and handles, click the
// outline to add a point, Alt-click a point to make it a corner or smooth,
// and Ctrl/Cmd-click to remove it.
// Corner Pin effects show their four corners, to drag onto the surface a
// picture is pinned to (then track it from the Inspector). Object masks are
// picked here too: click the object (Alt-click: not the
// object, drag: a box around it, Ctrl/Cmd-click a point: remove it), and the
// model segments that frame in the background.
#pragma once

#include <QImage>
#include <QObject>
#include <QPointF>
#include <QPolygonF>
#include <QThreadPool>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "core/MaskPath.h"
#include "core/Model.h"

class QPainter;
class QRectF;

namespace montage {

class EditorState;
class ViewerWidget;

class MaskOverlay : public QObject {
    Q_OBJECT
public:
    MaskOverlay(EditorState* state, ViewerWidget* viewer);

    struct Shape {
        Id clip = 0;
        Id effect = 0;
        int shape = 0;            // 1 ellipse, 2 rectangle, 5 Bézier path, 6 gradient
        double x = 0.5, y = 0.5;  // centre, fraction of the clip frame
        double w = 0.4, h = 0.4;  // size, fractions of the clip frame
        double rotation = 0;      // degrees
        std::vector<PathPoint> path;  // a Bézier mask's points (box units)
        bool open = false;            // still being drawn
        bool drawing() const { return shape == 5 && (open || path.size() < 3); }
        MaskBox box() const { return {x, y, w, h, rotation}; }
    };
    // The shape masks of the selected clip's effects at the playhead.
    std::vector<Shape> shapes() const;
    // Widget positions of a mask's centre and of its width and height handles.
    bool handles(const Shape& s, QPointF& center, QPointF& widthHandle, QPointF& heightHandle) const;
    // Widget position of the handle that turns a mask.
    bool rotateHandle(const Shape& s, QPointF& out) const;
    // Widget position of a point of a mask's box (box units: -0.5 to 0.5 across it).
    bool boxToWidget(const Shape& s, double bx, double by, QPointF& out) const;

    // The selected clip's Corner Pin effects at the playhead, with their corners
    // as fractions of the clip's frame: top left, top right, bottom right, bottom left.
    struct Pin {
        Id clip = 0;
        Id effect = 0;
        double u[4] = {0, 1, 1, 0}, v[4] = {0, 0, 1, 1};
    };
    std::vector<Pin> pins() const;
    // Widget position of corner `k` of a pin.
    bool cornerHandle(const Pin& pin, int k, QPointF& out) const;

    // The selected clip's effects with an Object mask, at the playhead.
    struct ObjectTarget {
        Id clip = 0;
        Id effect = 0;
        FrameTime local = 0;  // clip-local frame of the playhead
        std::shared_ptr<const ObjectMask> object;
        int64_t frame = 0;  // the media frame on screen
    };
    std::vector<ObjectTarget> objectTargets() const;
    // Replaces the clicks on the playhead's frame and segments it (asking to
    // download the model first if needed).
    void pickObject(const ObjectTarget& t, const std::vector<ObjectPoint>& points);
    // Segmentations still running.
    int pending() const { return pending_; }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    enum class Grab { None, Move, Width, Height, Rotate, Corner, PathPoint, PathHandle, NewPoint };
    void paint(QPainter& p, const QRectF& r) const;
    // Widget point <-> clip frame fractions (u, v) for the clip of `s`.
    bool toWidget(const Shape& s, double u, double v, QPointF& out) const;
    bool fromWidget(const Shape& s, const QPointF& pt, double& u, double& v) const;
    // A point of the mask outline (local coordinates in the clip's pixels) as clip fractions.
    void localToFrame(const Shape& s, double lx, double ly, double& u, double& v) const;
    void apply(const Shape& s);
    bool frameSize(const Shape& s, double& mw, double& mh) const;
    bool widgetToBox(const Shape& s, const QPointF& pt, double& bx, double& by) const;
    // The path in widget coordinates (closed unless it is being drawn).
    QPolygonF pathPolygon(const Shape& s) const;
    void paintPath(QPainter& p, const Shape& s) const;
    void paintGradient(QPainter& p, const Shape& s) const;
    // Clicks on a Bézier mask: its points and handles, its outline, or the next point while drawing.
    bool pathPress(const QPointF& pos, Qt::KeyboardModifiers mods);
    void applyPath(const Shape& s);
    // Changes a mask's effect at the playhead as one undo step (with the drag's).
    bool editMask(const Shape& s, const QString& label, const std::function<bool(Effect&, FrameTime)>& fn);
    std::optional<Shape> refreshed(const Shape& s) const;
    void applyPin(const Pin& pin);
    void paintPins(QPainter& p) const;
    bool toWidget(Id clip, double u, double v, QPointF& out) const;
    bool fromWidget(Id clip, const QPointF& pt, double& u, double& v) const;
    void paintObjects(QPainter& p) const;
    bool objectPress(const QPointF& pos);
    void objectRelease(const QPointF& pos, Qt::KeyboardModifiers mods);

    EditorState* state_;
    ViewerWidget* viewer_;
    Grab grab_ = Grab::None;
    Shape dragged_;
    Pin draggedPin_;
    int grabCorner_ = 0;
    int grabIndex_ = 0;     // the path point grabbed
    bool grabOut_ = false;  // its outgoing handle (else incoming)
    double grabBx_ = 0, grabBy_ = 0;  // pointer offset from the point, box units
    double grabDu_ = 0, grabDv_ = 0;  // pointer offset from the centre when moving
    int dragSerial_ = 0;              // one undo step per drag
    // Object picking: the press, and where the pointer is while dragging a box.
    bool objectGesture_ = false;
    ObjectTarget objectTarget_;
    QPointF pressPos_, dragPos_;
    QThreadPool pool_;  // one thread: segmentations finish in the order they were asked for
    int pending_ = 0;
    mutable QImage tint_;
    mutable QString tintKey_;
};

}  // namespace montage
