// Montage — Source and Program monitors: viewer, scrub bar and transport.
#pragma once

#include <QColor>
#include <QImage>
#include <QPointer>
#include <QWindow>
#include <QWidget>
#include <functional>
#include <memory>

#include "core/Model.h"
#include "DisplayCalibration.h"
#include "render/HdrView.h"

class QLabel;
class QTimer;
class QToolButton;
class QComboBox;
class QLineEdit;

namespace montage {

class EditorState;
class LiveFeed;
class PlaybackController;
struct Peaks;
class HdrSurface;

// Letterboxed frame display with optional safe-area guides.
class ViewerWidget : public QWidget {
    Q_OBJECT
public:
    explicit ViewerWidget(QWidget* parent = nullptr);
    void setImage(const QImage& img, montage::FrameTime t = -1);
    const QImage& image() const { return image_; }
    void setPlaceholder(const QString& text);
    void setSafeMargins(bool on);
    // An exposure check over the picture (ExposureView.h); 0 = off.
    void setExposureView(int mode);
    int exposureView() const { return exposure_; }
    // Focus peaking over the picture (ExposureView.h focusPeakingView), over any exposure check: edges in focus in
    // `colour`, at a sensitivity (0 low, 1 medium, 2 high), found in the picture as the monitor shows it (scaled to its
    // size) or in the picture's own pixels (1:1).
    void setFocusPeaking(bool on);
    bool focusPeaking() const { return peaking_; }
    void setPeakingColour(const QColor& colour);
    QColor peakingColour() const { return peakingColour_; }
    void setPeakingSensitivity(int sensitivity);
    int peakingSensitivity() const { return peakingSensitivity_; }
    void setPeakingAtMonitorSize(bool on);
    bool peakingAtMonitorSize() const { return peakingAtMonitorSize_; }
    // The frame as delivered (code values in `space`), for the out-of-gamut checks: used while it is the picture's.
    void setSignal(const QImage& signal, montage::FrameTime t, const QString& space);
    // The picture as drawn (with the exposure check and focus peaking; at the monitor's size when peaking is), for testing.
    QImage shownImage() const;
    // That through the display's calibration LUT (app/DisplayCalibration.h), as it reaches the screen.
    QImage drawnImage();
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
    const QImage& compareImage() const { return compare_; }
    QString compareLabel() const { return compareLabel_; }
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
    montage::HdrPicturePtr hdrPicture() const { return hdrPicture_; }
    bool hdrShowing() const;
    double hdrHeadroom() const;
    // The nits the HDR viewer shows SDR white at, where the system says; else BT.2408's 203.
    double hdrSdrWhiteNits() const;  // the display's peak over its SDR white, 1 when not showing HDR
    QString hdrStatus() const { return hdrStatus_; }  // why HDR is not showing, when asked for
    // The viewer's drawing without the picture (its area transparent), as the HDR surface draws it over the picture.
    QImage overlayImage();
    HdrSurface* hdrSurface() const;

signals:
    void hdrChanged();
    void displayMoved();  // its window moved to another screen
    void dragRequested();
    void lookStarted();
    void lookMoved(double dx, double dy);  // since the press
    void lookFinished();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    void paintContent(QPainter& p, bool picture);
    void updateHdr();
    void dropHdrSurface();
    void refreshOverlay(bool changed);  // false: uploaded only if it differs from the last

    QImage image_;
    CalibratedPicture calibrated_[4];  // the picture, the compare reference, the two-up halves
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
    mutable qint64 exposedKey_ = -1, exposedSignalKey_ = -1;
    FrameTime imageTime_ = -1, signalTime_ = -1;
    QImage signal_;
    QString signalSpace_;
    mutable int exposedMode_ = 0;
    bool overlaid() const { return exposure_ != 0 || peaking_; }  // the picture shown is not image_ as it is
    QImage exposedImage() const;                                   // image_ with the exposure check
    QSize peakingSize() const;                                     // the size peaking is found at
    bool peaking_ = false;
    QColor peakingColour_{255, 40, 40};
    int peakingSensitivity_ = 1;
    bool peakingAtMonitorSize_ = true;
    mutable QImage peaked_;  // exposed_ (or image_) with focus peaking
    mutable qint64 peakedKey_ = -1;
    mutable QString peakedState_;
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
    QPointer<QWindow> watchedWindow_;  // the top-level window whose display changes are watched
    QMetaObject::Connection screenWatch_;
    QPointer<QWindow> calibrationWindow_;  // the top-level window whose screen picks the calibration
    QMetaObject::Connection calibrationWatch_;
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
    // What a screen reader says of the picture: "00:00:10:00, In 00:00:05:00, Out not set".
    QString accessibleSummary() const;
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
    // (Its picture is the feed's own size, scaled down only where the monitor has fewer pixels than it (never below
    // 1280 by 720), and an HDR feed's (BT.2100 PQ or HLG) is shown as HDR while the HDR viewer shows HDR, tone mapped
    // as an HDR sequence's is otherwise.)
    // Source: a feed watched live (media/Capture.h LiveFeed), its newest picture shown with its timecode in the
    // counter, its sound's meters over the picture and a LIVE badge, while `feed` is set (null: the media as it is
    // again). `recording`: it is being recorded; `media`, the recording's media item once imported (0 until then).
    // In and Out marked while live are the pictures shown then, as frames of the recording; its length is noted as
    // the recorder knows it, so the range can be cut in at once from the growing file.
    void setLiveFeed(std::shared_ptr<LiveFeed> feed, Id media, const QString& label, bool recording = true);
    void setLiveMedia(Id media);  // (the recording's, once it has been imported)
    std::shared_ptr<LiveFeed> liveFeed() const { return live_; }
    bool liveShowing() const;  // live, and showing it (its LIVE badge on)
    Id liveMedia() const { return liveMedia_; }
    QString liveLabel() const { return liveLabel_; }
    // The live picture's place in the recording, as a frame of the Source monitor's sequence (-1: none yet).
    FrameTime liveFrame() const;
    // Mark In or Out at the picture shown (or, while live, the live picture).
    void markIn();
    void markOut();

signals:
    void activated();  // the user interacted with this monitor
    void exportFrameRequested();
    void liveChanged();  // a live feed set or ended, its media known, or its recording so far shown instead

protected:
    void mousePressEvent(QMouseEvent* e) override;

private:
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
    QToolButton* peaking_ = nullptr;  // focus peaking, with its menu
    std::string waveformRequested_;  // the media whose audio decode this monitor started
    // Live (setLiveFeed).
    void liveTick();
    bool markLive(bool in);  // false when not live
    std::shared_ptr<LiveFeed> live_;
    Id liveMedia_ = 0;
    QString liveLabel_;
    bool liveRecording_ = false;
    QTimer* liveTimer_ = nullptr;
    QToolButton* liveBadge_ = nullptr;
    std::vector<float> livePeaks_;
    int64_t liveShown_ = -1;  // the feed's picture count when last shown
    bool liveConverting_ = false;  // its newest picture is being made ready, off this thread
    void showLivePicture();
    void livePictureReady(const std::shared_ptr<LiveFeed>& feed, int64_t pictures, const QImage& view, montage::HdrPicturePtr hdr);

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
