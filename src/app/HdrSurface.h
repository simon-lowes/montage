// Montage — the viewer's HDR surface: a native window (embedded over the viewer) drawn with Qt's RHI into an
// extended-range swap chain (macOS EDR through Metal, Windows HDR through Direct3D 11's scRGB), or on Linux into a
// 10-bit OpenGL one written as BT.2100 PQ or HLG and described so to the Wayland compositor (app/WaylandColor.h), so
// HDR pictures are shown as the light they ask for instead of tone mapped to SDR. It draws the picture
// (render/HdrView.h light, rolled off to the display's headroom as it changes) and over it the viewer's own drawing
// (guides, handles, labels) at SDR white, and hands the viewer every mouse, wheel and drag-and-drop event it gets.
// Built with Qt 6.7 or later.
#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QPointer>
#include <QRectF>
#include <QTimer>
#include <QWindow>
#include <memory>

#include "render/HdrView.h"

class QWidget;

namespace montage {

// Whether this build has the HDR viewer (Qt 6.7 or later, with its RHI).
bool hdrViewerBuilt();
// Whether it can describe its surface to Wayland compositors (Linux: Montage's binding of Wayland's colour management,
// built where wayland-protocols 1.41 or later is found, else Qt's own from 6.10.2).
bool hdrViewerOnWaylandBuilt();

#ifdef MONTAGE_HDR_VIEWER

class HdrSurfaceGpu;
class WaylandColor;

class HdrSurface : public QWindow {
    Q_OBJECT
public:
    // Input goes on to `target`, the widget the surface covers.
    explicit HdrSurface(QWidget* target);
    ~HdrSurface() override;

    // Makes the native window and its GPU device and asks whether the display it is on takes an extended-range swap
    // chain; false (and the reason) if not, or if there is no GPU device.
    bool probe(QString* why = nullptr);
    // The swap chain is extended-range, on a display that shows it.
    bool hdrActive() const { return hdrActive_; }
    // The display's peak over its SDR white (1 on an SDR display); macOS changes it with the brightness.
    double headroom() const { return headroom_; }
    // What 1.0 (SDR white) is written as: 1 on macOS, SDR white's nits / 80 for Windows' scRGB.
    float scale() const { return scale_; }
    // SDR white in nits where the system says (Windows; on Wayland the compositor's reference white for the surface),
    // else 0 (macOS EDR is relative to the display's brightness).
    double sdrWhiteNits() const { return whiteNits_; }
    // Bits per channel of the swap chain's buffers: 16 (half floats) on macOS and Windows, 10 on Wayland.
    int bufferBits() const;
    // On Wayland, the transfer the surface is described with (PQ or HLG; Transfer::Linear while not yet described).
    Transfer describedTransfer() const;
    // Why HDR stopped or was refused (empty while it shows).
    const QString& problem() const { return problem_; }

    void setPicture(HdrPicturePtr picture, const QRectF& rect);  // rect in the window's coordinates
    void setOverlay(const QImage& overlay);                      // the viewer's drawing, the window's size
    const QImage& overlay() const { return overlay_; }
    int framesDrawn() const { return frames_; }

    // Tests: a GPU-less device (Qt's Null RHI) that claims an HDR display with this headroom (0 = off).
    static void setTestDisplay(double headroom);

signals:
    // The display changed what it can show (moved to another display, brightness, HDR switched off).
    void displayChanged();

protected:
    void exposeEvent(QExposeEvent*) override;
    bool event(QEvent* e) override;

private:
    bool forward(QEvent* e);
    void render();
    void readDisplay();
    bool displayShowsHdr();  // the display the viewer is on takes the extended-range swap chain (sets problem_ if not)
    void stopHdr(const QString& why);
    void describeSurface();  // Wayland: the surface's image description, for the picture shown (before each frame)

    QPointer<QWidget> target_;
    std::unique_ptr<HdrSurfaceGpu> gpu_;
    HdrPicturePtr picture_;
    QRectF rect_;
    bool pictureDirty_ = false;
    QImage overlay_;
    bool overlayDirty_ = false;
    bool hdrActive_ = false;
    double headroom_ = 1;
    float scale_ = 1;
    double whiteNits_ = 0;
    bool encoded_ = false;  // Linux: drawn as linear light, then written as PQ or HLG into a 10-bit swap chain
    std::unique_ptr<WaylandColor> wayland_;
    int frames_ = 0;
    QString problem_;
    QElapsedTimer displayRead_;  // since the display's HDR range was last asked
    QTimer displayTimer_;        // asks again while shown, paused too (brightness, the HDR switch)
};

#endif

}  // namespace montage
