// Montage — painting and picking on the Program monitor: while a Paint effect is armed (its Inspector button), drag
// to paint a clone or heal stroke (Alt-click first to choose where it copies from; after that the same offset is kept);
// while a Dust Spot Removal is armed, click dirt to mark a spot on that frame (Ctrl/Cmd-click a spot to remove it);
// while a Lens Blur (Depth) is armed, click what to focus on (a key there when its focus point is keyed, so clicks on
// two frames make a rack); while a Mesh Warp is armed, click to place a pin of the kind chosen in the Inspector, drag a
// pin to move it (a key there when it is keyed) and Ctrl/Cmd-click one to remove it; while a Grid Warp is armed, drag
// its points. The brush shows as a circle, the source as a cross, the focus point as a target, pins as dots (their
// reach as circles) and the grid as lines; this frame's strokes and spots are outlined. Esc stops.
#pragma once

#include <QObject>
#include <QPointF>
#include <array>
#include <optional>
#include <vector>

#include "core/Model.h"

class QPainter;
class QRectF;

namespace montage {

class EditorState;
class ViewerWidget;

class PaintOverlay : public QObject {
    Q_OBJECT
public:
    PaintOverlay(EditorState* state, ViewerWidget* viewer);

    // The armed effect, on a selected clip under the playhead.
    struct Target {
        Id clip = 0;
        Id effect = 0;
        bool dust = false;    // Dust Spot Removal
        bool focus = false;   // Lens Blur (Depth): a click picks the focus point (else Paint)
        bool mesh = false;    // Mesh Warp: pins
        bool grid = false;    // Grid Warp: its points
        FrameTime lt = 0;     // the clip frame at the playhead
        double brush = 30;    // the brush's size, sequence pixels
    };
    std::optional<Target> target() const;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    void paint(QPainter& p, const QRectF& picture);
    bool toWidget(Id clip, double u, double v, QPointF& out) const;
    bool fromWidget(Id clip, const QPointF& pt, double& u, double& v) const;
    // The brush's radius as a fraction of the clip frame's width, at a widget point.
    double brushRadius(const Target& t, const QPointF& at) const;
    void finishStroke();
    bool markSpot(const Target& t, const QPointF& pos, bool remove);
    void pickFocus(const Target& t, double u, double v);
    // A Mesh Warp's pins where the monitor shows them on this frame (k, widget point), and a Grid Warp's points
    // (r * 1000 + c, widget point); the one within reach of `at` (0 / -1: none).
    std::vector<std::pair<int, QPointF>> handles(const Target& t, const Effect& e) const;
    int handleAt(const Target& t, const Effect& e, const QPointF& at) const;
    void placePin(const Target& t, const QPointF& at);
    void dropHandle(const Target& t, const QPointF& at, bool remove);

    EditorState* state_;
    ViewerWidget* viewer_;
    QPointF mouse_;
    bool inside_ = false;
    std::optional<std::array<double, 2>> source_;   // an Alt-clicked source, for the next stroke
    std::vector<std::array<double, 2>> stroke_;     // the stroke being drawn (clip frame fractions)
    Target drawing_;
    int dragging_ = -1;  // the pin or grid point being dragged ...
    QPointF pressAt_, grab_;  // ... where it was pressed, and from there to the handle's middle
};

}  // namespace montage
