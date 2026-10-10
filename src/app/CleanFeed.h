// Montage — the Program monitor's picture alone, full screen on a chosen
// display (Premiere's Mercury Transmit, Resolve's Video Clean Feed, Final
// Cut's A/V Output, Avid's Full Screen Playback): for a client watching on a
// second screen or the TV, or for judging the picture without the interface.
// No overlays, black around the frame; Esc or a double-click closes it.
#pragma once

#include <QImage>
#include <QWidget>

namespace montage {

class CleanFeedWindow : public QWidget {
    Q_OBJECT
public:
    explicit CleanFeedWindow(QWidget* parent = nullptr);

    void setFrame(const QImage& image);
    const QImage& frame() const { return frame_; }
    // Where the frame is drawn (fitted, centred).
    QRect pictureRect() const;

signals:
    void closed();

protected:
    void paintEvent(QPaintEvent*) override;
    void keyPressEvent(QKeyEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    QImage frame_;
};

}  // namespace montage
