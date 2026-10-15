// Montage — restoration painting on the Program monitor: while a Paint effect is armed (its Inspector button), drag
// to paint a clone or heal stroke (Alt-click first to choose where it copies from; after that the same offset is kept),
// and while a Dust Spot Removal is armed, click dirt to mark a spot on that frame (Ctrl/Cmd-click a spot to remove it). The
// brush shows as a circle, the source as a cross; this frame's strokes and spots are outlined. Esc stops.
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
        bool dust = false;    // Dust Spot Removal (else Paint)
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

    EditorState* state_;
    ViewerWidget* viewer_;
    QPointF mouse_;
    bool inside_ = false;
    std::optional<std::array<double, 2>> source_;   // an Alt-clicked source, for the next stroke
    std::vector<std::array<double, 2>> stroke_;     // the stroke being drawn (clip frame fractions)
    Target drawing_;
};

}  // namespace montage
