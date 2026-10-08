// Montage — a surround panner for a mixer strip: the speakers of the
// sequence's layout round a circle, and the track as a dot (with its left
// and right channels either side) to drag. The wheel narrows or widens it,
// double-click puts it back, and the context menu sets its width and LFE.
#pragma once

#include <QWidget>

#include "core/Model.h"

namespace montage {

class SurroundPanner : public QWidget {
    Q_OBJECT
public:
    explicit SurroundPanner(QWidget* parent = nullptr);

    void setSpeakerLayout(const std::string& layout);
    void setPan(const SurroundPan& p);  // does not emit
    const SurroundPan& pan() const { return pan_; }
    // Where a position (x, y in -1..1) is drawn, and the reverse.
    QPointF toWidget(double x, double y) const;
    void fromWidget(QPointF p, double& x, double& y) const;

    QSize sizeHint() const override { return {58, 58}; }

signals:
    void changed(const montage::SurroundPan& p, bool final);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    void emitChange(bool final);

    std::string layout_ = "5.1";
    SurroundPan pan_;
    bool dragging_ = false;
};

}  // namespace montage
