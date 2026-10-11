// Montage — Source and Program monitors: viewer, scrub bar and transport.
#pragma once

#include <QImage>
#include <QPointer>
#include <QWidget>
#include <functional>
#include <memory>

#include "core/Model.h"
#include "render/HdrView.h"

class QLabel;
class QTimer;
class QToolButton;
class QComboBox;
class QLineEdit;

namespace montage {

class EditorState;
class PlaybackController;
struct Peaks;
class HdrSurface;

// Letterboxed frame display with optional safe-area guides.
class ViewerWidget : public QWidget {
    Q_OBJECT
public:
    explicit ViewerWidget(QWidget* parent = nullptr);
    void setImage(const QImage& img);
    const QImage& image() const { return image_; }
    void setPlaceholder(const QString& text);
    void setSafeMargins(bool on);
    // An exposure check over the picture (ExposureView.h); 0 = off.
    void setExposureView(int mode);
    int exposureView() const { return exposure_; }
    // The picture as drawn (with the exposure check), for testing.
    QImage shownImage() const;
    void setDragSource(bool on) { dragSource_ = on; }
    // Drawn over the picture, in the order added; each gets the rectangle the picture occupies.
    void addOverlay(std::function<void(QPainter&, const QRectF&)> paint) { overlays_.push_back(std::move(paint)); }
    QRectF imageRect() const;
    QSize sizeHint() const override { return {480, 270}; }

    // Split-screen compare: `reference` on the left of the divider, the
    // picture on the right. Drag the divider to wipe between them.
    void setCompare(const QImage& reference, const QString& label = QString());
    void clearCompare();
    bool comparing() const { return !compare_.isNull(); }
    double split() const { return split_; }
    void setSplit(double s);
    // The divider's x position in the widget.
    double dividerX() const;

    // Two-up (trimming): two pictures side by side, each fitted to its half and
    // labelled underneath, in place of the picture. A null image shows black.
    void setTwoUp(const QImage& left, const QImage& right, const QString& leftLabel, const QString& rightLabel);
    void clearTwoUp();
    bool twoUp() const { return twoUp_; }
    QString twoUpLabel(bool right) const { return twoUpLabels_[right ? 1 : 0]; }
    // Where each half's picture is drawn.
    QRectF twoUpRect(bool right) const;
    // Look around (a 360° view): dragging the picture reports how far it moved, as fractions of its width and
    // height, instead of starting a drag.
    void setLookAround(bool on);
    bool lookAround() const { return lookAround_; }
    // HDR viewing (app/HdrSurface.h): HDR pictures (render/HdrView.h) shown as the light they ask for on an HDR or EDR
    // display, the viewer's own drawing over them. On when asked for, built in and the display shows HDR (else the SDR
    // picture, as always); in compare, exposure check and two-up those are drawn over it in SDR.
    void setHdrViewer(bool on);
    bool hdrViewer() const { return hdrWanted_; }
    void setHdrPicture(montage::HdrPicturePtr picture);  // null: none (an SDR sequence)
    bool hdrShowing() const;
    double hdrHeadroom() const;  // the display's peak over its SDR white, 1 when not showing HDR
    QString hdrStatus() const { return hdrStatus_; }  // why HDR is not showing, when asked for
    // The viewer's drawing without the picture (its area transparent), as the HDR surface draws it over the picture.
    QImage overlayImage();
    HdrSurface* hdrSurface() const;

signals:
    void hdrChanged();
    void dragRequested();
    void lookStarted();
    void lookMoved(double dx, double dy);  // since the press
    void lookFinished();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    void paintContent(QPainter& p, bool picture);
    void updateHdr();
    void dropHdrSurface();
    void refreshOverlay();

    QImage image_;
    QImage compare_;
    QString compareLabel_;
    bool twoUp_ = false;
    QImage twoUpImages_[2];
    QString twoUpLabels_[2];
    double split_ = 0.5;
    bool draggingSplit_ = false;
    QString placeholder_;
    bool safe_ = false;
    int exposure_ = 0;
    mutable QImage exposed_;           // image_ with the exposure check
    mutable qint64 exposedKey_ = -1;
    mutable int exposedMode_ = 0;
    bool dragSource_ = false;
    bool lookAround_ = false, looking_ = false;
    std::vector<std::function<void(QPainter&, const QRectF&)>> overlays_;
    QPoint pressPos_;
    bool hdrWanted_ = false;
    montage::HdrPicturePtr hdrPicture_;
    QString hdrStatus_;
    HdrSurface* surface_ = nullptr;   // (owned by its container)
    QPointer<QWidget> container_;
    QTimer* overlayTimer_ = nullptr;
    QImage lastOverlay_;
    bool lastShowing_ = false, lastWanted_ = false;  // what hdrChanged last said
    double lastHeadroom_ = 1;
    QString lastStatus_;
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
    // An audio waveform behind the bar (the Source monitor's, as Premiere shows over its mini timeline): min/max
    // peaks, each frame `secondsPerFrame` long. Null clears it; the bar is taller while one is shown.
    void setWaveform(std::shared_ptr<const Peaks> peaks, double secondsPerFrame);
    bool hasWaveform() const { return bool(peaks_); }
    QSize sizeHint() const override { return {300, height()}; }

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
    std::shared_ptr<const Peaks> peaks_;
    double secondsPerFrame_ = 1.0 / 30;
};

class MonitorPanel : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Source, Program };
    MonitorPanel(Mode mode, EditorState* state, PlaybackController* controller, QWidget* parent = nullptr);

    ViewerWidget* viewer() const { return viewer_; }
    ScrubBar* scrubBar() const { return scrub_; }
    PlaybackController* controller() const { return controller_; }
    Mode mode() const { return mode_; }
    // Re-reads duration, marks and timecode from the state / controller.
    void refresh();
    // Two-up trim view (as in Premiere, Resolve, Avid and Final Cut): while an
    // edit is trimmed, the program frames either side of it (-1: none, shown
    // black) side by side, each labelled. Rendered in the background at the
    // viewer's size, the latest request winning; the picture comes back when it ends.
    void showTrimView(FrameTime left, FrameTime right, const QString& leftLabel, const QString& rightLabel);
    void endTrimView();
    bool trimViewShown() const { return trimView_; }
    // Program: the selected clip whose Reframe 360° view a drag on the picture aims (0 = none).
    Id lookClip() const { return lookClip_; }
    // HDR viewing (ViewerWidget::setHdrViewer): HDR sequences shown as HDR where the build and display can; the
    // controller renders their light only while that is so. An "HDR" badge says when it is.
    void setHdrViewer(bool on);

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
    void updateWaveform();  // Source: the media's audio waveform on the scrub bar

    Mode mode_;
    EditorState* state_;
    PlaybackController* controller_;
    ViewerWidget* viewer_;
    ScrubBar* scrub_;
    QLineEdit* timecode_;
    QLabel* durationLabel_;
    QLabel* hdrBadge_ = nullptr;
    QToolButton* playButton_;
    QComboBox* resolution_ = nullptr;
    QComboBox* exposure_ = nullptr;  // exposure check (ExposureView.h)
    std::string waveformRequested_;  // the media whose audio decode this monitor started

    void renderTrimView();
    void updateLookAround();
    Id lookClip_ = 0;
    double lookYaw_ = 0, lookPitch_ = 0, lookFov_ = 100;  // the view when the drag began
    FrameTime lookAt_ = 0;                               // clip-local frame it is aimed at
    bool trimView_ = false, trimBusy_ = false, trimPending_ = false;
    FrameTime trimFrames_[2] = {-1, -1};
    QString trimLabels_[2];
};

}  // namespace montage
