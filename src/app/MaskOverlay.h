// Montage — on-screen mask editing in the Program monitor: the ellipse and
// rectangle masks of the selected clip's effects are drawn over the picture,
// and can be moved by dragging inside them and resized with their handles.
#pragma once

#include <QObject>
#include <QPointF>
#include <vector>

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
        int shape = 0;            // 1 ellipse, 2 rectangle
        double x = 0.5, y = 0.5;  // centre, fraction of the clip frame
        double w = 0.4, h = 0.4;  // size, fractions of the clip frame
        double rotation = 0;      // degrees
    };
    // The shape masks of the selected clip's effects at the playhead.
    std::vector<Shape> shapes() const;
    // Widget positions of a mask's centre and of its width and height handles.
    bool handles(const Shape& s, QPointF& center, QPointF& widthHandle, QPointF& heightHandle) const;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    enum class Grab { None, Move, Width, Height };
    void paint(QPainter& p, const QRectF& r) const;
    // Widget point <-> clip frame fractions (u, v) for the clip of `s`.
    bool toWidget(const Shape& s, double u, double v, QPointF& out) const;
    bool fromWidget(const Shape& s, const QPointF& pt, double& u, double& v) const;
    // A point of the mask outline (local coordinates in the clip's pixels) as clip fractions.
    void localToFrame(const Shape& s, double lx, double ly, double& u, double& v) const;
    void apply(const Shape& s);

    EditorState* state_;
    ViewerWidget* viewer_;
    Grab grab_ = Grab::None;
    Shape dragged_;
    double grabDu_ = 0, grabDv_ = 0;  // pointer offset from the centre when moving
    int dragSerial_ = 0;              // one undo step per drag
};

}  // namespace montage
