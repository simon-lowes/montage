// Montage — a colour wheel for Lift, Gamma and Gain: drag the puck towards a
// hue to push that range of the picture towards it; the further out, the
// stronger. It sets the three channel offsets so they balance (brightness is
// left to the master control). Double-click to centre it.
#pragma once

#include <QWidget>

namespace montage {

class ColorWheel : public QWidget {
    Q_OBJECT
public:
    // `scale`: the channel offset at the rim.
    explicit ColorWheel(const QString& title, double scale, QWidget* parent = nullptr);

    // Channel offsets (r, g, b); their common part is ignored.
    void setBalance(double r, double g, double b);  // does not emit
    void balance(double& r, double& g, double& b) const;
    // The puck, in the unit disc (x right, y up).
    QPointF puck() const { return puck_; }
    void setPuck(QPointF p, bool final = true);  // emits
    bool isDragging() const { return dragging_; }

    // Converts between a puck position and channel offsets (rim = `scale`).
    static void puckToRgb(QPointF puck, double scale, double& r, double& g, double& b);
    static QPointF rgbToPuck(double r, double g, double b, double scale);

    QSize sizeHint() const override { return {110, 126}; }

signals:
    void changed(double r, double g, double b, bool final);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    QRectF disc() const;
    QPointF puckFrom(QPointF pos) const;

    QString title_;
    double scale_;
    QPointF puck_;
    bool dragging_ = false;
};

}  // namespace montage
