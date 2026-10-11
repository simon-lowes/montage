// Montage — Clip › Match Colour Chart… (Resolve's Color Match): the clip's frame with a ColorChecker's outline over
// it, found by Find Chart or dragged by its corners onto the patches; the 24 patches as shot, as they will be once
// matched and as the chart has them, with the delta E before and after; Apply puts a Chart Match on the clip (or on
// every clip of its media).
#pragma once

#include <QDialog>
#include <QImage>
#include <QWidget>

#include "media/Image.h"
#include "render/ColorChart.h"

class QCheckBox;
class QLabel;

namespace montage {

class ChartView : public QWidget {
    Q_OBJECT
public:
    explicit ChartView(QWidget* parent = nullptr);
    void setPicture(const QImage& img);
    void setQuad(const TrackQuad& q);
    const TrackQuad& quad() const { return quad_; }
    QRectF pictureRect() const;  // where the frame is drawn
    QSize sizeHint() const override { return {640, 360}; }

signals:
    void quadChanged();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    QPointF toWidget(const Point2& p) const;
    QImage picture_;
    TrackQuad quad_;
    int dragging_ = -1;
};

class ChartSwatches : public QWidget {
    Q_OBJECT
public:
    explicit ChartSwatches(QWidget* parent = nullptr);
    void setColors(const ChartColors& shot, const ChartColors& matched);
    QSize sizeHint() const override { return {360, 150}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    ChartColors shot_{}, matched_{};
};

class ChartMatchDialog : public QDialog {
    Q_OBJECT
public:
    ChartMatchDialog(const Image& frame, const QString& clipName, QWidget* parent = nullptr);
    bool findChart();  // the chart looked for in the frame; false (and said) if there is none
    void setQuad(const TrackQuad& q);
    TrackQuad quad() const;
    const ChartFit& fit() const { return fit_; }
    bool sameMedia() const;

private:
    void measure();  // the patches measured again and the fit made
    Image frame_;
    ChartView* view_;
    ChartSwatches* swatches_;
    QLabel* status_;
    QCheckBox* sameMedia_;
    ChartFit fit_;
};

}  // namespace montage
