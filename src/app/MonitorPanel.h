// Montage — Source and Program monitors: viewer, scrub bar and transport.
#pragma once

#include <QImage>
#include <QWidget>
#include <functional>

#include "core/Model.h"

class QLabel;
class QToolButton;
class QComboBox;
class QLineEdit;

namespace montage {

class EditorState;
class PlaybackController;

// Letterboxed frame display with optional safe-area guides.
class ViewerWidget : public QWidget {
    Q_OBJECT
public:
    explicit ViewerWidget(QWidget* parent = nullptr);
    void setImage(const QImage& img);
    const QImage& image() const { return image_; }
    void setPlaceholder(const QString& text);
    void setSafeMargins(bool on);
    void setDragSource(bool on) { dragSource_ = on; }
    // Drawn over the picture; gets the rectangle the picture occupies.
    void setOverlay(std::function<void(QPainter&, const QRectF&)> paint) { overlay_ = std::move(paint); }
    QRectF imageRect() const;
    QSize sizeHint() const override { return {480, 270}; }

signals:
    void dragRequested();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

private:
    QImage image_;
    QString placeholder_;
    bool safe_ = false;
    bool dragSource_ = false;
    std::function<void(QPainter&, const QRectF&)> overlay_;
    QPoint pressPos_;
};

// Thin timeline under a monitor: playhead, in/out range and markers.
class ScrubBar : public QWidget {
    Q_OBJECT
public:
    explicit ScrubBar(QWidget* parent = nullptr);
    void setRange(FrameTime duration);
    void setPosition(FrameTime t);
    void setMarks(FrameTime in, FrameTime out);
    void setMarkers(std::vector<FrameTime> markers);
    QSize sizeHint() const override { return {300, 18}; }

signals:
    void seekRequested(montage::FrameTime t);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;

private:
    FrameTime frameAt(int x) const;
    FrameTime duration_ = 1;
    FrameTime pos_ = 0;
    FrameTime in_ = -1, out_ = -1;
    std::vector<FrameTime> markers_;
};

class MonitorPanel : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Source, Program };
    MonitorPanel(Mode mode, EditorState* state, PlaybackController* controller, QWidget* parent = nullptr);

    ViewerWidget* viewer() const { return viewer_; }
    PlaybackController* controller() const { return controller_; }
    Mode mode() const { return mode_; }
    // Re-reads duration, marks and timecode from the state / controller.
    void refresh();

signals:
    void activated();  // the user interacted with this monitor
    void exportFrameRequested();

protected:
    void mousePressEvent(QMouseEvent* e) override;

private:
    void markIn();
    void markOut();
    void goToIn();
    void goToOut();
    FrameTime duration() const;
    FrameTime inPoint() const;
    FrameTime outPoint() const;

    Mode mode_;
    EditorState* state_;
    PlaybackController* controller_;
    ViewerWidget* viewer_;
    ScrubBar* scrub_;
    QLineEdit* timecode_;
    QLabel* durationLabel_;
    QToolButton* playButton_;
    QComboBox* resolution_ = nullptr;
};

}  // namespace montage
