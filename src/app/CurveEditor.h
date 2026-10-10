// Montage — a curve you shape by hand (for Curves and Hue Curves): drag a
// point, click the line to add one, double-click a point to remove it. The
// curve is kept as the effect stores it, "x,y x,y ...". Hue curves wrap round
// and sit on a rainbow; level curves (luma or saturation in, a change out)
// sit flat at the middle when they change nothing.
#pragma once

#include <QPointF>
#include <QString>
#include <QWidget>
#include <vector>

namespace montage {

class CurveEditor : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Tone, Hue, Level };
    explicit CurveEditor(Mode mode, QWidget* parent = nullptr);

    void setPoints(const QString& points);  // does not emit
    QString points() const;
    // The curve's value at x (as the renderer computes it), for display and tests.
    double valueAt(double x) const;
    // Widget position of point i, and of curve coordinates.
    QPointF toWidget(QPointF curve) const;
    QPointF fromWidget(QPointF pos) const;
    int count() const { return int(pts_.size()); }
    bool dragging() const { return drag_ >= 0; }

    QSize sizeHint() const override { return {220, 140}; }
    QSize minimumSizeHint() const override { return {120, 80}; }

signals:
    // While dragging (`final` false) and when the change is done (`final` true).
    void edited(const QString& points, bool final);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    QRectF plot() const;
    int pointAt(QPointF pos) const;
    void sortPoints();

    Mode mode_;
    std::vector<QPointF> pts_;
    int drag_ = -1;
};

}  // namespace montage
