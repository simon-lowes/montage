// Montage — Clip › Match Colour Chart… (Resolve's Color Match): the clip's frame with a ColorChecker's outline over
// it, found by Find Chart or dragged by its corners onto the patches; the 24 patches as shot, as they will be once
// matched and as the chart has them, with the delta E before and after; Apply puts a Chart Match on the clip (or on
// every clip of its media). The chart is looked for in the background as the dialog opens; the corners also move with
// the keyboard.
#pragma once

#include <QDialog>
#include <QFutureWatcher>
#include <QImage>
#include <QWidget>

#include "media/Image.h"
#include "render/ColorChart.h"

class QCheckBox;
class QLabel;
class QPushButton;

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
    // The corner the keys move: 1-4 or [ and ] pick it, the arrows move it a pixel of the frame (ten with Shift).
    int currentCorner() const { return corner_; }

signals:
    void quadChanged();
    void quadEdited();  // moved by hand (the mouse or the keys)

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool event(QEvent* e) override;  // (claims its keys before the editor's shortcuts)

private:
    static bool ownKey(int key);
    QPointF toWidget(const Point2& p) const;
    QImage picture_;
    TrackQuad quad_;
    int dragging_ = -1;
    int corner_ = 0;
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
    // `frame`: the picture the clip's Chart Match receives, in `space`; `view`: it as Rec.709, shown and searched.
    ChartMatchDialog(const Image& frame, const Image& view, const ColorSpace& space, const QString& clipName, QWidget* parent = nullptr);
    bool findChart();         // the chart looked for now; false (and said) if there is none
    void findChartLater();    // the same in the background (as the dialog opens, and Find Chart)
    bool searching() const { return searching_; }  // (until its result has reached the dialog)
    void setQuad(const TrackQuad& q);
    TrackQuad quad() const;
    const ChartFit& fit() const { return fit_; }
    // A fit worth applying: the chart found or its outline placed by hand, every patch inside the picture.
    bool ready() const { return ready_; }
    bool sameMedia() const;

private:
    void measure();  // the patches measured again and the fit made
    void found(bool ok, const TrackQuad& q);
    Image frame_, view_image_;
    const ColorSpace& space_;
    ChartView* view_;
    ChartSwatches* swatches_;
    QLabel* status_;
    QPushButton* find_;
    QPushButton* apply_;
    QCheckBox* sameMedia_;
    ChartFit fit_;
    bool placed_ = false;  // found, or moved by hand
    bool ready_ = false;
    bool searching_ = false;
    QFutureWatcher<std::pair<bool, TrackQuad>> watcher_;
};

}  // namespace montage
