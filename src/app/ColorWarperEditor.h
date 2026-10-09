// Montage — the Colour Warper's mesh to drag: a hue and saturation wheel (hue
// round it, saturation out from the grey centre) with twelve spokes by four
// rings of points. Drag a point to pull its colours to another hue and
// saturation (the mesh shows where everything goes); click one to select it
// and set its brightness; double-click a point to put it back.
#pragma once

#include <QImage>
#include <QPointF>
#include <QString>
#include <QWidget>

#include "core/ColorWarp.h"

namespace montage {

class ColorWarperEditor : public QWidget {
    Q_OBJECT
public:
    explicit ColorWarperEditor(QWidget* parent = nullptr);

    void setMesh(const QString& mesh);  // does not emit
    QString mesh() const { return QString::fromStdString(colorWarpToString(warp_)); }
    const ColorWarp& warp() const { return warp_; }
    // Widget position of a colour (hue in turns), and of a mesh point where it is now.
    QPointF colourPos(double hue, double sat) const;
    QPointF pointPos(int spoke, int ring) const;
    bool dragging() const { return dragSpoke_ >= 0; }
    // The selected point (spoke -1 for none), its brightness change, and putting it back.
    int selectedSpoke() const { return selSpoke_; }
    int selectedRing() const { return selRing_; }
    void select(int spoke, int ring);
    void setSelectedLuma(double stops);
    void resetSelected();

    QSize sizeHint() const override { return {240, 240}; }
    QSize minimumSizeHint() const override { return {140, 140}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w; }

signals:
    // While dragging (`final` false) and when a change is done (`final` true).
    void edited(const QString& mesh, bool final);
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    double radius() const;
    QPointF centre() const;
    bool pointAt(const QPointF& pos, int& spoke, int& ring) const;
    void fromWidget(const QPointF& pos, double& hue, double& sat) const;

    ColorWarp warp_;
    int dragSpoke_ = -1, dragRing_ = 0;
    int selSpoke_ = -1, selRing_ = 0;
    bool moved_ = false;
    mutable QImage wheel_;
};

}  // namespace montage
